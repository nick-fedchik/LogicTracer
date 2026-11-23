// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/NativeEvents.hpp"

#include <QtGlobal>

#ifdef __linux__
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/gpio.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#endif

namespace lt::events {
#ifdef __linux__
namespace {

struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

Result failure(const std::string& what)
{
    const int code = errno;
    const char* kind = (code == EACCES || code == EPERM) ? "PermissionDenied" :
                       (code == ENOENT || code == ENODEV || code == ENXIO) ? "NotFound" :
                       code == EBUSY ? "Busy" : code == ENETDOWN ? "InterfaceDown" :
                       (code == EOPNOTSUPP || code == EINVAL || code == ENOTTY) ? "Unsupported" : "InputError";
    return {Status::Error, what + ": " + std::strerror(code), {}, kind};
}

Result recordCan(const Request& r, const Cancel& cancel)
{
    if (r.device.size() >= IFNAMSIZ)
        return {Status::Error, "invalid CAN interface name: " + r.device, {}};
    Fd socket(::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, CAN_RAW));
    if (socket.value < 0)
        return failure("cannot open a SocketCAN receive socket");
    ifreq iface{};
    std::memcpy(iface.ifr_name, r.device.c_str(), r.device.size() + 1);
    if (::ioctl(socket.value, SIOCGIFINDEX, &iface) < 0)
        return failure("cannot find CAN interface " + r.device);
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = iface.ifr_ifindex;
    if (::ioctl(socket.value, SIOCGIFHWADDR, &iface) < 0)
        return failure("cannot inspect interface " + r.device);
    if (iface.ifr_hwaddr.sa_family != ARPHRD_CAN)
        return {Status::Error, r.device + " is not a CAN interface", {}};
    if (::ioctl(socket.value, SIOCGIFFLAGS, &iface) < 0)
        return failure("cannot inspect CAN link " + r.device);
    if (!(iface.ifr_flags & IFF_UP))
        return {Status::Error, r.device + " is down; select an already configured, up CAN interface", {}, "InterfaceDown"};
    const int on = 1;
    if (::setsockopt(socket.value, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0 ||
        ::setsockopt(socket.value, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0)
        return failure("cannot enable CAN FD reception and kernel receive timestamps");
    if (::bind(socket.value, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        return failure("cannot bind CAN receiver to " + r.device);
    return collectNativeEvents(socket.value, r, cancel);
}

Result recordGpio(const Request& r, const Cancel& cancel)
{
#ifdef GPIO_V2_GET_LINE_IOCTL
    Fd chip(::open(r.device.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (chip.value < 0)
        return failure("cannot open GPIO chip " + r.device);
    gpio_v2_line_info info{};
    info.offset = r.line;
    if (::ioctl(chip.value, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0)
        return failure("cannot inspect " + r.device + " line " + std::to_string(r.line));
    if (info.flags & GPIO_V2_LINE_FLAG_USED)
        return {Status::Error, "GPIO line is already in use; select a free input", {}, "Busy"};
    if ((info.flags & GPIO_V2_LINE_FLAG_OUTPUT) || !(info.flags & GPIO_V2_LINE_FLAG_INPUT))
        return {Status::Error, "GPIO line is not configured as an input; refusing to change its direction", {}, "NotInput"};
    gpio_v2_line_request req{};
    req.offsets[0] = r.line;
    req.num_lines = 1;
    std::strcpy(req.consumer, "logictracer");
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    // Stable GPIO v2 UAPI bit; JetPack backports it although Jammy's userspace header lacks the name.
    if (r.hte)
        req.config.flags |= uint64_t{1} << 12;
    if (::ioctl(chip.value, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
        return failure("cannot request " + r.device + " line " + std::to_string(r.line) +
                       (r.hte ? " with HTE (no fallback)" : " with monotonic timestamps"));
    Fd line(req.fd);
    return collectNativeEvents(line.value, r, cancel);
#else
    (void)r; (void)cancel;
    return {Status::Error, "GPIO character-device v2 is unavailable in this build", {}};
#endif
}

std::string readCan(int fd, nlohmann::json& event)
{
    canfd_frame frame{};
    iovec iov{&frame, sizeof(frame)};
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(timespec))]{};
    msghdr msg{};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
    const auto size = ::recvmsg(fd, &msg, MSG_DONTWAIT);
    if (size < 0)
        return (errno == EAGAIN || errno == EINTR) ? "" : std::strerror(errno);
    const bool isFd = size == CANFD_MTU;
    if ((!isFd && size != CAN_MTU) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        frame.len > (isFd ? CANFD_MAX_DLEN : CAN_MAX_DLEN))
        return "incomplete or invalid SocketCAN frame";
    for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPNS || c->cmsg_len < CMSG_LEN(sizeof(timespec)))
            continue;
        timespec ts{};
        std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
        const bool extended = frame.can_id & CAN_EFF_FLAG, rtr = frame.can_id & CAN_RTR_FLAG;
        if ((frame.can_id & CAN_ERR_FLAG) || (isFd && rtr))
            return "unexpected CAN error or FD remote frame";
        std::string data;
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned i = 0; !rtr && i < frame.len; ++i) {
            data += digits[frame.data[i] >> 4];
            data += digits[frame.data[i] & 15];
        }
        event = {{"timestamp_ns", static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec},
                 {"id", frame.can_id & (extended ? CAN_EFF_MASK : CAN_SFF_MASK)}, {"extended", extended},
                 {"fd", isFd}, {"rtr", rtr}, {"brs", isFd && (frame.flags & CANFD_BRS)},
                 {"esi", isFd && (frame.flags & CANFD_ESI)}, {"length", frame.len}, {"data_hex", data}};
        return {};
    }
    return "SocketCAN frame without a kernel receive timestamp";
}

}  // namespace

Result collectNativeEvents(int fd, const Request& r, const Cancel& cancel)
{
    using namespace std::chrono;
    using json = nlohmann::json;
    const bool gpio = r.source == Source::Gpio;
    json source = gpio ? json{{"kind", "gpio"}, {"chip", r.device}, {"line", r.line},
                              {"clock", r.hte ? "hte" : "monotonic"}, {"initial_level", nullptr}}
                       : json{{"kind", "socketcan"}, {"interface", r.device}, {"clock", "socket-software-realtime"}};
    auto rows = json::array();
#ifdef GPIO_V2_GET_LINE_IOCTL
    uint32_t previous = 0;
#endif
    const auto end = steady_clock::now() + milliseconds(r.durationMs);
    while (rows.size() < static_cast<size_t>(r.limit)) {
        if (cancel && cancel())
            return {Status::Cancelled, "event recording cancelled", {}};
        const auto left = ceil<milliseconds>(end - steady_clock::now()).count();
        if (left <= 0)
            break;
        pollfd pfd{fd, POLLIN, 0};
        const auto n = ::poll(&pfd, 1, static_cast<int>(std::min<int64_t>(left, 50)));
        if (n < 0 && errno != EINTR)
            return failure("polling " + r.device);
        if (n <= 0)
            continue;
        if (!(pfd.revents & POLLIN)) {
            if (!gpio && (pfd.revents & POLLERR)) {
                int reason = 0;
                socklen_t len = sizeof(reason);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &reason, &len) == 0 && reason == ENETDOWN)
                    return {Status::Error, r.device + " went down during event recording", {}, "InterfaceDown"};
            }
            return {Status::Error, r.device + ": input disconnected or link went down", {}};
        }
        json event;
        if (!gpio) {
            if (const auto error = readCan(fd, event); !error.empty())
                return {Status::Error, r.device + ": " + error, {}};
        } else {
#ifdef GPIO_V2_GET_LINE_IOCTL
            gpio_v2_line_event e{};
            const auto bytes = ::read(fd, &e, sizeof(e));
            if (bytes < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (bytes != sizeof(e) || e.offset != r.line ||
                (e.id != GPIO_V2_LINE_EVENT_RISING_EDGE && e.id != GPIO_V2_LINE_EVENT_FALLING_EDGE))
                return {Status::Error, r.device + ": invalid or truncated GPIO edge event", {}};
            event = {{"timestamp_ns", e.timestamp_ns}, {"sequence", e.seqno}, {"lost_before", e.seqno - previous - 1u},
                     {"edge", e.id == GPIO_V2_LINE_EVENT_RISING_EDGE ? "rising" : "falling"}};
            previous = e.seqno;
#else
            return {Status::Error, "GPIO v2 is unavailable", {}};
#endif
        }
        if (!event.is_null())
            rows.push_back(std::move(event));
    }
    if (rows.empty())
        return {Status::Timeout, "no events received within the requested duration", {}};
    return {Status::Ok, {}, {{"format", format}, {"schema_version", 1}, {"source", std::move(source)}, {"events", std::move(rows)}}};
}
#endif

Result record(const Request& r, const Cancel& cancel)
{
    if (const auto error = validate(r); !error.empty())
        return {Status::Error, error, {}};
    if (cancel && cancel())
        return {Status::Cancelled, "event recording cancelled", {}};
    if (qEnvironmentVariableIsSet("LOGICTRACER_DENY_HARDWARE"))
        return {Status::Error, "native input access is denied by LOGICTRACER_DENY_HARDWARE", {}, "PermissionDenied"};
#ifdef __linux__
    return r.source == Source::Can ? recordCan(r, cancel) : recordGpio(r, cancel);
#else
    return {Status::Error, "native SocketCAN/GPIO acquisition requires Linux; saved event recordings can be opened here", {}};
#endif
}
}  // namespace lt::events

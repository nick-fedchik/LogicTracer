// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Minimal emulator of the gusmanb LogicAnalyzer firmware on a pseudo-terminal, for tests that must not
// depend on real hardware. QSerialPort can open the slave side like any /dev/ttyACM*.

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fake {

struct Request {
    std::size_t size = 0;  // struct size without the command byte: 48 (V5 layout) or 56 (V6 layout)
    bool inverted = false;
    uint32_t pre = 0;
    uint32_t post = 0;
    uint32_t loop = 0;
    uint8_t mode = 0;

    [[nodiscard]] uint32_t totalSamples() const { return pre + post * (loop + 1); }
    [[nodiscard]] std::size_t bytesPerSample() const { return mode == 0 ? 1 : (mode == 1 ? 2 : 4); }
};

inline uint32_t u32(const std::vector<uint8_t>& p, std::size_t off)
{
    return p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
}

// payload[0] is the command byte, the struct follows.
inline Request parseRequest(const std::vector<uint8_t>& payload)
{
    Request r;
    r.size = payload.size() - 1;
    const auto* s = payload.data() + 1;
    const bool v6 = r.size == 56;
    const std::vector<uint8_t> st(s, s + r.size);
    r.inverted = st[2] != 0;
    r.pre = u32(st, v6 ? 44 : 36);
    r.post = u32(st, v6 ? 48 : 40);
    r.loop = v6 ? (st[52] | (st[53] << 8)) : st[44];
    r.mode = st[v6 ? 55 : 46];
    return r;
}

class Device {
public:
    using Handler = std::function<void(Device&, const std::vector<uint8_t>& payload)>;

    explicit Device(Handler handler) : m_handler(std::move(handler))
    {
        m_master = ::posix_openpt(O_RDWR | O_NOCTTY);
        ::grantpt(m_master);
        ::unlockpt(m_master);
        m_slaveName = ::ptsname(m_master);
        // Keep one slave descriptor open so the master never sees a hang-up before the host connects.
        m_keepAlive = ::open(m_slaveName.c_str(), O_RDWR | O_NOCTTY);
        termios t{};
        ::tcgetattr(m_keepAlive, &t);
        ::cfmakeraw(&t);
        ::tcsetattr(m_keepAlive, TCSANOW, &t);
        m_thread = std::thread([this] { run(); });
    }

    ~Device() { shutdown(); }

    [[nodiscard]] const std::string& port() const { return m_slaveName; }
    [[nodiscard]] int cancelCount() const { return m_cancels; }
    [[nodiscard]] std::vector<Request> requests() const
    {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }

    void send(const std::string& text) { send(std::vector<uint8_t>(text.begin(), text.end())); }
    void send(const std::vector<uint8_t>& bytes)
    {
        std::size_t done = 0;
        while (done < bytes.size() && m_master >= 0) {
            const auto n = ::write(m_master, bytes.data() + done, bytes.size() - done);
            if (n <= 0)
                return;
            done += static_cast<std::size_t>(n);
        }
    }

    // Simulates a cable being pulled: both pty ends vanish.
    void disappear()
    {
        m_stop = true;
        if (m_thread.joinable() && std::this_thread::get_id() != m_thread.get_id())
            m_thread.join();
        closeFds();
    }

    void recordRequest(const Request& r)
    {
        std::lock_guard lock(m_mutex);
        m_requests.push_back(r);
    }

private:
    void run()
    {
        std::vector<uint8_t> frame;
        int state = 0;  // 0 idle, 1 saw 0x55, 2 inside a frame
        while (!m_stop) {
            pollfd pfd{m_master, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0 || !(pfd.revents & POLLIN))
                continue;
            uint8_t buf[512];
            const auto n = ::read(m_master, buf, sizeof buf);
            if (n <= 0)
                continue;
            for (ssize_t i = 0; i < n; ++i) {
                const uint8_t b = buf[i];
                if (state == 0) {
                    if (b == 0x55)
                        state = 1;
                    else if (b == 0xFF)
                        ++m_cancels;
                } else if (state == 1) {
                    state = b == 0xAA ? 2 : 0;
                    frame.clear();
                } else {
                    frame.push_back(b);
                    const auto sz = frame.size();
                    if (sz >= 2 && frame[sz - 2] == 0xAA && frame[sz - 1] == 0x55) {
                        frame.resize(sz - 2);
                        state = 0;
                        dispatch(frame);
                    }
                }
            }
        }
    }

    void dispatch(const std::vector<uint8_t>& escaped)
    {
        std::vector<uint8_t> payload;
        for (std::size_t i = 0; i < escaped.size(); ++i)
            payload.push_back(escaped[i] == 0xF0 && i + 1 < escaped.size() ? static_cast<uint8_t>(escaped[++i] ^ 0xF0)
                                                                          : escaped[i]);
        if (!payload.empty())
            m_handler(*this, payload);
    }

    void closeFds()
    {
        if (m_keepAlive >= 0)
            ::close(m_keepAlive);
        if (m_master >= 0)
            ::close(m_master);
        m_keepAlive = m_master = -1;
    }

    void shutdown()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        closeFds();
    }

    Handler m_handler;
    int m_master = -1;
    int m_keepAlive = -1;
    std::string m_slaveName;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_cancels{0};
    mutable std::mutex m_mutex;
    std::vector<Request> m_requests;
};

// `version` is an upstream "V6_5" or a LogicTracer SemVer such as "0.2.0".
inline std::string idReply(const std::string& version, bool extended)
{
    const bool upstream = !version.empty() && version.front() == 'V';
    std::string s = (upstream ? "LOGIC_ANALYZER_PICO_" : "LOGICTRACER_PICO_") + version + "\n";
    if (extended)
        s += "FREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nCHANNELS:24\n";
    return s;
}

// length + samples (+ optional trailer). Every sample is `value` (little-endian, bytesPerSample wide).
inline std::vector<uint8_t> captureData(const Request& r, uint8_t value, bool withTrailer = true,
                                        uint32_t lengthOverride = 0)
{
    const uint32_t n = r.totalSamples();
    const uint32_t len = lengthOverride ? lengthOverride : n;
    std::vector<uint8_t> out{static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                             static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24)};
    for (uint32_t i = 0; i < n; ++i) {
        out.push_back(value);
        for (std::size_t b = 1; b < r.bytesPerSample(); ++b)
            out.push_back(0);
    }
    if (withTrailer)
        out.push_back(0);
    return out;
}

}  // namespace fake

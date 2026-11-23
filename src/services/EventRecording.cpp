// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/EventRecording.hpp"

#include <QFile>
#include <QSaveFile>

#include <limits>
#include <sstream>
#include <string_view>

namespace lt::events {
namespace {

using json = nlohmann::json;
constexpr qint64 maxBytes = 8 * 1024 * 1024;

bool number(const json& j, uint64_t max = std::numeric_limits<uint64_t>::max())
{
    return j.is_number_integer() && (j.is_number_unsigned() || j.get<int64_t>() >= 0) && j.get<uint64_t>() <= max;
}

bool valid(const json& doc)
{
    try {
        if (doc.size() != 4 || doc.at("format") != format || !number(doc.at("schema_version"), 1) || doc.at("schema_version") != 1)
            return false;
        const auto& s = doc.at("source");
        const auto& rows = doc.at("events");
        const bool gpio = s.at("kind") == "gpio";
        const auto& name = s.at(gpio ? "chip" : "interface");
        if (!name.is_string() || name.get_ref<const std::string&>().empty() || !rows.is_array() || rows.size() > maxEvents)
            return false;
        if (gpio) {
            if (s.size() != 5 || !number(s.at("line"), UINT32_MAX) || (s.at("clock") != "hte" && s.at("clock") != "monotonic") ||
                !(s.at("initial_level").is_null() || number(s.at("initial_level"), 1)))
                return false;
        } else if (s.size() != 3 || s.at("kind") != "socketcan" || s.at("clock") != "socket-software-realtime") {
            return false;
        }
        uint32_t previous = 0;
        for (const auto& e : rows) {
            if (!number(e.at("timestamp_ns")))
                return false;
            if (gpio) {
                if (e.size() != 4 || (e.at("edge") != "rising" && e.at("edge") != "falling") || !number(e.at("sequence"), UINT32_MAX) ||
                    !number(e.at("lost_before"), UINT32_MAX))
                    return false;
                const auto seq = e.at("sequence").get<uint32_t>();
                if (e.at("lost_before").get<uint32_t>() != seq - previous - 1u)
                    return false;
                previous = seq;
            } else {
                const bool fd = e.at("fd").get<bool>(), ext = e.at("extended").get<bool>(), rtr = e.at("rtr").get<bool>();
                const bool brs = e.at("brs").get<bool>(), esi = e.at("esi").get<bool>();
                const auto& data = e.at("data_hex").get_ref<const std::string&>();
                if (e.size() != 9 || (fd && rtr) || (!fd && (brs || esi)) || !number(e.at("id"), ext ? 0x1fffffff : 0x7ff) ||
                    !number(e.at("length"), fd ? 64 : 8) || data.size() != (rtr ? 0 : 2 * e.at("length").get<size_t>()) ||
                    data.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    return false;
            }
        }
    } catch (const json::exception&) {
        return false;
    }
    return true;
}

}  // namespace

std::string validate(const Request& r)
{
    if (r.device.empty() || r.device.find('\0') != std::string::npos)
        return "select a CAN interface or a GPIO chip explicitly";
    if (r.durationMs < 1 || r.durationMs > maxDurationMs || r.limit < 1 || r.limit > maxEvents)
        return "duration must be 1..3600000 ms and event limit must be 1..10000";
    if (r.source == Source::Can && r.hte)
        return "HTE is a GPIO event clock, not a SocketCAN clock";
    constexpr std::string_view prefix = "/dev/gpiochip";
    if (r.source == Source::Gpio && (!r.device.starts_with(prefix) || r.device.size() == prefix.size() ||
                                     r.device.find_first_not_of("0123456789", prefix.size()) != std::string::npos))
        return "select an explicit /dev/gpiochipN input chip";
    return {};
}

bool load(const QString& path, json& recording, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const auto bytes = file.read(maxBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maxBytes) {
        error = QStringLiteral("Cannot read event recording or file exceeds 8 MiB");
        return false;
    }
    auto doc = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (doc.is_discarded() || !valid(doc)) {
        error = QStringLiteral("Invalid LogicTracer event recording");
        return false;
    }
    recording = std::move(doc);
    return true;
}

bool save(const QString& path, const json& recording, QString& error)
{
    if (!valid(recording)) {
        error = QStringLiteral("Invalid LogicTracer event recording");
        return false;
    }
    QSaveFile file(path);
    const auto bytes = recording.dump(2) + '\n';
    const auto size = static_cast<qint64>(bytes.size());
    if (size > maxBytes) {
        error = QStringLiteral("Event recording exceeds 8 MiB");
        return false;
    }
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), size) != size || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

std::string describe(const json& doc)
{
    const auto& s = doc.at("source");
    const bool gpio = s.at("kind") == "gpio";
    std::ostringstream out;
    out << (gpio ? "GPIO " : "SocketCAN ") << s.at(gpio ? "chip" : "interface").get<std::string>();
    if (gpio)
        out << " line " << s.at("line") << ", initial level: "
            << (s.at("initial_level").is_null() ? "unknown" : s.at("initial_level").dump());
    out << " | clock: " << s.at("clock").get<std::string>() << " | " << doc.at("events").size() << " events\n";
    for (const auto& e : doc.at("events")) {
        out << e.at("timestamp_ns") << " ns  ";
        if (gpio) {
            out << e.at("edge").get<std::string>() << "  seq=" << e.at("sequence");
            if (e.at("lost_before") != 0)
                out << "  gap: " << e.at("lost_before") << " lost before this edge";
        } else {
            out << (e.at("fd").get<bool>() ? "CAN FD" : "CAN") << "  0x" << std::hex << e.at("id").get<uint32_t>()
                << std::dec << (e.at("extended").get<bool>() ? " (29-bit)" : " (11-bit)");
            if (e.at("rtr").get<bool>()) out << " RTR";
            if (e.at("brs").get<bool>()) out << " BRS";
            if (e.at("esi").get<bool>()) out << " ESI";
            out << "  len=" << e.at("length") << "  " << e.at("data_hex").get<std::string>();
        }
        out << '\n';
    }
    return out.str();
}

}  // namespace lt::events

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/DeviceProtocol.hpp"

#include <algorithm>
#include <charconv>
#include <regex>

namespace lt {

namespace {

void putU16(std::vector<uint8_t>& buf, std::size_t off, uint16_t v)
{
    buf[off] = static_cast<uint8_t>(v);
    buf[off + 1] = static_cast<uint8_t>(v >> 8);
}

void putU32(std::vector<uint8_t>& buf, std::size_t off, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        buf[off + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

bool needsEscape(uint8_t b) noexcept { return b == 0xAA || b == 0x55 || b == 0xF0; }

bool hasPicoHeader(std::string_view board) noexcept
{
    return board == "PICO" || board == "PICO_2" || board == "W" || board == "WIFI" || board == "2_W" ||
           board == "2_WIFI";
}

}  // namespace

std::size_t requestSize(ProtocolGeneration gen) noexcept
{
    return gen == ProtocolGeneration::V6 ? 56 : 48;
}

std::vector<uint8_t> serialize(const CaptureRequestPacket& req, ProtocolGeneration gen)
{
    // Offsets mirror the firmware's naturally aligned (ARM EABI) CAPTURE_REQUEST.
    const bool v6 = gen == ProtocolGeneration::V6;
    const std::size_t channelSlots = v6 ? 32 : 24;
    std::vector<uint8_t> buf(requestSize(gen), 0);

    buf[0] = static_cast<uint8_t>(req.triggerType);
    buf[1] = req.triggerChannel;
    buf[2] = req.invertedOrCount;
    putU16(buf, 4, req.triggerValue);
    std::copy_n(req.channels.begin(), channelSlots, buf.begin() + 6);
    const std::size_t afterChannels = 6 + channelSlots;  // 38 (V6) / 30 (V5)
    buf[afterChannels] = req.channelCount;
    const std::size_t freqOff = afterChannels + 2;       // 40 / 32
    putU32(buf, freqOff, req.frequency);
    putU32(buf, freqOff + 4, req.preSamples);
    putU32(buf, freqOff + 8, req.postSamples);
    std::size_t off = freqOff + 12;                      // 52 / 44
    if (v6) {
        putU16(buf, off, req.loopCount);
        off += 2;
    } else {
        buf[off++] = static_cast<uint8_t>(std::min<uint16_t>(req.loopCount, 255));
    }
    buf[off++] = req.measure;
    buf[off] = static_cast<uint8_t>(req.captureMode);
    return buf;
}

std::vector<uint8_t> encodeFrame(uint8_t command, std::span<const uint8_t> payload)
{
    std::vector<uint8_t> out;
    out.reserve(payload.size() * 2 + 6);
    out.push_back(0x55);
    out.push_back(0xAA);
    auto put = [&out](uint8_t b) {
        if (needsEscape(b)) {
            out.push_back(0xF0);
            out.push_back(static_cast<uint8_t>(b ^ 0xF0));
        } else {
            out.push_back(b);
        }
    };
    put(command);
    for (uint8_t b : payload)
        put(b);
    out.push_back(0xAA);
    out.push_back(0x55);
    return out;
}

std::optional<std::vector<uint8_t>> decodeFrame(std::span<const uint8_t> frame)
{
    if (frame.size() < 5 || frame[0] != 0x55 || frame[1] != 0xAA || frame[frame.size() - 2] != 0xAA ||
        frame.back() != 0x55)
        return std::nullopt;
    std::vector<uint8_t> out;
    for (std::size_t i = 2; i < frame.size() - 2; ++i) {
        if (frame[i] == 0xF0) {
            if (i + 1 >= frame.size() - 2)
                return std::nullopt;
            out.push_back(static_cast<uint8_t>(frame[++i] ^ 0xF0));
        } else {
            out.push_back(frame[i]);
        }
    }
    return out;
}

CaptureMode captureModeFor(std::span<const uint8_t> channels) noexcept
{
    uint8_t maxChannel = 0;
    for (uint8_t c : channels)
        maxChannel = std::max(maxChannel, c);
    if (maxChannel < 8)
        return CaptureMode::Channels8;
    if (maxChannel < 16)
        return CaptureMode::Channels16;
    return CaptureMode::Channels24;
}

std::string_view toString(CaptureMode mode) noexcept
{
    switch (mode) {
    case CaptureMode::Channels8: return "8ch";
    case CaptureMode::Channels16: return "16ch";
    case CaptureMode::Channels24: return "24ch";
    }
    return "unknown";
}

ProtocolGeneration requestLayoutFor(const FirmwareVersion& v)
{
    if (v.family != FirmwareFamily::Upstream)
        return ProtocolGeneration::V6;
    return (v.major > 6 || (v.major == 6 && v.minor >= 5)) ? ProtocolGeneration::V6 : ProtocolGeneration::V5;
}

std::chrono::milliseconds defaultPollLatencyFor(const FirmwareVersion& v) noexcept
{
    using namespace std::chrono_literals;
    switch (v.family) {
    case FirmwareFamily::LogicTracer: return 150ms;
    case FirmwareFamily::Replay: return 0ms;
    case FirmwareFamily::Pynq: return 0ms;
    case FirmwareFamily::Upstream: break;
    }
    return 2200ms;
}

std::optional<uint8_t> gpioForChannel(std::string_view board, uint8_t channel) noexcept
{
    // Matches PIN_MAP in firmware/rp2040/LogicAnalyzer_Board_Settings.h.
    static constexpr uint8_t kPicoMap[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 26, 27, 28, 1};
    static constexpr uint8_t kZeroMap[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 26, 27, 28, 29, 22, 23, 24, 25, 18};
    static constexpr uint8_t kInterceptorMap[] = {
        6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 2, 3, 4, 5, 1};

    if (hasPicoHeader(board)) {
        if (channel < std::size(kPicoMap))
            return kPicoMap[channel];
        return std::nullopt;
    }
    if (board == "ZERO") {
        if (channel < std::size(kZeroMap))
            return kZeroMap[channel];
        return std::nullopt;
    }
    if (board == "INTERCEPTOR") {
        if (channel < std::size(kInterceptorMap))
            return kInterceptorMap[channel];
        return std::nullopt;
    }
    return std::nullopt;
}

std::span<const PicoHeaderPin> picoHeaderPins(std::string_view board) noexcept
{
    if (!hasPicoHeader(board))
        return {};
    // Board header: https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html
    // Only the board-to-GPIO mapping lives here; capture channels come from the firmware map above.
    static const auto header = [] {
        constexpr auto none = std::nullopt;
        std::array<PicoHeaderPin, 40> pins{{
            {1, 0, "TRIG OUT"}, {2, 1, "EXT TRIG"}, {3, none, "GND"}, {4, 2, {}}, {5, 3, {}},
            {6, 4, {}}, {7, 5, {}}, {8, none, "GND"}, {9, 6, {}}, {10, 7, {}},
            {11, 8, {}}, {12, 9, {}}, {13, none, "GND"}, {14, 10, {}}, {15, 11, {}},
            {16, 12, {}}, {17, 13, {}}, {18, none, "GND"}, {19, 14, {}}, {20, 15, {}},
            {21, 16, {}}, {22, 17, {}}, {23, none, "GND"}, {24, 18, {}}, {25, 19, {}},
            {26, 20, {}}, {27, 21, {}}, {28, none, "GND"}, {29, 22, {}}, {30, none, "RUN"},
            {31, 26, {}}, {32, 27, {}}, {33, none, "AGND"}, {34, 28, {}}, {35, none, "ADC_VREF"},
            {36, none, "3V3(OUT)"}, {37, none, "3V3_EN"}, {38, none, "GND"}, {39, none, "VSYS"}, {40, none, "VBUS"}
        }};
        for (uint8_t ch = 0; ch < 24; ++ch) {
            const auto gp = gpioForChannel("PICO", ch);
            for (auto& pin : pins)
                if (gp && pin.gpio == gp)
                    pin.channel = ch;
        }
        return pins;
    }();
    return header;
}

std::optional<FirmwareVersion> parseVersionLine(std::string_view line)
{
    auto toInt = [](const std::ssub_match& sm, int& out) {
        const auto [ptr, ec] = std::from_chars(&*sm.first, &*sm.first + sm.length(), out);
        return ec == std::errc{} && ptr == &*sm.first + sm.length();
    };
    const std::string s(line);
    std::smatch m;
    FirmwareVersion v;
    v.raw = s;

    // SemVer 2.0: numeric identifiers without leading zeros, optional -pre.release and +build.
    static const std::regex lt(
        R"(^LOGICTRACER_([A-Z0-9_]+)_(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))"
        R"((?:-((?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?)"
        R"((?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)");
    if (std::regex_match(s, m, lt)) {
        v.family = FirmwareFamily::LogicTracer;
        v.board = m[1].str();
        if (!toInt(m[2], v.major) || !toInt(m[3], v.minor) || !toInt(m[4], v.patch))
            return std::nullopt;
        v.preRelease = m[5].str();
        v.buildMetadata = m[6].str();
        return v;
    }

    static const std::regex upstream(R"(^LOGIC_ANALYZER_(.*)_V([0-9]+)_([0-9]+)$)");
    if (!std::regex_match(s, m, upstream))
        return std::nullopt;
    v.family = FirmwareFamily::Upstream;
    v.board = m[1].str();
    if (!toInt(m[2], v.major) || !toInt(m[3], v.minor))
        return std::nullopt;
    return v;
}

std::string FirmwareVersion::versionString() const
{
    if (family == FirmwareFamily::Replay || family == FirmwareFamily::Pynq)
        return {};
    if (family == FirmwareFamily::Upstream)
        return std::to_string(major) + "." + std::to_string(minor);
    std::string out = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!preRelease.empty())
        out += "-" + preRelease;
    if (!buildMetadata.empty())
        out += "+" + buildMetadata;
    return out;
}

std::string_view toString(FirmwareFamily family) noexcept
{
    switch (family) {
    case FirmwareFamily::LogicTracer: return "logictracer";
    case FirmwareFamily::Replay: return "replay";
    case FirmwareFamily::Pynq: return "pynq";
    case FirmwareFamily::Upstream: break;
    }
    return "upstream";
}

std::optional<uint32_t> parseKeyValue(std::string_view line, std::string_view key)
{
    if (line.size() <= key.size() + 1 || line.substr(0, key.size()) != key || line[key.size()] != ':')
        return std::nullopt;
    const auto digits = line.substr(key.size() + 1);
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || ptr != digits.data() + digits.size())
        return std::nullopt;
    return value;
}

CaptureLimits DeviceInfo::limits(CaptureMode mode) const noexcept
{
    const uint32_t total = bufferSize / static_cast<uint32_t>(bytesPerSample(mode));
    CaptureLimits l;
    if (total < l.minPreSamples + l.minPostSamples) {
        // Degenerate buffer: leave every maximum at 0 so validation always rejects the request.
        l.maxPreSamples = l.maxPostSamples = l.maxTotalSamples = 0;
        return l;
    }
    l.maxPreSamples = std::max(total / 10, l.minPreSamples);
    l.maxPostSamples = total - l.minPreSamples;
    l.maxTotalSamples = total;
    return l;
}

std::vector<uint8_t> serialize(const TestSignalRequest& req)
{
    std::vector<uint8_t> out;
    out.push_back(req.enable ? 1 : 0);
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(req.pulses));
    out.push_back(static_cast<uint8_t>(req.pulses >> 8));
    for (const uint32_t v : {req.highNs, req.delayNs, req.periodNs, req.gapNs})
        for (int b = 0; b < 4; ++b)
            out.push_back(static_cast<uint8_t>(v >> (8 * b)));
    return out;  // 20 bytes, TEST_SIGNAL_REQUEST in the firmware
}

std::optional<TestSignalInfo> parseTestSignalReply(std::string_view line)
{
    constexpr std::string_view prefix = "TESTSIGNAL:";
    if (!line.starts_with(prefix))
        return std::nullopt;
    line.remove_prefix(prefix.size());
    TestSignalInfo info;
    if (line == "OFF")
        return info;
    uint64_t v[6] = {};
    for (int k = 0; k < 6; ++k) {
        const auto comma = k < 5 ? line.find(',') : line.size();
        if (comma == std::string_view::npos)
            return std::nullopt;
        const auto field = line.substr(0, comma);
        const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), v[k]);
        if (ec != std::errc{} || ptr != field.data() + field.size())
            return std::nullopt;
        line.remove_prefix(k < 5 ? comma + 1 : comma);
    }
    if (v[0] == 0 || v[0] > UINT32_MAX || v[5] == 0)
        return std::nullopt;
    info.running = true;
    info.sysclkHz = static_cast<uint32_t>(v[0]);
    info.highCycles = v[1];
    info.delayCycles = v[2];
    info.periodCycles = v[3];
    info.gapCycles = v[4];
    info.pulses = static_cast<uint32_t>(v[5]);
    return info;
}

}  // namespace lt

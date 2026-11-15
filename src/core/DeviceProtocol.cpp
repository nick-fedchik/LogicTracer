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
    return (v.major > 6 || (v.major == 6 && v.minor >= 5)) ? ProtocolGeneration::V6 : ProtocolGeneration::V5;
}

std::chrono::milliseconds defaultPollLatencyFor(const FirmwareVersion& v) noexcept
{
    using namespace std::chrono_literals;
    return (v.major > 6 || (v.major == 6 && v.minor >= 6)) ? 150ms : 2200ms;
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

    if (board == "PICO" || board == "PICO_2" || board == "W" || board == "WIFI" || board == "2_W" ||
        board == "2_WIFI") {
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

std::optional<FirmwareVersion> parseVersionLine(std::string_view line)
{
    static const std::regex re(R"(^LOGIC_ANALYZER_(.*)_V([0-9]+)_([0-9]+)$)");
    std::string s(line);
    std::smatch m;
    if (!std::regex_match(s, m, re))
        return std::nullopt;
    auto toInt = [](const std::ssub_match& sm, int& out) {
        const auto [ptr, ec] = std::from_chars(&*sm.first, &*sm.first + sm.length(), out);
        return ec == std::errc{} && ptr == &*sm.first + sm.length();
    };
    FirmwareVersion v;
    v.raw = s;
    v.board = m[1].str();
    if (!toInt(m[2], v.major) || !toInt(m[3], v.minor))
        return std::nullopt;
    return v;
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

}  // namespace lt

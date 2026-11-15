// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Pure (I/O-free) implementation of the gusmanb LogicAnalyzer wire protocol.
//
// Host -> device: binary frames  0x55 0xAA <escaped payload> 0xAA 0x55
//   bytes 0xAA, 0x55 and 0xF0 inside the payload are sent as {0xF0, byte ^ 0xF0}.
//   payload[0] is the command: 0 = ID/handshake, 1 = capture request, ...
// Device -> host: '\n'-terminated ASCII lines, then (after a capture) raw binary:
//   uint32 LE sample count, samples (1/2/4 bytes each, LE), uint8 timestamp count,
//   and stampCount * uint32 timestamps when stampCount > 1.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

// Firmware generations differ in the CAPTURE_REQUEST layout:
//   V5 (V5_x and V6_0..V6_4): channels[24], uint8_t loopCount  -> 48 bytes
//   V6 (V6_5+, LogicTracer):  channels[32], uint16_t loopCount -> 56 bytes
// The firmware casts the payload onto a non-packed struct compiled for ARM EABI,
// so natural alignment padding is part of the wire format.
enum class ProtocolGeneration { V5, V6 };

enum class TriggerType : uint8_t { Edge = 0, Complex = 1, Fast = 2, Blast = 3 };

enum class CaptureMode : uint8_t { Channels8 = 0, Channels16 = 1, Channels24 = 2 };

namespace cmd {
inline constexpr uint8_t Id = 0;
inline constexpr uint8_t Capture = 1;
inline constexpr uint8_t Bootloader = 4;
inline constexpr uint8_t BlinkOn = 5;
inline constexpr uint8_t BlinkOff = 6;
}  // namespace cmd

// Any single byte sent while a capture is running aborts it (no reply is sent).
inline constexpr uint8_t kCancelByte = 0xFF;

struct CaptureRequestPacket {
    TriggerType triggerType = TriggerType::Edge;
    uint8_t triggerChannel = 0;   // 0-based channel number (or base channel for patterns)
    uint8_t invertedOrCount = 0;  // edge: 0 = rising/high, 1 = falling/low; pattern: bit count
    uint16_t triggerValue = 0;    // pattern value
    std::array<uint8_t, 32> channels{};  // 0-based channel numbers; bit i of a sample = channels[i]
    uint8_t channelCount = 0;
    uint32_t frequency = 0;
    uint32_t preSamples = 0;
    uint32_t postSamples = 0;
    uint16_t loopCount = 0;
    uint8_t measure = 0;
    CaptureMode captureMode = CaptureMode::Channels8;
};

[[nodiscard]] std::size_t requestSize(ProtocolGeneration gen) noexcept;
[[nodiscard]] std::vector<uint8_t> serialize(const CaptureRequestPacket& req, ProtocolGeneration gen);

[[nodiscard]] std::vector<uint8_t> encodeFrame(uint8_t command, std::span<const uint8_t> payload = {});
// Inverse of encodeFrame (used for testing / emulation). Returns command + payload.
[[nodiscard]] std::optional<std::vector<uint8_t>> decodeFrame(std::span<const uint8_t> frame);

[[nodiscard]] constexpr std::size_t bytesPerSample(CaptureMode mode) noexcept
{
    switch (mode) {
    case CaptureMode::Channels8: return 1;
    case CaptureMode::Channels16: return 2;
    case CaptureMode::Channels24: return 4;  // 24-bit samples travel as 32-bit words
    }
    return 4;
}

// Mode is selected by the highest 0-based channel number requested.
[[nodiscard]] CaptureMode captureModeFor(std::span<const uint8_t> channels) noexcept;
[[nodiscard]] std::string_view toString(CaptureMode mode) noexcept;

// Firmware families speaking the same wire protocol:
//   Upstream     gusmanb LogicAnalyzer, identifies as  LOGIC_ANALYZER_<BOARD>_V<major>_<minor>
//   LogicTracer  this project's firmware, identifies as LOGICTRACER_<BOARD>_<SemVer 2.0>
//                (e.g. LOGICTRACER_PICO_0.2.0); built on the upstream V6_5 protocol with fast polling.
//   Replay       a recorded capture played back by the host (no firmware involved).
enum class FirmwareFamily { Upstream, LogicTracer, Replay };

struct FirmwareVersion {
    std::string raw;    // full identification line, e.g. "LOGICTRACER_PICO_0.2.0"
    std::string board;  // e.g. "PICO", "PICO_2"
    FirmwareFamily family = FirmwareFamily::Upstream;
    int major = 0;
    int minor = 0;
    int patch = 0;             // LogicTracer only
    std::string preRelease;    // SemVer pre-release, e.g. "rc.1" (LogicTracer only)
    std::string buildMetadata; // SemVer build metadata (LogicTracer only)

    // "6.5" for upstream, the SemVer string for LogicTracer.
    [[nodiscard]] std::string versionString() const;
};

[[nodiscard]] std::string_view toString(FirmwareFamily family) noexcept;

[[nodiscard]] std::optional<FirmwareVersion> parseVersionLine(std::string_view line);
// Request layout accepted by the firmware (upstream: 56 bytes since V6_5; LogicTracer: always 56 bytes).
[[nodiscard]] ProtocolGeneration requestLayoutFor(const FirmwareVersion& v);
// Worst-case completion-poll latency of the firmware main loop (upstream ~2 s; LogicTracer 10 ms ticks -> 150 ms).
[[nodiscard]] std::chrono::milliseconds defaultPollLatencyFor(const FirmwareVersion& v) noexcept;
// Maps a 0-based channel (or external trigger slot == channels) to the physical RP2040/RP2350 GPIO number.
[[nodiscard]] std::optional<uint8_t> gpioForChannel(std::string_view board, uint8_t channel) noexcept;
// Parses "KEY:<uint>" lines; returns nullopt if the key does not match.
[[nodiscard]] std::optional<uint32_t> parseKeyValue(std::string_view line, std::string_view key);

struct CaptureLimits {
    uint32_t minPreSamples = 2;
    uint32_t maxPreSamples = 0;
    uint32_t minPostSamples = 2;
    uint32_t maxPostSamples = 0;
    uint32_t maxTotalSamples = 0;
};

struct DeviceInfo {
    std::string port;
    std::string serialNumber;
    FirmwareVersion version;
    ProtocolGeneration generation = ProtocolGeneration::V6;
    uint32_t maxFrequency = 100'000'000;
    uint32_t blastFrequency = 0;
    uint32_t bufferSize = 128 * 1024;
    uint8_t channels = 24;

    [[nodiscard]] uint32_t minFrequency() const noexcept { return (maxFrequency * 2ull) / 65535u; }
    [[nodiscard]] CaptureLimits limits(CaptureMode mode) const noexcept;
};

}  // namespace lt

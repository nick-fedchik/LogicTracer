// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Device-independent description of a capture request and its outcome. Pure C++20 (no Qt), so the
// analysis layer and front-ends can use it without linking the serial driver.

#include "core/CaptureBuffer.hpp"
#include "core/DeviceProtocol.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lt {

using namespace std::chrono_literals;

// Free-form description of the test setup recorded with a capture (DUT, bitstream hash, wiring,
// switch positions), so every result can be traced back to exactly what was connected.
// Keys use [A-Za-z0-9_.-]; values are single-line text.
using SetupInfo = std::vector<std::pair<std::string, std::string>>;

struct DetectedPort {
    std::string port;  // system location, e.g. /dev/ttyACM0
    std::string description;
    std::string manufacturer;
    std::string serialNumber;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    bool vidPidMatch = false;
};

// Firmware edge trigger. With LogicTracer firmware that supports it (DeviceInfo::supportsEdgeTrigger) only a
// real transition fires; older firmware is level-sensitive ("jmp pin"): a rising trigger fires as soon as the
// line is HIGH, falling as soon as it is LOW.
struct EdgeTrigger {
    uint8_t channel = 0;    // 0-based (or base channel for pattern trigger)
    bool inverted = false;  // edge: false = rising/high, true = falling/low
    uint8_t bitCount = 0;   // 0 = single-channel edge trigger; 1..16 = contiguous pattern trigger width
    uint16_t pattern = 0;   // bit k (0 <= k < bitCount) = required level on channel (channel + k)

    [[nodiscard]] bool isPattern() const noexcept { return bitCount > 0; }
    [[nodiscard]] TriggerType patternTriggerType() const noexcept
    {
        return bitCount <= 5 ? TriggerType::Fast : TriggerType::Complex;
    }
    [[nodiscard]] std::string patternBits() const
    {
        std::string s;
        s.reserve(bitCount);
        for (uint8_t i = 0; i < bitCount; ++i)
            s.push_back(((pattern >> i) & 1u) ? '1' : '0');
        return s;
    }
    [[nodiscard]] std::string describe() const
    {
        if (isPattern()) {
            std::string s = "CH" + std::to_string(channel + 1);
            if (bitCount > 1)
                s += "-CH" + std::to_string(channel + bitCount);
            s += "==" + patternBits();
            return s;
        }
        return "CH" + std::to_string(channel + 1) + (inverted ? " falling" : " rising");
    }
};

// Segmented capture: the firmware re-arms after each post-trigger window (loopCount = count - 1) and stores
// the windows back to back: pre + post x count samples, the pre-trigger part before the first window only.
// The time between windows is not recorded.
struct SegmentLayout {
    uint32_t count = 1;
    uint32_t preSamples = 0;
    uint32_t postSamples = 0;
    std::optional<uint8_t> triggerChannel;  // hardware channel (0-based) whose edge starts every window
    bool triggerFalling = false;
    // Trigger time of every window relative to window 1 (empty when the device did not record it), and the
    // resolution of those times (firmware burst timestamps: one system clock cycle).
    std::vector<double> triggerTimesSeconds;
    double triggerTimeResolutionSeconds = 0.0;

    [[nodiscard]] uint64_t totalSamples() const noexcept
    {
        return preSamples + static_cast<uint64_t>(postSamples) * count;
    }
};

struct CaptureConfig {
    std::vector<ChannelInfo> channels;
    uint32_t frequency = 1'000'000;
    uint32_t preSamples = 2;
    uint32_t postSamples = 1000;
    // nullopt => immediate capture (auto-selects the trigger level of the first channel).
    std::optional<EdgeTrigger> trigger;
    // Max time to wait for the trigger. The firmware reports completion only on its poll tick
    // (~150 ms for LogicTracer firmware, ~2.2 s for upstream), so the device driver adds that latency on top.
    std::chrono::milliseconds timeout = 5000ms;
    // On TIMEOUT, run probe() on the same channels to report the current line state.
    bool probeOnTimeout = true;
    // Trigger windows to record (needs a trigger); see SegmentLayout. 1 = an ordinary capture.
    uint32_t segments = 1;
};

enum class CaptureStatus { Ok, Timeout, Error, Cancelled };

// Reported through PicoConnection::setProgressCallback (called from the thread running the operation).
enum class CaptureStage { Starting, WaitingForTrigger, Downloading, Done };
[[nodiscard]] std::string_view toString(CaptureStatus status) noexcept;

struct CaptureResult {
    CaptureStatus status = CaptureStatus::Error;
    std::string error;
    bool usageError = false;  // the device rejected the request itself (validate()); nothing was armed
    std::optional<CaptureBuffer> buffer;
    CaptureMode mode = CaptureMode::Channels8;
    std::optional<EdgeTrigger> triggerUsed;
    int attempts = 0;
    std::chrono::milliseconds elapsed{0};
    std::optional<CaptureBuffer> lineDump;  // filled on TIMEOUT when probeOnTimeout is set
    bool portLost = false;                  // true when the serial device disconnected mid-operation
    bool edgeSensitive = false;             // the trigger fired on a real transition (not on a level)
    std::vector<std::string> warnings;      // caveats of the data source (e.g. a replayed VCD with x/z levels)
    std::optional<SegmentLayout> segments;  // set for a segmented capture (CaptureConfig::segments > 1)
};

struct ProbeResult {
    CaptureStatus status = CaptureStatus::Error;
    std::string error;
    std::optional<CaptureBuffer> buffer;  // untriggered observation window
    std::optional<EdgeTrigger> triggerUsed;
    int attempts = 0;
    std::chrono::milliseconds elapsed{0};
    bool portLost = false;
};

}  // namespace lt

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

// Firmware edge trigger. NOTE: the PIO program is level sensitive ("jmp pin"):
// a rising trigger fires as soon as the line is HIGH, falling as soon as it is LOW.
struct EdgeTrigger {
    uint8_t channel = 0;    // 0-based
    bool inverted = false;  // false = rising/high, true = falling/low
};

struct CaptureConfig {
    std::vector<ChannelInfo> channels;
    uint32_t frequency = 1'000'000;
    uint32_t preSamples = 2;
    uint32_t postSamples = 1000;
    // nullopt => immediate capture (auto-selects the trigger level of the first channel).
    std::optional<EdgeTrigger> trigger;
    // Max time to wait for the trigger. The firmware reports completion only on its poll tick
    // (~150 ms for V6_6+, ~2.2 s before), so the device driver adds that latency on top.
    std::chrono::milliseconds timeout = 5000ms;
    // On TIMEOUT, run probe() on the same channels to report the current line state.
    bool probeOnTimeout = true;
};

enum class CaptureStatus { Ok, Timeout, Error, Cancelled };

// Reported through PicoConnection::setProgressCallback (called from the thread running the operation).
enum class CaptureStage { Starting, WaitingForTrigger, Downloading, Done };
[[nodiscard]] std::string_view toString(CaptureStatus status) noexcept;

struct CaptureResult {
    CaptureStatus status = CaptureStatus::Error;
    std::string error;
    std::optional<CaptureBuffer> buffer;
    CaptureMode mode = CaptureMode::Channels8;
    std::optional<EdgeTrigger> triggerUsed;
    int attempts = 0;
    std::chrono::milliseconds elapsed{0};
    std::optional<CaptureBuffer> lineDump;  // filled on TIMEOUT when probeOnTimeout is set
    bool portLost = false;                  // true when the serial device disconnected mid-operation
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

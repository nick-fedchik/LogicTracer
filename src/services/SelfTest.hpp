// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Firmware self-test: the device's own test-signal generator (GP0 = A, GP1 = B, timing exact in system clock
// cycles) is looped back by two jumpers into two capture channels, and every capture feature the host relies
// on is checked against that known stimulus: edge trigger position, pulse widths, cross-channel latency
// bounds, segmented trigger times, trigger timeout and idle levels.

#include "device/IDevice.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace lt {

struct SelfTestOptions {
    uint8_t channelA = 2;    // 0-based capture channel wired to GP0 (CH3 = GP4)
    uint8_t channelB = 3;    // 0-based capture channel wired to GP1 (CH4 = GP5)
    uint32_t frequency = 0;  // sample rate; 0 = the device maximum (at least kMinFrequency)
    std::string keepDir;     // when set, the capture of every failed check is saved there as lossless JSON

    static constexpr uint32_t kMinFrequency = 1'000'000;
};

struct SelfTestOutcome {
    nlohmann::json doc;  // {"command":"selftest","status":"OK"|"FAILED","checks":[...],...}
    bool passed = false;
    std::string error;   // non-empty: the self-test could not run (doc is then empty)
};

// Runs every check on an open device; the generator is always stopped before returning.
[[nodiscard]] SelfTestOutcome runSelfTest(IDevice& device, const SelfTestOptions& options);

}  // namespace lt

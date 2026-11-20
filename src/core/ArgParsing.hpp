// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/CaptureBuffer.hpp"
#include "core/CaptureTypes.hpp"
#include "core/ProtocolDecoder.hpp"
#include "core/SignalAnalyzer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lt::cli {

// "1000", "1k", "2.5M", "1MHz", "500k" -> integer value (SI multipliers k/M/G).
[[nodiscard]] std::optional<uint64_t> parseQuantity(std::string_view text);

// 1-based channel list: "1,2,24", "1-8", "1:STIM,2:REACT,24:JETSON_DO1".
[[nodiscard]] std::optional<std::vector<ChannelInfo>> parseChannelList(std::string_view text, int maxChannels,
                                                                       std::string* error);

// "50.12ms", "2us", "100ns", "1.5s", "-200us" -> seconds. A unit is required (except for "0").
[[nodiscard]] std::optional<double> parseTimeSeconds(std::string_view text);

// Setup metadata entries ("dut.bitstream=burst.bit"): key 1..64 chars of [A-Za-z0-9_.-],
// value up to 512 printable characters without line breaks.
[[nodiscard]] bool isValidSetupEntry(std::string_view key, std::string_view value, std::string* error = nullptr);
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parseSetupArg(std::string_view text,
                                                                               std::string* error = nullptr);
// Adds or replaces `key` keeping the original insertion order.
void setSetupEntry(SetupInfo& setup, std::string key, std::string value);

struct TriggerArg {
    bool none = true;
    EdgeTrigger trigger;
};

// "24:falling", "3:rising", "5:high", "5:low", "none".
[[nodiscard]] std::optional<TriggerArg> parseTrigger(std::string_view text, int maxChannels, std::string* error);

struct LatencyArg {
    uint8_t stimChannel = 0;   // 0-based hardware channel
    bool stimFalling = false;  // false = rising, true = falling
    uint8_t reactChannel = 1;  // 0-based hardware channel
    bool reactFalling = false;
};

// "1..2" (shell-safe) or "1->2", "24:falling..2:rising", "24:falling->2:rising". Channels are 1-based.
[[nodiscard]] std::optional<LatencyArg> parseLatencyArg(std::string_view text, int maxChannels, std::string* error);

enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

struct ExpectationClause {
    std::string raw;
    std::string target;  // empty = latency metric ("p99_ns", "drops"), non-empty = channel ("CH24", "24", "DO1")
    std::string metric;
    CompareOp op = CompareOp::Ge;
    double threshold = 0.0;
};

struct ExpectationCheck {
    std::string expression;
    bool passed = false;
    std::optional<double> actual;
    std::string detail;
};

// Semicolon or comma separated assertions, e.g.:
//   "p99_ns<=60,drops==0,CH24:low_pulses>=1,DO1:edges>=2"
[[nodiscard]] std::optional<std::vector<ExpectationClause>> parseExpectations(std::string_view text,
                                                                              std::string* error);
[[nodiscard]] std::vector<ExpectationCheck> evaluateExpectations(
    const std::vector<ExpectationClause>& clauses,
    const std::vector<ChannelStats>& channelStats,
    const std::optional<LatencyStats>& latencyStats,
    const std::vector<DecodedBus>& decoded = {});

}  // namespace lt::cli

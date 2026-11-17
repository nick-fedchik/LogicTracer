// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Protocol invariants checked over a capture in one pass over its transitions:
//   never(EXPR [, settle=T])        EXPR (channels with ! & | and parentheses) is never true longer than T
//   always(EXPR [, settle=T])       EXPR is never false longer than T
//   onehot(A, B, ... [, settle=T])  exactly one of the lines is HIGH (onehot0: at most one)
//   within(A[:edge], B[:edge], T)   every A edge is followed by a B edge within T
//   stable(D1, D2, ..., CLK[:edge], setup=T1, hold=T2)
//                                   no data line changes from T1 before to T2 after a clock edge
//   bus(D1, D2, ..., settle=T [, gap=G])
//                                   every bus transition completes within T (bit skew); changes closer than
//                                   G (default 4*T) belong to one transition
// Rules are separated by ';' (or ',' outside parentheses). Channels are names, CHn or n (1-based). Times
// take units: 20ns, 1.5us, 2ms. Edges: rising (default), falling or any.

#include "core/CaptureBuffer.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lt {

struct InvariantRule;

// Boolean expression over channel levels.
struct InvariantExpr {
    enum class Op { Channel, Not, And, Or };
    Op op = Op::Channel;
    std::string channel;                // Op::Channel: reference as written
    std::vector<InvariantExpr> args;    // Not: 1, And/Or: 2+
};

struct EdgeRef {
    std::string channel;
    enum class Edge { Rising, Falling, Any } edge = Edge::Rising;
};

struct InvariantRule {
    enum class Kind { Never, Always, OneHot, OneHot0, Within, Stable, Bus };
    Kind kind = Kind::Never;
    std::string text;                    // the rule as written, for reports
    InvariantExpr expr;                  // Never / Always
    std::vector<std::string> channels;   // OneHot / Stable data lines / Bus
    EdgeRef from, to;                    // Within: from -> to; Stable: `from` is the clock
    double seconds = 0.0;                // Within: deadline; Never/Always/OneHot/Bus: settle
    double setupSeconds = 0.0;           // Stable
    double holdSeconds = 0.0;            // Stable
    double gapSeconds = -1.0;            // Bus: transition grouping gap (< 0: 4 * settle)
};

struct InvariantViolation {
    std::size_t sample = 0;
    std::string detail;
    std::optional<uint32_t> segment;     // 1-based window of a segmented capture (`sample` is window-local)
    std::optional<double> timeSeconds;   // relative to the window's trigger; else derived from `sample`
};

struct InvariantResult {
    std::string rule;
    bool passed = false;
    std::string error;                   // e.g. a channel that is not in the capture (the rule then fails)
    uint64_t checked = 0;                // events examined (intervals, edges, clock edges, transactions)
    uint64_t violations = 0;
    uint64_t censored = 0;               // Within: edges too close to the end of the capture to judge
    std::vector<InvariantViolation> examples;  // first few, in time order
    static constexpr std::size_t kMaxExamples = 16;
};

// Parses a rule list; returns nullopt with `error` on a syntax error. Channel names are resolved later,
// against the capture being checked.
[[nodiscard]] std::optional<std::vector<InvariantRule>> parseInvariants(std::string_view text, std::string* error);

[[nodiscard]] std::vector<InvariantResult> evaluateInvariants(const CaptureBuffer& buffer,
                                                              const std::vector<InvariantRule>& rules);

[[nodiscard]] nlohmann::json toJson(const CaptureBuffer& buffer, const std::vector<InvariantResult>& results);
[[nodiscard]] bool allPassed(const std::vector<InvariantResult>& results);

}  // namespace lt

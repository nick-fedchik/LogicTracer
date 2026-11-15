// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// "Copy for AI": machine-readable twins of what the engineer sees on screen. All three views
// (Markdown summary, ASCII timeline, JSON edge list) describe the same sample range, so a human
// and an AI agent reason about identical data. Times are relative to the trigger (t = 0).

#include "core/CaptureBuffer.hpp"
#include "core/SignalAnalyzer.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

// Controls how much detail Copy-for-AI emits for chat vs programmatic consumption.
enum class CopyMode {
    Brief,        // Markdown summary + causal edge log + ASCII timeline (compact, no raw JSON block)
    CursorsOnly,  // Strictly the [cursorA..cursorB] span (Markdown + causal log + ASCII + JSON)
    Full,         // Full visible window: Markdown + causal log + ASCII + JSON
};

[[nodiscard]] std::string_view toString(CopyMode mode) noexcept;
[[nodiscard]] std::optional<CopyMode> parseCopyMode(std::string_view text) noexcept;

// A marker placed on the timeline by the engineer or by an AI agent (via the session socket / MCP).
// The time is relative to the trigger, like every other time in the reports.
struct Annotation {
    uint64_t id = 0;
    double timeSeconds = 0.0;
    double durationSeconds = 0.0;  // 0 = a single point in time
    std::string label;
    std::string color = "red";  // Qt/CSS colour name or #rrggbb
    std::string author = "ai";
};

struct ReportOptions {
    std::size_t first = 0;                                    // first visible sample
    std::size_t last = std::numeric_limits<std::size_t>::max();  // one past the last visible sample
    std::optional<std::size_t> cursorA;                       // absolute sample indices
    std::optional<std::size_t> cursorB;
    std::vector<Annotation> annotations;  // all annotations; the reports flag those inside the range
    std::size_t timelineColumns = 100;
    std::size_t maxEdgesPerChannel = 200;     // JSON edge list is truncated beyond this
    std::size_t maxChronologicalEvents = 32;  // causal cross-channel transition log cap (0 = omit)
    std::size_t maxAnomalies = 5;             // anomaly spotlight entries with +-4 transition windows (0 = omit)
    CopyMode mode = CopyMode::Full;
    std::string title = "LogicTracer capture";
    std::string triggerDescription;           // e.g. "CH24 falling"; empty = unknown/immediate
    std::string deviceVersion;                // e.g. "LOGIC_ANALYZER_PICO_V6_6"
    std::string board;                        // e.g. "PICO" (inferred from deviceVersion when empty)
    std::string userNote;                     // optional engineer context / question to AI
    std::optional<LatencyPairSpec> latency;   // optional paired STIM -> REACT measurement
};

[[nodiscard]] std::string markdownSummary(const CaptureBuffer& buffer, const ReportOptions& options);
[[nodiscard]] std::string asciiTimeline(const CaptureBuffer& buffer, const ReportOptions& options);
[[nodiscard]] nlohmann::json jsonMetadata(const CaptureBuffer& buffer, const ReportOptions& options);
// Formatted report according to options.mode (Brief omits JSON; CursorsOnly zooms to A..B).
[[nodiscard]] std::string copyForAi(const CaptureBuffer& buffer, const ReportOptions& options);
// Compact AI-ready Markdown summary for a triggerless `probe` JSON document.
[[nodiscard]] std::string probeForAi(const nlohmann::json& probeDoc, std::string_view userNote = {});

}  // namespace lt

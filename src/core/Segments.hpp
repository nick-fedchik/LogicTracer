// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Segmented captures (CaptureConfig::segments > 1): the windows are stored back to back without the time
// between them, so analysing the concatenation would invent edges and latency pairs at every seam. Each
// window is analysed on its own and the results are combined.

#include "core/AiReport.hpp"
#include "core/CaptureTypes.hpp"
#include "core/Invariants.hpp"
#include "core/SignalAnalyzer.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lt {

// {"count", "pre_samples", "post_samples", "trigger_channel" (1-based), "trigger_edge"}
[[nodiscard]] nlohmann::json toJson(const SegmentLayout& layout);
[[nodiscard]] std::optional<SegmentLayout> segmentLayoutFromJson(const nlohmann::json& j, std::string* error = nullptr);

// Firmware burst timestamps -> trigger time of every window relative to window 1. The firmware records one
// stamp when armed and one after every window ((loops << 24) | SysTick down-counter at `clockHz`); windows have
// equal length, so the spacing of their end stamps is the spacing of their triggers. Intervals up to 2^32
// cycles (21 s at 200 MHz) are unambiguous. nullopt with `error` when the stamps do not match the windows.
[[nodiscard]] std::optional<std::vector<double>> triggerTimesFromTimestamps(const std::vector<uint32_t>& stamps,
                                                                            uint32_t windows, uint32_t clockHz,
                                                                            std::string* error = nullptr);

// Window k as its own buffer with t = 0 at its trigger. Windows after the first have no pre-trigger samples;
// when the trigger channel was captured, one sample with that channel at its pre-trigger level is prepended
// so the trigger edge stays visible (trigger_sample 1). Empty when the buffer does not match the layout.
[[nodiscard]] std::vector<CaptureBuffer> splitSegments(const CaptureBuffer& buffer, const SegmentLayout& layout);

// Trigger-to-trigger intervals when the layout carries trigger times.
struct TriggerSpacing {
    double min = 0, median = 0, max = 0;  // seconds
};
[[nodiscard]] std::optional<TriggerSpacing> triggerSpacing(const SegmentLayout& layout);

// First stored sample of every window.
[[nodiscard]] std::vector<std::size_t> segmentStarts(const SegmentLayout& layout);
// Stored-buffer index of sample `local` of window `window` as split by splitSegments(buffer, layout).
[[nodiscard]] std::size_t storedSample(const CaptureBuffer& buffer, const SegmentLayout& layout, std::size_t window,
                                       std::size_t local);
// Anomalies found window by window (the seams are not analysed), at stored-buffer samples, most severe first;
// each detail names its window. `options` range fields are ignored (whole windows).
[[nodiscard]] std::vector<AnomalyIncident> findSegmentAnomalies(const CaptureBuffer& buffer, const SegmentLayout& layout,
                                                                const ReportOptions& options);

// Counts add up over the windows; widths keep min/max and a pulse-weighted mean; frequency and duty are the
// edge-weighted mean of the per-window values (each window is continuous); periods and edge lists are dropped.
[[nodiscard]] std::vector<ChannelStats> mergeChannelStats(const std::vector<std::vector<ChannelStats>>& windows);

struct SegmentAnalysis {
    SegmentLayout layout;
    std::vector<CaptureBuffer> windows;
    std::vector<ChannelStats> stats;                        // combined over the windows
    std::optional<LatencyStats> latency;                    // all windows combined
    std::optional<std::vector<InvariantResult>> invariants; // all windows combined
    nlohmann::json perWindow = nlohmann::json::array();     // one compact summary per window
};

[[nodiscard]] SegmentAnalysis analyzeSegments(const CaptureBuffer& buffer, const SegmentLayout& layout,
                                              const std::optional<LatencyPairSpec>& latency, bool keepLatencyEvents,
                                              const std::vector<InvariantRule>* invariants);

// {"layout": ..., "windows": [...]} (latency and invariants are reported by the caller in their usual places).
[[nodiscard]] nlohmann::json toJson(const SegmentAnalysis& analysis);

// AI report: combined activity and latency, the windows that need attention, then window 1 in detail.
[[nodiscard]] std::string segmentsForAi(const SegmentAnalysis& analysis, const ReportOptions& options);

}  // namespace lt

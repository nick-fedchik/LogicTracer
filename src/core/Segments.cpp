// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/Segments.hpp"

#include "core/Format.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace lt {

nlohmann::json toJson(const SegmentLayout& layout)
{
    const bool timed = layout.triggerTimesSeconds.size() == layout.count;
    nlohmann::json j = {{"count", layout.count},
                        {"pre_samples", layout.preSamples},
                        {"post_samples", layout.postSamples},
                        {"note", timed ? "windows are stored back to back; trigger_times_us says when each one started"
                                       : "windows are stored back to back; the time between them is not recorded"}};
    if (timed) {
        auto times = nlohmann::json::array();
        for (const double t : layout.triggerTimesSeconds)
            times.push_back(t * 1e6);
        j["trigger_times_us"] = std::move(times);
        j["trigger_time_resolution_ns"] = layout.triggerTimeResolutionSeconds * 1e9;
    }
    if (layout.triggerChannel) {
        j["trigger_channel"] = *layout.triggerChannel + 1;
        j["trigger_edge"] = layout.triggerFalling ? "falling" : "rising";
    }
    return j;
}

std::optional<SegmentLayout> segmentLayoutFromJson(const nlohmann::json& j, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<SegmentLayout> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    if (!j.is_object())
        return fail("segment layout must be an object");
    auto uint = [&](const char* key, uint64_t lo, uint64_t hi) -> std::optional<uint32_t> {
        if (!j.contains(key) || !j[key].is_number_integer())
            return std::nullopt;
        const auto v = j[key].get<int64_t>();
        return v >= 0 && static_cast<uint64_t>(v) >= lo && static_cast<uint64_t>(v) <= hi
                   ? std::optional<uint32_t>(static_cast<uint32_t>(v))
                   : std::nullopt;
    };
    SegmentLayout l;
    const auto count = uint("count", 1, 65536);
    const auto pre = uint("pre_samples", 0, UINT32_MAX);
    const auto post = uint("post_samples", 1, UINT32_MAX);
    if (!count || !pre || !post)
        return fail("segment layout needs count (1..65536), pre_samples and post_samples (>= 1)");
    l.count = *count;
    l.preSamples = *pre;
    l.postSamples = *post;
    if (j.contains("trigger_channel")) {
        const auto ch = uint("trigger_channel", 1, 32);
        if (!ch)
            return fail("segment layout trigger_channel must be 1..32");
        l.triggerChannel = static_cast<uint8_t>(*ch - 1);
        l.triggerFalling = j.value("trigger_edge", std::string("rising")) == "falling";
    }
    if (j.contains("trigger_times_us")) {
        const auto& t = j["trigger_times_us"];
        if (!t.is_array() || t.size() != l.count)
            return fail("segment layout trigger_times_us must list one time per window");
        for (const auto& v : t) {
            if (!v.is_number() || v.get<double>() < 0 ||
                (!l.triggerTimesSeconds.empty() && v.get<double>() * 1e-6 <= l.triggerTimesSeconds.back()))
                return fail("segment layout trigger_times_us must be increasing non-negative numbers");
            l.triggerTimesSeconds.push_back(v.get<double>() * 1e-6);
        }
        if (j.contains("trigger_time_resolution_ns") && j["trigger_time_resolution_ns"].is_number())
            l.triggerTimeResolutionSeconds = std::max(0.0, j["trigger_time_resolution_ns"].get<double>() * 1e-9);
    }
    return l;
}

std::optional<std::vector<double>> triggerTimesFromTimestamps(const std::vector<uint32_t>& stamps, uint32_t windows,
                                                             uint32_t clockHz, std::string* error)
{
    if (clockHz == 0 || windows < 2 || stamps.size() != static_cast<std::size_t>(windows) + 1) {
        if (error)
            *error = std::to_string(stamps.size()) + " timestamps for " + std::to_string(windows) +
                     " windows (expected one per window plus the arm stamp)";
        return std::nullopt;
    }
    auto ticks = [](uint32_t s) { return (s & 0xFF000000u) + (0x00FFFFFFu - (s & 0x00FFFFFFu)); };
    std::vector<double> out;
    const uint32_t first = ticks(stamps[1]);
    for (std::size_t k = 1; k < stamps.size(); ++k)
        out.push_back(static_cast<double>(static_cast<uint32_t>(ticks(stamps[k]) - first)) / clockHz);
    for (std::size_t k = 1; k < out.size(); ++k)
        if (out[k] <= out[k - 1]) {
            if (error)
                *error = "timestamps are not increasing (window " + std::to_string(k + 1) + ")";
            return std::nullopt;
        }
    return out;
}

std::vector<CaptureBuffer> splitSegments(const CaptureBuffer& buffer, const SegmentLayout& layout)
{
    std::vector<CaptureBuffer> out;
    if (layout.count == 0 || layout.postSamples == 0 || buffer.size() != layout.totalSamples())
        return out;
    std::optional<std::size_t> trigBit;
    if (layout.triggerChannel)
        for (std::size_t i = 0; i < buffer.channels().size(); ++i)
            if (buffer.channels()[i].number == *layout.triggerChannel)
                trigBit = i;
    const auto& s = buffer.samples();
    out.reserve(layout.count);
    out.emplace_back(buffer.channels(), buffer.frequency(), layout.preSamples,
                     std::vector<uint32_t>(s.begin(), s.begin() + layout.preSamples + layout.postSamples));
    // The device may have moved t = 0 of window 1 back onto its trigger edge (one sample); keep that.
    if (buffer.triggerIndex() + 1 == static_cast<int64_t>(layout.preSamples))
        out.back().setTriggerIndex(buffer.triggerIndex());
    out.back().setExactTiming(buffer.exactTiming());
    for (uint32_t k = 1; k < layout.count; ++k) {
        const auto first = s.begin() + layout.preSamples + static_cast<std::ptrdiff_t>(k) * layout.postSamples;
        std::vector<uint32_t> w;
        w.reserve(layout.postSamples + 1);
        if (trigBit) {
            const uint32_t mask = 1u << *trigBit;
            w.push_back(layout.triggerFalling ? (*first | mask) : (*first & ~mask));
        }
        w.insert(w.end(), first, first + layout.postSamples);
        CaptureBuffer win(buffer.channels(), buffer.frequency(), 0, std::move(w));
        win.setTriggerIndex(trigBit ? 1 : 0);
        win.setExactTiming(buffer.exactTiming());
        out.push_back(std::move(win));
    }
    return out;
}

namespace {

bool hasSyntheticSample(const CaptureBuffer& buffer, const SegmentLayout& layout)
{
    if (!layout.triggerChannel)
        return false;
    return std::any_of(buffer.channels().begin(), buffer.channels().end(),
                       [&](const ChannelInfo& c) { return c.number == *layout.triggerChannel; });
}

}  // namespace

std::optional<TriggerSpacing> triggerSpacing(const SegmentLayout& layout)
{
    const auto& t = layout.triggerTimesSeconds;
    if (layout.count < 2 || t.size() != layout.count)
        return std::nullopt;
    std::vector<double> d;
    for (std::size_t k = 1; k < t.size(); ++k)
        d.push_back(t[k] - t[k - 1]);
    std::sort(d.begin(), d.end());
    return TriggerSpacing{d.front(), d[(d.size() - 1) / 2], d.back()};
}

std::vector<std::size_t> segmentStarts(const SegmentLayout& layout)
{
    std::vector<std::size_t> out;
    for (uint32_t k = 0; k < layout.count; ++k)
        out.push_back(k == 0 ? 0 : layout.preSamples + static_cast<std::size_t>(k) * layout.postSamples);
    return out;
}

std::size_t storedSample(const CaptureBuffer& buffer, const SegmentLayout& layout, std::size_t window, std::size_t local)
{
    if (window == 0)
        return local;
    const std::size_t start = layout.preSamples + window * layout.postSamples;
    const std::size_t shift = hasSyntheticSample(buffer, layout) ? 1 : 0;
    return start + (local > shift ? local - shift : 0);
}

std::vector<AnomalyIncident> findSegmentAnomalies(const CaptureBuffer& buffer, const SegmentLayout& layout,
                                                  const ReportOptions& options)
{
    std::vector<AnomalyIncident> out;
    ReportOptions o = options;
    o.first = 0;
    o.last = std::numeric_limits<std::size_t>::max();
    o.cursorA.reset();
    o.cursorB.reset();
    o.annotations.clear();
    const auto windows = splitSegments(buffer, layout);
    for (std::size_t k = 0; k < windows.size(); ++k)
        for (auto a : findAnomalies(windows[k], o)) {
            a.sample = storedSample(buffer, layout, k, a.sample);
            a.detail = "window " + std::to_string(k + 1) + ": " + a.detail;
            out.push_back(std::move(a));
        }
    std::stable_sort(out.begin(), out.end(), [](const AnomalyIncident& a, const AnomalyIncident& b) {
        return a.severity > b.severity;
    });
    return out;
}

std::vector<ChannelStats> mergeChannelStats(const std::vector<std::vector<ChannelStats>>& windows)
{
    if (windows.empty())
        return {};
    std::vector<ChannelStats> out;
    for (std::size_t c = 0; c < windows.front().size(); ++c) {
        ChannelStats m;
        const auto& f = windows.front()[c];
        m.channelIndex = f.channelIndex;
        m.channel = f.channel;
        m.sampleRateHz = f.sampleRateHz;
        m.triggerSample = f.triggerSample;
        m.idleLevel = f.idleLevel;
        m.finalLevel = windows.back()[c].finalLevel;
        m.trailingSamples = windows.back()[c].trailingSamples;
        double highSamples = 0, freqSum = 0, freqWeight = 0, dutySum = 0, dutyWeight = 0;
        auto mergeWidth = [](std::optional<PulseWidth>& into, uint64_t& intoCount, const std::optional<PulseWidth>& w,
                             uint64_t count) {
            if (!w || count == 0)
                return;
            if (!into) {
                into = *w;
            } else {
                into->minSamples = std::min(into->minSamples, w->minSamples);
                into->maxSamples = std::max(into->maxSamples, w->maxSamples);
                into->meanSamples = (into->meanSamples * static_cast<double>(intoCount) +
                                     w->meanSamples * static_cast<double>(count)) /
                                    static_cast<double>(intoCount + count);
            }
            intoCount += count;
        };
        uint64_t highCount = 0, lowCount = 0;
        for (const auto& win : windows) {
            const auto& st = win[c];
            m.sampleCount += st.sampleCount;
            m.transitions += st.transitions;
            m.risingEdges += st.risingEdges;
            m.fallingEdges += st.fallingEdges;
            m.glitchCount += st.glitchCount;
            m.pulseCountHigh += st.pulseCountHigh;
            m.pulseCountLow += st.pulseCountLow;
            mergeWidth(m.highWidth, highCount, st.highWidth, st.pulseCountHigh);
            mergeWidth(m.lowWidth, lowCount, st.lowWidth, st.pulseCountLow);
            highSamples += st.highTimePercent / 100.0 * static_cast<double>(st.sampleCount);
            const auto w = static_cast<double>(st.transitions);
            if (st.meanFrequencyHz && w > 0) {
                freqSum += *st.meanFrequencyHz * w;
                freqWeight += w;
            }
            if (st.dutyCyclePercent && w > 0) {
                dutySum += *st.dutyCyclePercent * w;
                dutyWeight += w;
            }
            if (!m.firstEdgeTimeS && st.firstEdgeTimeS)
                m.firstEdgeTimeS = st.firstEdgeTimeS;
        }
        m.highTimePercent = m.sampleCount ? 100.0 * highSamples / static_cast<double>(m.sampleCount) : 0.0;
        if (freqWeight > 0)
            m.meanFrequencyHz = freqSum / freqWeight;
        if (dutyWeight > 0)
            m.dutyCyclePercent = dutySum / dutyWeight;
        out.push_back(std::move(m));
    }
    return out;
}

namespace {

std::vector<InvariantResult> mergeInvariants(const std::vector<std::vector<InvariantResult>>& perWindow,
                                             const std::vector<CaptureBuffer>& windows)
{
    std::vector<InvariantResult> out;
    if (perWindow.empty())
        return out;
    out.resize(perWindow.front().size());
    for (std::size_t r = 0; r < out.size(); ++r) {
        auto& m = out[r];
        m.rule = perWindow.front()[r].rule;
        for (std::size_t w = 0; w < perWindow.size(); ++w) {
            const auto& res = perWindow[w][r];
            if (m.error.empty())
                m.error = res.error;
            m.checked += res.checked;
            m.violations += res.violations;
            m.censored += res.censored;
            for (auto ex : res.examples) {
                if (m.examples.size() >= InvariantResult::kMaxExamples)
                    break;
                ex.segment = static_cast<uint32_t>(w + 1);
                ex.timeSeconds = windows[w].timeOfSample(ex.sample);
                ex.detail = "window " + std::to_string(w + 1) + ": " + ex.detail;
                m.examples.push_back(std::move(ex));
            }
        }
        m.passed = m.error.empty() && m.violations == 0;
    }
    return out;
}

}  // namespace

SegmentAnalysis analyzeSegments(const CaptureBuffer& buffer, const SegmentLayout& layout,
                                const std::optional<LatencyPairSpec>& latency, bool keepLatencyEvents,
                                const std::vector<InvariantRule>* invariants)
{
    SegmentAnalysis a;
    a.layout = layout;
    a.windows = splitSegments(buffer, layout);
    std::vector<std::vector<ChannelStats>> stats;
    std::vector<LatencyStats> lat;
    std::vector<std::vector<InvariantResult>> inv;
    for (std::size_t k = 0; k < a.windows.size(); ++k) {
        const auto& w = a.windows[k];
        stats.push_back(SignalAnalyzer{}.analyze(w));
        uint64_t edges = 0;
        for (const auto& st : stats.back())
            edges += st.transitions;
        nlohmann::json row = {{"window", k + 1}, {"samples", w.size()}, {"edges", edges}};
        if (layout.triggerTimesSeconds.size() == layout.count)
            row["trigger_time_us"] = layout.triggerTimesSeconds[k] * 1e6;
        if (latency) {
            lat.push_back(SignalAnalyzer::analyzeLatency(w, *latency, true));
            const auto& l = lat.back();
            row["pairs"] = l.count;
            row["drops"] = l.unmatchedStimCount;
            row["spurious"] = l.spuriousReactCount;
            if (l.count)
                row["max_delay_samples"] = l.maxSamples;
        }
        if (invariants) {
            inv.push_back(evaluateInvariants(w, *invariants));
            uint64_t v = 0;
            for (const auto& r : inv.back())
                v += r.violations + (r.error.empty() ? 0 : 1);
            row["invariant_violations"] = v;
        }
        a.perWindow.push_back(std::move(row));
    }
    a.stats = mergeChannelStats(stats);
    if (latency)
        a.latency = SignalAnalyzer::mergeLatency(lat, keepLatencyEvents);
    if (invariants)
        a.invariants = mergeInvariants(inv, a.windows);
    return a;
}

nlohmann::json toJson(const SegmentAnalysis& analysis)
{
    return {{"layout", toJson(analysis.layout)}, {"windows", analysis.perWindow}};
}

std::string segmentsForAi(const SegmentAnalysis& a, const ReportOptions& options)
{
    std::ostringstream ss;
    const auto& L = a.layout;
    const double rate = a.windows.empty() ? 0.0 : static_cast<double>(a.windows.front().frequency());
    ss << "## " << (options.title.empty() ? std::string("LogicTracer segmented capture") : options.title) << "\n\n";
    if (!options.userNote.empty())
        ss << "> " << options.userNote << "\n\n";
    for (const auto& w : options.warnings)
        ss << "> WARNING: " << w << "\n";
    if (!options.warnings.empty())
        ss << "\n";
    ss << "- Segmented capture: **" << L.count << " trigger windows** of " << L.postSamples << " samples";
    if (rate > 0)
        ss << " (" << formatTime(L.postSamples / rate) << ") at " << formatHz(static_cast<uint32_t>(rate));
    ss << "; " << L.preSamples << " pre-trigger samples before window 1 only\n";
    if (!options.triggerDescription.empty())
        ss << "- Every window starts on: " << options.triggerDescription << "\n";
    if (const auto spacing = triggerSpacing(L)) {
        ss << "- Trigger-to-trigger spacing (" << L.count - 1 << " intervals, ±"
           << formatTime(2 * L.triggerTimeResolutionSeconds) << "): min " << formatTime(spacing->min) << ", median "
           << formatTime(spacing->median) << ", max " << formatTime(spacing->max) << "; window 1 .. " << L.count
           << " span " << formatTime(L.triggerTimesSeconds.back()) << "\n";
    } else {
        ss << "- The time between windows is NOT recorded: do not infer periods or gaps across windows\n";
    }
    if (!options.deviceVersion.empty())
        ss << "- Device: " << options.deviceVersion << "\n";
    for (const auto& [k, v] : options.setup)
        ss << "- Setup " << k << ": " << v << "\n";

    ss << "\n### Activity over all windows\n\n| Ch | Name | Edges | Rising | Falling | HIGH pulses | HIGH width "
          "(samples) | Freq in window |\n|---|---|---|---|---|---|---|---|\n";
    for (const auto& st : a.stats) {
        ss << "| " << st.channel.displayNumber() << " | " << st.channel.label() << " | " << st.transitions << " | "
           << st.risingEdges << " | " << st.fallingEdges << " | " << st.pulseCountHigh << " | ";
        if (st.highWidth)
            ss << st.highWidth->minSamples << ".." << st.highWidth->maxSamples;
        else
            ss << "-";
        ss << " | " << (st.meanFrequencyHz ? formatHz(static_cast<uint32_t>(*st.meanFrequencyHz)) : std::string("-"))
           << " |\n";
    }
    if (a.latency)
        ss << latencyMarkdown(*a.latency);

    std::vector<std::string> attention;
    std::size_t flagged = 0;
    for (const auto& w : a.perWindow) {
        std::string why;
        if (w.value("edges", 0) == 0)
            why += "no edges; ";
        if (w.contains("pairs") && w.value("pairs", 0) == 0)
            why += "no latency pair; ";
        if (w.value("drops", 0) > 0)
            why += std::to_string(w.value("drops", 0)) + " drop(s); ";
        if (w.value("spurious", 0) > 0)
            why += std::to_string(w.value("spurious", 0)) + " spurious; ";
        if (w.value("invariant_violations", 0) > 0)
            why += std::to_string(w.value("invariant_violations", 0)) + " invariant violation(s); ";
        if (why.empty())
            continue;
        ++flagged;
        if (attention.size() < 16)
            attention.push_back("- window " + std::to_string(w.value("window", 0)) + ": " + why.substr(0, why.size() - 2));
    }
    ss << "\n### Windows needing attention: " << flagged << " of " << L.count << "\n\n";
    for (const auto& line : attention)
        ss << line << "\n";
    if (flagged > attention.size())
        ss << "- ... " << flagged - attention.size() << " more (see `segments.windows` in the JSON)\n";

    if (!a.windows.empty()) {
        ReportOptions first = options;
        first.title = "Window 1 of " + std::to_string(L.count) + " in detail";
        first.userNote.clear();
        first.warnings.clear();
        ss << "\n" << copyForAi(a.windows.front(), first);
    }
    return ss.str();
}

}  // namespace lt

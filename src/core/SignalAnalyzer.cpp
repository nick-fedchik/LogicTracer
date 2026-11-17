// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/SignalAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lt {

namespace {

struct WidthAccumulator {
    uint64_t count = 0;
    uint64_t min = std::numeric_limits<uint64_t>::max();
    uint64_t max = 0;
    uint64_t sum = 0;

    void add(uint64_t w)
    {
        ++count;
        min = std::min(min, w);
        max = std::max(max, w);
        sum += w;
    }

    [[nodiscard]] std::optional<PulseWidth> result() const
    {
        if (count == 0)
            return std::nullopt;
        return PulseWidth{min, max, static_cast<double>(sum) / static_cast<double>(count)};
    }
};

}  // namespace

ChannelStats SignalAnalyzer::analyzeChannel(const CaptureBuffer& buffer, std::size_t channelIndex) const
{
    ChannelStats st;
    st.channelIndex = channelIndex;
    if (channelIndex < buffer.channels().size())
        st.channel = buffer.channels()[channelIndex];
    st.sampleCount = buffer.size();
    st.sampleRateHz = buffer.frequency();
    st.triggerSample = buffer.triggerIndex();

    const std::size_t n = buffer.size();
    if (n == 0 || channelIndex >= buffer.channels().size() || channelIndex >= 32)
        return st;

    WidthAccumulator high, low;
    uint64_t highSamples = 0;
    std::optional<std::size_t> lastEdge, firstEdge;
    // Same-direction edges and the cumulative high-sample count at each, for period/duty analysis.
    std::vector<uint32_t> riseAt, fallAt;
    std::vector<uint64_t> highAtRise, highAtFall;

    int prev = buffer.bit(0, channelIndex) ? 1 : 0;
    st.idleLevel = prev;
    if (prev)
        ++highSamples;

    for (std::size_t i = 1; i < n; ++i) {
        const int cur = buffer.bit(i, channelIndex) ? 1 : 0;
        if (cur != prev) {
            ++st.transitions;
            if (m_options.collectEdges)
                st.edges.push_back(static_cast<uint32_t>(i));
            if (lastEdge) {
                // The segment [lastEdge, i) is a complete pulse at level `prev`.
                const uint64_t width = i - *lastEdge;
                (prev ? high : low).add(width);
                if (width < m_options.glitchThresholdSamples) {
                    ++st.glitchCount;
                    if (st.glitchSamples.size() < 16)
                        st.glitchSamples.push_back(static_cast<uint32_t>(*lastEdge));
                }
            } else {
                firstEdge = i;
            }
            lastEdge = i;
            if (cur) {
                ++st.risingEdges;
                riseAt.push_back(static_cast<uint32_t>(i));
                highAtRise.push_back(highSamples);
            } else {
                ++st.fallingEdges;
                fallAt.push_back(static_cast<uint32_t>(i));
                highAtFall.push_back(highSamples);
            }
            prev = cur;
        }
        if (cur)
            ++highSamples;
    }

    st.finalLevel = prev;
    st.pulseCountHigh = high.count;
    st.pulseCountLow = low.count;
    st.highWidth = high.result();
    st.lowWidth = low.result();
    st.highTimePercent = 100.0 * static_cast<double>(highSamples) / static_cast<double>(n);

    // Periods between same-direction edges (rising, else falling). Periods longer than
    // kBurstGapFactor x median are idle gaps between bursts and are excluded from frequency/duty.
    const bool useRise = riseAt.size() >= 2;
    const auto& at = useRise ? riseAt : fallAt;
    const auto& hiAt = useRise ? highAtRise : highAtFall;
    if (at.size() >= 2) {
        std::vector<uint32_t> periods(at.size() - 1);
        for (std::size_t k = 0; k + 1 < at.size(); ++k)
            periods[k] = at[k + 1] - at[k];
        std::vector<uint32_t> sorted = periods;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        const uint32_t median = sorted[sorted.size() / 2];

        PeriodStats ps;
        ps.fromRising = useRise;
        ps.medianSamples = median;
        ps.burstGapSamples = median * PeriodStats::kBurstGapFactor;
        ps.minSamples = std::numeric_limits<uint32_t>::max();
        uint64_t intraSum = 0, intraHigh = 0;
        double sumSq = 0.0;
        uint32_t gaps = 0;
        for (std::size_t k = 0; k < periods.size(); ++k) {
            const uint32_t p = periods[k];
            if (p > ps.burstGapSamples) {
                ps.minGapSamples = gaps ? std::min(ps.minGapSamples, p) : p;
                ps.maxGapSamples = std::max(ps.maxGapSamples, p);
                ++gaps;
                continue;
            }
            ++ps.count;
            ps.minSamples = std::min(ps.minSamples, p);
            ps.maxSamples = std::max(ps.maxSamples, p);
            intraSum += p;
            intraHigh += hiAt[k + 1] - hiAt[k];
            sumSq += static_cast<double>(p) * static_cast<double>(p);
            if (2 * p < median || 2 * p > 3 * median) {
                ++ps.irregularCount;
                if (ps.irregularSamples.size() < 16)
                    ps.irregularSamples.push_back(at[k + 1]);
            }
        }
        ps.burstCount = gaps + 1;
        if (ps.count > 0) {
            ps.meanSamples = static_cast<double>(intraSum) / static_cast<double>(ps.count);
            const double var = std::max(0.0, sumSq / static_cast<double>(ps.count) - ps.meanSamples * ps.meanSamples);
            ps.cv = ps.meanSamples > 0 ? std::sqrt(var) / ps.meanSamples : 0.0;
            st.meanFrequencyHz = buffer.frequency() / ps.meanSamples;
            st.dutyCyclePercent = 100.0 * static_cast<double>(intraHigh) / static_cast<double>(intraSum);
        } else {
            ps.minSamples = 0;
        }
        ps.kind = gaps > 0 ? PeriodKind::Bursty : ps.cv <= PeriodStats::kPeriodicCv ? PeriodKind::Periodic
                                                                                     : PeriodKind::Jittery;
        st.period = std::move(ps);
    }

    if (firstEdge) {
        st.firstEdgeTimeS = buffer.timeOfSample(*firstEdge);
        st.lastEdgeTimeS = buffer.timeOfSample(*lastEdge);
        st.trailingSamples = n - *lastEdge;
    } else {
        st.trailingSamples = n;
    }
    return st;
}

std::vector<ChannelStats> SignalAnalyzer::analyze(const CaptureBuffer& buffer) const
{
    std::vector<ChannelStats> out;
    out.reserve(buffer.channels().size());
    for (std::size_t i = 0; i < buffer.channels().size(); ++i)
        out.push_back(analyzeChannel(buffer, i));
    return out;
}

namespace {

// Delay statistics over paired events (out.count and quantizationStepNs are set by the caller).
void computeDelayStats(LatencyStats& out, std::vector<LatencyEvent> events, bool keepEvents)
{
    const double dtNs = out.quantizationStepNs;
    std::vector<uint32_t> sorted(events.size());
    double sum = 0.0;
    out.coincidentCount = 0;
    for (std::size_t k = 0; k < events.size(); ++k) {
        sorted[k] = events[k].deltaSamples;
        sum += static_cast<double>(sorted[k]);
        out.coincidentCount += sorted[k] == 0;
    }
    std::sort(sorted.begin(), sorted.end());

    // Nearest-rank percentile: the smallest measured value with at least q of the events at or below it.
    auto pct = [&](double q) {
        const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(out.count)));
        return sorted[std::clamp<std::size_t>(rank, 1, static_cast<std::size_t>(out.count)) - 1];
    };
    out.minSamples = sorted.front();
    out.maxSamples = sorted.back();
    out.p50Samples = pct(0.50);
    out.p90Samples = pct(0.90);
    out.p95Samples = pct(0.95);
    out.p99Samples = pct(0.99);
    out.minNs = out.minSamples * dtNs;
    out.maxNs = out.maxSamples * dtNs;
    out.p50Ns = out.p50Samples * dtNs;
    out.p90Ns = out.p90Samples * dtNs;
    out.p95Ns = out.p95Samples * dtNs;
    out.p99Ns = out.p99Samples * dtNs;
    out.meanNs = sum / static_cast<double>(out.count) * dtNs;
    out.stdevNs = 0.0;
    if (out.count > 1) {
        double sq = 0.0;
        for (uint32_t v : sorted) {
            const double d = static_cast<double>(v) * dtNs - out.meanNs;
            sq += d * d;
        }
        out.stdevNs = std::sqrt(sq / static_cast<double>(out.count - 1));
    }

    out.histogram.clear();
    for (std::size_t k = 0; k < sorted.size();) {
        std::size_t j = k;
        while (j < sorted.size() && sorted[j] == sorted[k])
            ++j;
        out.histogram.emplace_back(sorted[k], j - k);
        k = j;
    }

    // Robust outliers: |d - median| > max(2, 5 * MAD) samples (MAD is 0 on 1-2 bin histograms).
    const uint32_t median = sorted[(sorted.size() - 1) / 2];
    std::vector<uint32_t> dev(sorted.size());
    for (std::size_t k = 0; k < sorted.size(); ++k)
        dev[k] = sorted[k] > median ? sorted[k] - median : median - sorted[k];
    std::nth_element(dev.begin(), dev.begin() + static_cast<std::ptrdiff_t>((dev.size() - 1) / 2), dev.end());
    const uint32_t mad = dev[(dev.size() - 1) / 2];
    const uint64_t limit = std::max<uint64_t>(2, 5ull * mad);
    std::vector<std::pair<uint32_t, std::size_t>> flagged;  // (deviation, event index)
    for (std::size_t k = 0; k < events.size(); ++k) {
        const uint32_t d = events[k].deltaSamples;
        const uint32_t deviation = d > median ? d - median : median - d;
        if (deviation > limit)
            flagged.emplace_back(deviation, k);
    }
    out.outlierCount = flagged.size();
    out.outliers.clear();
    std::stable_sort(flagged.begin(), flagged.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t k = 0; k < flagged.size() && k < LatencyStats::kMaxListed; ++k)
        out.outliers.push_back(events[flagged[k].second]);
    if (keepEvents)
        out.events = std::move(events);
}

}  // namespace

LatencyStats SignalAnalyzer::mergeLatency(const std::vector<LatencyStats>& windows, bool keepEvents)
{
    LatencyStats out;
    if (windows.empty())
        return out;
    const auto& first = windows.front();
    out.stimChannel = first.stimChannel;
    out.stimFalling = first.stimFalling;
    out.reactChannel = first.reactChannel;
    out.reactFalling = first.reactFalling;
    out.sampleRateHz = first.sampleRateHz;
    out.quantizationStepNs = first.quantizationStepNs;
    std::vector<LatencyEvent> events;
    for (std::size_t w = 0; w < windows.size(); ++w) {
        const auto& l = windows[w];
        out.count += l.count;
        out.unmatchedStimCount += l.unmatchedStimCount;
        out.censoredStimCount += l.censoredStimCount;
        out.censoredReactCount += l.censoredReactCount;
        out.spuriousReactCount += l.spuriousReactCount;
        out.maxReactsPerStim = std::max(out.maxReactsPerStim, l.maxReactsPerStim);
        out.censorWindowSamples = std::max(out.censorWindowSamples, l.censorWindowSamples);
        for (auto ev : l.events) {
            ev.segment = static_cast<uint32_t>(w + 1);
            ev.eventId = static_cast<uint32_t>(events.size() + 1);
            events.push_back(ev);
        }
    }
    if (events.size() != out.count)  // a window was analysed without its events: no delay statistics
        return out;
    if (out.count > 0)
        computeDelayStats(out, std::move(events), keepEvents);
    return out;
}

LatencyStats SignalAnalyzer::analyzeLatency(const CaptureBuffer& buffer, const LatencyPairSpec& spec,
                                            bool collectEvents)
{
    return analyzeLatencyImpl(buffer, spec, collectEvents, true);
}

std::string SignalAnalyzer::reversedHint(const LatencyStats& l)
{
    if (!l.reversed)
        return {};
    return "every " + l.reactChannel.label() + " edge arrives about one stimulus period after its " +
           l.stimChannel.label() + " edge (median " + std::to_string(l.p50Samples) + " of a " +
           std::to_string(l.reversed->stimPeriodSamples) + "-sample period), while " + l.reactChannel.label() +
           " -> " + l.stimChannel.label() + " pairs " + std::to_string(l.reversed->reverseCount) +
           " times with a median of " + std::to_string(l.reversed->reverseP50Samples) +
           " samples: the stimulus and reaction channels are probably swapped (measure CH" +
           std::to_string(l.reactChannel.displayNumber()) + ".." + std::to_string(l.stimChannel.displayNumber()) + ")";
}

LatencyStats SignalAnalyzer::analyzeLatencyImpl(const CaptureBuffer& buffer, const LatencyPairSpec& spec,
                                                bool collectEvents, bool checkDirection)
{
    LatencyStats out;
    if (spec.stimChannelIndex < buffer.channels().size())
        out.stimChannel = buffer.channels()[spec.stimChannelIndex];
    if (spec.reactChannelIndex < buffer.channels().size())
        out.reactChannel = buffer.channels()[spec.reactChannelIndex];
    out.stimFalling = spec.stimFalling;
    out.reactFalling = spec.reactFalling;
    out.sampleRateHz = buffer.frequency();
    out.quantizationStepNs = buffer.frequency() > 0 ? 1e9 / buffer.frequency() : 0.0;

    const std::size_t n = buffer.size();
    if (n < 2 || spec.stimChannelIndex >= buffer.channels().size() ||
        spec.reactChannelIndex >= buffer.channels().size() || spec.stimChannelIndex >= 32 ||
        spec.reactChannelIndex >= 32 || buffer.frequency() == 0) {
        return out;
    }

    const double dtNs = out.quantizationStepNs;
    const std::optional<uint64_t> maxDeltaSamples =
        spec.maxDeltaSeconds ? std::optional<uint64_t>(static_cast<uint64_t>(
                                   std::llround(*spec.maxDeltaSeconds * buffer.frequency())))
                             : std::nullopt;

    struct Pair { uint32_t stim, react; };
    std::vector<Pair> pairs;
    std::vector<uint32_t> drops;          // superseded or too-late stimuli
    std::vector<uint32_t> orphanReacts;   // REACT with no pending STIM
    std::optional<uint32_t> firstStim;
    std::optional<uint32_t> pendingStim;
    uint32_t reactsForLastPair = 0;

    bool prevStim = buffer.bit(0, spec.stimChannelIndex);
    bool prevReact = buffer.bit(0, spec.reactChannelIndex);

    for (std::size_t i = 1; i < n; ++i) {
        const bool curStim = buffer.bit(i, spec.stimChannelIndex);
        const bool curReact = buffer.bit(i, spec.reactChannelIndex);
        const auto idx = static_cast<uint32_t>(i);

        const bool stimEdge = spec.stimFalling ? (prevStim && !curStim) : (!prevStim && curStim);
        const bool reactEdge = spec.reactFalling ? (prevReact && !curReact) : (!prevReact && curReact);

        if (stimEdge) {
            if (pendingStim)
                drops.push_back(*pendingStim);
            pendingStim = idx;
            if (!firstStim)
                firstStim = idx;
            reactsForLastPair = 0;
        }
        if (reactEdge) {
            if (pendingStim) {
                const uint32_t ds = idx - *pendingStim;
                if (!maxDeltaSamples || ds <= *maxDeltaSamples) {
                    pairs.push_back({*pendingStim, idx});
                    reactsForLastPair = 1;
                    out.maxReactsPerStim = std::max<uint32_t>(out.maxReactsPerStim, 1);
                } else {
                    drops.push_back(*pendingStim);  // reaction arrived too late to count
                }
                pendingStim.reset();
            } else {
                orphanReacts.push_back(idx);
                if (reactsForLastPair > 0)
                    out.maxReactsPerStim = std::max(out.maxReactsPerStim, ++reactsForLastPair);
            }
        }

        prevStim = curStim;
        prevReact = curReact;
    }

    // Edges near the capture borders whose partner may lie outside the window are censored, not
    // counted as failures: the reaction window is maxDelta when given, else 4 x the slowest pair + 2.
    uint32_t maxPairDelta = 0;
    for (const auto& p : pairs)
        maxPairDelta = std::max(maxPairDelta, p.react - p.stim);
    const uint64_t window = maxDeltaSamples ? *maxDeltaSamples : (pairs.empty() ? 0 : 4ull * maxPairDelta + 2);
    out.censorWindowSamples = static_cast<uint32_t>(std::min<uint64_t>(window, UINT32_MAX));
    if (pendingStim) {
        if (window > 0 && (n - 1) - *pendingStim < window)
            ++out.censoredStimCount;
        else
            drops.push_back(*pendingStim);
    }
    for (uint32_t r : orphanReacts) {
        if (window > 0 && (!firstStim || r < *firstStim) && r <= window) {
            ++out.censoredReactCount;
            continue;
        }
        ++out.spuriousReactCount;
        if (out.spuriousSamples.size() < LatencyStats::kMaxListed)
            out.spuriousSamples.push_back(r);
    }
    std::sort(drops.begin(), drops.end());
    out.unmatchedStimCount = drops.size();
    for (std::size_t k = 0; k < drops.size() && k < LatencyStats::kMaxListed; ++k)
        out.dropSamples.push_back(drops[k]);

    out.count = pairs.size();
    if (out.count == 0)
        return out;

    std::vector<LatencyEvent> events;
    events.reserve(pairs.size());
    for (std::size_t k = 0; k < pairs.size(); ++k) {
        LatencyEvent ev;
        ev.eventId = static_cast<uint32_t>(k + 1);
        ev.stimSample = pairs[k].stim;
        ev.stimTimeNs = buffer.timeOfSample(pairs[k].stim) * 1e9;
        ev.reactSample = pairs[k].react;
        ev.reactTimeNs = buffer.timeOfSample(pairs[k].react) * 1e9;
        ev.deltaSamples = pairs[k].react - pairs[k].stim;
        ev.deltaNs = static_cast<double>(ev.deltaSamples) * dtNs;
        events.push_back(ev);
    }
    computeDelayStats(out, std::move(events), collectEvents);

    // Direction check (one level deep: the reverse analysis does not check its own direction).
    if (checkDirection && pairs.size() >= 3) {
        std::vector<uint32_t> spacing;
        spacing.reserve(pairs.size() - 1);
        for (std::size_t k = 1; k < pairs.size(); ++k)
            spacing.push_back(pairs[k].stim - pairs[k - 1].stim);
        std::nth_element(spacing.begin(), spacing.begin() + static_cast<std::ptrdiff_t>(spacing.size() / 2), spacing.end());
        const uint64_t period = spacing[spacing.size() / 2];
        if (period > 0 && 4ull * out.minSamples >= 3ull * period) {
            const LatencyPairSpec reverse{spec.reactChannelIndex, spec.reactFalling, spec.stimChannelIndex,
                                          spec.stimFalling, spec.maxDeltaSeconds};
            const auto back = analyzeLatencyImpl(buffer, reverse, false, false);
            if (back.count * 10 >= out.count * 9 && 4ull * back.p50Samples <= period)
                out.reversed = LatencyStats::ReversedHint{static_cast<uint32_t>(period), back.p50Samples, back.count};
        }
    }
    return out;
}

LineDiagnosis SignalAnalyzer::diagnose(const ChannelStats& st) noexcept
{
    if (st.transitions == 0) {
        if (st.idleLevel)
            return {LineState::High, "pulled_up_or_driven_high",
                    "Stable HIGH: line is pulled up or actively driven high (pull-up OK)."};
        return {LineState::Low, "stuck_low_or_floating",
                "Stable LOW: driven low / shorted to GND (stuck-at-0), or floating "
                "(RP2040 inputs default to an internal pull-down)."};
    }
    const uint64_t pulses = st.pulseCountHigh + st.pulseCountLow;
    if (pulses > 0 && st.glitchCount * 5 >= pulses) {
        // Distinguish a clean Nyquist-rate clock (every half-period is exactly 1 sample) from random noise.
        const bool uniformUnitHigh = !st.highWidth || (st.highWidth->minSamples == 1 && st.highWidth->maxSamples == 1);
        const bool uniformUnitLow = !st.lowWidth || (st.lowWidth->minSamples == 1 && st.lowWidth->maxSamples == 1);
        if (pulses >= 4 && uniformUnitHigh && uniformUnitLow) {
            return {LineState::Toggling, "nyquist_limit_toggling",
                    "Signal toggles every sample (at Nyquist limit): increase sample rate for accurate widths."};
        }
        return {LineState::Noisy, "floating_or_noisy",
                "Many sub-sample-period pulses: line is likely floating (High-Z) or very noisy."};
    }
    return {LineState::Toggling, "active_signal", "Line is toggling: an active signal is present."};
}

std::string_view toString(PeriodKind kind) noexcept
{
    switch (kind) {
    case PeriodKind::None: return "none";
    case PeriodKind::Periodic: return "periodic";
    case PeriodKind::Jittery: return "jittery";
    case PeriodKind::Bursty: return "bursty";
    }
    return "none";
}

std::string_view toString(LineState state) noexcept
{
    switch (state) {
    case LineState::Low: return "LOW";
    case LineState::High: return "HIGH";
    case LineState::Toggling: return "TOGGLING";
    case LineState::Noisy: return "NOISY";
    }
    return "UNKNOWN";
}

}  // namespace lt

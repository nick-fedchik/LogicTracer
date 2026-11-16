// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/CaptureBuffer.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace lt {

struct PulseWidth {
    uint64_t minSamples = 0;
    uint64_t maxSamples = 0;
    double meanSamples = 0.0;
};

// Periodicity of one channel, measured between same-direction edges (rising, else falling).
// Bursty signals (gaps > kBurstGapFactor x median period) are segmented so frequency and duty
// describe the signal inside the bursts instead of being averaged across the idle gaps.
enum class PeriodKind { None, Periodic, Jittery, Bursty };

struct PeriodStats {
    static constexpr uint32_t kBurstGapFactor = 4;  // a period longer than 4 x median splits bursts
    static constexpr double kPeriodicCv = 0.05;     // CV <= 5 % counts as periodic

    bool fromRising = true;
    uint64_t count = 0;               // intra-burst periods
    uint32_t minSamples = 0;
    uint32_t medianSamples = 0;
    uint32_t maxSamples = 0;
    double meanSamples = 0.0;
    double cv = 0.0;                  // stdev / mean of intra-burst periods
    PeriodKind kind = PeriodKind::None;
    uint32_t burstGapSamples = 0;     // threshold used to split bursts
    uint32_t burstCount = 0;          // 1 when no gap was found
    uint32_t minGapSamples = 0;       // between bursts (0 if a single burst)
    uint32_t maxGapSamples = 0;
    uint64_t irregularCount = 0;      // intra-burst periods outside [0.5, 1.5] x median (missing/extra edge)
    std::vector<uint32_t> irregularSamples;  // edge ending each irregular period (first 16)
};

struct ChannelStats {
    std::size_t channelIndex = 0;  // bit position inside the samples
    ChannelInfo channel;
    std::size_t sampleCount = 0;
    double sampleRateHz = 0.0;
    int64_t triggerSample = 0;  // sample index of t = 0 (may lie outside the analysed range)

    int idleLevel = 0;   // level of the first sample
    int finalLevel = 0;  // level of the last sample
    uint64_t transitions = 0;
    uint64_t risingEdges = 0;
    uint64_t fallingEdges = 0;

    // Only complete pulses (bounded by an edge on both sides) are counted.
    uint64_t pulseCountHigh = 0;
    uint64_t pulseCountLow = 0;
    std::optional<PulseWidth> highWidth;
    std::optional<PulseWidth> lowWidth;

    double highTimePercent = 0.0;           // share of the whole window spent high
    std::optional<double> meanFrequencyHz;  // over intra-burst periods (never averaged across idle gaps)
    std::optional<double> dutyCyclePercent; // over the same intra-burst periods
    std::optional<PeriodStats> period;      // when >= 2 same-direction periods exist
    uint64_t glitchCount = 0;               // complete pulses shorter than the glitch threshold (unresolved)
    std::vector<uint32_t> glitchSamples;    // start sample of the first 16 such pulses

    std::optional<double> firstEdgeTimeS;   // relative to trigger
    std::optional<double> lastEdgeTimeS;
    uint64_t trailingSamples = 0;           // samples held at finalLevel after the last edge (or full window)
    std::vector<uint32_t> edges;            // sample index of each transition (if requested)

    [[nodiscard]] double samplesToSeconds(double samples) const noexcept
    {
        return sampleRateHz > 0 ? samples / sampleRateHz : 0.0;
    }
    [[nodiscard]] double timeOfSample(uint64_t index) const noexcept
    {
        return samplesToSeconds(static_cast<double>(index) - static_cast<double>(triggerSample));
    }
};

// Specification for cross-channel propagation latency (e.g. STIM -> REACT in HIL bursts).
struct LatencyPairSpec {
    std::size_t stimChannelIndex = 0;
    bool stimFalling = false;  // false = rising, true = falling
    std::size_t reactChannelIndex = 1;
    bool reactFalling = false;
    std::optional<double> maxDeltaSeconds;  // optional max valid window between stimulus and reaction
};

struct LatencyEvent {
    uint32_t eventId = 0;      // 1-based paired event index
    uint32_t stimSample = 0;
    double stimTimeNs = 0.0;   // relative to trigger (t = 0)
    uint32_t reactSample = 0;
    double reactTimeNs = 0.0;
    uint32_t deltaSamples = 0;
    double deltaNs = 0.0;
};

struct LatencyStats {
    static constexpr std::size_t kMaxListed = 32;  // cap for drop/spurious/outlier lists

    ChannelInfo stimChannel;
    bool stimFalling = false;
    ChannelInfo reactChannel;
    bool reactFalling = false;
    double sampleRateHz = 0.0;
    double quantizationStepNs = 0.0;  // 1 LSB = 1e9 / sampleRateHz

    uint64_t count = 0;               // matched STIM -> REACT pairs
    // STIM edges with no matching REACT: superseded by the next STIM, or the REACT came later than
    // maxDelta, or EOF arrived although the reaction window fitted inside the capture.
    uint64_t unmatchedStimCount = 0;
    // STIM edges too close to the end of the capture for their reaction to be observable. Not drops.
    uint64_t censoredStimCount = 0;
    // REACT edges before the first STIM that may answer a stimulus preceding the capture. Not spurious.
    uint64_t censoredReactCount = 0;
    uint64_t spuriousReactCount = 0;  // REACT edges with no pending STIM (bounce, double reaction)
    uint32_t maxReactsPerStim = 0;    // 1 for a clean 1:1 link
    uint64_t coincidentCount = 0;     // pairs with deltaSamples == 0 (true delay < 1 sample period)
    uint32_t censorWindowSamples = 0; // reaction window used for censoring

    // Integer sample counts: the true delay of a pair measured as d samples lies in
    // ((d - 1) * T, (d + 1) * T) because STIM and REACT are both sampled asynchronously.
    uint32_t minSamples = 0;
    uint32_t maxSamples = 0;
    uint32_t p50Samples = 0;
    uint32_t p90Samples = 0;
    uint32_t p95Samples = 0;
    uint32_t p99Samples = 0;
    double minNs = 0.0;
    double maxNs = 0.0;
    double meanNs = 0.0;
    double stdevNs = 0.0;             // sample standard deviation (0 when count <= 1)
    double p50Ns = 0.0;               // nearest-rank percentiles: sorted[ceil(q * n) - 1]
    double p90Ns = 0.0;
    double p95Ns = 0.0;
    double p99Ns = 0.0;
    std::vector<std::pair<uint32_t, uint64_t>> histogram;  // (delta samples, count), ascending

    uint64_t outlierCount = 0;        // |d - median| > max(2, 5 * MAD) samples
    std::vector<LatencyEvent> outliers;      // worst first, at most kMaxListed
    std::vector<uint32_t> dropSamples;       // STIM sample of each drop (first kMaxListed)
    std::vector<uint32_t> spuriousSamples;   // REACT sample of each spurious edge (first kMaxListed)
    std::vector<LatencyEvent> events;

    // Direction check: set when every reaction lags its stimulus by about one stimulus period while the
    // opposite pairing (REACT -> STIM) is short and complete, i.e. the two channels are probably swapped.
    struct ReversedHint {
        uint32_t stimPeriodSamples = 0;   // median spacing of paired stimuli
        uint32_t reverseP50Samples = 0;   // median REACT -> STIM delay
        uint64_t reverseCount = 0;        // pairs found in the opposite direction
    };
    std::optional<ReversedHint> reversed;

    [[nodiscard]] double lowerBoundNs(uint32_t deltaSamples) const noexcept
    {
        return deltaSamples > 0 ? static_cast<double>(deltaSamples - 1) * quantizationStepNs : 0.0;
    }
    [[nodiscard]] double upperBoundNs(uint32_t deltaSamples) const noexcept
    {
        return static_cast<double>(deltaSamples + 1) * quantizationStepNs;
    }
    // If every pair has the same true delay, each measured d confines it to ((d-1)T, (d+1)T); the
    // intersection over all pairs is ((max-1)T, (min+1)T). It is empty (inconsistent) when the spread
    // exceeds one sample, which proves the true delay itself varies (real jitter, not sampling phase).
    [[nodiscard]] bool constantDelayConsistent() const noexcept { return count > 0 && maxSamples - minSamples <= 1; }
    [[nodiscard]] double constantDelayLowerNs() const noexcept { return lowerBoundNs(maxSamples); }
    [[nodiscard]] double constantDelayUpperNs() const noexcept { return upperBoundNs(minSamples); }
};

enum class LineState { Low, High, Toggling, Noisy };

struct LineDiagnosis {
    LineState state = LineState::Low;
    std::string_view code;     // machine-readable diagnosis
    std::string_view message;  // human-readable explanation
};

class SignalAnalyzer {
public:
    struct Options {
        bool collectEdges = false;
        uint32_t glitchThresholdSamples = 2;  // pulses strictly shorter than this are glitches
    };

    SignalAnalyzer() = default;
    explicit SignalAnalyzer(Options options) : m_options(options) {}

    // Single O(N) pass over one channel. An empty buffer or a channelIndex outside the buffer's
    // channel list yields metadata only (no levels, edges or timing).
    [[nodiscard]] ChannelStats analyzeChannel(const CaptureBuffer& buffer, std::size_t channelIndex) const;
    [[nodiscard]] std::vector<ChannelStats> analyze(const CaptureBuffer& buffer) const;

    // Cross-channel stimulus -> reaction latency distribution (min/max/mean/stdev/p50/p90/p95/p99).
    [[nodiscard]] static LatencyStats analyzeLatency(const CaptureBuffer& buffer, const LatencyPairSpec& spec,
                                                     bool collectEvents = false);
    // Human-readable direction warning for LatencyStats::reversed (empty when not set).
    [[nodiscard]] static std::string reversedHint(const LatencyStats& stats);

    // Heuristic classification of a short, untriggered observation window (probe()).
    [[nodiscard]] static LineDiagnosis diagnose(const ChannelStats& stats) noexcept;

private:
    static LatencyStats analyzeLatencyImpl(const CaptureBuffer& buffer, const LatencyPairSpec& spec,
                                           bool collectEvents, bool checkDirection);
    Options m_options;
};

[[nodiscard]] std::string_view toString(LineState state) noexcept;
[[nodiscard]] std::string_view toString(PeriodKind kind) noexcept;

}  // namespace lt

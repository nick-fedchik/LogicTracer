// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/AiReport.hpp"

#include "core/DeviceProtocol.hpp"
#include "core/Exporters.hpp"
#include "core/Format.hpp"
#include "core/SignalAnalyzer.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace lt {

namespace {

// Free text from users/agents is embedded in Markdown: collapse line breaks and control characters so a
// label or note cannot start a forged section ("\n## Verdict"), and cap its length.
std::string inlineText(std::string_view text, std::size_t maxChars = 300)
{
    std::string out;
    out.reserve(std::min(text.size(), maxChars));
    for (const char c : text) {
        if (out.size() >= maxChars)
            break;
        out.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? ' ' : c);
    }
    return out;
}

}  // namespace

std::string_view toString(CopyMode mode) noexcept
{
    switch (mode) {
    case CopyMode::Brief: return "brief";
    case CopyMode::CursorsOnly: return "cursors";
    case CopyMode::Full: return "full";
    }
    return "full";
}

std::optional<CopyMode> parseCopyMode(std::string_view text) noexcept
{
    std::string s(text);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "brief" || s == "compact" || s == "chat")
        return CopyMode::Brief;
    if (s == "cursors" || s == "ab" || s == "a..b" || s == "region")
        return CopyMode::CursorsOnly;
    if (s == "full" || s == "all")
        return CopyMode::Full;
    return std::nullopt;
}

namespace {

constexpr double kUs = 1e6;
constexpr double kNs = 1e9;

struct Range {
    std::size_t first = 0;
    std::size_t last = 0;  // exclusive
    bool clampedToCursors = false;
    [[nodiscard]] std::size_t size() const { return last - first; }
};

Range normalize(const CaptureBuffer& b, const ReportOptions& o)
{
    Range r;
    if (o.mode == CopyMode::CursorsOnly && o.cursorA && o.cursorB && b.size() > 0) {
        const std::size_t lo = std::min(*o.cursorA, *o.cursorB);
        const std::size_t hi = std::min(std::max(*o.cursorA, *o.cursorB) + 1, b.size());
        // If Cursor A sits directly on a transition (e.g. via snap-to-edge), include one
        // preceding baseline sample so the transition at Cursor A itself is captured in
        // edge counts, chronological transitions, and STIM->REACT latency.
        const std::size_t nCh = b.channels().size();
        const uint32_t mask = nCh >= 32 ? 0xFFFFFFFFu : ((1u << nCh) - 1u);
        const bool edgeAtLo = lo > 0 && lo < b.size() && ((b.samples()[lo] ^ b.samples()[lo - 1]) & mask) != 0;
        r.last = hi;
        r.first = std::min(edgeAtLo ? lo - 1 : lo, hi);
        r.clampedToCursors = true;
        return r;
    }
    r.last = std::min(o.last, b.size());
    r.first = std::min(o.first, r.last);
    return r;
}

std::string resolveBoard(const ReportOptions& o)
{
    if (!o.board.empty())
        return o.board;
    if (!o.deviceVersion.empty()) {
        if (const auto ver = parseVersionLine(o.deviceVersion))
            return ver->board;
    }
    return {};
}

std::string channelPinTag(std::string_view board, const ChannelInfo& ch)
{
    if (board.empty())
        return {};
    if (const auto gp = gpioForChannel(board, ch.number))
        return "GP" + std::to_string(*gp);
    return {};
}

// "3-8, 10, 12-14" for a sorted list of 1-based channel numbers.
std::string compactChannelList(std::vector<int> numbers)
{
    std::sort(numbers.begin(), numbers.end());
    std::string out;
    for (std::size_t i = 0; i < numbers.size();) {
        std::size_t j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1)
            ++j;
        out += (out.empty() ? "" : ", ") + std::to_string(numbers[i]);
        if (j > i)
            out += (j == i + 1 ? ", " : "-") + std::to_string(numbers[j]);
        i = j + 1;
    }
    return out;
}

// Constant (edge-free) channels with the same level, merged so idle lines do not flood a report.
struct ConstantGroup {
    int level = 0;
    std::vector<std::size_t> indexes;  // into the channel list
    std::vector<int> numbers;          // 1-based
};

constexpr std::size_t kMinMergedConstantChannels = 3;

std::vector<ConstantGroup> constantGroups(const std::vector<ChannelStats>& stats)
{
    ConstantGroup low, high;
    high.level = 1;
    for (std::size_t i = 0; i < stats.size(); ++i) {
        if (stats[i].transitions != 0 || stats[i].sampleCount == 0)
            continue;
        auto& g = stats[i].idleLevel ? high : low;
        g.indexes.push_back(i);
        g.numbers.push_back(stats[i].channel.displayNumber());
    }
    std::vector<ConstantGroup> out;
    for (auto* g : {&low, &high})
        if (g->indexes.size() >= kMinMergedConstantChannels)
            out.push_back(std::move(*g));
    return out;
}

std::vector<ChannelStats> analyzeRange(const CaptureBuffer& b, const Range& r)
{
    SignalAnalyzer::Options opts;
    opts.collectEdges = true;
    return SignalAnalyzer(opts).analyze(b.slice(r.first, r.size()));
}

struct ChronologicalEdge {
    std::size_t sample = 0;
    std::size_t channelIndex = 0;
    bool rising = false;
};

struct ChronologicalLog {
    std::size_t total = 0;
    std::vector<ChronologicalEdge> events;
};

ChronologicalLog collectChronologicalEdges(const CaptureBuffer& b, const Range& r, std::size_t maxEvents)
{
    ChronologicalLog log;
    if (r.size() < 2 || b.channels().empty())
        return log;
    const std::size_t nCh = b.channels().size();
    const uint32_t mask = nCh >= 32 ? 0xFFFFFFFFu : ((1u << nCh) - 1u);
    log.events.reserve(std::min<std::size_t>(maxEvents, 64));
    for (std::size_t i = r.first + 1; i < r.last; ++i) {
        uint32_t diff = (b.samples()[i] ^ b.samples()[i - 1]) & mask;
        while (diff != 0) {
            const auto c = static_cast<std::size_t>(std::countr_zero(diff));
            diff &= (diff - 1u);
            ++log.total;
            if (log.events.size() < maxEvents)
                log.events.push_back({i, c, b.bit(i, c)});
        }
    }
    return log;
}

std::optional<LatencyStats> computeLatency(const CaptureBuffer& b, const Range& r, const ReportOptions& o);

// ---------------------------------------------------------------------------------------------
// Motif compression and anomaly spotlight: instead of the first N transitions, describe the
// repeating cross-channel pattern once and list only the places where the capture deviates.

constexpr std::size_t kMaxInsightTransitions = 1'000'000;
constexpr std::size_t kMaxMotifCycle = 16;
constexpr std::size_t kWindowTransitions = 4;  // micro-window: +-4 transitions around an anomaly

struct MotifPosition {
    std::size_t channelIndex = 0;
    bool rising = false;
    uint32_t medianDelta = 0;  // samples since the previous transition (position 0: since the last of the cycle)
    uint32_t minDelta = 0;     // over occurrences that are not timing outliers
    uint32_t maxDelta = 0;
};

struct Motif {
    std::vector<MotifPosition> positions;
    std::size_t repetitions = 0;
    std::size_t matched = 0;
    std::size_t structuralDeviations = 0;
    std::size_t timingDeviations = 0;
    std::size_t burstBoundaries = 0;
};

struct Anomaly {
    std::size_t sample = 0;  // absolute sample index
    int severity = 1;        // 3 = functional (drop, spurious, unexpected edge), 2 = timing, 1 = signal quality
    std::string kind;
    std::string detail;
};

struct Insights {
    ChronologicalLog transitions;  // complete up to kMaxInsightTransitions
    std::optional<Motif> motif;
    std::vector<Anomaly> anomalies;  // sorted by severity, then time
};

uint32_t medianOf(std::vector<uint32_t> v)
{
    if (v.empty())
        return 0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>((v.size() - 1) / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

std::string tokenName(const CaptureBuffer& b, std::size_t ch, bool rising)
{
    return b.channels()[ch].label() + (rising ? "↑" : "↓");
}

std::optional<Motif> detectMotif(const CaptureBuffer& b, const std::vector<ChronologicalEdge>& trs,
                                 std::vector<Anomaly>& anomalies)
{
    const std::size_t e = trs.size();
    if (e < 6)
        return std::nullopt;
    auto token = [&](std::size_t i) { return trs[i].channelIndex * 2 + (trs[i].rising ? 1 : 0); };

    std::size_t cycle = 0;
    for (std::size_t p = 1; p <= kMaxMotifCycle && 3 * p <= e; ++p) {
        std::size_t same = 0;
        for (std::size_t i = 0; i + p < e; ++i)
            same += token(i) == token(i + p);
        if (static_cast<double>(same) >= 0.9 * static_cast<double>(e - p)) {
            cycle = p;
            break;
        }
    }
    if (cycle == 0)
        return std::nullopt;

    // The reference cycle comes from the longest uninterrupted periodic stretch, so any number of startup
    // transients (even ones that happen to repeat) are reported as deviations instead of defining the motif.
    std::size_t refStart = 0, bestRun = 0, run = 0;
    for (std::size_t i = 0; i + cycle < e; ++i) {
        run = token(i) == token(i + cycle) ? run + 1 : 0;
        if (run > bestRun) {
            bestRun = run;
            refStart = i + 1 - run;
        }
    }

    Motif m;
    m.positions.resize(cycle);
    for (std::size_t k = 0; k < cycle; ++k) {
        m.positions[k].channelIndex = trs[refStart + k].channelIndex;
        m.positions[k].rising = trs[refStart + k].rising;
    }
    auto expectedToken = [&](std::size_t pos) { return m.positions[pos].channelIndex * 2 + (m.positions[pos].rising ? 1 : 0); };

    // Pass 1: follow the motif, resynchronising after an unexpected transition.
    std::vector<std::vector<uint32_t>> deltas(cycle);
    struct Matched {
        std::size_t index;
        std::size_t position;
        uint32_t delta;  // samples since the previous matched transition
    };
    std::vector<Matched> matchedAt;
    std::optional<std::size_t> lastMatchedSample;
    // Start at the motif position matching the first transition: extra or missing edges before the reference
    // stretch shift the phase, so positions cannot be derived from indexes alone.
    std::size_t phase = 0;
    for (std::size_t k = 0; k < cycle; ++k) {
        if (expectedToken(k) == token(0)) {
            phase = k;
            break;
        }
    }
    for (std::size_t i = 0; i < e; ++i) {
        if (token(i) == expectedToken(phase)) {
            if (lastMatchedSample) {
                const auto d = static_cast<uint32_t>(trs[i].sample - *lastMatchedSample);
                deltas[phase].push_back(d);
                matchedAt.push_back({i, phase, d});
            }
            lastMatchedSample = trs[i].sample;
            ++m.matched;
            phase = (phase + 1) % cycle;
            continue;
        }
        ++m.structuralDeviations;
        anomalies.push_back({trs[i].sample, 3, "UnexpectedTransition",
                             tokenName(b, trs[i].channelIndex, trs[i].rising) + " where the motif expects " +
                                 tokenName(b, m.positions[phase].channelIndex, m.positions[phase].rising)});
        for (std::size_t k = 1; k <= cycle; ++k) {
            const std::size_t cand = (phase + k) % cycle;
            if (expectedToken(cand) == token(i)) {
                phase = (cand + 1) % cycle;
                break;
            }
        }
    }

    // Pass 2: timing outliers per motif position, robust to 1-2 bin quantization (limit >= 2 samples).
    std::vector<uint32_t> limit(cycle, 2);
    for (std::size_t k = 0; k < cycle; ++k) {
        auto& pos = m.positions[k];
        pos.medianDelta = medianOf(deltas[k]);
        std::vector<uint32_t> dev;
        dev.reserve(deltas[k].size());
        for (uint32_t d : deltas[k])
            dev.push_back(d > pos.medianDelta ? d - pos.medianDelta : pos.medianDelta - d);
        limit[k] = std::max<uint32_t>(2, 5 * medianOf(dev));
        pos.minDelta = pos.maxDelta = pos.medianDelta;
    }
    for (const auto& [i, k, d] : matchedAt) {
        auto& pos = m.positions[k];
        const uint32_t dev = d > pos.medianDelta ? d - pos.medianDelta : pos.medianDelta - d;
        if (dev > limit[k]) {
            // A long pause before the first transition of a cycle separates bursts; it is not a fault.
            if (k == 0 && d > pos.medianDelta * PeriodStats::kBurstGapFactor) {
                ++m.burstBoundaries;
                continue;
            }
            ++m.timingDeviations;
            anomalies.push_back({trs[i].sample, 2, "TimingDeviation",
                                 tokenName(b, pos.channelIndex, pos.rising) + " came " + std::to_string(d) +
                                     " samples after the previous transition (motif median " +
                                     std::to_string(pos.medianDelta) + ")"});
            continue;
        }
        pos.minDelta = std::min(pos.minDelta, d);
        pos.maxDelta = std::max(pos.maxDelta, d);
    }
    m.repetitions = m.matched / cycle;
    return m;
}

Insights computeInsights(const CaptureBuffer& b, const Range& r, const std::vector<ChannelStats>& stats,
                         const std::optional<LatencyStats>& lat)
{
    Insights in;
    in.transitions = collectChronologicalEdges(b, r, kMaxInsightTransitions);
    in.motif = detectMotif(b, in.transitions.events, in.anomalies);

    // Analyzer results refer to the visible slice; shift them back to absolute samples.
    const std::size_t off = r.first;
    if (lat) {
        for (uint32_t s : lat->dropSamples)
            in.anomalies.push_back({off + s, 3, "DroppedStimulus",
                                    lat->stimChannel.label() + " edge without a " + lat->reactChannel.label() +
                                        " reaction"});
        for (uint32_t s : lat->spuriousSamples)
            in.anomalies.push_back({off + s, 3, "SpuriousReaction",
                                    lat->reactChannel.label() + " edge without a pending " + lat->stimChannel.label()});
        for (const auto& ev : lat->outliers)
            in.anomalies.push_back({off + ev.stimSample, 2, "LatencyOutlier",
                                    "pair #" + std::to_string(ev.eventId) + ": delay " + std::to_string(ev.deltaSamples) +
                                        " samples (" + formatTime(ev.deltaNs * 1e-9) + "), median " +
                                        std::to_string(lat->p50Samples)});
    }
    for (const auto& st : stats) {
        if (st.period) {
            for (uint32_t s : st.period->irregularSamples)
                in.anomalies.push_back({off + s, 2, "IrregularPeriod",
                                        st.channel.label() + " period outside 0.5..1.5 x median (" +
                                            std::to_string(st.period->medianSamples) + " samples): missing or extra edge"});
        }
        for (uint32_t s : st.glitchSamples)
            in.anomalies.push_back({off + s, 1, "UnresolvedPulse",
                                    st.channel.label() + " pulse shorter than 2 samples (glitch or under-sampled strobe)"});
    }

    // One physical incident usually trips several detectors (a drop is also an unexpected edge and an
    // irregular period). Findings within one motif cycle of the incident's first finding form a single incident.
    std::size_t radius = 2;
    if (in.motif) {
        radius = 0;
        for (const auto& p : in.motif->positions)
            radius += p.medianDelta;
        // Strictly less than one cycle: failures in adjacent cycles are separate incidents.
        radius = std::max<std::size_t>(radius > 0 ? radius - 1 : 0, 2);
    }
    std::sort(in.anomalies.begin(), in.anomalies.end(), [](const Anomaly& a, const Anomaly& c) { return a.sample < c.sample; });
    std::vector<Anomaly> merged;
    std::size_t groupFirst = 0, related = 0;
    auto flush = [&] {
        if (!merged.empty() && related > 0)
            merged.back().detail += " (+" + std::to_string(related) + " related finding" + (related > 1 ? "s" : "") + ")";
        related = 0;
    };
    for (auto& a : in.anomalies) {
        if (merged.empty() || a.sample - groupFirst > radius) {
            flush();
            groupFirst = a.sample;
            merged.push_back(std::move(a));
            continue;
        }
        auto& g = merged.back();
        if (g.kind.find(a.kind) == std::string::npos)
            g.kind += "+" + a.kind;
        ++related;
        if (a.severity > g.severity) {  // the most severe finding anchors the incident
            g.severity = a.severity;
            g.sample = a.sample;
            g.detail = std::move(a.detail);
        }
    }
    flush();
    std::stable_sort(merged.begin(), merged.end(),
                     [](const Anomaly& a, const Anomaly& c) { return a.severity > c.severity; });
    in.anomalies = std::move(merged);
    return in;
}

std::string deltaCell(const CaptureBuffer& b, std::size_t samples)
{
    return "+" + std::to_string(samples) + " (" + formatTime(static_cast<double>(samples) * b.samplePeriodSeconds()) + ")";
}

void writeMicroWindow(std::ostringstream& ss, const CaptureBuffer& b, const std::vector<ChronologicalEdge>& trs,
                      std::size_t anchor, std::string_view board)
{
    const auto it = std::lower_bound(trs.begin(), trs.end(), anchor,
                                     [](const ChronologicalEdge& e, std::size_t s) { return e.sample < s; });
    const auto center = static_cast<std::size_t>(it - trs.begin());
    const std::size_t lo = center > kWindowTransitions ? center - kWindowTransitions : 0;
    const std::size_t hi = std::min(trs.size(), center + kWindowTransitions + 1);
    if (lo >= hi)
        return;
    ss << "\n   | Δs vs anomaly | Time (rel T) | Event |\n   |---|---|---|\n";
    for (std::size_t i = lo; i < hi; ++i) {
        const auto rel = static_cast<long long>(trs[i].sample) - static_cast<long long>(anchor);
        const auto& ch = b.channels()[trs[i].channelIndex];
        std::string pin;
        if (!board.empty())
            if (const auto gp = gpioForChannel(board, ch.number))
                pin = " (GP" + std::to_string(*gp) + ")";
        ss << "   | " << (rel > 0 ? "+" : "") << rel << " | " << formatTime(b.timeOfSample(trs[i].sample)) << " | CH"
           << ch.displayNumber() << pin << " " << tokenName(b, trs[i].channelIndex, trs[i].rising) << " |\n";
    }
}

std::optional<LatencyStats> computeLatency(const CaptureBuffer& b, const Range& r, const ReportOptions& o)
{
    if (!o.latency || r.size() < 2)
        return std::nullopt;
    if (o.latency->stimChannelIndex >= b.channels().size() || o.latency->reactChannelIndex >= b.channels().size())
        return std::nullopt;
    return SignalAnalyzer::analyzeLatency(b.slice(r.first, r.size()), *o.latency, false);
}

// Sample position (fractional) of an annotation on this capture's time base.
double annotationSample(const CaptureBuffer& b, const Annotation& a)
{
    return static_cast<double>(b.triggerIndex()) + a.timeSeconds * b.frequency();
}

bool annotationVisible(const CaptureBuffer& b, const Range& r, const Annotation& a)
{
    const double s = annotationSample(b, a);
    return r.size() > 0 && s >= static_cast<double>(r.first) && s < static_cast<double>(r.last);
}

std::string cursorLine(const CaptureBuffer& b, const ReportOptions& o)
{
    std::ostringstream ss;
    auto name = [&](const char* label, std::optional<std::size_t> c) {
        if (c)
            ss << label << " = sample " << *c << " (t = " << formatTime(b.timeOfSample(*c)) << ")  ";
    };
    name("A", o.cursorA);
    name("B", o.cursorB);
    if (o.cursorA && o.cursorB) {
        const double dt = b.timeOfSample(*o.cursorB) - b.timeOfSample(*o.cursorA);
        ss << "Δt = " << formatTime(std::abs(dt)) << " (" << (*o.cursorB >= *o.cursorA ? "" : "B before A, ")
           << std::abs(static_cast<long long>(*o.cursorB) - static_cast<long long>(*o.cursorA)) << " samples)";
        if (dt != 0.0)
            ss << ", 1/Δt = " << formatHz(1.0 / std::abs(dt));
    }
    return ss.str();
}

}  // namespace

std::string latencyMarkdown(const LatencyStats& l)
{
    std::ostringstream ss;
    ss << "\n### Propagation latency: CH" << l.stimChannel.displayNumber() << " (" << l.stimChannel.label() << " "
       << (l.stimFalling ? "falling" : "rising") << ") -> CH" << l.reactChannel.displayNumber() << " ("
       << l.reactChannel.label() << " " << (l.reactFalling ? "falling" : "rising") << ")\n\n";
    if (l.reversed)
        ss << "- **Direction check:** " << SignalAnalyzer::reversedHint(l) << "\n";
    ss << "- Paired events: **" << l.count << "**, drops: **" << l.unmatchedStimCount << "**, spurious reactions: **"
       << l.spuriousReactCount << "**, censored at window edges: " << (l.censoredStimCount + l.censoredReactCount)
       << ", coincident (Δs = 0): " << l.coincidentCount << ", max reactions per stimulus: " << l.maxReactsPerStim
       << "\n";
    if (l.count > 0) {
        const double t = l.quantizationStepNs;
        auto ns = [&](double v) {
            std::ostringstream x;
            x << std::fixed << std::setprecision(2) << v;
            return x.str();
        };
        ss << "- Step T = " << ns(t) << " ns; a delay measured as d samples is truly in ((d-1)·T, (d+1)·T)\n";
        std::string hist;
        for (const auto& [d, c] : l.histogram)
            hist += (hist.empty() ? "" : ", ") + std::to_string(d) + "×" + std::to_string(c);
        ss << "- Δs histogram (samples×count): **" << hist << "**\n";
        ss << "- Delay: min " << l.minSamples << " (" << ns(l.minNs) << " ns), p50 " << l.p50Samples << " ("
           << ns(l.p50Ns) << " ns), p99 " << l.p99Samples << " (" << ns(l.p99Ns) << " ns, true ≤ "
           << ns(l.upperBoundNs(l.p99Samples)) << " ns), **max " << l.maxSamples << " (" << ns(l.maxNs)
           << " ns, true ≤ " << ns(l.upperBoundNs(l.maxSamples)) << " ns)**; mean " << ns(l.meanNs)
           << " ns, stdev " << ns(l.stdevNs) << " ns (nearest-rank percentiles)\n";
        if (l.constantDelayConsistent())
            ss << "- **Verdict: constant-delay consistent.** All pairs fit ONE fixed delay in (" << ns(l.constantDelayLowerNs()) << ", "
               << ns(l.constantDelayUpperNs())
               << ") ns; the histogram spread is sampling phase, not proven jitter\n";
        else
            ss << "- Constant-delay check: spread " << (l.maxSamples - l.minSamples)
               << " samples > 1, so the true delay varies (real jitter or outliers)\n";
        if (l.outlierCount > 0)
            ss << "- Outliers: " << l.outlierCount << " pair(s) beyond median ± max(2, 5·MAD) samples\n";
    }
    return ss.str();
}

std::string markdownSummary(const CaptureBuffer& b, const ReportOptions& o)
{
    const Range r = normalize(b, o);
    const auto stats = analyzeRange(b, r);
    const std::string board = resolveBoard(o);
    std::ostringstream ss;

    ss << "## " << o.title << "\n\n";
    if (!o.banner.empty())
        ss << "> **" << o.banner << "**\n\n";
    if (!o.userNote.empty())
        ss << "> **Context / Question:** " << inlineText(o.userNote, 2000) << "\n\n";

    if (o.captureId != 0)
        ss << "- Capture ID: " << o.captureId << "\n";
    for (const auto& w : o.warnings)
        ss << "- Capture warning: " << w << "\n";
    if (!o.deviceVersion.empty() || !board.empty()) {
        ss << "- Device: " << (!o.deviceVersion.empty() ? o.deviceVersion : board);
        if (!board.empty() && !b.channels().empty()) {
            std::string pins;
            for (const auto& ch : b.channels()) {
                const auto gp = channelPinTag(board, ch);
                if (!gp.empty())
                    pins += (pins.empty() ? "" : ", ") + ("CH" + std::to_string(ch.displayNumber()) + "=" + gp);
            }
            if (!pins.empty())
                ss << " | GPIO map: " << pins;
        }
        ss << "\n";
    }
    if (!o.setup.empty()) {
        ss << "- Setup:";
        for (const auto& [k, v] : o.setup)
            ss << " " << k << "=" << v << ";";
        ss << "\n";
    }

    ss << "- Sample rate: " << formatHz(b.frequency()) << " (period " << formatTime(b.samplePeriodSeconds())
       << "), " << b.size() << " samples, duration "
       << formatTime(static_cast<double>(b.size()) * b.samplePeriodSeconds())
       << " | quantization ±1 sample (±" << formatTime(b.samplePeriodSeconds()) << ")\n";
    ss << "- Trigger: " << (o.triggerDescription.empty() ? "immediate / unknown" : o.triggerDescription)
       << ", t = 0 at sample " << b.triggerIndex() << "\n";
    ss << "- " << (r.clampedToCursors ? "Cursor range (A..B)" : "Visible range") << ": samples [" << r.first << ", "
       << r.last << ") = " << formatTime(r.size() ? b.timeOfSample(r.first) : 0.0) << " .. "
       << formatTime(r.size() ? b.timeOfSample(r.last - 1) : 0.0) << " (" << r.size() << " samples, "
       << formatTime(static_cast<double>(r.size()) * b.samplePeriodSeconds()) << ")\n";
    if (o.cursorA || o.cursorB)
        ss << "- Cursors: " << cursorLine(b, o) << "\n";

    const bool showPinCol = !board.empty();
    ss << "\n| Ch | Name | Idle | Final | Edges (rise/fall) | Pulses H/L | Frequency | Duty % | Min pulse | Max pulse "
          "| Glitches |"
       << (showPinCol ? " Pin |\n" : "\n")
       << "|---|---|---|---|---|---|---|---|---|---|---|" << (showPinCol ? "---|\n" : "\n");
    std::vector<std::string> notes;
    if (o.mode == CopyMode::CursorsOnly && (!o.cursorA || !o.cursorB))
        notes.push_back("Cursors A..B mode requested, but both cursors are not set; showing the visible range");

    const auto groups = r.size() ? constantGroups(stats) : std::vector<ConstantGroup>{};
    std::vector<bool> merged(stats.size(), false);
    for (const auto& g : groups)
        for (const auto i : g.indexes)
            merged[i] = true;

    for (std::size_t si = 0; si < stats.size(); ++si) {
        const auto& st = stats[si];
        if (merged[si])
            continue;
        std::optional<double> minW, maxW;
        for (const auto& w : {st.highWidth, st.lowWidth}) {
            if (!w)
                continue;
            const double mn = st.samplesToSeconds(static_cast<double>(w->minSamples));
            const double mx = st.samplesToSeconds(static_cast<double>(w->maxSamples));
            minW = minW ? std::min(*minW, mn) : mn;
            maxW = maxW ? std::max(*maxW, mx) : mx;
        }
        std::ostringstream duty;
        if (st.dutyCyclePercent)
            duty << std::fixed << std::setprecision(1) << *st.dutyCyclePercent;
        ss << "| " << st.channel.displayNumber() << " | " << st.channel.label() << " | " << st.idleLevel << " | "
           << st.finalLevel << " | " << st.transitions << " (" << st.risingEdges << "/" << st.fallingEdges << ") | "
           << st.pulseCountHigh << "/" << st.pulseCountLow << " | "
           << (st.meanFrequencyHz ? formatHz(*st.meanFrequencyHz) : "-") << " | "
           << (st.dutyCyclePercent ? duty.str() : "-") << " | " << (minW ? formatTime(*minW) : "-") << " | "
           << (maxW ? formatTime(*maxW) : "-") << " | " << st.glitchCount << " |";
        if (showPinCol) {
            const auto gp = channelPinTag(board, st.channel);
            ss << " " << (gp.empty() ? "-" : gp) << " |";
        }
        ss << "\n";

        if (r.size() && st.transitions == 0)
            notes.push_back(st.channel.label() + ": no activity, constant " + (st.idleLevel ? "HIGH" : "LOW") +
                            " in the visible range");
        if (st.glitchCount > 0)
            notes.push_back(st.channel.label() + ": " + std::to_string(st.glitchCount) +
                            " unresolved pulse(s) shorter than 2 samples (" + formatTime(2 * b.samplePeriodSeconds()) +
                            "): a glitch or a strobe too short for this sample rate");
        if (st.period && st.period->kind == PeriodKind::Bursty)
            notes.push_back(st.channel.label() + ": bursty, " + std::to_string(st.period->burstCount) +
                            " bursts; frequency/duty are intra-burst (period median " +
                            std::to_string(st.period->medianSamples) + " samples = " +
                            formatTime(st.period->medianSamples * b.samplePeriodSeconds()) + "), gaps " +
                            formatTime(st.period->minGapSamples * b.samplePeriodSeconds()) + " .. " +
                            formatTime(st.period->maxGapSamples * b.samplePeriodSeconds()));
        else if (st.period && st.period->kind == PeriodKind::Jittery) {
            std::ostringstream cv;
            cv << std::fixed << std::setprecision(1) << st.period->cv * 100.0;
            notes.push_back(st.channel.label() + ": aperiodic/jittery, period " + std::to_string(st.period->minSamples) +
                            ".." + std::to_string(st.period->maxSamples) + " samples (CV " + cv.str() +
                            " %); frequency is a mean, not a clock rate");
        }
        if (st.period && st.period->irregularCount > 0)
            notes.push_back(st.channel.label() + ": " + std::to_string(st.period->irregularCount) +
                            " irregular period(s) inside bursts (missing or extra edge)");
        if (o.mode != CopyMode::Brief && st.edges.size() > o.maxEdgesPerChannel)
            notes.push_back(st.channel.label() + ": JSON edge_times_us truncated to first " +
                            std::to_string(o.maxEdgesPerChannel) + " of " + std::to_string(st.edges.size()) + " edges");
    }

    for (const auto& g : groups) {
        ss << "| " << compactChannelList(g.numbers) << " | " << g.indexes.size() << " constant channels | " << g.level
           << " | " << g.level << " | 0 (0/0) | 0/0 | - | - | - | - | 0 |" << (showPinCol ? " see GPIO map |" : "") << "\n";
        notes.push_back("CH " + compactChannelList(g.numbers) + " (" + std::to_string(g.indexes.size()) +
                        " channels): no activity, constant " + (g.level ? "HIGH" : "LOW") + " in the visible range");
    }

    const auto lat = computeLatency(b, r, o);
    if (lat)
        ss << latencyMarkdown(*lat);

    if (o.cursorA && o.cursorB && r.size() && !r.clampedToCursors) {
        const auto lo = std::min(*o.cursorA, *o.cursorB), hi = std::max(*o.cursorA, *o.cursorB);
        ss << "\nBetween the cursors (samples " << lo << ".." << hi << "):\n\n| Ch | Name | Level at A | Level at B "
              "| Edges between |\n|---|---|---|---|---|\n";
        for (std::size_t c = 0; c < b.channels().size(); ++c) {
            std::size_t edges = 0;
            for (std::size_t i = lo + 1; i <= hi && i < b.size(); ++i)
                edges += ((b.samples()[i] ^ b.samples()[i - 1]) >> c) & 1u;
            const auto level = [&](std::size_t i) { return i < b.size() ? int(b.bit(i, c)) : -1; };
            ss << "| " << b.channels()[c].displayNumber() << " | " << b.channels()[c].label() << " | "
               << level(*o.cursorA) << " | " << level(*o.cursorB) << " | " << edges << " |\n";
        }
    }

    const Insights insights = computeInsights(b, r, stats, lat);
    if (insights.motif) {
        const auto& m = *insights.motif;
        ss << "\n### Chronological transitions: repeating motif ×" << m.repetitions << " (cycle of "
           << m.positions.size() << " transitions; " << insights.transitions.total << " transitions in range)\n\n`";
        for (std::size_t k = 0; k < m.positions.size(); ++k) {
            const auto& p = m.positions[k];
            if (k > 0) {
                ss << " +" << p.medianDelta;
                if (p.minDelta != p.maxDelta)
                    ss << "[" << p.minDelta << ".." << p.maxDelta << "]";
                ss << " ";
            }
            ss << tokenName(b, p.channelIndex, p.rising);
        }
        const auto& first = m.positions.front();
        ss << " +" << first.medianDelta;
        if (first.minDelta != first.maxDelta)
            ss << "[" << first.minDelta << ".." << first.maxDelta << "]";
        ss << " ↻`  (Δ in samples, T = " << formatTime(b.samplePeriodSeconds()) << ")\n";
        ss << "\nDeviations from the motif: " << m.structuralDeviations << " unexpected transition(s), "
           << m.timingDeviations << " timing outlier(s)";
        if (m.burstBoundaries > 0)
            ss << ", " << m.burstBoundaries << " burst gap(s) (not counted as faults)";
        ss << "\n";
    } else if (o.maxChronologicalEvents > 0 && insights.transitions.total > 0) {
        const auto& log = insights.transitions;
        const std::size_t shown = std::min(log.events.size(), o.maxChronologicalEvents);
        ss << "\n### Chronological transitions (";
        if (log.total > shown)
            ss << "first " << shown << " of " << log.total;
        else
            ss << log.total << (log.total == 1 ? " event" : " events");
        ss << ")\n\n| # | Sample | Time (rel T) | Δs prev | Channel | Edge |\n|---|---|---|---|---|---|\n";
        for (std::size_t idx = 0; idx < shown; ++idx) {
            const auto& ev = log.events[idx];
            const auto& ch = b.channels()[ev.channelIndex];
            const auto gp = channelPinTag(board, ch);
            std::string chDesc = "CH" + std::to_string(ch.displayNumber());
            if (!gp.empty())
                chDesc += " (" + gp + ")";
            if (!ch.name.empty())
                chDesc += " " + ch.name;
            std::string dsStr = "-";
            if (idx > 0) {
                const std::size_t ds = ev.sample - log.events[idx - 1].sample;
                dsStr = ds == 0 ? "0 (same sample)" : deltaCell(b, ds);
            }
            ss << "| " << (idx + 1) << " | " << ev.sample << " | " << formatTime(b.timeOfSample(ev.sample)) << " | "
               << dsStr << " | " << chDesc << " | " << (ev.rising ? "0 -> 1 (rising)" : "1 -> 0 (falling)") << " |\n";
        }
    }

    if (!insights.anomalies.empty() && o.maxAnomalies > 0) {
        const std::size_t shown = std::min(insights.anomalies.size(), o.maxAnomalies);
        ss << "\n### Anomalies (top " << shown << " of " << insights.anomalies.size()
           << " incidents, most severe first; Δs in samples)\n";
        for (std::size_t k = 0; k < shown; ++k) {
            const auto& a = insights.anomalies[k];
            ss << "\n" << (k + 1) << ". **" << a.kind << "** at sample " << a.sample << " (t = "
               << formatTime(b.timeOfSample(a.sample)) << "): " << a.detail << "\n";
            writeMicroWindow(ss, b, insights.transitions.events, a.sample, board);
        }
    } else if (insights.transitions.total > 0) {
        ss << "\nAnomalies: none detected (no drops, spurious reactions, latency/timing outliers, irregular periods or "
              "unresolved pulses)\n";
    }

    if (!o.annotations.empty()) {
        std::size_t visible = 0;
        for (const auto& a : o.annotations)
            visible += annotationVisible(b, r, a);
        ss << "\nAnnotations (" << o.annotations.size() << " total, " << visible << " in the visible range):\n";
        for (const auto& a : o.annotations) {
            ss << "- #" << a.id << " t = " << formatTime(a.timeSeconds);
            if (a.durationSeconds > 0)
                ss << " for " << formatTime(a.durationSeconds);
            ss << " [" << inlineText(a.author, 40) << "] \"" << inlineText(a.label) << "\"" << (annotationVisible(b, r, a) ? "" : " (outside the visible range)")
               << "\n";
        }
    }

    if (!notes.empty()) {
        ss << "\nObservations:\n";
        for (const auto& n : notes)
            ss << "- " << n << "\n";
    }
    return ss.str();
}

std::string asciiTimeline(const CaptureBuffer& b, const ReportOptions& o)
{
    const Range r = normalize(b, o);
    std::ostringstream ss;
    if (r.size() == 0)
        return "(empty range)\n";

    const std::size_t cols = std::max<std::size_t>(1, std::min(o.timelineColumns, r.size()));
    auto colStart = [&](std::size_t c) { return r.first + c * r.size() / cols; };

    std::size_t labelWidth = 4;
    for (const auto& ch : b.channels())
        labelWidth = std::max(labelWidth, ch.label().size());

    // Constant channels (no edge in the range) carry no timing: merge rows with the same level, and omit the
    // timeline entirely when nothing toggles.
    std::vector<int> constLevel(b.channels().size(), -1);
    {
        const std::size_t nCh = b.channels().size();
        const uint32_t mask = nCh >= 32 ? 0xFFFFFFFFu : ((1u << nCh) - 1u);
        uint32_t changed = 0;
        for (std::size_t i = r.first + 1; i < r.last; ++i)
            changed |= (b.samples()[i] ^ b.samples()[i - 1]) & mask;
        for (std::size_t ch = 0; ch < nCh; ++ch)
            if (!((changed >> ch) & 1u))
                constLevel[ch] = b.bit(r.first, ch) ? 1 : 0;
        if (std::none_of(constLevel.begin(), constLevel.end(), [](int v) { return v < 0; }))
            return "(omitted: no channel changes level in this range; every line is constant, see the table)\n";
    }
    std::vector<int> constNumbers[2];
    for (std::size_t ch = 0; ch < b.channels().size(); ++ch)
        if (constLevel[ch] >= 0)
            constNumbers[constLevel[ch]].push_back(b.channels()[ch].displayNumber());
    const bool mergeConst[2] = {constNumbers[0].size() >= kMinMergedConstantChannels,
                                constNumbers[1].size() >= kMinMergedConstantChannels};
    std::string mergedLabel[2];
    for (int lv = 0; lv < 2; ++lv)
        if (mergeConst[lv]) {
            mergedLabel[lv] = "CH " + compactChannelList(constNumbers[lv]);
            labelWidth = std::max(labelWidth, mergedLabel[lv].size());
        }


    // Cursor marker row.
    auto colOf = [&](std::size_t sample) -> std::optional<std::size_t> {
        if (sample < r.first || sample >= r.last)
            return std::nullopt;
        return std::min(cols - 1, (sample - r.first) * cols / r.size());
    };
    if (o.cursorA || o.cursorB) {
        std::string marks(cols, ' ');
        if (o.cursorA)
            if (auto c = colOf(*o.cursorA))
                marks[*c] = 'A';
        if (o.cursorB)
            if (auto c = colOf(*o.cursorB))
                marks[*c] = marks[*c] == 'A' ? '*' : 'B';
        ss << std::string(labelWidth, ' ') << " |" << marks << "|\n";
    }

    std::vector<const Annotation*> shown;
    for (const auto& a : o.annotations)
        if (annotationVisible(b, r, a))
            shown.push_back(&a);
    if (!shown.empty()) {
        std::string marks(cols, ' ');
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const auto col = colOf(static_cast<std::size_t>(std::max(0.0, annotationSample(b, *shown[i]))));
            if (col)
                marks[*col] = i < 9 ? static_cast<char>('1' + i) : '+';
        }
        ss << std::string(labelWidth - 4, ' ') << "note |" << marks << "|\n";
    }

    std::vector<std::string> rows;
    std::size_t busyColumns = 0;
    bool mergedEmitted[2] = {false, false};
    for (std::size_t ch = 0; ch < b.channels().size(); ++ch) {
        if (constLevel[ch] >= 0 && mergeConst[constLevel[ch]]) {
            const int lv = constLevel[ch];
            if (!mergedEmitted[lv]) {
                std::string label = mergedLabel[lv];
                label.resize(labelWidth, ' ');
                rows.push_back(label + " |" + std::string(cols, lv ? '#' : '_') + "|\n");
                mergedEmitted[lv] = true;
            }
            continue;
        }
        std::string line;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t s0 = colStart(c), s1 = std::max(s0 + 1, colStart(c + 1));
            bool anyHigh = false, anyLow = false;
            // A transition at the column boundary belongs to the column it enters.
            for (std::size_t i = s0; i < s1 && i < r.last; ++i)
                (b.bit(i, ch) ? anyHigh : anyLow) = true;
            line += anyHigh && anyLow ? '|' : (anyHigh ? '#' : '_');
        }
        std::string label = b.channels()[ch].label();
        label.resize(labelWidth, ' ');
        rows.push_back(label + " |" + line + "|\n");
        busyColumns += static_cast<std::size_t>(std::count(line.begin(), line.end(), '|'));
    }
    // When nearly every column holds transitions the picture carries no information: save the tokens.
    if (o.mode == CopyMode::Brief && cols >= 20 && busyColumns * 10 >= cols * rows.size() * 9)
        return "(omitted: " + std::to_string(r.size() / cols) +
               " samples per column hide the timing; zoom in or use cursors, see the motif above)\n";
    for (const auto& row : rows)
        ss << row;
    ss << std::string(labelWidth, ' ') << " |" << formatTime(b.timeOfSample(r.first)) << " .. "
       << formatTime(b.timeOfSample(r.last - 1)) << " | 1 column = "
       << formatTime(static_cast<double>(r.size()) / static_cast<double>(cols) * b.samplePeriodSeconds()) << "\n"
       << "Legend: # high, _ low, | transition inside the column" << (o.cursorA || o.cursorB ? ", A/B cursors" : "")
       << (shown.empty() ? "" : ", 1-9 annotations") << "\n";
    for (std::size_t i = 0; i < shown.size(); ++i)
        ss << "  [" << (i < 9 ? std::to_string(i + 1) : "+") << "] t = " << formatTime(shown[i]->timeSeconds) << " ["
           << inlineText(shown[i]->author, 40) << "] " << inlineText(shown[i]->label) << "\n";
    return ss.str();
}

nlohmann::json jsonMetadata(const CaptureBuffer& b, const ReportOptions& o)
{
    const Range r = normalize(b, o);
    const auto stats = analyzeRange(b, r);
    const std::string board = resolveBoard(o);
    auto atUs = [&](std::size_t s) { return b.timeOfSample(s) * kUs; };

    nlohmann::json doc = {
        {"schema_version", 1},
        {"mode", toString(o.mode)},
        {"time_unit", "us"},
        {"time_origin", "trigger (t=0)"},
        {"sample_rate_hz", b.frequency()},
        {"quantization_step_ns", b.samplePeriodSeconds() * kNs},
        {"total_samples", b.size()},
        {"trigger_sample", b.triggerIndex()},
        {"trigger", o.triggerDescription.empty() ? nlohmann::json(nullptr) : nlohmann::json(o.triggerDescription)},
        {"visible",
         {{"first_sample", r.first},
          {"end_sample", r.last},
          {"samples", r.size()},
          {"clamped_to_cursors", r.clampedToCursors},
          {"start_us", r.size() ? nlohmann::json(atUs(r.first)) : nlohmann::json(nullptr)},
          {"end_us", r.size() ? nlohmann::json(atUs(r.last - 1)) : nlohmann::json(nullptr)}}},
    };
    if (o.captureId != 0)
        doc["capture_id"] = o.captureId;
    if (!o.warnings.empty())
        doc["warnings"] = o.warnings;
    if (!o.banner.empty())
        doc["banner"] = o.banner;
    if (!o.deviceVersion.empty())
        doc["device_version"] = o.deviceVersion;
    if (!board.empty())
        doc["board"] = board;
    if (!o.userNote.empty())
        doc["user_note"] = o.userNote;
    if (!o.setup.empty())
        doc["setup"] = toJson(o.setup);

    nlohmann::json cursors = nlohmann::json::object();
    if (o.cursorA)
        cursors["a"] = {{"sample", *o.cursorA}, {"us", atUs(*o.cursorA)}};
    if (o.cursorB)
        cursors["b"] = {{"sample", *o.cursorB}, {"us", atUs(*o.cursorB)}};
    if (o.cursorA && o.cursorB) {
        cursors["delta_us"] = atUs(*o.cursorB) - atUs(*o.cursorA);
        if (r.size() > 0) {
            const auto lo = std::min(*o.cursorA, *o.cursorB), hi = std::max(*o.cursorA, *o.cursorB);
            auto between = nlohmann::json::array();
            for (std::size_t c = 0; c < b.channels().size(); ++c) {
                std::size_t edges = 0;
                for (std::size_t i = lo + 1; i <= hi && i < b.size(); ++i)
                    edges += ((b.samples()[i] ^ b.samples()[i - 1]) >> c) & 1u;
                const auto level = [&](std::size_t i) { return i < b.size() ? int(b.bit(i, c)) : -1; };
                between.push_back({{"channel", b.channels()[c].displayNumber()},
                                   {"name", b.channels()[c].label()},
                                   {"level_at_a", level(*o.cursorA)},
                                   {"level_at_b", level(*o.cursorB)},
                                   {"edges_between", edges}});
            }
            cursors["between"] = std::move(between);
        }
    }
    doc["cursors"] = std::move(cursors);

    const auto lat = computeLatency(b, r, o);
    if (lat) {
        doc["latency"] = toJson(*lat, false);
        doc["latency"]["sample_offset"] = r.first;  // sample fields inside latency are relative to this
    }

    {
        const Insights insights = computeInsights(b, r, stats, lat);
        if (insights.motif) {
            const auto& m = *insights.motif;
            auto pos = nlohmann::json::array();
            for (const auto& p : m.positions)
                pos.push_back({{"channel", b.channels()[p.channelIndex].displayNumber()},
                               {"name", b.channels()[p.channelIndex].label()},
                               {"edge", p.rising ? "rising" : "falling"},
                               {"delta_prev_samples_median", p.medianDelta},
                               {"delta_prev_samples_min", p.minDelta},
                               {"delta_prev_samples_max", p.maxDelta}});
            doc["motif"] = {{"cycle_transitions", m.positions.size()},
                            {"repetitions", m.repetitions},
                            {"unexpected_transitions", m.structuralDeviations},
                            {"timing_deviations", m.timingDeviations},
                            {"burst_gaps", m.burstBoundaries},
                            {"positions", std::move(pos)}};
        }
        auto anomalies = nlohmann::json::array();
        for (std::size_t k = 0; k < insights.anomalies.size() && k < 64; ++k) {
            const auto& a = insights.anomalies[k];
            anomalies.push_back({{"kind", a.kind},
                                 {"severity", a.severity},
                                 {"sample", a.sample},
                                 {"time_us", atUs(a.sample)},
                                 {"detail", a.detail}});
        }
        doc["anomalies_total"] = insights.anomalies.size();
        doc["anomalies"] = std::move(anomalies);
    }

    if (o.maxChronologicalEvents > 0) {
        const auto log = collectChronologicalEdges(b, r, o.maxChronologicalEvents);
        auto evArr = nlohmann::json::array();
        std::optional<double> prevUs;
        for (const auto& ev : log.events) {
            const double tUs = atUs(ev.sample);
            const auto& ch = b.channels()[ev.channelIndex];
            nlohmann::json item = {{"sample", ev.sample},
                                   {"time_us", tUs},
                                   {"channel", ch.displayNumber()},
                                   {"name", ch.label()},
                                   {"edge", ev.rising ? "rising" : "falling"}};
            if (prevUs)
                item["delta_prev_ns"] = (tUs - *prevUs) * 1e3;
            if (const auto gp = channelPinTag(board, ch); !gp.empty())
                item["gpio"] = gp;
            prevUs = tUs;
            evArr.push_back(std::move(item));
        }
        doc["chronological_edges"] = {
            {"total", log.total}, {"truncated", log.total > log.events.size()}, {"events", std::move(evArr)}};
    }

    auto notes = nlohmann::json::array();
    for (const auto& a : o.annotations) {
        nlohmann::json n = {{"id", a.id},
                            {"time_us", a.timeSeconds * kUs},
                            {"label", a.label},
                            {"color", a.color},
                            {"author", a.author},
                            {"visible", annotationVisible(b, r, a)}};
        if (a.durationSeconds > 0)
            n["duration_us"] = a.durationSeconds * kUs;
        notes.push_back(std::move(n));
    }
    doc["annotations"] = std::move(notes);

    auto channels = nlohmann::json::array();
    for (const auto& st : stats) {
        nlohmann::json ch = toJson(st, false);
        if (const auto gp = channelPinTag(board, st.channel); !gp.empty())
            ch["gpio"] = gp;
        ch["initial_level"] = st.idleLevel;
        auto edges = nlohmann::json::array();
        const std::size_t shown = std::min(st.edges.size(), o.maxEdgesPerChannel);
        for (std::size_t i = 0; i < shown; ++i)
            edges.push_back(atUs(r.first + st.edges[i]));
        ch["edge_times_us"] = std::move(edges);
        ch["edges_shown"] = shown;
        ch["edges_note"] = "level toggles at each edge, starting from initial_level";
        ch["edges_truncated"] = st.edges.size() > shown;
        channels.push_back(std::move(ch));
    }
    doc["channels"] = std::move(channels);
    return doc;
}

std::string copyForAi(const CaptureBuffer& b, const ReportOptions& o)
{
    std::ostringstream ss;
    ss << markdownSummary(b, o) << "\n### ASCII timeline\n\n```text\n" << asciiTimeline(b, o) << "```\n";
    if (o.mode != CopyMode::Brief)
        ss << "\n### JSON\n\n```json\n" << jsonMetadata(b, o).dump(2) << "\n```\n";
    return ss.str();
}

std::string probeForAi(const nlohmann::json& doc, std::string_view userNote)
{
    std::ostringstream ss;
    ss << "## LogicTracer input probe\n\n";
    if (!userNote.empty())
        ss << "> **Context / Question:** " << inlineText(userNote, 2000) << "\n\n";
    std::string board;
    if (doc.contains("device") && doc["device"].is_object()) {
        const auto ver = doc["device"].value("version", std::string{});
        board = doc["device"].value("board", std::string{});
        if (board.empty() && !ver.empty()) {
            if (const auto pv = parseVersionLine(ver))
                board = pv->board;
        }
        ss << "- Device: " << ver << " on `" << doc.value("port", std::string{"?"}) << "`\n";
    } else if (doc.contains("port")) {
        ss << "- Port: `" << doc.value("port", std::string{"?"}) << "`\n";
    }
    ss << "- Status: **" << doc.value("status", std::string{"UNKNOWN"}) << "**";
    if (doc.contains("samples") && doc.contains("sample_rate_hz")) {
        ss << " (" << doc["samples"].get<std::size_t>() << " samples @ "
           << formatHz(doc["sample_rate_hz"].get<double>()) << " in " << doc.value("elapsed_ms", 0ll) << " ms)";
    }
    ss << "\n";
    if (doc.value("status", std::string{}) != "OK") {
        ss << "- Error: " << doc.value("error", std::string{"failed"}) << "\n";
        return ss.str();
    }

    ss << "\n| Ch | Pin | Name | State | High % | Edges | Freq | Diagnosis |\n"
          "|---|---|---|---|---|---|---|---|\n";
    if (doc.contains("channels") && doc["channels"].is_array()) {
        for (const auto& c : doc["channels"]) {
            const int chNum = c.value("channel", 0);
            std::string pin = "-";
            if (!board.empty() && chNum >= 1) {
                if (const auto gp = gpioForChannel(board, static_cast<uint8_t>(chNum - 1)))
                    pin = "GP" + std::to_string(*gp);
            }
            const double highPct = c.contains("high_time_percent") ? c.value("high_time_percent", 0.0)
                                                                   : c.value("high_pct", 0.0);
            const std::size_t edges = c.contains("transitions") ? c.value("transitions", std::size_t{0})
                                                                : c.value("edges", std::size_t{0});
            std::ostringstream high;
            high << std::fixed << std::setprecision(1) << highPct << "%";
            std::string freq = "-";
            if (c.contains("mean_frequency_hz") && c["mean_frequency_hz"].is_number())
                freq = formatHz(c["mean_frequency_hz"].get<double>());
            else if (c.contains("frequency_hz") && c["frequency_hz"].is_number())
                freq = formatHz(c["frequency_hz"].get<double>());
            const std::string diag = c.contains("message") && c["message"].is_string()
                                         ? c["message"].get<std::string>()
                                         : c.value("diagnosis", std::string{});
            ss << "| " << chNum << " | " << pin << " | " << c.value("name", std::string{}) << " | **"
               << c.value("state", std::string{}) << "** | " << high.str() << " | " << edges << " | " << freq << " | "
               << diag << " |\n";
        }
    }
    if (const auto note = inputPhysicsNote(board); !note.empty())
        ss << "\n" << note << "\n";
    return ss.str();
}

std::string inputPhysicsNote(std::string_view board)
{
    if (board != "PICO" && board != "PICO_2" && board != "W" && board != "WIFI" && board != "2_W" &&
        board != "2_WIFI" && board != "ZERO")
        return {};
    return "Input physics (RP2040/RP2350): inputs default to a weak internal pull-down, so LOW means either a driven 0 V or an "
           "unpulled/open/floating line, not a proven short to GND; HIGH needs a driver or an external pull-up. "
           "Inputs are 3.3 V only (not 5 V tolerant).";
}

std::string timeoutForAi(const CaptureBuffer& lineDump, std::string_view status, std::string_view error,
                         std::string_view triggerDescription,
                         std::string_view board, std::string_view userNote, uint64_t previousCaptureId)
{
    std::ostringstream ss;
    ss << "## LogicTracer capture failed (" << status << ")\n\n";
    if (!userNote.empty())
        ss << "> **Context / Question:** " << inlineText(userNote, 2000) << "\n\n";
    ss << "- Status: **" << status << "** (" << error << ")\n";
    ss << "- Trigger: " << (triggerDescription.empty() ? "unknown" : std::string(triggerDescription)) << "\n";
    if (previousCaptureId != 0)
        ss << "- The waveform on screen is the previous capture #" << previousCaptureId
           << "; it does NOT describe this attempt.\n";
    if (lineDump.size() == 0) {
        ss << "\nThe device returned no line levels.\n";
        if (const auto note = inputPhysicsNote(board); !note.empty())
            ss << "\n" << note << "\n";
        return ss.str();
    }
    ss << "\nLine levels read right after the failure:\n\n| Ch | Pin | Name | State | High % | Edges | Diagnosis |\n"
          "|---|---|---|---|---|---|---|\n";
    for (const auto& st : SignalAnalyzer{}.analyze(lineDump)) {
        const auto diag = SignalAnalyzer::diagnose(st);
        const auto gp = channelPinTag(board, st.channel);
        std::ostringstream high;
        high << std::fixed << std::setprecision(1) << st.highTimePercent;
        ss << "| " << st.channel.displayNumber() << " | " << (gp.empty() ? "-" : gp) << " | " << st.channel.label()
           << " | **" << toString(diag.state) << "** | " << high.str() << "% | " << st.transitions << " | "
           << diag.message << " |\n";
    }
    if (const auto note = inputPhysicsNote(board); !note.empty())
        ss << "\n" << note << "\n";
    return ss.str();
}

std::vector<AnomalyIncident> findAnomalies(const CaptureBuffer& b, const ReportOptions& o)
{
    const Range r = normalize(b, o);
    const auto stats = analyzeRange(b, r);
    const auto lat = computeLatency(b, r, o);
    std::vector<AnomalyIncident> out;
    for (auto& a : computeInsights(b, r, stats, lat).anomalies)
        out.push_back({a.sample, a.severity, std::move(a.kind), std::move(a.detail)});
    return out;
}

}  // namespace lt

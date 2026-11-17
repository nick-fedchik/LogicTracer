// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/CaptureDiff.hpp"

#include "core/AiReport.hpp"
#include "core/Exporters.hpp"
#include "core/Format.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace lt {

namespace {

using json = nlohmann::json;

std::optional<std::size_t> indexOf(const CaptureBuffer& b, uint8_t number)
{
    for (std::size_t i = 0; i < b.channels().size(); ++i)
        if (b.channels()[i].number == number)
            return i;
    return std::nullopt;
}

std::optional<LatencyPairSpec> resolve(const CaptureBuffer& b, const DiffOptions& o)
{
    if (!o.latency)
        return std::nullopt;
    const auto s = indexOf(b, o.latency->stimChannel), r = indexOf(b, o.latency->reactChannel);
    if (!s || !r)
        return std::nullopt;
    return LatencyPairSpec{*s, o.latency->stimFalling, *r, o.latency->reactFalling, std::nullopt};
}

json side(const CaptureBuffer& b)
{
    return {{"sample_rate_hz", b.frequency()},
            {"samples", b.size()},
            {"quantization_step_ns", b.samplePeriodSeconds() * 1e9},
            {"channels", b.channels().size()}};
}

json channelSummary(const ChannelStats& st)
{
    std::optional<double> minW, maxW;
    for (const auto& w : {st.highWidth, st.lowWidth}) {
        if (!w)
            continue;
        const double mn = st.samplesToSeconds(static_cast<double>(w->minSamples)) * 1e9;
        const double mx = st.samplesToSeconds(static_cast<double>(w->maxSamples)) * 1e9;
        minW = minW ? std::min(*minW, mn) : mn;
        maxW = maxW ? std::max(*maxW, mx) : mx;
    }
    auto opt = [](const std::optional<double>& v) { return v ? json(*v) : json(nullptr); };
    return {{"transitions", st.transitions},
            {"frequency_hz", opt(st.meanFrequencyHz)},
            {"duty_percent", opt(st.dutyCyclePercent)},
            {"min_pulse_ns", opt(minW)},
            {"max_pulse_ns", opt(maxW)},
            {"glitches", st.glitchCount}};
}

// Each anomaly incident's kind may combine several detectors ("DroppedStimulus+UnexpectedTransition").
std::map<std::string, std::size_t> kindCounts(const std::vector<AnomalyIncident>& incidents)
{
    std::map<std::string, std::size_t> out;
    for (const auto& a : incidents) {
        std::size_t start = 0;
        for (;;) {
            const auto plus = a.kind.find('+', start);
            out[a.kind.substr(start, plus == std::string::npos ? std::string::npos : plus - start)]++;
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
    }
    return out;
}

}  // namespace

json diffCaptures(const CaptureBuffer& a, const CaptureBuffer& b, const DiffOptions& options)
{
    json out = {{"a", side(a)}, {"b", side(b)}};
    json regressions = json::array(), improvements = json::array(), notes = json::array();

    // Channels, matched by hardware number.
    SignalAnalyzer analyzer;
    const auto statsA = analyzer.analyze(a), statsB = analyzer.analyze(b);
    json channels = json::array();
    for (std::size_t i = 0; i < statsA.size(); ++i) {
        const auto& ch = statsA[i].channel;
        json row = {{"channel", ch.displayNumber()}, {"name", ch.label()}, {"a", channelSummary(statsA[i])}};
        if (const auto j = indexOf(b, ch.number)) {
            row["b"] = channelSummary(statsB[*j]);
            row["transitions_delta"] =
                static_cast<int64_t>(statsB[*j].transitions) - static_cast<int64_t>(statsA[i].transitions);
            if (statsA[i].transitions > 0 && statsB[*j].transitions == 0)
                regressions.push_back(ch.label() + " stopped toggling (" + std::to_string(statsA[i].transitions) +
                                      " -> 0 transitions)");
        } else {
            row["b"] = nullptr;
            regressions.push_back(ch.label() + " is missing from B");
        }
        channels.push_back(std::move(row));
    }
    for (const auto& st : statsB)
        if (!indexOf(a, st.channel.number))
            notes.push_back(st.channel.label() + " exists only in B");
    out["channels"] = std::move(channels);

    // Latency: a difference is real only beyond what the +-1 sample quantization of both captures allows.
    const auto specA = resolve(a, options), specB = resolve(b, options);
    std::optional<LatencyStats> latA, latB;
    if (options.latency) {
        if (specA)
            latA = SignalAnalyzer::analyzeLatency(a, *specA);
        if (specB)
            latB = SignalAnalyzer::analyzeLatency(b, *specB);
        if (!latA || !latB)
            notes.push_back("the latency channels are not present in both captures");
    }
    if (latA && latB) {
        const auto& la = *latA;
        const auto& lb = *latB;
        const double tolerance = la.uncertaintyNs() + lb.uncertaintyNs();
        auto metric = [&](const char* name, double va, double vb, bool hasValue) {
            json m = {{"a", hasValue ? json(va) : json(nullptr)}, {"b", hasValue ? json(vb) : json(nullptr)}};
            if (hasValue) {
                m["delta"] = vb - va;
                m["within_quantization"] = std::abs(vb - va) <= tolerance;
                if (vb - va > tolerance)
                    regressions.push_back(std::string("latency ") + name + " increased by " + formatTime((vb - va) * 1e-9) +
                                          " (" + formatTime(va * 1e-9) + " -> " + formatTime(vb * 1e-9) +
                                          ", beyond the +-" + formatTime(tolerance * 1e-9) + " quantization)");
                else if (va - vb > tolerance)
                    improvements.push_back(std::string("latency ") + name + " decreased by " +
                                           formatTime((va - vb) * 1e-9));
            }
            return m;
        };
        const bool both = la.count > 0 && lb.count > 0;
        json lat = {{"tolerance_ns", tolerance},
                    {"count", {{"a", la.count}, {"b", lb.count}}},
                    {"drops", {{"a", la.unmatchedStimCount}, {"b", lb.unmatchedStimCount}}},
                    {"spurious", {{"a", la.spuriousReactCount}, {"b", lb.spuriousReactCount}}},
                    {"p50_ns", metric("p50", la.p50Ns, lb.p50Ns, both)},
                    {"p99_ns", metric("p99", la.p99Ns, lb.p99Ns, both)},
                    {"max_ns", metric("max", la.maxNs, lb.maxNs, both)},
                    {"stdev_ns", {{"a", la.stdevNs}, {"b", lb.stdevNs}}},
                    {"histogram_a", toJson(la)["histogram"]},
                    {"histogram_b", toJson(lb)["histogram"]}};
        if (lb.unmatchedStimCount > la.unmatchedStimCount)
            regressions.push_back("dropped stimuli increased (" + std::to_string(la.unmatchedStimCount) + " -> " +
                                  std::to_string(lb.unmatchedStimCount) + ")");
        else if (lb.unmatchedStimCount < la.unmatchedStimCount)
            improvements.push_back("dropped stimuli decreased (" + std::to_string(la.unmatchedStimCount) + " -> " +
                                   std::to_string(lb.unmatchedStimCount) + ")");
        if (lb.spuriousReactCount > la.spuriousReactCount)
            regressions.push_back("spurious reactions increased (" + std::to_string(la.spuriousReactCount) + " -> " +
                                  std::to_string(lb.spuriousReactCount) + ")");
        // Both captures may be explained by one fixed delay: then the histograms differ by sampling phase only.
        if (la.constantDelayConsistent() && lb.constantDelayConsistent()) {
            // Asynchronous captures bound the delay by an open interval, exact (in-design) ones by a point [d, d]:
            // the intersection is non-empty if lo < hi, or lo == hi with both ends closed.
            constexpr double kEps = 1e-6;  // ns
            const double aLo = la.constantDelayLowerNs(), bLo = lb.constantDelayLowerNs();
            const double aHi = la.constantDelayUpperNs(), bHi = lb.constantDelayUpperNs();
            const double lo = std::max(aLo, bLo), hi = std::min(aHi, bHi);
            const bool loClosed = (std::abs(aLo - lo) > kEps || la.exact) && (std::abs(bLo - lo) > kEps || lb.exact);
            const bool hiClosed = (std::abs(aHi - hi) > kEps || la.exact) && (std::abs(bHi - hi) > kEps || lb.exact);
            const bool overlap = hi - lo > kEps || (std::abs(hi - lo) <= kEps && loClosed && hiClosed);
            const auto range = (loClosed ? "[" : "(") + formatTime(lo * 1e-9) + ", " + formatTime(hi * 1e-9) +
                               (hiClosed ? "]" : ")");
            lat["sampling_phase_verdict"] =
                overlap ? (la.exact && lb.exact ? "same fixed delay: " + formatTime(lo * 1e-9) + " exactly in both captures"
                                                : "same fixed delay: both captures fit one delay in " + range +
                                                      "; the histogram difference is sampling phase")
                        : std::string("different fixed delays: the constant-delay intervals do not overlap");
            lat["same_fixed_delay_possible"] = overlap;
        } else {
            lat["sampling_phase_verdict"] =
                la.exact || lb.exact ? "at least one capture shows a varying delay (beyond its timing resolution)"
                                     : "at least one capture shows real jitter (spread > 1 sample)";
            lat["same_fixed_delay_possible"] = false;
        }
        for (const auto* l : {&la, &lb})
            if (l->reversed)
                notes.push_back(SignalAnalyzer::reversedHint(*l));
        out["latency"] = std::move(lat);
    }

    // Anomalies by detector kind.
    ReportOptions ra, rb;
    ra.latency = specA;
    rb.latency = specB;
    const auto incA = findAnomalies(a, ra), incB = findAnomalies(b, rb);
    const auto kindsA = kindCounts(incA), kindsB = kindCounts(incB);
    json kinds = json::object();
    for (const auto& [k, n] : kindsA)
        kinds[k] = {{"a", n}, {"b", kindsB.contains(k) ? kindsB.at(k) : 0}};
    for (const auto& [k, n] : kindsB) {
        if (!kindsA.contains(k))
            kinds[k] = {{"a", 0}, {"b", n}};
        const std::size_t before = kindsA.contains(k) ? kindsA.at(k) : 0;
        if (n > before)
            regressions.push_back(k + " incidents increased (" + std::to_string(before) + " -> " + std::to_string(n) + ")");
    }
    for (const auto& [k, n] : kindsA) {
        const std::size_t after = kindsB.contains(k) ? kindsB.at(k) : 0;
        if (after < n)
            improvements.push_back(k + " incidents decreased (" + std::to_string(n) + " -> " + std::to_string(after) + ")");
    }
    out["anomalies"] = {{"a_total", incA.size()}, {"b_total", incB.size()}, {"by_kind", std::move(kinds)}};

    if (a.frequency() != b.frequency())
        notes.push_back("sample rates differ (" + formatHz(a.frequency()) + " vs " + formatHz(b.frequency()) +
                        "): compare times, not sample counts");
    out["verdict"] = !regressions.empty()    ? "regression"
                     : !improvements.empty() ? "improved"
                                             : "equivalent";
    out["regressions"] = std::move(regressions);
    out["improvements"] = std::move(improvements);
    out["notes"] = std::move(notes);
    return out;
}

std::string diffToMarkdown(const json& d, const std::string& nameA, const std::string& nameB)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << "## LogicTracer diff: " << nameA << " (A) vs " << nameB << " (B)\n\n";
    ss << "- Verdict: **" << d["verdict"].get<std::string>() << "**\n";
    ss << "- A: " << formatHz(d["a"]["sample_rate_hz"].get<double>()) << ", " << d["a"]["samples"].get<std::size_t>()
       << " samples | B: " << formatHz(d["b"]["sample_rate_hz"].get<double>()) << ", "
       << d["b"]["samples"].get<std::size_t>() << " samples\n";
    auto list = [&](const char* title, const json& items) {
        if (items.empty())
            return;
        ss << "\n### " << title << "\n\n";
        for (const auto& i : items)
            ss << "- " << i.get<std::string>() << "\n";
    };
    list("Regressions", d["regressions"]);
    list("Improvements", d["improvements"]);
    if (d.contains("latency")) {
        const auto& l = d["latency"];
        auto ns = [](const json& v) { return v.is_null() ? std::string("-") : formatTime(v.get<double>() * 1e-9); };
        ss << "\n### Latency\n\n| Metric | A | B | Within +-quantization |\n|---|---|---|---|\n";
        ss << "| pairs | " << l["count"]["a"].get<uint64_t>() << " | " << l["count"]["b"].get<uint64_t>() << " | |\n";
        ss << "| drops | " << l["drops"]["a"].get<uint64_t>() << " | " << l["drops"]["b"].get<uint64_t>() << " | |\n";
        ss << "| spurious | " << l["spurious"]["a"].get<uint64_t>() << " | " << l["spurious"]["b"].get<uint64_t>() << " | |\n";
        for (const char* key : {"p50_ns", "p99_ns", "max_ns"}) {
            const auto& m = l[key];
            ss << "| " << std::string(key).substr(0, std::string(key).size() - 3) << " | " << ns(m["a"]) << " | "
               << ns(m["b"]) << " | " << (m.contains("within_quantization") ? (m["within_quantization"].get<bool>() ? "yes" : "no") : "-")
               << " |\n";
        }
        ss << "\nSampling phase: " << l["sampling_phase_verdict"].get<std::string>() << "\n";
    }
    ss << "\n### Channels\n\n| Ch | Name | Transitions A | Transitions B | Frequency A | Frequency B |\n|---|---|---|---|---|---|\n";
    for (const auto& c : d["channels"]) {
        auto hz = [](const json& v) { return v.is_null() ? std::string("-") : formatHz(v.get<double>()); };
        ss << "| " << c["channel"].get<int>() << " | " << c["name"].get<std::string>() << " | "
           << c["a"]["transitions"].get<uint64_t>() << " | "
           << (c["b"].is_null() ? std::string("missing") : std::to_string(c["b"]["transitions"].get<uint64_t>())) << " | "
           << hz(c["a"]["frequency_hz"]) << " | " << (c["b"].is_null() ? std::string("-") : hz(c["b"]["frequency_hz"]))
           << " |\n";
    }
    ss << "\nAnomaly incidents: A " << d["anomalies"]["a_total"].get<std::size_t>() << ", B "
       << d["anomalies"]["b_total"].get<std::size_t>() << "\n";
    list("Notes", d["notes"]);
    return ss.str();
}

}  // namespace lt

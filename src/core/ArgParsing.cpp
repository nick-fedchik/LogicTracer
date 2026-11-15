// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/ArgParsing.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>

namespace lt::cli {

namespace {

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::optional<int> parseInt(std::string_view s)
{
    s = trim(s);
    int v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const auto pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return out;
}

}  // namespace

std::optional<uint64_t> parseQuantity(std::string_view text)
{
    std::string s = lower(trim(text));
    for (std::string_view unit : {"hz", "sps"})
        if (s.size() > unit.size() && s.ends_with(unit))
            s.resize(s.size() - unit.size());
    if (s.empty())
        return std::nullopt;

    double mult = 1.0;
    switch (s.back()) {
    case 'k': mult = 1e3; break;
    case 'm': mult = 1e6; break;
    case 'g': mult = 1e9; break;
    default: break;
    }
    if (mult != 1.0)
        s.pop_back();
    if (s.empty() || !(std::isdigit(static_cast<unsigned char>(s.front())) || s.front() == '.'))
        return std::nullopt;

    double value = 0.0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size() || value < 0)
        return std::nullopt;
    const double scaled = value * mult;
    const double rounded = std::round(scaled);
    if (std::abs(scaled - rounded) > 1e-6 || rounded > 1.8e19)
        return std::nullopt;
    return static_cast<uint64_t>(rounded);
}

std::optional<double> parseTimeSeconds(std::string_view text)
{
    const std::string s = lower(trim(text));
    if (s == "0")
        return 0.0;
    struct Unit {
        std::string_view suffix;
        double scale;
    };
    // Longest suffixes first: "ms" must not be read as "s".
    for (const Unit& u : {Unit{"ms", 1e-3}, Unit{"us", 1e-6}, Unit{"ns", 1e-9}, Unit{"s", 1.0}}) {
        if (s.size() <= u.suffix.size() || !s.ends_with(u.suffix))
            continue;
        const std::string number = s.substr(0, s.size() - u.suffix.size());
        double value = 0.0;
        const auto [p, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || p != number.data() + number.size() || !std::isfinite(value))
            return std::nullopt;
        return value * u.scale;
    }
    return std::nullopt;
}

std::optional<std::vector<ChannelInfo>> parseChannelList(std::string_view text, int maxChannels, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<std::vector<ChannelInfo>> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    std::vector<ChannelInfo> out;
    std::set<int> seen;
    auto add = [&](int n, std::string name) -> bool {
        if (n < 1 || n > maxChannels) {
            fail("channel " + std::to_string(n) + " out of range 1.." + std::to_string(maxChannels));
            return false;
        }
        if (!seen.insert(n).second) {
            fail("channel " + std::to_string(n) + " specified twice");
            return false;
        }
        out.push_back({static_cast<uint8_t>(n - 1), std::move(name)});
        return true;
    };

    for (auto token : split(text, ',')) {
        token = trim(token);
        if (token.empty())
            return fail("empty channel entry in '" + std::string(text) + "'");
        std::string name;
        if (const auto colon = token.find(':'); colon != std::string_view::npos) {
            name = std::string(trim(token.substr(colon + 1)));
            token = trim(token.substr(0, colon));
            if (name.empty())
                return fail("empty channel name in '" + std::string(text) + "'");
        }
        if (const auto dash = token.find('-'); dash != std::string_view::npos && dash > 0) {
            const auto a = parseInt(token.substr(0, dash));
            const auto b = parseInt(token.substr(dash + 1));
            if (!a || !b || *a > *b)
                return fail("invalid channel range '" + std::string(token) + "'");
            if (!name.empty())
                return fail("a name cannot be assigned to a channel range");
            for (int n = *a; n <= *b; ++n)
                if (!add(n, {}))
                    return std::nullopt;
        } else {
            const auto n = parseInt(token);
            if (!n)
                return fail("invalid channel '" + std::string(token) + "'");
            if (!add(*n, std::move(name)))
                return std::nullopt;
        }
    }
    if (out.empty())
        return fail("no channels given");
    return out;
}

std::optional<TriggerArg> parseTrigger(std::string_view text, int maxChannels, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<TriggerArg> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    const std::string t = lower(trim(text));
    if (t.empty() || t == "none" || t == "immediate")
        return TriggerArg{};

    const auto colon = t.find(':');
    const auto ch = parseInt(std::string_view(t).substr(0, colon));
    if (!ch || *ch < 1 || *ch > maxChannels)
        return fail("invalid trigger channel in '" + std::string(text) + "' (expected 1.." +
                    std::to_string(maxChannels) + ")");
    const std::string edge = colon == std::string::npos ? "rising" : t.substr(colon + 1);

    TriggerArg arg;
    arg.none = false;
    arg.trigger.channel = static_cast<uint8_t>(*ch - 1);
    if (edge == "rising" || edge == "rise" || edge == "high" || edge == "pos" || edge == "r")
        arg.trigger.inverted = false;
    else if (edge == "falling" || edge == "fall" || edge == "low" || edge == "neg" || edge == "f")
        arg.trigger.inverted = true;
    else
        return fail("invalid trigger edge '" + edge + "' (expected rising|falling)");
    return arg;
}

std::optional<LatencyArg> parseLatencyArg(std::string_view text, int maxChannels, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<LatencyArg> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    std::size_t sep = text.find("->");
    std::size_t sepLen = 2;
    if (sep == std::string_view::npos) {
        sep = text.find("..");
        sepLen = 2;
    }
    if (sep == std::string_view::npos)
        return fail("invalid latency spec '" + std::string(text) +
                    "' (expected STIM[:edge]..REACT[:edge] or 'STIM->REACT', e.g. 1..2)");

    std::string err;
    const auto lhs = parseTrigger(text.substr(0, sep), maxChannels, &err);
    if (!lhs || lhs->none)
        return fail("invalid stimulus in '" + std::string(text) + "': " + (err.empty() ? "channel required" : err));
    const auto rhs = parseTrigger(text.substr(sep + sepLen), maxChannels, &err);
    if (!rhs || rhs->none)
        return fail("invalid reaction in '" + std::string(text) + "': " + (err.empty() ? "channel required" : err));

    return LatencyArg{lhs->trigger.channel, lhs->trigger.inverted, rhs->trigger.channel, rhs->trigger.inverted};
}

std::optional<std::vector<ExpectationClause>> parseExpectations(std::string_view text, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<std::vector<ExpectationClause>> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    std::vector<ExpectationClause> out;
    // Normalize ';' into ',' so both separators work naturally in shell scripts.
    std::string normalized(text);
    std::replace(normalized.begin(), normalized.end(), ';', ',');

    struct OpToken {
        std::string_view str;
        CompareOp op;
    };
    static constexpr OpToken kOps[] = {{"<=", CompareOp::Le}, {">=", CompareOp::Ge}, {"==", CompareOp::Eq},
                                       {"!=", CompareOp::Ne}, {"<", CompareOp::Lt},  {">", CompareOp::Gt}};

    for (auto rawClause : split(normalized, ',')) {
        rawClause = trim(rawClause);
        if (rawClause.empty())
            continue;
        std::size_t opPos = std::string_view::npos;
        std::size_t opLen = 0;
        CompareOp op = CompareOp::Eq;
        for (const auto& candidate : kOps) {
            if (const auto p = rawClause.find(candidate.str); p != std::string_view::npos) {
                opPos = p;
                opLen = candidate.str.size();
                op = candidate.op;
                break;
            }
        }
        if (opPos == std::string_view::npos)
            return fail("missing comparison operator in --expect clause '" + std::string(rawClause) + "'");

        const std::string_view lhs = trim(rawClause.substr(0, opPos));
        const std::string_view rhs = trim(rawClause.substr(opPos + opLen));
        if (lhs.empty() || rhs.empty())
            return fail("incomplete --expect clause '" + std::string(rawClause) + "'");

        double threshold = 0.0;
        const auto [ptr, ec] = std::from_chars(rhs.data(), rhs.data() + rhs.size(), threshold);
        if (ec != std::errc{} || ptr != rhs.data() + rhs.size() || !std::isfinite(threshold))
            return fail("invalid numeric threshold in --expect clause '" + std::string(rawClause) + "'");

        ExpectationClause c;
        c.raw = std::string(rawClause);
        c.op = op;
        c.threshold = threshold;
        if (const auto colon = lhs.find(':'); colon != std::string_view::npos) {
            c.target = std::string(trim(lhs.substr(0, colon)));
            c.metric = lower(trim(lhs.substr(colon + 1)));
            if (c.target.empty() || c.metric.empty())
                return fail("invalid channel:metric in --expect clause '" + std::string(rawClause) + "'");
        } else {
            c.metric = lower(lhs);
        }
        out.push_back(std::move(c));
    }
    if (out.empty())
        return fail("no assertions found in --expect");
    return out;
}

std::vector<ExpectationCheck> evaluateExpectations(const std::vector<ExpectationClause>& clauses,
                                                   const std::vector<ChannelStats>& channelStats,
                                                   const std::optional<LatencyStats>& latencyStats)
{
    auto compare = [](double a, CompareOp op, double b) {
        switch (op) {
        case CompareOp::Eq: return std::abs(a - b) <= 1e-9;
        case CompareOp::Ne: return std::abs(a - b) > 1e-9;
        case CompareOp::Lt: return a < b;
        case CompareOp::Le: return a <= b + 1e-9;
        case CompareOp::Gt: return a > b;
        case CompareOp::Ge: return a + 1e-9 >= b;
        }
        return false;
    };

    std::vector<ExpectationCheck> results;
    results.reserve(clauses.size());

    for (const auto& c : clauses) {
        ExpectationCheck chk;
        chk.expression = c.raw;

        if (c.target.empty()) {
            if (!latencyStats) {
                chk.detail = "latency metric '" + c.metric + "' requires --measure-latency";
                results.push_back(std::move(chk));
                continue;
            }
            const auto& l = *latencyStats;
            // *_ub_ns / *_lb_ns: guaranteed bounds of the true delay given +-1 sample quantization.
            auto boundOf = [&](const std::string& base, bool upper) -> std::optional<double> {
                uint32_t s = 0;
                if (base == "min") s = l.minSamples;
                else if (base == "max") s = l.maxSamples;
                else if (base == "p50") s = l.p50Samples;
                else if (base == "p90") s = l.p90Samples;
                else if (base == "p95") s = l.p95Samples;
                else if (base == "p99") s = l.p99Samples;
                else if (base == "mean")
                    return upper ? l.meanNs + l.quantizationStepNs : std::max(0.0, l.meanNs - l.quantizationStepNs);
                else
                    return std::nullopt;
                return upper ? l.upperBoundNs(s) : l.lowerBoundNs(s);
            };
            const bool isUb = c.metric.size() > 6 && c.metric.ends_with("_ub_ns");
            const bool isLb = c.metric.size() > 6 && c.metric.ends_with("_lb_ns");
            if (c.metric == "events" || c.metric == "count" || c.metric == "n")
                chk.actual = static_cast<double>(l.count);
            else if (c.metric == "drops" || c.metric == "unmatched")
                chk.actual = static_cast<double>(l.unmatchedStimCount);
            else if (c.metric == "spurious")
                chk.actual = static_cast<double>(l.spuriousReactCount);
            else if (c.metric == "censored")
                chk.actual = static_cast<double>(l.censoredStimCount + l.censoredReactCount);
            else if (c.metric == "coincident")
                chk.actual = static_cast<double>(l.coincidentCount);
            else if (c.metric == "outliers")
                chk.actual = static_cast<double>(l.outlierCount);
            else if (c.metric == "reacts_per_stim")
                chk.actual = static_cast<double>(l.maxReactsPerStim);
            else if (l.count == 0) {
                chk.detail = "no paired latency events captured (count == 0)";
                results.push_back(std::move(chk));
                continue;
            } else if (isUb || isLb) {
                chk.actual = boundOf(c.metric.substr(0, c.metric.size() - 6), isUb);
                if (!chk.actual) {
                    chk.detail = "unknown latency bound metric '" + c.metric + "'";
                    results.push_back(std::move(chk));
                    continue;
                }
            } else if (c.metric == "min_ns") chk.actual = l.minNs;
            else if (c.metric == "max_ns") chk.actual = l.maxNs;
            else if (c.metric == "mean_ns") chk.actual = l.meanNs;
            else if (c.metric == "stdev_ns" || c.metric == "jitter_ns") chk.actual = l.stdevNs;
            else if (c.metric == "p50_ns") chk.actual = l.p50Ns;
            else if (c.metric == "p90_ns") chk.actual = l.p90Ns;
            else if (c.metric == "p95_ns") chk.actual = l.p95Ns;
            else if (c.metric == "p99_ns") chk.actual = l.p99Ns;
            else {
                chk.detail = "unknown latency metric '" + c.metric + "'";
                results.push_back(std::move(chk));
                continue;
            }
        } else {
            const ChannelStats* match = nullptr;
            const std::string tLow = lower(c.target);
            for (const auto& st : channelStats) {
                const std::string numStr = std::to_string(st.channel.displayNumber());
                if (c.target == st.channel.name || c.target == st.channel.label() || tLow == "ch" + numStr ||
                    c.target == numStr) {
                    match = &st;
                    break;
                }
            }
            if (!match) {
                chk.detail = "channel '" + c.target + "' is not in the capture";
                results.push_back(std::move(chk));
                continue;
            }
            if (c.metric == "edges" || c.metric == "transitions") chk.actual = static_cast<double>(match->transitions);
            else if (c.metric == "rising_edges") chk.actual = static_cast<double>(match->risingEdges);
            else if (c.metric == "falling_edges") chk.actual = static_cast<double>(match->fallingEdges);
            else if (c.metric == "pulses")
                chk.actual = static_cast<double>(match->pulseCountHigh + match->pulseCountLow);
            else if (c.metric == "high_pulses") chk.actual = static_cast<double>(match->pulseCountHigh);
            else if (c.metric == "low_pulses") chk.actual = static_cast<double>(match->pulseCountLow);
            else if (c.metric == "glitches" || c.metric == "unresolved_pulses")
                chk.actual = static_cast<double>(match->glitchCount);
            else if (c.metric == "bursts")
                chk.actual = match->period ? static_cast<double>(match->period->burstCount) : 0.0;
            else if (c.metric == "irregular_periods")
                chk.actual = match->period ? static_cast<double>(match->period->irregularCount) : 0.0;
            else if (c.metric == "period_cv") {
                if (!match->period) {
                    chk.detail = "channel '" + c.target + "' has fewer than 2 same-direction edges (no period)";
                    results.push_back(std::move(chk));
                    continue;
                }
                chk.actual = match->period->cv;
            }
            else if (c.metric == "idle" || c.metric == "idle_level") chk.actual = match->idleLevel;
            else if (c.metric == "final" || c.metric == "final_level") chk.actual = match->finalLevel;
            else if (c.metric == "high_pct") chk.actual = match->highTimePercent;
            else if (c.metric == "trailing_us")
                chk.actual = match->samplesToSeconds(static_cast<double>(match->trailingSamples)) * 1e6;
            else if (c.metric == "freq_hz") {
                if (!match->meanFrequencyHz) {
                    chk.detail = "channel '" + c.target + "' has fewer than 2 same-direction edges (no frequency)";
                    results.push_back(std::move(chk));
                    continue;
                }
                chk.actual = *match->meanFrequencyHz;
            } else if (c.metric == "duty_pct") {
                if (!match->dutyCyclePercent) {
                    chk.detail = "channel '" + c.target + "' has no full period (no duty_pct)";
                    results.push_back(std::move(chk));
                    continue;
                }
                chk.actual = *match->dutyCyclePercent;
            } else if (c.metric == "min_width_us" || c.metric == "max_width_us") {
                std::optional<double> minW, maxW;
                for (const auto& w : {match->highWidth, match->lowWidth}) {
                    if (!w)
                        continue;
                    const double mn = match->samplesToSeconds(static_cast<double>(w->minSamples)) * 1e6;
                    const double mx = match->samplesToSeconds(static_cast<double>(w->maxSamples)) * 1e6;
                    minW = minW ? std::min(*minW, mn) : mn;
                    maxW = maxW ? std::max(*maxW, mx) : mx;
                }
                if (!minW) {
                    chk.detail = "channel '" + c.target + "' has no complete bounded pulse";
                    results.push_back(std::move(chk));
                    continue;
                }
                chk.actual = c.metric == "min_width_us" ? *minW : *maxW;
            } else {
                chk.detail = "unknown channel metric '" + c.metric + "'";
                results.push_back(std::move(chk));
                continue;
            }
        }

        chk.passed = compare(*chk.actual, c.op, c.threshold);
        std::ostringstream ss;
        ss.imbue(std::locale::classic());
        if (chk.passed)
            ss << "ok";
        else
            ss << "actual=" << *chk.actual;
        // A point estimate can pass while the true value may still violate an upper limit.
        if (chk.passed && c.target.empty() && latencyStats && latencyStats->count > 0 &&
            (c.op == CompareOp::Le || c.op == CompareOp::Lt) && c.metric.ends_with("_ns") &&
            !c.metric.ends_with("_ub_ns") && !c.metric.ends_with("_lb_ns") && c.metric != "stdev_ns" &&
            c.metric != "jitter_ns") {
            const double ub = *chk.actual + latencyStats->quantizationStepNs;
            if (!compare(ub, c.op, c.threshold))
                ss << "; INCONCLUSIVE within +-1 sample: true value may reach " << ub << " ns, use "
                   << c.metric.substr(0, c.metric.size() - 3) << "_ub_ns for a guaranteed check";
        }
        chk.detail = ss.str();
        results.push_back(std::move(chk));
    }
    return results;
}

}  // namespace lt::cli

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/Invariants.hpp"

#include "core/ArgParsing.hpp"
#include "core/Format.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>

namespace lt {

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Splits on any of `seps` outside parentheses; nullopt on unbalanced parentheses.
std::optional<std::vector<std::string_view>> splitTopLevel(std::string_view text, std::string_view seps)
{
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(')
            ++depth;
        else if (c == ')' && --depth < 0)
            return std::nullopt;
        else if (depth == 0 && seps.find(c) != std::string_view::npos) {
            out.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (depth != 0)
        return std::nullopt;
    out.push_back(trim(text.substr(start)));
    return out;
}

bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
}

// Recursive-descent parser: expr := term ('|' term)*; term := factor ('&' factor)*;
// factor := '!' factor | '(' expr ')' | channel.
class ExprParser {
public:
    explicit ExprParser(std::string_view text) : m_text(text) {}

    std::optional<InvariantExpr> parse(std::string& error)
    {
        auto e = parseOr(error);
        skip();
        if (e && m_pos != m_text.size()) {
            error = "unexpected '" + std::string(m_text.substr(m_pos)) + "' in expression";
            return std::nullopt;
        }
        return e;
    }

private:
    void skip()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }
    bool take(char c)
    {
        skip();
        if (m_pos < m_text.size() && m_text[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }
    std::optional<InvariantExpr> parseOr(std::string& error)
    {
        auto left = parseAnd(error);
        if (!left)
            return std::nullopt;
        if (skip(), m_pos >= m_text.size() || m_text[m_pos] != '|')
            return left;
        InvariantExpr e;
        e.op = InvariantExpr::Op::Or;
        e.args.push_back(std::move(*left));
        while (take('|')) {
            auto r = parseAnd(error);
            if (!r)
                return std::nullopt;
            e.args.push_back(std::move(*r));
        }
        return e;
    }
    std::optional<InvariantExpr> parseAnd(std::string& error)
    {
        auto left = parseFactor(error);
        if (!left)
            return std::nullopt;
        if (skip(), m_pos >= m_text.size() || m_text[m_pos] != '&')
            return left;
        InvariantExpr e;
        e.op = InvariantExpr::Op::And;
        e.args.push_back(std::move(*left));
        while (take('&')) {
            auto r = parseFactor(error);
            if (!r)
                return std::nullopt;
            e.args.push_back(std::move(*r));
        }
        return e;
    }
    std::optional<InvariantExpr> parseFactor(std::string& error)
    {
        if (take('!')) {
            auto inner = parseFactor(error);
            if (!inner)
                return std::nullopt;
            InvariantExpr e;
            e.op = InvariantExpr::Op::Not;
            e.args.push_back(std::move(*inner));
            return e;
        }
        if (take('(')) {
            auto inner = parseOr(error);
            if (!inner)
                return std::nullopt;
            if (!take(')')) {
                error = "missing ')' in expression";
                return std::nullopt;
            }
            return inner;
        }
        skip();
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() && isIdentChar(m_text[m_pos]))
            ++m_pos;
        if (m_pos == start) {
            error = "expected a channel in expression '" + std::string(m_text) + "'";
            return std::nullopt;
        }
        InvariantExpr e;
        e.channel = std::string(m_text.substr(start, m_pos - start));
        return e;
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
};

std::optional<EdgeRef> parseEdgeRef(std::string_view text, std::string& error)
{
    EdgeRef ref;
    const auto colon = text.find(':');
    ref.channel = std::string(trim(text.substr(0, colon)));
    if (ref.channel.empty()) {
        error = "missing channel in '" + std::string(text) + "'";
        return std::nullopt;
    }
    if (colon != std::string_view::npos) {
        const auto edge = lower(trim(text.substr(colon + 1)));
        if (edge == "rising" || edge == "rise" || edge == "r")
            ref.edge = EdgeRef::Edge::Rising;
        else if (edge == "falling" || edge == "fall" || edge == "f")
            ref.edge = EdgeRef::Edge::Falling;
        else if (edge == "any" || edge == "both")
            ref.edge = EdgeRef::Edge::Any;
        else {
            error = "invalid edge '" + edge + "' (expected rising, falling or any)";
            return std::nullopt;
        }
    }
    return ref;
}

std::optional<double> parseDuration(std::string_view text, std::string& error)
{
    const auto t = cli::parseTimeSeconds(trim(text));
    if (!t || *t < 0) {
        error = "invalid time '" + std::string(trim(text)) + "' (use a unit: 20ns, 1.5us, 2ms)";
        return std::nullopt;
    }
    return t;
}

std::optional<InvariantRule> parseRule(std::string_view text, std::string& error)
{
    InvariantRule rule;
    rule.text = std::string(text);
    const auto open = text.find('(');
    if (open == std::string_view::npos || text.back() != ')') {
        error = "invariant '" + std::string(text) + "' must look like name(arguments)";
        return std::nullopt;
    }
    const std::string name = lower(trim(text.substr(0, open)));
    const auto parts = splitTopLevel(text.substr(open + 1, text.size() - open - 2), ",");
    if (!parts) {
        error = "unbalanced parentheses in '" + std::string(text) + "'";
        return std::nullopt;
    }
    std::vector<std::string_view> positional;
    std::vector<std::pair<std::string, std::string_view>> named;
    for (const auto p : *parts) {
        if (p.empty()) {
            error = "empty argument in '" + std::string(text) + "'";
            return std::nullopt;
        }
        const auto eq = p.find('=');
        if (eq != std::string_view::npos)
            named.emplace_back(lower(trim(p.substr(0, eq))), trim(p.substr(eq + 1)));
        else
            positional.push_back(p);
    }
    auto takeNamed = [&](const char* key) -> std::optional<std::string_view> {
        for (auto it = named.begin(); it != named.end(); ++it)
            if (it->first == key) {
                const auto v = it->second;
                named.erase(it);
                return v;
            }
        return std::nullopt;
    };
    auto durationArg = [&](const char* key, double& out) {
        if (const auto v = takeNamed(key)) {
            const auto d = parseDuration(*v, error);
            if (!d)
                return false;
            out = *d;
        }
        return true;
    };

    if (name == "never" || name == "always") {
        rule.kind = name == "never" ? InvariantRule::Kind::Never : InvariantRule::Kind::Always;
        if (positional.size() != 1) {
            error = name + "() takes one expression, e.g. never(COMMIT & REJECT)";
            return std::nullopt;
        }
        auto e = ExprParser(positional[0]).parse(error);
        if (!e || !durationArg("settle", rule.seconds))
            return std::nullopt;
        rule.expr = std::move(*e);
    } else if (name == "onehot" || name == "onehot0") {
        rule.kind = name == "onehot" ? InvariantRule::Kind::OneHot : InvariantRule::Kind::OneHot0;
        if (positional.size() < 2) {
            error = name + "() needs at least two channels";
            return std::nullopt;
        }
        for (const auto p : positional)
            rule.channels.emplace_back(p);
        if (!durationArg("settle", rule.seconds))
            return std::nullopt;
    } else if (name == "within") {
        rule.kind = InvariantRule::Kind::Within;
        if (positional.size() != 3) {
            error = "within() takes FROM[:edge], TO[:edge], TIME, e.g. within(STIM, REACT, 100ns)";
            return std::nullopt;
        }
        auto from = parseEdgeRef(positional[0], error);
        auto to = from ? parseEdgeRef(positional[1], error) : std::nullopt;
        auto t = to ? parseDuration(positional[2], error) : std::nullopt;
        if (!t)
            return std::nullopt;
        rule.from = *from;
        rule.to = *to;
        rule.seconds = *t;
    } else if (name == "stable") {
        rule.kind = InvariantRule::Kind::Stable;
        if (positional.size() < 2) {
            error = "stable() takes data lines and a clock edge, e.g. stable(D0, D1, CLK:rising, setup=5ns, hold=2ns)";
            return std::nullopt;
        }
        auto clk = parseEdgeRef(positional.back(), error);
        if (!clk)
            return std::nullopt;
        rule.from = *clk;
        for (std::size_t i = 0; i + 1 < positional.size(); ++i)
            rule.channels.emplace_back(positional[i]);
        if (!durationArg("setup", rule.setupSeconds) || !durationArg("hold", rule.holdSeconds))
            return std::nullopt;
    } else if (name == "bus") {
        rule.kind = InvariantRule::Kind::Bus;
        if (positional.size() < 2) {
            error = "bus() needs at least two data lines and settle=TIME";
            return std::nullopt;
        }
        for (const auto p : positional)
            rule.channels.emplace_back(p);
        const auto settle = takeNamed("settle");
        if (!settle) {
            error = "bus() needs settle=TIME, e.g. bus(D0, D1, D2, settle=20ns)";
            return std::nullopt;
        }
        const auto d = parseDuration(*settle, error);
        if (!d)
            return std::nullopt;
        rule.seconds = *d;
        if (!durationArg("gap", rule.gapSeconds))
            return std::nullopt;
        if (rule.gapSeconds >= 0 && rule.gapSeconds < rule.seconds) {
            error = "bus() gap must not be shorter than settle";
            return std::nullopt;
        }
    } else {
        error = "unknown invariant '" + name + "' (expected never, always, onehot, onehot0, within, stable, bus)";
        return std::nullopt;
    }
    if (!named.empty()) {
        error = "unknown argument '" + named.front().first + "' in '" + std::string(text) + "'";
        return std::nullopt;
    }
    return rule;
}

// ---------------------------------------------------------------------------------------------
// Evaluation

std::optional<std::size_t> resolveChannel(const CaptureBuffer& b, const std::string& ref)
{
    const auto& chs = b.channels();
    for (std::size_t i = 0; i < chs.size(); ++i)
        if (!chs[i].name.empty() && chs[i].name == ref)
            return i;
    std::string_view digits = ref;
    if (digits.size() > 2 && (digits.substr(0, 2) == "CH" || digits.substr(0, 2) == "ch"))
        digits.remove_prefix(2);
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const int n = std::stoi(std::string(digits));
        for (std::size_t i = 0; i < chs.size(); ++i)
            if (chs[i].displayNumber() == n)
                return i;
    }
    return std::nullopt;
}

struct Context {
    const CaptureBuffer& b;
    std::vector<std::pair<std::size_t, uint32_t>> changes;  // (sample, new word) where any bit changed
    double rate;

    [[nodiscard]] uint64_t samplesFor(double seconds) const
    {
        return static_cast<uint64_t>(std::floor(seconds * rate + 1e-9));
    }
    [[nodiscard]] std::string duration(uint64_t samples) const
    {
        return std::to_string(samples) + " samples (" + formatTime(static_cast<double>(samples) / rate) + ")";
    }
};

void addViolation(InvariantResult& r, std::size_t sample, std::string detail)
{
    ++r.violations;
    if (r.examples.size() < InvariantResult::kMaxExamples)
        r.examples.push_back({sample, std::move(detail)});
}

// Intervals in which `bad(word)` holds: a violation when one lasts longer than `settle` samples.
template <typename Bad>
void checkIntervals(const Context& c, InvariantResult& r, uint64_t settle, Bad&& bad, const std::string& what)
{
    const auto& s = c.b.samples();
    bool state = bad(s.front());
    std::size_t start = 0;
    auto close = [&](std::size_t end, bool atEnd) {
        const uint64_t len = end - start;
        ++r.checked;
        if (len > settle)
            addViolation(r, start, what + " for " + c.duration(len) + (atEnd ? " (until the end of the capture)" : ""));
    };
    for (const auto& [sample, word] : c.changes) {
        const bool now = bad(word);
        if (now && !state)
            start = sample;
        else if (!now && state)
            close(sample, false);
        state = now;
    }
    if (state)
        close(s.size(), true);
}

bool evalExpr(const InvariantExpr& e, const std::vector<std::size_t>& index, std::size_t& next, uint32_t word)
{
    switch (e.op) {
    case InvariantExpr::Op::Channel: return (word >> index[next++]) & 1u;
    case InvariantExpr::Op::Not: return !evalExpr(e.args[0], index, next, word);
    case InvariantExpr::Op::And: {
        bool v = true;
        for (const auto& a : e.args)
            v = evalExpr(a, index, next, word) && v;  // evaluate all so `next` stays aligned
        return v;
    }
    case InvariantExpr::Op::Or: {
        bool v = false;
        for (const auto& a : e.args)
            v = evalExpr(a, index, next, word) || v;
        return v;
    }
    }
    return false;
}

void collectChannels(const InvariantExpr& e, std::vector<std::string>& out)
{
    if (e.op == InvariantExpr::Op::Channel)
        out.push_back(e.channel);
    for (const auto& a : e.args)
        collectChannels(a, out);
}

bool edgeMatches(EdgeRef::Edge edge, bool rising)
{
    return edge == EdgeRef::Edge::Any || (edge == EdgeRef::Edge::Rising) == rising;
}

InvariantResult evaluate(const Context& c, const InvariantRule& rule)
{
    InvariantResult r;
    r.rule = rule.text;
    auto resolveAll = [&](const std::vector<std::string>& refs, std::vector<std::size_t>& out) {
        for (const auto& ref : refs) {
            const auto i = resolveChannel(c.b, ref);
            if (!i) {
                r.error = "channel '" + ref + "' is not in the capture";
                return false;
            }
            out.push_back(*i);
        }
        return true;
    };

    switch (rule.kind) {
    case InvariantRule::Kind::Never:
    case InvariantRule::Kind::Always: {
        std::vector<std::string> refs;
        collectChannels(rule.expr, refs);
        std::vector<std::size_t> index;
        if (!resolveAll(refs, index))
            break;
        const bool never = rule.kind == InvariantRule::Kind::Never;
        checkIntervals(c, r, c.samplesFor(rule.seconds),
                       [&](uint32_t word) {
                           std::size_t next = 0;
                           return evalExpr(rule.expr, index, next, word) == never;
                       },
                       never ? "the expression was true" : "the expression was false");
        break;
    }
    case InvariantRule::Kind::OneHot:
    case InvariantRule::Kind::OneHot0: {
        std::vector<std::size_t> index;
        if (!resolveAll(rule.channels, index))
            break;
        uint32_t mask = 0;
        for (const auto i : index)
            mask |= 1u << i;
        const bool exact = rule.kind == InvariantRule::Kind::OneHot;
        checkIntervals(c, r, c.samplesFor(rule.seconds),
                       [&](uint32_t word) {
                           const int high = __builtin_popcount(word & mask);
                           return exact ? high != 1 : high > 1;
                       },
                       exact ? "not exactly one line HIGH" : "more than one line HIGH");
        break;
    }
    case InvariantRule::Kind::Within: {
        std::vector<std::size_t> index;
        if (!resolveAll({rule.from.channel, rule.to.channel}, index))
            break;
        const uint64_t deadline = c.samplesFor(rule.seconds);
        const std::size_t end = c.b.samples().size();
        std::deque<std::size_t> pending;
        uint32_t prev = c.b.samples().front();
        auto expire = [&](std::size_t now) {
            while (!pending.empty() && now > pending.front() + deadline) {
                addViolation(r, pending.front(), rule.to.channel + " did not follow within " + c.duration(deadline));
                pending.pop_front();
            }
        };
        for (const auto& [sample, word] : c.changes) {
            expire(sample);
            const uint32_t diff = word ^ prev;
            if ((diff >> index[0]) & 1u && edgeMatches(rule.from.edge, (word >> index[0]) & 1u)) {
                ++r.checked;
                pending.push_back(sample);
            }
            if ((diff >> index[1]) & 1u && edgeMatches(rule.to.edge, (word >> index[1]) & 1u))
                pending.clear();  // one reaction answers every stimulus still waiting
            prev = word;
        }
        expire(end);
        r.censored = pending.size();  // the window after these edges reaches past the capture
        break;
    }
    case InvariantRule::Kind::Stable: {
        std::vector<std::size_t> data;
        std::vector<std::size_t> clk;
        if (!resolveAll(rule.channels, data) || !resolveAll({rule.from.channel}, clk))
            break;
        uint32_t dataMask = 0;
        for (const auto i : data)
            dataMask |= 1u << i;
        std::vector<std::pair<std::size_t, uint32_t>> dataChanges;  // (sample, changed data bits)
        std::vector<std::size_t> clockEdges;
        uint32_t prev = c.b.samples().front();
        for (const auto& [sample, word] : c.changes) {
            const uint32_t diff = word ^ prev;
            if (diff & dataMask)
                dataChanges.emplace_back(sample, diff & dataMask);
            if ((diff >> clk[0]) & 1u && edgeMatches(rule.from.edge, (word >> clk[0]) & 1u))
                clockEdges.push_back(sample);
            prev = word;
        }
        const uint64_t setup = c.samplesFor(rule.setupSeconds), hold = c.samplesFor(rule.holdSeconds);
        std::size_t k = 0;
        for (const auto edge : clockEdges) {
            ++r.checked;
            const std::size_t lo = edge > setup ? edge - setup : 0;
            while (k < dataChanges.size() && dataChanges[k].first < lo)
                ++k;
            if (k < dataChanges.size() && dataChanges[k].first <= edge + hold) {
                std::string lines;
                for (const auto i : data)
                    if ((dataChanges[k].second >> i) & 1u)
                        lines += (lines.empty() ? "" : ", ") + c.b.channels()[i].label();
                const auto offset = static_cast<int64_t>(dataChanges[k].first) - static_cast<int64_t>(edge);
                addViolation(r, edge, lines + " changed " + std::to_string(offset) + " samples from the " +
                                          rule.from.channel + " edge (window -" + std::to_string(setup) + "..+" +
                                          std::to_string(hold) + ")");
            }
        }
        break;
    }
    case InvariantRule::Kind::Bus: {
        std::vector<std::size_t> data;
        if (!resolveAll(rule.channels, data))
            break;
        uint32_t mask = 0;
        for (const auto i : data)
            mask |= 1u << i;
        const uint64_t settle = c.samplesFor(rule.seconds);
        const uint64_t gap = c.samplesFor(rule.gapSeconds >= 0 ? rule.gapSeconds : 4 * rule.seconds);
        uint32_t prev = c.b.samples().front();
        std::optional<std::size_t> first, last;
        auto closeTransaction = [&] {
            if (!first)
                return;
            ++r.checked;
            if (*last - *first > settle)
                addViolation(r, *first, "the bus kept changing for " + c.duration(*last - *first) + " (settle " +
                                            c.duration(settle) + ")");
        };
        for (const auto& [sample, word] : c.changes) {
            if ((word ^ prev) & mask) {
                if (last && sample - *last > gap) {
                    closeTransaction();
                    first.reset();
                }
                if (!first)
                    first = sample;
                last = sample;
            }
            prev = word;
        }
        closeTransaction();
        break;
    }
    }
    r.passed = r.error.empty() && r.violations == 0;
    return r;
}

}  // namespace

std::optional<std::vector<InvariantRule>> parseInvariants(std::string_view text, std::string* error)
{
    std::string err;
    const auto parts = splitTopLevel(text, ";,");
    if (!parts) {
        if (error)
            *error = "unbalanced parentheses in --invariant";
        return std::nullopt;
    }
    std::vector<InvariantRule> out;
    for (const auto p : *parts) {
        if (p.empty())
            continue;
        auto rule = parseRule(p, err);
        if (!rule) {
            if (error)
                *error = err;
            return std::nullopt;
        }
        out.push_back(std::move(*rule));
    }
    if (out.empty()) {
        if (error)
            *error = "no invariants given";
        return std::nullopt;
    }
    return out;
}

std::vector<InvariantResult> evaluateInvariants(const CaptureBuffer& buffer, const std::vector<InvariantRule>& rules)
{
    std::vector<InvariantResult> out;
    if (buffer.size() == 0 || buffer.frequency() == 0) {
        for (const auto& rule : rules)
            out.push_back({rule.text, false, "the capture has no samples", 0, 0, 0, {}});
        return out;
    }
    Context c{buffer, {}, static_cast<double>(buffer.frequency())};
    const auto& s = buffer.samples();
    for (std::size_t i = 1; i < s.size(); ++i)
        if (s[i] != s[i - 1])
            c.changes.emplace_back(i, s[i]);
    for (const auto& rule : rules)
        out.push_back(evaluate(c, rule));
    return out;
}

nlohmann::json toJson(const CaptureBuffer& buffer, const std::vector<InvariantResult>& results)
{
    auto arr = nlohmann::json::array();
    for (const auto& r : results) {
        auto examples = nlohmann::json::array();
        for (const auto& v : r.examples)
            examples.push_back({{"sample", v.sample}, {"time_us", buffer.timeOfSample(v.sample) * 1e6}, {"detail", v.detail}});
        nlohmann::json j = {{"rule", r.rule},
                            {"passed", r.passed},
                            {"checked", r.checked},
                            {"violations", r.violations},
                            {"examples", std::move(examples)}};
        if (r.censored)
            j["censored"] = r.censored;
        if (!r.error.empty())
            j["error"] = r.error;
        arr.push_back(std::move(j));
    }
    return {{"passed", allPassed(results)}, {"rules", std::move(arr)}};
}

bool allPassed(const std::vector<InvariantResult>& results)
{
    return std::all_of(results.begin(), results.end(), [](const InvariantResult& r) { return r.passed; });
}

}  // namespace lt

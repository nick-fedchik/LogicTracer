// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/SelfTest.hpp"

#include "core/Exporters.hpp"
#include "core/Paths.hpp"
#include "core/Segments.hpp"
#include "core/SignalAnalyzer.hpp"
#include "services/Operations.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>

namespace lt {

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kPreSamples = 64;

std::string fmtNs(double ns)
{
    std::ostringstream s;
    s.precision(ns < 100 ? 3 : 4);
    s << ns << " ns";
    return s.str();
}

class Runner {
public:
    Runner(IDevice& device, const SelfTestOptions& options) : m_dev(device), m_opt(options) {}

    SelfTestOutcome run()
    {
        SelfTestOutcome out;
        const auto& info = m_dev.info();
        if (!info.supportsTestSignal()) {
            out.error = "the device has no test-signal generator (needs LogicTracer firmware 0.6+ on a Pico or Pico 2, "
                        "FEATURES bit 2)";
            return out;
        }
        if (m_opt.channelA == m_opt.channelB || m_opt.channelA >= info.channels || m_opt.channelB >= info.channels) {
            out.error = "--a and --b must be two different channels of the device (1.." +
                        std::to_string(info.channels) + ")";
            return out;
        }
        m_freq = m_opt.frequency ? m_opt.frequency : info.maxFrequency;
        if (m_freq < SelfTestOptions::kMinFrequency || m_freq > info.maxFrequency) {
            out.error = "the self-test sample rate must be " + std::to_string(SelfTestOptions::kMinFrequency) +
                        ".." + std::to_string(info.maxFrequency) + " Hz";
            return out;
        }
        // Pulse widths, periods and gaps grow with the sample period so every check sees the same number of
        // samples per pulse (about 200 at the 100 MHz reference); the A -> B delays stay absolute.
        m_scale = std::max<uint32_t>(1, static_cast<uint32_t>((100'000'000ull + m_freq - 1) / m_freq));
        m_channels = {{m_opt.channelA, "A"}, {m_opt.channelB, "B"}};
        const auto started = std::chrono::steady_clock::now();

        // Generator first: without it nothing else can be judged.
        std::string error;
        const auto base = start(baseRequest(), error);
        if (!base) {
            out.error = error;
            stop();
            return out;
        }
        m_sysclk = base->sysclkHz;
        add("generator", "PASS",
            "running at " + std::to_string(m_sysclk) + " Hz system clock",
            signalJson(*base));

        if (checkWiring()) {
            checkTrigger(false);
            checkTrigger(true);
            checkPreTrigger();
            checkWaveform();
            for (const uint32_t delayNs : {0u, 20u, 50u, 100u, 1000u})
                checkLatency(delayNs);
            checkSegments();
            stop();
            checkIdle();
            checkTimeout();
        } else {
            for (const char* name : {"trigger_rising", "trigger_falling", "pretrigger", "waveform", "latency", "segments",
                                     "idle", "timeout"})
                add(name, "SKIP", "skipped: the wiring check failed");
        }
        stop();

        int passed = 0, failed = 0, skipped = 0;
        for (const auto& c : m_checks) {
            const auto st = c["status"].get<std::string>();
            (st == "PASS" ? passed : st == "FAIL" ? failed : skipped)++;
        }
        out.passed = failed == 0;
        out.doc = {
            {"schema_version", kCaptureSchemaVersion},
            {"command", "selftest"},
            {"status", out.passed ? "OK" : "FAILED"},
            {"device", toJson(info)},
            {"setup",
             {{"a", channelJson(false)},
              {"b", channelJson(true)},
              {"frequency", m_freq},
              {"sysclk_hz", m_sysclk}}},
            {"summary", {{"passed", passed}, {"failed", failed}, {"skipped", skipped}}},
            {"checks", m_checks},
            {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                  started).count()},
            {"warnings", json::array()},
        };
        return out;
    }

    ~Runner() { stop(); }

private:
    TestSignalRequest baseRequest() const
    {
        TestSignalRequest r;
        r.pulses = 20;
        r.highNs = 2000 * m_scale;
        r.delayNs = 50;
        r.periodNs = 5000 * m_scale;
        r.gapNs = 100'000 * m_scale;
        return r;
    }

    std::optional<uint8_t> gpio(bool b) const
    {
        return gpioForChannel(m_dev.info().version.board, b ? m_opt.channelB : m_opt.channelA);
    }

    json channelJson(bool b) const
    {
        json j = {{"channel", (b ? m_opt.channelB : m_opt.channelA) + 1}, {"source", b ? "GP1" : "GP0"}};
        if (const auto g = gpio(b))
            j["gpio"] = *g;
        return j;
    }

    std::optional<TestSignalInfo> start(const TestSignalRequest& req, std::string& error)
    {
        auto info = m_dev.setTestSignal(req, error);
        m_running = info.has_value();
        return info;
    }

    void stop()
    {
        if (!m_running)
            return;
        TestSignalRequest off;
        off.enable = false;
        std::string ignored;
        (void)m_dev.setTestSignal(off, ignored);
        m_running = false;
    }

    double ns(uint64_t cycles) const { return m_sysclk ? cycles * 1e9 / m_sysclk : 0.0; }
    double sampleNs() const { return 1e9 / m_freq; }
    std::string chName(bool b) const { return "CH" + std::to_string((b ? m_opt.channelB : m_opt.channelA) + 1); }
    std::string gpioName(bool b) const
    {
        const auto g = gpio(b);
        return g ? "GP" + std::to_string(*g) : chName(b);
    }

    json signalJson(const TestSignalInfo& s) const
    {
        return {{"sysclk_hz", s.sysclkHz},
                {"high_ns", ns(s.highCycles)},
                {"delay_ns", ns(s.delayCycles)},
                {"period_ns", ns(s.periodCycles)},
                {"gap_ns", ns(s.gapCycles)},
                {"pulses", s.pulses}};
    }

    // Records a check. The capture it was judged on (the last one) is saved for a failed check with --keep.
    void add(const std::string& name, const std::string& status, const std::string& message, json data = json::object())
    {
        if (status == "FAIL" && m_last && m_last->second.buffer && !m_opt.keepDir.empty()) {
            const auto path = lt::utf8FromPath(lt::pathFromUtf8(m_opt.keepDir) / lt::pathFromUtf8(name + ".json"));
            if (keep(path))
                data["capture_file"] = path;
        }
        m_last.reset();
        m_checks.push_back({{"name", name}, {"status", status}, {"message", message}, {"data", std::move(data)}});
    }

    bool keep(const std::string& path)
    {
        const auto& [config, result] = *m_last;
        CaptureRequestSpec spec;
        spec.channels = config.channels;
        spec.frequency = config.frequency;
        spec.samples = config.preSamples + config.postSamples;
        spec.preSamples = config.preSamples;
        spec.trigger = config.trigger;
        spec.timeout = config.timeout;
        spec.segments = config.segments;
        DocumentOptions options;
        options.collectEdges = true;
        auto document = captureDocument(m_dev.info(), spec, config, result, {}, options);
        writeCaptureOutputs(document.doc, result, document, OutputPaths{{}, path, {}, {}},
                            m_dev.info().version.board);
        return document.doc["outputs"].contains("json");
    }

    CaptureResult capture(uint32_t post, std::optional<EdgeTrigger> trigger, std::chrono::milliseconds timeout = 2000ms,
                          uint32_t segments = 1, uint32_t pre = kPreSamples)
    {
        CaptureConfig c;
        c.channels = m_channels;
        c.frequency = m_freq;
        c.preSamples = pre;  // immediate captures drop them (the device still needs its minimum)
        c.postSamples = post;
        c.trigger = trigger;
        c.timeout = timeout;
        c.probeOnTimeout = true;
        c.segments = segments;
        if (auto why = m_dev.validate(c)) {
            CaptureResult r;
            r.error = *why;
            return r;
        }
        m_last.emplace(c, m_dev.capture(c));
        return m_last->second;
    }

    std::string failure(const CaptureResult& r) const
    {
        return "capture " + std::string(toString(r.status)) + (r.error.empty() ? "" : ": " + r.error);
    }

    bool checkWiring()
    {
        const auto r = m_dev.probe(m_channels, 2ms, std::min<uint32_t>(m_freq, 10'000'000));
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add("wiring", "FAIL", "probe " + std::string(toString(r.status)) + ": " + r.error);
            return false;
        }
        const auto stats = SignalAnalyzer{}.analyze(*r.buffer);
        json data = json::object();
        std::string missing;
        for (std::size_t i = 0; i < 2; ++i) {
            data[i ? "b_transitions" : "a_transitions"] = stats[i].transitions;
            if (stats[i].transitions == 0) {
                const bool b = i == 1;
                missing += (missing.empty() ? "" : "; ") + chName(b) + " sees no signal: fit a jumper from " +
                           (b ? "GP1" : "GP0") + " to " + gpioName(b) + " (" + chName(b) + ")";
            }
        }
        if (!missing.empty()) {
            add("wiring", "FAIL", missing, data);
            return false;
        }
        add("wiring", "PASS", chName(false) + " and " + chName(true) + " toggle", data);
        return true;
    }

    // The edge-sensitive trigger must place the edge exactly at t = 0: the sample before is the old level.
    void checkTrigger(bool falling)
    {
        const std::string name = falling ? "trigger_falling" : "trigger_rising";
        const auto r = capture(1024, EdgeTrigger{m_opt.channelA, falling});
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add(name, "FAIL", failure(r));
            return;
        }
        const auto& buf = *r.buffer;
        const auto ti = buf.triggerIndex();
        const bool after = !falling;
        json data = {{"trigger_index", ti}, {"edge_sensitive", r.edgeSensitive}};
        // Nearest matching edge on A, as an offset from the trigger sample, for the report.
        std::optional<int64_t> nearest;
        for (std::size_t s = 1; s < buf.size(); ++s)
            if (buf.bit(s, 0) == after && buf.bit(s - 1, 0) != after) {
                const int64_t off = static_cast<int64_t>(s) - ti;
                if (!nearest || std::abs(off) < std::abs(*nearest))
                    nearest = off;
            }
        if (nearest)
            data["nearest_edge_offset_samples"] = *nearest;
        const bool ok = ti >= 1 && static_cast<std::size_t>(ti) < buf.size() && buf.bit(ti, 0) == after &&
                        buf.bit(ti - 1, 0) != after;
        if (ok)
            add(name, "PASS", "the " + std::string(falling ? "falling" : "rising") + " edge is at the trigger sample",
                data);
        else
            add(name, "FAIL",
                nearest ? "the nearest " + std::string(falling ? "falling" : "rising") + " edge is " +
                              std::to_string(*nearest) + " samples from the trigger sample"
                        : "no " + std::string(falling ? "falling" : "rising") + " edge in the capture",
                data);
    }

    // A gapless periodic signal and a pre-trigger window of three periods: the trigger (the next rising edge)
    // fires within one period, before the window fills. The samples shown must all be recorded levels (no pulse
    // shorter than the true one) and the shortened pre-trigger part must come with a warning.
    void checkPreTrigger()
    {
        auto req = baseRequest();
        req.gapNs = 0;
        std::string error;
        const auto started = start(req, error);
        if (!started) {
            add("pretrigger", "FAIL", error);
            return;
        }
        const auto& s = *started;
        const auto pre = static_cast<uint32_t>(std::ceil(3 * s.seconds(s.periodCycles) * m_freq));
        const auto r = capture(256, EdgeTrigger{m_opt.channelA, false}, 2000ms, 1, pre);
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add("pretrigger", "FAIL", failure(r));
            return;
        }
        if (!m_dev.info().supportsRecordedCount()) {
            add("pretrigger", "SKIP", "the firmware does not report recorded samples (FEATURES bit 3)");
            return;
        }
        const auto a = SignalAnalyzer{}.analyzeChannel(*r.buffer, 0);
        const auto kept = r.buffer->preSamples();
        const bool warned = std::any_of(r.warnings.begin(), r.warnings.end(),
                                        [](const std::string& w) { return w.find("pre-trigger window") != std::string::npos; });
        json data = {{"requested_pre_samples", pre}, {"pre_samples", kept}, {"partial_warning", warned}};
        if (a.highWidth)
            data["high_samples"] = {a.highWidth->minSamples, a.highWidth->maxSamples};
        if (a.highWidth && !admits(a.highWidth->minSamples, a.highWidth->maxSamples, ns(s.highCycles), sampleNs()))
            add("pretrigger", "FAIL",
                "a pre-trigger pulse of " + std::to_string(a.highWidth->minSamples) +
                    " samples was never generated (unrecorded samples shown as levels)",
                data);
        else if (!warned || kept >= pre)
            add("pretrigger", "FAIL", "the trigger came within one period, yet the pre-trigger part is reported complete",
                data);
        else
            add("pretrigger", "PASS",
                std::to_string(kept) + " of " + std::to_string(pre) + " pre-trigger samples recorded" +
                    (warned ? ", the rest left out with a warning" : ""),
                data);
    }

    // Measured d samples confine a true width W to ((d-1)T, (d+1)T); all pulses are identical, so the
    // widest and the narrowest measurement must both admit it.
    static bool admits(uint64_t minSamples, uint64_t maxSamples, double trueNs, double tNs)
    {
        const double lo = maxSamples > 0 ? (maxSamples - 1.0) * tNs : 0.0;
        const double hi = (minSamples + 1.0) * tNs;
        return lo <= trueNs + 1e-6 && trueNs <= hi + 1e-6;
    }

    uint32_t burstSamples(const TestSignalInfo& s) const
    {
        const double seconds = s.seconds(s.periodCycles * static_cast<uint64_t>(s.pulses) + s.gapCycles);
        return static_cast<uint32_t>(std::ceil(seconds * m_freq)) + 16;
    }

    void checkWaveform()
    {
        std::string error;
        const auto started = start(baseRequest(), error);  // the checks before may have changed the signal
        if (!started) {
            add("waveform", "FAIL", error);
            return;
        }
        const auto& s = *started;
        // Two bursts and their gaps: a complete burst lies between two gaps wherever the trigger falls.
        const auto r = capture(2 * burstSamples(s), EdgeTrigger{m_opt.channelA, false});
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add("waveform", "FAIL", failure(r));
            return;
        }
        const auto a = SignalAnalyzer{}.analyzeChannel(*r.buffer, 0);
        const auto b = SignalAnalyzer{}.analyzeChannel(*r.buffer, 1);
        const double t = sampleNs();
        std::vector<std::string> problems;
        json data = {{"expected_high_ns", ns(s.highCycles)}, {"expected_period_ns", ns(s.periodCycles)},
                     {"sample_ns", t}};
        for (const auto* st : {&a, &b}) {
            const std::string ch = st == &a ? "a" : "b";
            if (!st->highWidth || !st->period) {
                problems.push_back(chName(st == &b) + ": no complete pulses");
                continue;
            }
            data[ch + "_high_samples"] = {st->highWidth->minSamples, st->highWidth->maxSamples};
            data[ch + "_period_samples"] = {st->period->minSamples, st->period->maxSamples};
            data[ch + "_bursts"] = st->period->burstCount;
            if (!admits(st->highWidth->minSamples, st->highWidth->maxSamples, ns(s.highCycles), t))
                problems.push_back(chName(st == &b) + " high width " + std::to_string(st->highWidth->minSamples) +
                                   ".." + std::to_string(st->highWidth->maxSamples) + " samples does not admit " +
                                   fmtNs(ns(s.highCycles)));
            if (!admits(st->period->minSamples, st->period->maxSamples, ns(s.periodCycles), t))
                problems.push_back(chName(st == &b) + " period " + std::to_string(st->period->minSamples) + ".." +
                                   std::to_string(st->period->maxSamples) + " samples does not admit " +
                                   fmtNs(ns(s.periodCycles)));
            if (st->glitchCount)
                problems.push_back(chName(st == &b) + ": " + std::to_string(st->glitchCount) + " glitches");
        }
        // Count the pulses of the complete burst on A.
        uint64_t burstPulses = 0;
        if (a.period && a.period->burstCount >= 2) {
            const auto& buf = *r.buffer;
            const auto gapSamples = a.period->burstGapSamples;
            std::vector<std::size_t> rises;
            for (std::size_t i = 1; i < buf.size(); ++i)
                if (buf.bit(i, 0) && !buf.bit(i - 1, 0))
                    rises.push_back(i);
            std::vector<std::size_t> starts;  // first rise of each burst (after a gap)
            for (std::size_t k = 1; k < rises.size(); ++k)
                if (rises[k] - rises[k - 1] > gapSamples)
                    starts.push_back(k);
            if (starts.size() >= 2)
                burstPulses = starts[1] - starts[0];
        }
        if (!burstPulses) {
            problems.push_back("no complete burst between two gaps on " + chName(false));
        } else {
            data["burst_pulses"] = burstPulses;
            if (burstPulses != s.pulses)
                problems.push_back("a burst has " + std::to_string(burstPulses) + " pulses, expected " +
                                   std::to_string(s.pulses));
        }
        if (problems.empty())
            add("waveform", "PASS",
                "high " + fmtNs(ns(s.highCycles)) + " and period " + fmtNs(ns(s.periodCycles)) +
                    " within one sample on both channels",
                data);
        else {
            std::string msg;
            for (const auto& p : problems)
                msg += (msg.empty() ? "" : "; ") + p;
            add("waveform", "FAIL", msg, data);
        }
    }

    void checkLatency(uint32_t delayNs)
    {
        const std::string name = "latency_" + std::to_string(delayNs) + "ns";
        auto req = baseRequest();
        req.delayNs = delayNs;
        std::string error;
        const auto s = start(req, error);
        if (!s) {
            add(name, "SKIP", "the generator cannot make this delay at this system clock: " + error);
            return;
        }
        const auto r = capture(burstSamples(*s), EdgeTrigger{m_opt.channelA, false});
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add(name, "FAIL", failure(r));
            return;
        }
        const double trueNs = ns(s->delayCycles);
        json data = {{"true_delay_ns", trueNs}, {"sample_ns", sampleNs()}};
        std::vector<std::string> problems;
        for (const bool falling : {false, true}) {
            LatencyPairSpec spec;
            spec.stimChannelIndex = 0;
            spec.reactChannelIndex = 1;
            spec.stimFalling = spec.reactFalling = falling;
            spec.maxDeltaSeconds = s->seconds(s->periodCycles) / 2;
            const auto lat = SignalAnalyzer::analyzeLatency(*r.buffer, spec);
            const std::string edge = falling ? "falling" : "rising";
            data[edge] = {{"pairs", lat.count},
                          {"drops", lat.unmatchedStimCount},
                          {"spurious", lat.spuriousReactCount},
                          {"min_samples", lat.minSamples},
                          {"max_samples", lat.maxSamples},
                          {"bound_ns", {lat.constantDelayLowerNs(), lat.constantDelayUpperNs()}}};
            if (lat.count < s->pulses / 2)
                problems.push_back(edge + ": only " + std::to_string(lat.count) + " pairs");
            if (lat.unmatchedStimCount || lat.spuriousReactCount)
                problems.push_back(edge + ": " + std::to_string(lat.unmatchedStimCount) + " drops, " +
                                   std::to_string(lat.spuriousReactCount) + " spurious");
            if (lat.count && !admits(lat.minSamples, lat.maxSamples, trueNs, sampleNs()))
                problems.push_back(edge + ": " + std::to_string(lat.minSamples) + ".." +
                                   std::to_string(lat.maxSamples) + " samples does not admit the true " +
                                   fmtNs(trueNs));
        }
        if (problems.empty())
            add(name, "PASS", "every pair admits the true delay " + fmtNs(trueNs) + " on both edges", data);
        else {
            std::string msg;
            for (const auto& p : problems)
                msg += (msg.empty() ? "" : "; ") + p;
            add(name, "FAIL", msg, data);
        }
    }

    // Firmware burst timestamps: the trigger-to-trigger spacing is one period inside a burst and
    // period + gap across bursts; the trigger itself is detected at the sample clock, so allow T + resolution.
    void checkSegments()
    {
        if (!m_dev.info().supportsEdgeBurstTimestamps()) {
            add("segments", "SKIP", "the firmware does not record trigger times (FEATURES bit 1)");
            return;
        }
        TestSignalRequest req = baseRequest();
        req.pulses = 8;
        req.gapNs = 50'000;
        std::string error;
        const auto s = start(req, error);
        if (!s) {
            add("segments", "FAIL", error);
            return;
        }
        constexpr uint32_t kWindows = 20;
        const uint32_t post = std::max<uint32_t>(16, static_cast<uint32_t>(s->seconds(s->highCycles) * m_freq / 2));
        const auto r = capture(post, EdgeTrigger{m_opt.channelA, false}, 2000ms, kWindows, 8);
        if (r.status != CaptureStatus::Ok || !r.segments) {
            add("segments", "FAIL", failure(r));
            return;
        }
        // Window 1: the trigger edge must be at t = 0 like in a single-window capture.
        const auto windows = splitSegments(*r.buffer, *r.segments);
        if (windows.empty()) {
            add("segments", "FAIL", "the capture does not match its segment layout");
            return;
        }
        const auto& w1 = windows.front();
        const auto t1 = w1.triggerIndex();
        if (t1 < 1 || static_cast<std::size_t>(t1) >= w1.size() || !w1.bit(t1, 0) || w1.bit(t1 - 1, 0)) {
            add("segments", "FAIL", "window 1: the rising edge is not at the trigger sample",
                {{"window1_trigger_index", t1}});
            return;
        }
        const auto& times = r.segments->triggerTimesSeconds;
        if (times.size() != kWindows) {
            add("segments", "FAIL", "the capture has " + std::to_string(times.size()) + " trigger times, expected " +
                                        std::to_string(kWindows));
            return;
        }
        const double period = s->seconds(s->periodCycles);
        const double across = s->seconds(s->periodCycles + s->gapCycles);
        const double tol = 1.0 / m_freq + r.segments->triggerTimeResolutionSeconds;
        uint32_t inBurst = 0, acrossBursts = 0;
        double worst = 0;
        std::vector<std::string> bad;
        for (std::size_t k = 1; k < times.size(); ++k) {
            const double d = times[k] - times[k - 1];
            const double ePeriod = std::abs(d - period), eAcross = std::abs(d - across);
            if (ePeriod <= tol) {
                ++inBurst;
                worst = std::max(worst, ePeriod);
            } else if (eAcross <= tol) {
                ++acrossBursts;
                worst = std::max(worst, eAcross);
            } else if (bad.size() < 4) {
                bad.push_back(fmtNs(d * 1e9) + " before window " + std::to_string(k + 1));
            }
        }
        json data = {{"windows", kWindows},
                     {"in_burst", inBurst},
                     {"across_bursts", acrossBursts},
                     {"expected_ns", {period * 1e9, across * 1e9}},
                     {"tolerance_ns", tol * 1e9},
                     {"worst_error_ns", worst * 1e9}};
        const uint32_t expectedAcross = (kWindows - 1) / s->pulses;  // at least this many burst boundaries
        if (!bad.empty()) {
            std::string msg = "unexpected trigger spacing:";
            for (const auto& b : bad)
                msg += " " + b;
            add("segments", "FAIL", msg, data);
        } else if (acrossBursts < expectedAcross || acrossBursts > expectedAcross + 1) {
            add("segments", "FAIL",
                std::to_string(acrossBursts) + " burst boundaries in " + std::to_string(kWindows) + " windows", data);
        } else {
            add("segments", "PASS",
                "window 1 edge at t = 0; every trigger spacing is a period or a burst gap within " + fmtNs(tol * 1e9) +
                    " (worst " +
                    fmtNs(worst * 1e9) + ")",
                data);
        }
    }

    // With the generator stopped, GP0/GP1 are inputs again and the pull-downs keep both channels low.
    void checkIdle()
    {
        const auto r = capture(4096, std::nullopt);
        if (r.status != CaptureStatus::Ok || !r.buffer) {
            add("idle", "FAIL", failure(r));
            return;
        }
        const auto stats = SignalAnalyzer{}.analyze(*r.buffer);
        const bool ok = stats.size() == 2 && stats[0].transitions == 0 && stats[1].transitions == 0 &&
                        stats[0].idleLevel == 0 && stats[1].idleLevel == 0;
        json data = {{"a_transitions", stats[0].transitions}, {"b_transitions", stats[1].transitions},
                     {"a_level", stats[0].idleLevel}, {"b_level", stats[1].idleLevel}};
        add("idle", ok ? "PASS" : "FAIL",
            ok ? "both channels stay low after the generator stops" : "the lines are not idle low after the stop",
            data);
    }

    // No edge arrives: the capture must end as TIMEOUT, close to the requested time, with the line state.
    void checkTimeout()
    {
        constexpr auto kTimeout = 300ms;
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = capture(1024, EdgeTrigger{m_opt.channelA, false}, kTimeout);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        // Upper bound: the firmware reports on its poll tick and the host then reads the line state.
        constexpr auto kSlack = 400ms;
        json data = {{"timeout_ms", kTimeout.count()}, {"elapsed_ms", elapsed.count()},
                     {"status", std::string(toString(r.status))}, {"line_state", r.lineDump.has_value()}};
        if (r.status != CaptureStatus::Timeout)
            add("timeout", "FAIL", "expected TIMEOUT, got " + failure(r), data);
        else if (elapsed < kTimeout || elapsed > kTimeout + kSlack)
            add("timeout", "FAIL",
                "TIMEOUT after " + std::to_string(elapsed.count()) + " ms, expected " +
                    std::to_string(kTimeout.count()) + ".." + std::to_string((kTimeout + kSlack).count()) + " ms",
                data);
        else if (!r.lineDump)
            add("timeout", "FAIL", "TIMEOUT without the current line state", data);
        else
            add("timeout", "PASS", "TIMEOUT after " + std::to_string(elapsed.count()) + " ms with the line state",
                data);
    }

    IDevice& m_dev;
    SelfTestOptions m_opt;
    std::vector<ChannelInfo> m_channels;
    uint32_t m_freq = 0;
    uint32_t m_sysclk = 0;
    uint32_t m_scale = 1;
    bool m_running = false;
    std::optional<std::pair<CaptureConfig, CaptureResult>> m_last;
    json m_checks = json::array();
};

}  // namespace

SelfTestOutcome runSelfTest(IDevice& device, const SelfTestOptions& options)
{
    return Runner(device, options).run();
}

}  // namespace lt

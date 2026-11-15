// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/Operations.hpp"

#include "core/ArgParsing.hpp"
#include "core/Exporters.hpp"
#include "services/Session.hpp"

#include <algorithm>
#include <cstdio>

namespace lt {

using json = nlohmann::json;

namespace {

std::string hex4(uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04x", v);
    return buf;
}

json errorDocument(const char* command, CaptureStatus status, const std::string& message, const DeviceInfo& info,
                   const std::vector<std::string>& warnings)
{
    return {{"command", command},
            {"status", std::string(toString(status))},
            {"error", message},
            {"port", info.port},
            {"device", toJson(info)},
            {"warnings", warnings}};
}

}  // namespace

namespace {

// Number or string with SI suffix -> integer quantity.
std::optional<uint64_t> quantityOf(const json& v)
{
    if (v.is_number_unsigned())
        return v.get<uint64_t>();
    if (v.is_number_integer())
        return v.get<int64_t>() >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(v.get<int64_t>())) : std::nullopt;
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return d >= 0 && d == static_cast<double>(static_cast<uint64_t>(d)) ? std::optional<uint64_t>(static_cast<uint64_t>(d))
                                                                            : std::nullopt;
    }
    if (v.is_string())
        return cli::parseQuantity(v.get<std::string>());
    return std::nullopt;
}

// "1,2:NAME", "1-8" or [1, 2, "3:NAME"] -> text accepted by parseChannelList.
std::optional<std::string> channelText(const json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (!v.is_array())
        return std::nullopt;
    std::string out;
    for (const auto& e : v) {
        if (!out.empty())
            out += ',';
        if (e.is_string())
            out += e.get<std::string>();
        else if (e.is_number_integer())
            out += std::to_string(e.get<int64_t>());
        else
            return std::nullopt;
    }
    return out;
}

}  // namespace

json channelBrief(const ChannelInfo& ch, std::string_view board)
{
    json j = {{"channel", ch.displayNumber()}, {"name", ch.label()}};
    if (!board.empty()) {
        if (auto gp = gpioForChannel(board, ch.number))
            j["gpio"] = *gp;
    }
    return j;
}

json diagnosisJson(const ChannelStats& st, std::string_view board)
{
    const auto d = SignalAnalyzer::diagnose(st);
    json j = {{"channel", st.channel.displayNumber()},
              {"name", st.channel.label()},
              {"state", std::string(toString(d.state))},
              {"level", st.transitions == 0 ? json(st.idleLevel) : json(nullptr)},
              {"transitions", st.transitions},
              {"high_time_percent", st.highTimePercent},
              {"mean_frequency_hz", st.meanFrequencyHz ? json(*st.meanFrequencyHz) : json(nullptr)},
              {"glitches", st.glitchCount},
              {"diagnosis", std::string(d.code)},
              {"message", std::string(d.message)}};
    if (!board.empty()) {
        if (auto gp = gpioForChannel(board, st.channel.number))
            j["gpio"] = *gp;
    }
    return j;
}

json detectDocument(bool includeAllAcm, bool handshake, std::chrono::milliseconds timeout,
                    const QString& sessionSocketPath)
{
    std::string sessionHeldPort;
    json sessionDeviceJson;
    if (handshake && !sessionSocketPath.isEmpty()) {
        const auto state = session::call(sessionSocketPath, "get_state", json::object(), 300);
        if (state.ok && state.result.value("connected", false) && state.result.contains("device") &&
            state.result["device"].is_object()) {
            sessionDeviceJson = state.result["device"];
            sessionHeldPort = sessionDeviceJson.value("port", std::string{});
        }
    }

    auto devices = json::array();
    int found = 0;
    for (const auto& port : PicoConnection::enumerate(includeAllAcm)) {
        json d = {{"port", port.port},
                  {"usb_id", hex4(port.vendorId) + ":" + hex4(port.productId)},
                  {"vid_pid_match", port.vidPidMatch},
                  {"serial_number", port.serialNumber},
                  {"manufacturer", port.manufacturer},
                  {"description", port.description}};
        if (handshake) {
            if (!sessionHeldPort.empty() && port.port == sessionHeldPort) {
                d["responding"] = true;
                d["held_by_session"] = true;
                d["device"] = sessionDeviceJson;
                ++found;
            } else {
                PicoConnection conn;
                if (conn.open(port.port, timeout)) {
                    d["responding"] = true;
                    d["device"] = toJson(conn.info());
                    ++found;
                } else {
                    d["responding"] = false;
                    d["error"] = conn.lastError();
                }
            }
        } else if (port.vidPidMatch) {
            ++found;
        }
        devices.push_back(std::move(d));
    }
    return {{"schema_version", kCaptureSchemaVersion},
            {"command", "detect"},
            {"status", found > 0 ? "OK" : "NOT_FOUND"},
            {"count", found},
            {"devices", devices}};
}

bool openDevice(PicoConnection& conn, const std::string& port, std::chrono::milliseconds timeout, std::string& error)
{
    std::string target = port;
    if (target.empty()) {
        const auto ports = PicoConnection::enumerate(false);
        if (ports.empty()) {
            error = "no capture device found (USB 1209:3020); connect one or pass a port";
            return false;
        }
        target = ports.front().port;
    }
    if (!conn.open(target, timeout)) {
        error = conn.lastError();
        return false;
    }
    return true;
}

std::optional<CaptureConfig> buildCaptureConfig(const DeviceInfo& info, const CaptureRequestSpec& spec,
                                                std::vector<std::string>& warnings, std::string& error)
{
    std::vector<uint8_t> numbers;
    for (const auto& ch : spec.channels)
        numbers.push_back(ch.number);
    const auto mode = captureModeFor(numbers);
    const auto limits = info.limits(mode);

    CaptureConfig cfg;
    cfg.channels = spec.channels;
    cfg.frequency = spec.frequency;
    cfg.timeout = spec.timeout;
    cfg.probeOnTimeout = spec.probeOnTimeout;

    if (!spec.trigger) {
        if (spec.preSamples)
            warnings.push_back("--pre-samples ignored for an immediate (untriggered) capture");
        if (spec.samples > limits.maxPostSamples) {
            error = "immediate capture supports at most " + std::to_string(limits.maxPostSamples) + " samples in " +
                    std::string(toString(mode)) + " mode (the firmware keeps " + std::to_string(limits.minPreSamples) +
                    " pre-trigger samples)";
            return std::nullopt;
        }
        cfg.preSamples = limits.minPreSamples;
        cfg.postSamples = static_cast<uint32_t>(spec.samples);
        return cfg;
    }

    cfg.trigger = spec.trigger;
    const uint64_t pre = spec.preSamples
                             ? *spec.preSamples
                             : std::clamp<uint64_t>(spec.samples / 10, limits.minPreSamples,
                                                    std::max(limits.minPreSamples, limits.maxPreSamples));
    if (pre >= spec.samples) {
        error = "pre-trigger samples (" + std::to_string(pre) + ") must be fewer than the total (" +
                std::to_string(spec.samples) + ")";
        return std::nullopt;
    }
    cfg.preSamples = static_cast<uint32_t>(pre);
    cfg.postSamples = static_cast<uint32_t>(spec.samples - pre);
    return cfg;
}

CaptureDocument captureDocument(const DeviceInfo& info, const CaptureRequestSpec& spec, const CaptureConfig& cfg,
                                const CaptureResult& result, const std::vector<std::string>& warnings,
                                const DocumentOptions& options)
{
    std::vector<std::string> allWarnings = warnings;
    std::vector<uint8_t> numbers;
    auto channels = json::array();
    for (const auto& ch : cfg.channels) {
        numbers.push_back(ch.number);
        channels.push_back(channelBrief(ch, info.version.board));
    }
    json trigger = cfg.trigger ? json{{"type", "edge"},
                                      {"channel", cfg.trigger->channel + 1},
                                      {"edge", cfg.trigger->inverted ? "falling" : "rising"}}
                               : json{{"type", "immediate"}};
    if (cfg.trigger && !info.version.board.empty()) {
        if (auto gp = gpioForChannel(info.version.board, cfg.trigger->channel))
            trigger["gpio"] = *gp;
    }

    // Warn when a level-sensitive PIO trigger fired immediately because the line was already active.
    if (cfg.trigger && result.status == CaptureStatus::Ok && result.buffer && result.buffer->size() > 0) {
        for (std::size_t idx = 0; idx < cfg.channels.size(); ++idx) {
            if (cfg.channels[idx].number == cfg.trigger->channel) {
                const bool expectedIdle = cfg.trigger->inverted;  // falling needs HIGH idle; rising needs LOW idle
                const std::size_t checkEnd = std::min<std::size_t>(cfg.preSamples, result.buffer->size() - 1);
                bool sawOppositeIdle = false;
                for (std::size_t s = 0; s <= checkEnd; ++s) {
                    if (result.buffer->bit(s, idx) == expectedIdle) {
                        sawOppositeIdle = true;
                        break;
                    }
                }
                if (!sawOppositeIdle) {
                    allWarnings.push_back(
                        "trigger_already_active: CH" + std::to_string(cfg.trigger->channel + 1) + " was already " +
                        (cfg.trigger->inverted ? "LOW" : "HIGH") +
                        " when capture started (level-sensitive PIO trigger fired without an edge; check pull-up or "
                        "arm before applying stimulus)");
                }
                break;
            }
        }
    }

    CaptureDocument out;
    out.doc = {
        {"schema_version", kCaptureSchemaVersion},
        {"command", "capture"},
        {"status", std::string(toString(result.status))},
        {"port", info.port},
        {"device", toJson(info)},
        {"config",
         {{"frequency_hz", cfg.frequency},
          {"samples", spec.samples},
          {"pre_trigger_samples", cfg.trigger ? cfg.preSamples : 0},
          {"post_trigger_samples", cfg.trigger ? cfg.postSamples : static_cast<uint32_t>(spec.samples)},
          {"mode", std::string(toString(captureModeFor(numbers)))},
          {"channels", channels},
          {"trigger", trigger},
          {"timeout_ms", cfg.timeout.count()},
          {"firmware_poll_latency_ms", defaultPollLatencyFor(info.version).count()}}},
        {"attempts", result.attempts},
        {"elapsed_ms", result.elapsed.count()},
        {"warnings", allWarnings},
        {"outputs", json::object()},
    };
    if (!result.error.empty())
        out.doc["error"] = result.error;
    if (!options.setup.empty())
        out.doc["setup"] = toJson(options.setup);

    if (result.buffer) {
        SignalAnalyzer::Options opts;
        opts.collectEdges = options.collectEdges || options.summaryEdges;
        out.stats = SignalAnalyzer(opts).analyze(*result.buffer);
        if (options.summary || options.summaryEdges)
            out.doc["capture"] = captureToJson(*result.buffer, *out.stats, options.summaryEdges, info.version.board);

        if (options.latency) {
            std::optional<std::size_t> stimIdx, reactIdx;
            for (std::size_t i = 0; i < result.buffer->channels().size(); ++i) {
                if (result.buffer->channels()[i].number == options.latency->stimChannel)
                    stimIdx = i;
                if (result.buffer->channels()[i].number == options.latency->reactChannel)
                    reactIdx = i;
            }
            if (stimIdx && reactIdx) {
                LatencyPairSpec pair{*stimIdx, options.latency->stimFalling, *reactIdx,
                                     options.latency->reactFalling, std::nullopt};
                out.latency = SignalAnalyzer::analyzeLatency(*result.buffer, pair, options.collectLatencyEvents);
                out.doc["latency"] = toJson(*out.latency, false);
            }
        }

        if (options.expectations) {
            const auto checks = cli::evaluateExpectations(*options.expectations, *out.stats, out.latency);
            bool allOk = !checks.empty();
            auto arr = json::array();
            for (const auto& c : checks) {
                allOk = allOk && c.passed;
                arr.push_back({{"expression", c.expression},
                               {"passed", c.passed},
                               {"actual", c.actual ? json(*c.actual) : json(nullptr)},
                               {"detail", c.detail}});
            }
            out.expectationsPassed = allOk;
            out.doc["assertions"] = {{"passed", allOk}, {"checks", std::move(arr)}};
        }
    }
    if (result.lineDump) {
        auto dump = json::array();
        for (const auto& st : SignalAnalyzer{}.analyze(*result.lineDump))
            dump.push_back(diagnosisJson(st, info.version.board));
        out.doc["line_dump"] = {{"sample_rate_hz", result.lineDump->frequency()},
                                {"samples", result.lineDump->size()},
                                {"channels", dump}};
    }
    return out;
}

void writeCaptureOutputs(json& doc, const CaptureResult& result, const CaptureDocument& document,
                         const OutputPaths& paths, std::string_view board)
{
    json outputs = json::object();
    std::string error;
    if (!paths.vcdPath.empty() && result.buffer) {
        if (writeVcdFile(paths.vcdPath, *result.buffer, &error))
            outputs["vcd"] = paths.vcdPath;
        else
            doc["warnings"].push_back(error);
    }
    if (!paths.csvPath.empty() && result.buffer) {
        if (writeCsvFile(paths.csvPath, *result.buffer, &error))
            outputs["csv"] = paths.csvPath;
        else
            doc["warnings"].push_back(error);
    }
    if (!paths.eventsCsvPath.empty() && document.latency) {
        if (writeEventsCsvFile(paths.eventsCsvPath, document.latency->events, &error))
            outputs["events_csv"] = paths.eventsCsvPath;
        else
            doc["warnings"].push_back(error);
    }
    if (!paths.jsonPath.empty()) {
        outputs["json"] = paths.jsonPath;
        json file = doc;
        file["outputs"] = outputs;
        if (document.stats && result.buffer)
            file["capture"] = captureToJson(*result.buffer, *document.stats, true, board);
        if (!writeJsonFile(paths.jsonPath, file, &error)) {
            outputs.erase("json");
            doc["warnings"].push_back(error);
        }
    }
    doc["outputs"] = outputs;
}

void writeCaptureOutputs(json& doc, const CaptureResult& result, const std::optional<std::vector<ChannelStats>>& stats,
                         const std::string& vcdPath, const std::string& jsonPath)
{
    CaptureDocument d;
    d.stats = stats;
    writeCaptureOutputs(doc, result, d, OutputPaths{vcdPath, jsonPath, {}, {}});
}

CaptureOutcome performCapture(PicoConnection& conn, const CaptureRequestSpec& spec, const DocumentOptions& options,
                              std::vector<std::string> warnings)
{
    CaptureOutcome out;
    const auto& info = conn.info();
    std::string error;
    auto cfg = buildCaptureConfig(info, spec, warnings, error);
    if (!cfg) {
        out.usageError = true;
        out.result.status = CaptureStatus::Error;
        out.result.error = error;
        out.document.doc = errorDocument("capture", CaptureStatus::Error, error, info, warnings);
        return out;
    }
    if (auto err = conn.validate(*cfg)) {
        out.usageError = true;
        out.result.status = CaptureStatus::Error;
        out.result.error = *err;
        out.document.doc = errorDocument("capture", CaptureStatus::Error, *err, info, warnings);
        return out;
    }
    out.result = conn.capture(*cfg);
    out.document = captureDocument(info, spec, *cfg, out.result, warnings, options);
    return out;
}

ProbeDocument probeDocument(const DeviceInfo& info, const ProbeResult& r, uint32_t requestedRate,
                            std::chrono::milliseconds requestedWindow)
{
    ProbeDocument out;
    if (r.status != CaptureStatus::Ok || !r.buffer) {
        out.doc = {{"command", "probe"},
                   {"status", std::string(toString(r.status))},
                   {"error", r.error},
                   {"port", info.port},
                   {"elapsed_ms", r.elapsed.count()}};
        return out;
    }

    out.stats = SignalAnalyzer{}.analyze(*r.buffer);
    json chans = json::array();
    for (const auto& st : *out.stats)
        chans.push_back(diagnosisJson(st, info.version.board));

    // probe() clamps rate/window to what the device can do; tell the caller instead of hiding it.
    json warnings = json::array();
    const double actualWindowUs = static_cast<double>(r.buffer->size()) * r.buffer->samplePeriodSeconds() * 1e6;
    if (r.buffer->frequency() != requestedRate)
        warnings.push_back("--rate adjusted to " + std::to_string(r.buffer->frequency()) + " Hz (device limits " +
                           std::to_string(info.minFrequency()) + ".." + std::to_string(info.maxFrequency) + " Hz)");
    if (actualWindowUs < static_cast<double>(requestedWindow.count()) * 1000.0 * 0.999)
        warnings.push_back("--window shortened to " + std::to_string(actualWindowUs / 1000.0) +
                           " ms (device buffer limit)");

    out.doc = {
        {"schema_version", kCaptureSchemaVersion},
        {"command", "probe"},
        {"status", "OK"},
        {"port", info.port},
        {"device", toJson(info)},
        {"sample_rate_hz", r.buffer->frequency()},
        {"samples", r.buffer->size()},
        {"window_us", actualWindowUs},
        {"auto_trigger",
         {{"channel", r.triggerUsed ? r.triggerUsed->channel + 1 : 0},
          {"level", r.triggerUsed && r.triggerUsed->inverted ? "low" : "high"},
          {"attempts", r.attempts}}},
        {"elapsed_ms", r.elapsed.count()},
        {"warnings", warnings},
        {"channels", chans},
        {"notes",
         {"The firmware cannot configure pull resistors; RP2040 inputs keep their default weak "
          "pull-down, so a floating (High-Z) line usually reads LOW.",
          defaultPollLatencyFor(info.version).count() < 500
              ? "LogicTracer firmware: ~10 ms completion polling (~80 ms probe)."
              : "Upstream LogicAnalyzer firmware: ~2 s polling cycles; probe takes 2-8 s."}},
    };
    return out;
}

ProbeDocument performProbe(PicoConnection& conn, const std::vector<ChannelInfo>& channels,
                           std::chrono::milliseconds window, uint32_t rate)
{
    for (const auto& ch : channels) {
        if (ch.number >= conn.info().channels) {
            ProbeDocument d;
            d.doc = {{"command", "probe"},
                     {"status", "ERROR"},
                     {"error", "channel " + std::to_string(ch.displayNumber()) + " out of range 1.." +
                                   std::to_string(conn.info().channels)},
                     {"port", conn.info().port},
                     {"device", toJson(conn.info())},
                     {"usage_error", true}};
            return d;
        }
    }
    const auto r = conn.probe(channels, window, rate);
    return probeDocument(conn.info(), r, rate, window);
}

std::optional<CaptureRequestSpec> captureSpecFromJson(const json& p, int deviceChannels, std::string& error)
{
    CaptureRequestSpec spec;
    const int maxCh = std::min(32, deviceChannels);

    const auto chText = p.contains("channels") ? channelText(p["channels"]) : std::optional<std::string>("1");
    if (!chText) {
        error = "channels must be a string like \"1,2:NAME\" or an array";
        return std::nullopt;
    }
    auto channels = cli::parseChannelList(*chText, maxCh, &error);
    if (!channels)
        return std::nullopt;
    spec.channels = std::move(*channels);

    const char* rateKey = p.contains("freq") ? "freq" : p.contains("frequency") ? "frequency" : "rate";
    if (p.contains(rateKey)) {
        const auto f = quantityOf(p[rateKey]);
        if (!f || *f == 0 || *f > UINT32_MAX) {
            error = "invalid sample rate";
            return std::nullopt;
        }
        spec.frequency = static_cast<uint32_t>(*f);
    }
    if (p.contains("samples")) {
        const auto n = quantityOf(p["samples"]);
        if (!n || *n < 4 || *n > UINT32_MAX) {
            error = "invalid samples (expected 4..4294967295)";
            return std::nullopt;
        }
        spec.samples = *n;
    }
    if (p.contains("pre_samples") && !p["pre_samples"].is_null()) {
        const auto n = quantityOf(p["pre_samples"]);
        if (!n || *n >= spec.samples) {
            error = "invalid pre_samples (must be fewer than samples)";
            return std::nullopt;
        }
        spec.preSamples = *n;
    }
    if (p.contains("trigger") && !p["trigger"].is_null()) {
        if (!p["trigger"].is_string()) {
            error = "trigger must be a string like \"24:falling\" or \"none\"";
            return std::nullopt;
        }
        const auto t = cli::parseTrigger(p["trigger"].get<std::string>(), maxCh + 1, &error);
        if (!t)
            return std::nullopt;
        if (!t->none)
            spec.trigger = t->trigger;
    }
    if (p.contains("timeout_ms")) {
        const auto t = quantityOf(p["timeout_ms"]);
        if (!t || *t > 3'600'000) {
            error = "invalid timeout_ms (0..3600000)";
            return std::nullopt;
        }
        spec.timeout = std::chrono::milliseconds(*t);
    }
    if (p.contains("probe_on_timeout") && p["probe_on_timeout"].is_boolean())
        spec.probeOnTimeout = p["probe_on_timeout"].get<bool>();
    return spec;
}

DocumentOptions documentOptionsFromJson(const json& p, bool defaultSummary)
{
    DocumentOptions o;
    o.summary = p.contains("summary") && p["summary"].is_boolean() ? p["summary"].get<bool>() : defaultSummary;
    o.summaryEdges = p.contains("edges") && p["edges"].is_boolean() && p["edges"].get<bool>();
    return o;
}

std::optional<DocumentOptions> documentOptionsFromJson(const json& p, bool defaultSummary, int deviceChannels,
                                                       const std::vector<ChannelInfo>& capturedChannels,
                                                       std::string& error)
{
    DocumentOptions o = documentOptionsFromJson(p, defaultSummary);
    const int maxCh = std::min(32, deviceChannels);
    if (p.contains("latency") && !p["latency"].is_null()) {
        if (!p["latency"].is_string()) {
            error = "latency must be a string like \"1->2\" or \"24:falling->2:rising\"";
            return std::nullopt;
        }
        auto lat = cli::parseLatencyArg(p["latency"].get<std::string>(), maxCh, &error);
        if (!lat)
            return std::nullopt;
        auto inCapture = [&](uint8_t hwCh) {
            return std::any_of(capturedChannels.begin(), capturedChannels.end(),
                               [&](const ChannelInfo& c) { return c.number == hwCh; });
        };
        if (!inCapture(lat->stimChannel) || !inCapture(lat->reactChannel)) {
            error = "both latency channels must be included in 'channels'";
            return std::nullopt;
        }
        o.latency = *lat;
    }
    if (p.contains("meta") && !p["meta"].is_null()) {
        if (!p["meta"].is_object()) {
            error = "meta must be an object of strings, e.g. {\"dut.board\": \"PYNQ-Z1\"}";
            return std::nullopt;
        }
        for (auto it = p["meta"].begin(); it != p["meta"].end(); ++it) {
            if (!it.value().is_string()) {
                error = "meta value for '" + it.key() + "' must be a string";
                return std::nullopt;
            }
            if (!cli::isValidSetupEntry(it.key(), it.value().get_ref<const std::string&>(), &error))
                return std::nullopt;
            cli::setSetupEntry(o.setup, it.key(), it.value().get<std::string>());
        }
    }
    if (p.contains("expect") && !p["expect"].is_null()) {
        if (!p["expect"].is_string()) {
            error = "expect must be a string like \"p99_ns<=60,drops==0\"";
            return std::nullopt;
        }
        auto exp = cli::parseExpectations(p["expect"].get<std::string>(), &error);
        if (!exp)
            return std::nullopt;
        o.expectations = std::move(*exp);
    }
    return o;
}

std::optional<ProbeParams> probeParamsFromJson(const json& p, int deviceChannels, std::string& error)
{
    ProbeParams out;
    if (p.contains("channels") && !p["channels"].is_null()) {
        const auto text = channelText(p["channels"]);
        if (!text) {
            error = "channels must be a string or an array";
            return std::nullopt;
        }
        auto channels = cli::parseChannelList(*text, std::min(32, deviceChannels), &error);
        if (!channels)
            return std::nullopt;
        out.channels = std::move(*channels);
    }
    if (p.contains("window_ms")) {
        const auto w = quantityOf(p["window_ms"]);
        if (!w || *w == 0 || *w > 10'000) {
            error = "invalid window_ms (1..10000)";
            return std::nullopt;
        }
        out.window = std::chrono::milliseconds(*w);
    }
    if (p.contains("rate")) {
        const auto r = quantityOf(p["rate"]);
        if (!r || *r == 0 || *r > UINT32_MAX) {
            error = "invalid rate";
            return std::nullopt;
        }
        out.rate = static_cast<uint32_t>(*r);
    }
    return out;
}

}  // namespace lt

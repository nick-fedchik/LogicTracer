// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

// logictracer-cli: headless front-end for RP2040/RP2350 capture devices running the
// LogicAnalyzer firmware (gusmanb protocol).
//
// Exit codes:
//   0 = OK, 1 = error / nothing found, 2 = TIMEOUT, 3 = --expect assertion failed, 64 = usage error.

#include "core/AiReport.hpp"
#include "core/ArgParsing.hpp"
#include "services/Clipboard.hpp"
#include "core/Exporters.hpp"
#include "core/Format.hpp"
#include "services/Operations.hpp"
#include "device/PicoConnection.hpp"
#include "services/Session.hpp"
#include "core/SignalAnalyzer.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QProcess>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

enum ExitCode : int { ExitOk = 0, ExitError = 1, ExitTimeout = 2, ExitExpectFailed = 3, ExitUsage = 64 };

// Upper bound for syntax checks before the device reports its real channel count.
constexpr int kMaxProtocolChannels = 32;

struct Output {
    std::string command;
    bool json = false;
};

std::string str(const QString& s) { return s.toStdString(); }

int report(const Output& o, ExitCode code, const std::string& message, json extra = json::object())
{
    if (o.json) {
        json j = {{"command", o.command}, {"status", code == ExitTimeout ? "TIMEOUT" : "ERROR"}, {"error", message}};
        j.update(extra);
        std::cout << j.dump(2) << std::endl;
    } else {
        std::cerr << "logictracer-cli " << o.command << ": " << message << std::endl;
    }
    return code;
}

// Parses sub-command arguments; returns an exit code if the program must stop.
std::optional<int> parse(QCommandLineParser& p, const QStringList& args, const Output& o)
{
    p.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    const auto helpOption = p.addHelpOption();
    if (!p.parse(args))
        return report(o, ExitUsage, str(p.errorText()));
    if (p.isSet(helpOption)) {
        std::cout << str(p.helpText());
        return ExitOk;
    }
    if (!p.positionalArguments().isEmpty())
        return report(o, ExitUsage, "unexpected argument '" + str(p.positionalArguments().first()) + "'");
    return std::nullopt;
}

std::optional<std::chrono::milliseconds> parseMs(const QString& text)
{
    bool ok = false;
    const auto v = text.toLongLong(&ok);
    if (!ok || v < 0)
        return std::nullopt;
    return std::chrono::milliseconds(v);
}

// Opens --port (serial port or replay:<file>), or the first device with the capture-device VID/PID.
std::unique_ptr<lt::IDevice> openDevice(const QCommandLineParser& p, std::chrono::milliseconds timeout,
                                        std::string& error)
{
    return lt::openDevice(p.isSet("port") ? str(p.value("port")) : std::string(), timeout, error);
}

void printDiagnosisTable(const std::vector<lt::ChannelStats>& stats)
{
    std::cout << std::left << std::setw(5) << "CH" << std::setw(14) << "NAME" << std::setw(10) << "STATE"
              << std::setw(8) << "EDGES" << std::setw(8) << "HIGH%" << std::setw(14) << "FREQ"
              << "DIAGNOSIS\n";
    for (const auto& st : stats) {
        const auto d = lt::SignalAnalyzer::diagnose(st);
        std::ostringstream high;
        high << std::fixed << std::setprecision(1) << st.highTimePercent;
        std::cout << std::left << std::setw(5) << st.channel.displayNumber() << std::setw(14) << st.channel.label()
                  << std::setw(10) << lt::toString(d.state) << std::setw(8) << st.transitions << std::setw(8)
                  << high.str() << std::setw(14) << (st.meanFrequencyHz ? lt::formatHz(*st.meanFrequencyHz) : "-")
                  << d.code << '\n';
    }
}

void printSummaryTable(const std::vector<lt::ChannelStats>& stats)
{
    std::cout << std::left << std::setw(5) << "CH" << std::setw(14) << "NAME" << std::setw(6) << "IDLE"
              << std::setw(8) << "EDGES" << std::setw(8) << "PULSES" << std::setw(14) << "FREQ" << std::setw(8)
              << "DUTY%" << std::setw(12) << "MIN PW" << std::setw(12) << "MAX PW"
              << "GLITCH\n";
    for (const auto& st : stats) {
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
        else
            duty << "-";
        std::cout << std::left << std::setw(5) << st.channel.displayNumber() << std::setw(14) << st.channel.label()
                  << std::setw(6) << st.idleLevel << std::setw(8) << st.transitions << std::setw(8)
                  << (st.pulseCountHigh + st.pulseCountLow) << std::setw(14)
                  << (st.meanFrequencyHz ? lt::formatHz(*st.meanFrequencyHz) : "-") << std::setw(8) << duty.str()
                  << std::setw(12) << (minW ? lt::formatTime(*minW) : "-") << std::setw(12) << (maxW ? lt::formatTime(*maxW) : "-")
                  << st.glitchCount << '\n';
    }
}

void printLatency(const lt::LatencyStats& lat)
{
    std::cout << "Latency " << lat.stimChannel.label() << " -> " << lat.reactChannel.label() << ": N=" << lat.count
              << ", drops=" << lat.unmatchedStimCount << ", spurious=" << lat.spuriousReactCount
              << ", censored=" << (lat.censoredStimCount + lat.censoredReactCount);
    if (lat.count > 0) {
        std::cout << std::fixed << std::setprecision(2) << ", min=" << lat.minNs << " ns, mean=" << lat.meanNs
                  << " ns, p50=" << lat.p50Ns << " ns, p99=" << lat.p99Ns << " ns (true <= "
                  << lat.upperBoundNs(lat.p99Samples) << "), max=" << lat.maxNs << " ns (true <= "
                  << lat.upperBoundNs(lat.maxSamples) << "), stdev=" << lat.stdevNs << " ns\n  histogram (samples x count):";
        for (const auto& [d, c] : lat.histogram)
            std::cout << ' ' << d << 'x' << c;
        if (lat.constantDelayConsistent())
            std::cout << " | consistent with one fixed delay in (" << lat.constantDelayLowerNs() << ", "
                      << lat.constantDelayUpperNs() << ") ns";
        else
            std::cout << " | true delay varies (spread > 1 sample)";
        std::cout << std::defaultfloat;
    }
    std::cout << '\n';
}

// Failed checks, and passing checks whose quantization bound could still violate the limit.
void printAssertionIssues(const json& assertions)
{
    for (const auto& chk : assertions["checks"]) {
        const std::string detail = chk.value("detail", std::string{});
        if (!chk.value("passed", false))
            std::cerr << "  FAIL: " << chk.value("expression", std::string{}) << " (" << detail << ")\n";
        else if (detail.find("INCONCLUSIVE") != std::string::npos)
            std::cerr << "  WARN: " << chk.value("expression", std::string{}) << " (" << detail << ")\n";
    }
}

QString socketPathFrom(const QCommandLineParser& p)
{
    return p.isSet("socket") ? p.value("socket") : lt::session::defaultSocketPath();
}

// True when a running GUI session is connected to the requested port (or any device when port is empty).
bool sessionHoldsTargetPort(const QString& socket, const std::string& requestedPort)
{
    if (socket.isEmpty())
        return false;
    const auto r = lt::session::call(socket, "get_state", json::object(), 300);
    if (!r.ok || !r.result.value("connected", false))
        return false;
    if (requestedPort.empty())
        return true;
    return r.result.contains("device") && r.result["device"].is_object() &&
           r.result["device"].value("port", std::string{}) == requestedPort;
}

std::optional<lt::CopyMode> parseAiModeOption(const QCommandLineParser& p, lt::CopyMode fallback, std::string& err)
{
    if (!p.isSet("ai-mode"))
        return fallback;
    const auto m = lt::parseCopyMode(str(p.value("ai-mode")));
    if (!m)
        err = "invalid --ai-mode '" + str(p.value("ai-mode")) + "' (expected brief, cursors, or full)";
    return m;
}

bool copyAndConfirm(std::string_view text, std::string& err)
{
    std::string backend;
    if (!lt::copyTextToClipboard(text, &backend, &err))
        return false;
    const std::size_t approxTokens = (text.size() + 3) / 4;
    std::cerr << "[copied " << text.size() << " chars (~" << approxTokens << " tokens) to clipboard via " << backend
              << "]\n";
    return true;
}

// Collects repeated --meta key=value options (later keys override earlier ones).
std::optional<lt::SetupInfo> parseMetaOptions(const QCommandLineParser& p, lt::SetupInfo base, std::string& error)
{
    for (const auto& text : p.values("meta")) {
        auto entry = lt::cli::parseSetupArg(str(text), &error);
        if (!entry) {
            error = "--meta: " + error;
            return std::nullopt;
        }
        lt::cli::setSetupEntry(base, std::move(entry->first), std::move(entry->second));
    }
    return base;
}

std::string buildBufferAiReport(const lt::CaptureBuffer& buf, lt::CopyMode mode, std::string title,
                                std::string triggerDesc, std::string deviceVersion, std::string board,
                                std::string userNote, const std::optional<lt::cli::LatencyArg>& latencyArg,
                                const std::vector<lt::Annotation>& annotations = {}, const json* assertions = nullptr,
                                const lt::SetupInfo& setup = {})
{
    lt::ReportOptions ro;
    ro.mode = mode;
    ro.title = std::move(title);
    ro.triggerDescription = std::move(triggerDesc);
    ro.deviceVersion = std::move(deviceVersion);
    ro.board = std::move(board);
    ro.userNote = std::move(userNote);
    ro.annotations = annotations;
    ro.setup = setup;
    if (latencyArg) {
        std::optional<std::size_t> sIdx, rIdx;
        for (std::size_t i = 0; i < buf.channels().size(); ++i) {
            if (buf.channels()[i].number == latencyArg->stimChannel)
                sIdx = i;
            if (buf.channels()[i].number == latencyArg->reactChannel)
                rIdx = i;
        }
        if (sIdx && rIdx)
            ro.latency = lt::LatencyPairSpec{*sIdx, latencyArg->stimFalling, *rIdx, latencyArg->reactFalling, {}};
    }

    std::string out = lt::copyForAi(buf, ro);
    if (assertions && assertions->is_object() && assertions->contains("checks")) {
        std::ostringstream ss;
        ss << "\n### Assertions: " << (assertions->value("passed", false) ? "PASS" : "FAIL") << "\n\n";
        for (const auto& c : (*assertions)["checks"]) {
            ss << "- [" << (c.value("passed", false) ? "PASS" : "FAIL") << "] `"
               << c.value("expression", std::string{}) << "` (" << c.value("detail", std::string{}) << ")\n";
        }
        out += ss.str();
    }
    return out;
}

std::string buildTimeoutAiReport(const json& doc, const lt::CaptureBuffer* lineDump, std::string_view userNote)
{
    std::ostringstream ss;
    ss << "## LogicTracer capture TIMEOUT\n\n";
    if (!userNote.empty())
        ss << "> **Context / Question:** " << userNote << "\n\n";
    if (doc.contains("device") && doc["device"].is_object())
        ss << "- Device: " << doc["device"].value("version", std::string{}) << " on `"
           << doc.value("port", std::string{"?"}) << "`\n";
    ss << "- Error: **" << doc.value("error", std::string{"Timed out waiting for trigger"}) << "** ("
       << doc.value("elapsed_ms", 0ll) << " ms)\n";
    if (lineDump && lineDump->size() > 0) {
        const std::string board =
            doc.contains("device") && doc["device"].is_object() ? doc["device"].value("board", std::string{}) : "";
        ss << "\nCurrent input state (post-timeout probe):\n\n"
              "| Ch | Pin | Name | State | High % | Edges | Diagnosis |\n"
              "|---|---|---|---|---|---|---|\n";
        for (const auto& st : lt::SignalAnalyzer{}.analyze(*lineDump)) {
            const auto d = lt::SignalAnalyzer::diagnose(st);
            std::string pin = "-";
            if (!board.empty()) {
                if (const auto gp = lt::gpioForChannel(board, st.channel.number))
                    pin = "GP" + std::to_string(*gp);
            }
            std::ostringstream hp;
            hp << std::fixed << std::setprecision(1) << st.highTimePercent << "%";
            ss << "| " << st.channel.displayNumber() << " | " << pin << " | " << st.channel.label() << " | **"
               << lt::toString(d.state) << "** | " << hp.str() << " | " << st.transitions << " | " << d.message
               << " |\n";
        }
    }
    return ss.str();
}

// ---------------------------------------------------------------------------------------------
int cmdDetect(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription("List connected capture devices (USB 1209:3020) and query their limits.");
    p.addOptions({
        {"json", "Machine-readable JSON output."},
        {"all", "Also list (and handshake with) every /dev/ttyACM* port, not only 1209:3020 devices."},
        {"no-handshake", "Do not open the ports; report USB descriptors only."},
        {"timeout", "Handshake timeout per device in ms (default 3000).", "ms", "3000"},
        {"socket", "Session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"},
        {"no-session", "Do not query a running GUI session for ports it holds."},
    });
    if (auto rc = parse(p, args, o))
        return *rc;
    const auto timeout = parseMs(p.value("timeout"));
    if (!timeout)
        return report(o, ExitUsage, "invalid --timeout");

    const QString sock = p.isSet("no-session") ? QString{} : socketPathFrom(p);
    const json doc = lt::detectDocument(p.isSet("all"), !p.isSet("no-handshake"), *timeout, sock);
    const bool found = doc["count"].get<int>() > 0;

    if (o.json) {
        std::cout << doc.dump(2) << std::endl;
    } else if (doc["devices"].empty()) {
        std::cout << "No capture devices found.\n";
    } else {
        for (const auto& d : doc["devices"]) {
            std::cout << d["port"].get<std::string>() << "  [" << d["usb_id"].get<std::string>() << "]  SN "
                      << d["serial_number"].get<std::string>();
            if (d.contains("device")) {
                const auto& dev = d["device"];
                std::cout << "  " << dev["version"].get<std::string>() << "  " << dev["channels"].get<int>()
                          << " ch, max " << lt::formatHz(dev["max_frequency_hz"].get<double>()) << ", buffer "
                          << dev["buffer_bytes"].get<uint32_t>() / 1024 << " KiB";
            } else if (d.contains("error")) {
                std::cout << "  (not responding: " << d["error"].get<std::string>() << ")";
            }
            std::cout << '\n';
        }
    }
    return found ? ExitOk : ExitError;
}

// ---------------------------------------------------------------------------------------------
int cmdProbe(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription(
        "Snapshot the current logic level of the inputs without a trigger (~10 ms window)\n"
        "and classify each line: HIGH (pull-up OK), LOW (stuck-at-0 / floating), TOGGLING, NOISY.");
    p.addOptions({
        {"json", "Machine-readable JSON output."},
        {"ai", "Compact Markdown report ready to paste into an AI chat."},
        {"note", "Context or question prepended to the AI report.", "text"},
        {"copy", "Copy the AI report (or JSON when --json is set) to the system clipboard."},
        {"port", "Serial port (default: first detected device).", "path"},
        {"channels", "1-based channels, e.g. 1,2,24 or 1-8 or 1:SDA,2:SCL (default: all).", "list"},
        {"window", "Observation window in ms (default 10).", "ms", "10"},
        {"rate", "Sample rate, e.g. 1M, 100k (default 1M).", "Hz", "1M"},
        {"timeout", "Connection/handshake timeout in ms (default 3000).", "ms", "3000"},
        {"socket", "Session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"},
        {"no-session", "Do not proxy through a running GUI session holding the port."},
    });
    if (auto rc = parse(p, args, o))
        return *rc;

    const auto window = parseMs(p.value("window"));
    const auto rate = lt::cli::parseQuantity(str(p.value("rate")));
    const auto timeout = parseMs(p.value("timeout"));
    if (!window || window->count() == 0 || window->count() > 10'000)
        return report(o, ExitUsage, "invalid --window (1..10000 ms)");
    if (!rate || *rate == 0 || *rate > UINT32_MAX)
        return report(o, ExitUsage, "invalid --rate");
    if (!timeout)
        return report(o, ExitUsage, "invalid --timeout");

    std::optional<std::vector<lt::ChannelInfo>> channels;
    std::string error;
    if (p.isSet("channels")) {
        channels = lt::cli::parseChannelList(str(p.value("channels")), kMaxProtocolChannels, &error);
        if (!channels)
            return report(o, ExitUsage, error);
    }

    const QString sock = p.isSet("no-session") ? QString{} : socketPathFrom(p);
    const std::string reqPort = p.isSet("port") ? str(p.value("port")) : std::string{};
    if (sessionHoldsTargetPort(sock, reqPort)) {
        json params = {{"window_ms", window->count()}, {"rate", *rate}};
        if (p.isSet("channels"))
            params["channels"] = str(p.value("channels"));
        const auto sr = lt::session::call(sock, "probe", params, 60'000);
        if (!sr.ok)
            return report(o, sr.errorCode == lt::session::error::InvalidParams ? ExitUsage : ExitError, sr.error);
        const json& doc = sr.result;
        const std::string status = doc.value("status", std::string("ERROR"));
        const std::string aiText = (p.isSet("ai") || (p.isSet("copy") && !o.json))
                                       ? lt::probeForAi(doc, str(p.value("note")))
                                       : std::string{};
        if (o.json) {
            const std::string dumped = doc.dump(2);
            std::cout << dumped << std::endl;
            if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
                return report(o, ExitError, error);
        } else if (p.isSet("ai")) {
            std::cout << aiText;
            if (p.isSet("copy") && !copyAndConfirm(aiText, error))
                return report(o, ExitError, error);
        } else if (status == "OK") {
            std::cout << "Probe (via GUI session) " << doc["port"].get<std::string>() << ": "
                      << doc["samples"].get<std::size_t>() << " samples @ "
                      << lt::formatHz(doc["sample_rate_hz"].get<double>()) << " in "
                      << doc["elapsed_ms"].get<long long>() << " ms\n";
            for (const auto& c : doc["channels"])
                std::cout << "  CH" << c["channel"].get<int>() << " (" << c["name"].get<std::string>()
                          << "): " << c["state"].get<std::string>() << " - " << c["diagnosis"].get<std::string>()
                          << '\n';
            if (p.isSet("copy") && !copyAndConfirm(aiText, error))
                return report(o, ExitError, error);
        } else {
            std::cerr << "logictracer-cli probe: " << doc.value("error", std::string("failed")) << std::endl;
        }
        return status == "OK" ? ExitOk : status == "TIMEOUT" ? ExitTimeout : ExitError;
    }

    auto device = openDevice(p, *timeout, error);
    if (!device)
        return report(o, ExitError, error);
    lt::IDevice& conn = *device;
    if (!channels) {
        channels.emplace();
        for (uint8_t i = 0; i < conn.info().channels; ++i)
            channels->push_back({i, {}});
    }

    const auto probe = lt::performProbe(conn, *channels, *window, static_cast<uint32_t>(*rate));
    const json& doc = probe.doc;
    const std::string status = doc["status"].get<std::string>();
    if (status != "OK") {
        const ExitCode code = doc.value("usage_error", false) ? ExitUsage : status == "TIMEOUT" ? ExitTimeout : ExitError;
        if (o.json) {
            std::cout << doc.dump(2) << std::endl;
        } else {
            std::cerr << "logictracer-cli probe: " << doc["error"].get<std::string>() << std::endl;
        }
        return code;
    }

    const std::string aiText =
        (p.isSet("ai") || (p.isSet("copy") && !o.json)) ? lt::probeForAi(doc, str(p.value("note"))) : std::string{};
    if (o.json) {
        const std::string dumped = doc.dump(2);
        std::cout << dumped << std::endl;
        if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
            return report(o, ExitError, error);
    } else if (p.isSet("ai")) {
        std::cout << aiText;
        for (const auto& w : doc["warnings"])
            std::cerr << "warning: " << w.get<std::string>() << '\n';
        if (p.isSet("copy") && !copyAndConfirm(aiText, error))
            return report(o, ExitError, error);
    } else {
        std::cout << "Probe " << conn.info().port << " (" << conn.info().version.raw << "): "
                  << doc["samples"].get<std::size_t>() << " samples @ "
                  << lt::formatHz(doc["sample_rate_hz"].get<double>()) << " in " << doc["elapsed_ms"].get<long long>()
                  << " ms\n";
        for (const auto& w : doc["warnings"])
            std::cerr << "warning: " << w.get<std::string>() << '\n';
        printDiagnosisTable(*probe.stats);
        if (p.isSet("copy") && !copyAndConfirm(aiText, error))
            return report(o, ExitError, error);
    }
    return ExitOk;
}

// ---------------------------------------------------------------------------------------------
int cmdCapture(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription(
        "Run a capture and report per-channel analytics (edges, pulses, frequency, duty cycle, glitches).\n"
        "Example: capture --port /dev/ttyACM0 --freq 1M --samples 100k --channels 1:STIM,2:REACT,24:DO1\n"
        "         --trigger 24:falling --timeout 5000 --out-json cap.json --out-vcd cap.vcd --summary --json");
    p.addOptions({
        {"json", "Machine-readable JSON output on stdout."},
        {"ai", "Print an AI-ready Markdown + ASCII report instead of the plain text summary."},
        {"ai-mode", "AI report detail: brief (default, chat-ready), cursors, or full.", "mode", "brief"},
        {"note", "Context or question prepended to the AI report.", "text"},
        {"meta", "Record test-setup metadata with the result, repeatable: --meta dut.board=PYNQ-Z1.", "key=value"},
        {"copy", "Copy the AI report (or JSON when --json is set) to the system clipboard."},
        {"summary", "Include per-channel analytics in the output."},
        {"edges", "Include every edge timestamp in the stdout JSON (always included in --out-json)."},
        {"port", "Serial port (default: first detected device).", "path"},
        {"freq", "Sample rate, e.g. 1M, 250k, 100000 (default 1M).", "Hz", "1M"},
        {"samples", "Total samples (pre + post trigger), e.g. 10k (default 10k).", "n", "10k"},
        {"pre-samples", "Samples kept before the trigger (default: 10% of --samples).", "n"},
        {"channels", "1-based channels with optional names, e.g. 1:STIM,2:REACT,24:DO1 or 1-8 (default 1).",
         "list", "1"},
        {"trigger", "Trigger CH:rising|falling (level-sensitive in firmware) or 'none' for an immediate capture.",
         "spec", "none"},
        {"pull-up", "Channels that should have pull-ups (not supported by the firmware; reported as warning).",
         "list"},
        {"timeout", "Max time to wait for the trigger in ms (default 5000).", "ms", "5000"},
        {"no-probe", "On TIMEOUT do not run the extra probe that reports the current line state."},
        {"connect-timeout", "Connection/handshake timeout in ms (default 3000).", "ms", "3000"},
        {"ready-file", "Atomically write 'ARMED\\n' to this file once armed and pre-buffer is filled.", "file"},
        {"on-armed", "Launch a shell command asynchronously once armed and waiting for trigger.", "cmd"},
        {"measure-latency", "Cross-channel latency STIM[:edge]->REACT[:edge], e.g. 1->2 or 24:falling->2:rising.", "spec"},
        {"expect", "Pass/fail assertions (exit 3 on failure), e.g. 'p99_ns<=60,drops==0,DO1:low_pulses>=1'.", "expr"},
        {"out-json", "Write lossless capture JSON v1 (with edge_samples) to a file.", "file"},
        {"out-vcd", "Write the waveform to a VCD file (GTKWave / PulseView).", "file"},
        {"out-csv", "Write sample-by-sample CSV table (SampleIndex,TimeRelativeNs,...).", "file"},
        {"out-events-csv", "Write paired STIM->REACT latency events CSV (requires --measure-latency).", "file"},
        {"socket", "Session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"},
        {"no-session", "Do not proxy through a running GUI session holding the port."},
    });
    if (auto rc = parse(p, args, o))
        return *rc;

    std::string error;
    const auto aiMode = parseAiModeOption(p, lt::CopyMode::Brief, error);
    if (!aiMode)
        return report(o, ExitUsage, error);

    const auto freq = lt::cli::parseQuantity(str(p.value("freq")));
    const auto samples = lt::cli::parseQuantity(str(p.value("samples")));
    const auto timeout = parseMs(p.value("timeout"));
    const auto connectTimeout = parseMs(p.value("connect-timeout"));
    if (!freq || *freq == 0 || *freq > UINT32_MAX)
        return report(o, ExitUsage, "invalid --freq '" + str(p.value("freq")) + "'");
    if (!samples || *samples < 4 || *samples > UINT32_MAX)
        return report(o, ExitUsage, "invalid --samples '" + str(p.value("samples")) + "'");
    if (!timeout || !connectTimeout)
        return report(o, ExitUsage, "invalid timeout value");
    std::optional<uint64_t> preSamples;
    if (p.isSet("pre-samples")) {
        preSamples = lt::cli::parseQuantity(str(p.value("pre-samples")));
        if (!preSamples || *preSamples >= *samples)
            return report(o, ExitUsage, "invalid --pre-samples (must be < --samples)");
    }
    const auto channels = lt::cli::parseChannelList(str(p.value("channels")), kMaxProtocolChannels, &error);
    if (!channels)
        return report(o, ExitUsage, error);
    const auto trigger = lt::cli::parseTrigger(str(p.value("trigger")), kMaxProtocolChannels + 1, &error);
    if (!trigger)
        return report(o, ExitUsage, error);

    std::optional<lt::cli::LatencyArg> latencyArg;
    if (p.isSet("measure-latency")) {
        latencyArg = lt::cli::parseLatencyArg(str(p.value("measure-latency")), kMaxProtocolChannels, &error);
        if (!latencyArg)
            return report(o, ExitUsage, error);
        auto inCapture = [&](uint8_t hwCh) {
            return std::any_of(channels->begin(), channels->end(),
                               [&](const lt::ChannelInfo& c) { return c.number == hwCh; });
        };
        if (!inCapture(latencyArg->stimChannel) || !inCapture(latencyArg->reactChannel))
            return report(o, ExitUsage, "--measure-latency channels must be included in --channels");
    }
    if (p.isSet("out-events-csv") && !latencyArg)
        return report(o, ExitUsage, "--out-events-csv requires --measure-latency");

    std::optional<std::vector<lt::cli::ExpectationClause>> expectations;
    if (p.isSet("expect")) {
        expectations = lt::cli::parseExpectations(str(p.value("expect")), &error);
        if (!expectations)
            return report(o, ExitUsage, error);
    }
    const auto setup = parseMetaOptions(p, {}, error);
    if (!setup)
        return report(o, ExitUsage, error);

    std::vector<std::string> warnings;
    if (p.isSet("pull-up")) {
        const auto pulls = lt::cli::parseChannelList(str(p.value("pull-up")), kMaxProtocolChannels, &error);
        if (!pulls)
            return report(o, ExitUsage, "--pull-up: " + error);
        std::string list;
        for (const auto& ch : *pulls)
            list += (list.empty() ? "" : ",") + std::to_string(ch.displayNumber());
        warnings.push_back("--pull-up " + list +
                           " ignored: the firmware protocol cannot configure pull resistors (RP2040 inputs keep "
                           "their default pull-down). Fit an external pull-up resistor.");
    }

    const bool hasArmedHook = p.isSet("ready-file") || p.isSet("on-armed");
    const QString sock = p.isSet("no-session") ? QString{} : socketPathFrom(p);
    const std::string reqPort = p.isSet("port") ? str(p.value("port")) : std::string{};
    if (!hasArmedHook && sessionHoldsTargetPort(sock, reqPort)) {
        json params = {
            {"channels", str(p.value("channels"))},
            {"freq", *freq},
            {"samples", *samples},
            {"trigger", str(p.value("trigger"))},
            {"timeout_ms", timeout->count()},
            {"probe_on_timeout", !p.isSet("no-probe")},
            {"summary", p.isSet("summary")},
            {"edges", p.isSet("edges")},
        };
        if (preSamples) params["pre_samples"] = *preSamples;
        if (p.isSet("measure-latency")) params["latency"] = str(p.value("measure-latency"));
        if (p.isSet("expect")) params["expect"] = str(p.value("expect"));
        if (!setup->empty()) params["meta"] = lt::toJson(*setup);
        if (p.isSet("out-vcd")) params["out_vcd"] = str(p.value("out-vcd"));
        if (p.isSet("out-json")) params["out_json"] = str(p.value("out-json"));
        if (p.isSet("out-csv")) params["out_csv"] = str(p.value("out-csv"));
        if (p.isSet("out-events-csv")) params["out_events_csv"] = str(p.value("out-events-csv"));

        const int64_t capMs = static_cast<int64_t>((*samples * 1000ull) / std::max<uint64_t>(1, *freq));
        const int ipcWait = static_cast<int>(std::clamp<int64_t>(timeout->count() + capMs + 20'000, 120'000, 3'700'000));
        const auto sr = lt::session::call(sock, "capture", params, ipcWait);
        if (!sr.ok)
            return report(o, sr.errorCode == lt::session::error::InvalidParams ? ExitUsage : ExitError, sr.error);
        json doc = sr.result;
        for (const auto& w : warnings)
            doc["warnings"].push_back(w);
        const std::string st = doc.value("status", std::string("ERROR"));
        const bool expectOk = !doc.contains("assertions") || doc["assertions"].value("passed", true);
        const int rc = st == "OK" ? (expectOk ? ExitOk : ExitExpectFailed) : st == "TIMEOUT" ? ExitTimeout : ExitError;

        std::string sessionAi;
        if ((p.isSet("ai") || (p.isSet("copy") && !o.json)) && st == "OK") {
            json repParams = {{"mode", std::string(lt::toString(*aiMode))}};
            if (p.isSet("note"))
                repParams["note"] = str(p.value("note"));
            if (p.isSet("measure-latency"))
                repParams["latency"] = str(p.value("measure-latency"));
            const auto rep = lt::session::call(sock, "get_report", repParams, 5000);
            if (rep.ok && rep.result.contains("report"))
                sessionAi = rep.result["report"].get<std::string>();
        }

        if (o.json) {
            const std::string dumped = doc.dump(2);
            std::cout << dumped << std::endl;
            if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
                return rc == ExitOk ? ExitError : rc;
        } else if (p.isSet("ai") && !sessionAi.empty()) {
            std::cout << sessionAi;
            if (p.isSet("copy") && !copyAndConfirm(sessionAi, error))
                return rc == ExitOk ? ExitError : rc;
        } else {
            std::cout << "Capture " << st << " (via GUI session), " << doc.value("elapsed_ms", 0) << " ms\n";
            if (p.isSet("copy") && !sessionAi.empty() && !copyAndConfirm(sessionAi, error))
                return rc == ExitOk ? ExitError : rc;
        }
        return rc;
    }

    auto device = openDevice(p, *connectTimeout, error);
    if (!device)
        return report(o, ExitError, error);
    lt::IDevice& conn = *device;

    bool armedHookFired = false;
    if (hasArmedHook) {
        const std::string readyFile = str(p.value("ready-file"));
        const QString onArmedCmd = p.value("on-armed");
        conn.setProgressCallback([&, readyFile, onArmedCmd](lt::CaptureStage stage) {
            if (stage != lt::CaptureStage::WaitingForTrigger || armedHookFired)
                return;
            armedHookFired = true;
            if (!readyFile.empty()) {
                const std::string tmp = readyFile + ".tmp";
                {
                    std::ofstream rf(tmp, std::ios::trunc);
                    rf << "ARMED\n";
                }
                std::error_code ec;
                std::filesystem::rename(tmp, readyFile, ec);
                if (ec) {
                    std::ofstream rf(readyFile, std::ios::trunc);
                    rf << "ARMED\n";
                }
            }
            if (!onArmedCmd.isEmpty())
                QProcess::startDetached("/bin/sh", {"-c", onArmedCmd});
        });
    }

    lt::CaptureRequestSpec spec;
    spec.channels = *channels;
    spec.frequency = static_cast<uint32_t>(*freq);
    spec.samples = *samples;
    spec.preSamples = preSamples;
    if (!trigger->none)
        spec.trigger = trigger->trigger;
    spec.timeout = *timeout;
    spec.probeOnTimeout = !p.isSet("no-probe");

    lt::DocumentOptions docOptions;
    docOptions.collectEdges = p.isSet("out-json");
    docOptions.summary = p.isSet("summary");
    docOptions.summaryEdges = p.isSet("edges");
    docOptions.latency = latencyArg;
    docOptions.collectLatencyEvents = p.isSet("out-events-csv");
    docOptions.expectations = expectations;
    docOptions.setup = *setup;

    auto outcome = lt::performCapture(conn, spec, docOptions, warnings);
    json& doc = outcome.document.doc;
    const auto& result = outcome.result;
    const auto& stats = outcome.document.stats;
    if (outcome.usageError) {
        if (o.json)
            std::cout << doc.dump(2) << std::endl;
        else
            std::cerr << "logictracer-cli capture: " << result.error << std::endl;
        return ExitUsage;
    }

    lt::writeCaptureOutputs(
        doc, result, outcome.document,
        lt::OutputPaths{str(p.value("out-vcd")), str(p.value("out-json")), str(p.value("out-csv")),
                        str(p.value("out-events-csv"))},
        conn.info().version.board);
    const json& outputs = doc["outputs"];

    const int rc = result.status == lt::CaptureStatus::Ok
                       ? (outcome.document.expectationsPassed ? ExitOk : ExitExpectFailed)
                   : result.status == lt::CaptureStatus::Timeout ? ExitTimeout
                                                                 : ExitError;
    if (o.json) {
        const std::string dumped = doc.dump(2);
        std::cout << dumped << std::endl;
        if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
            return rc == ExitOk ? ExitError : rc;
        return rc;
    }

    const auto& info = conn.info();
    const std::string trigDesc = spec.trigger ? "CH" + std::to_string(spec.trigger->channel + 1) +
                                                    (spec.trigger->inverted ? " falling" : " rising")
                                              : std::string("immediate");
    std::string aiText;
    if (p.isSet("ai") || p.isSet("copy")) {
        if (result.buffer) {
            const json* assertionsPtr = doc.contains("assertions") ? &doc["assertions"] : nullptr;
            aiText = buildBufferAiReport(*result.buffer, *aiMode, "LogicTracer capture (" + info.port + ")", trigDesc,
                                         info.version.raw, info.version.board, str(p.value("note")), latencyArg, {},
                                         assertionsPtr, *setup);
        } else if (result.status == lt::CaptureStatus::Timeout) {
            aiText = buildTimeoutAiReport(doc, result.lineDump ? &*result.lineDump : nullptr, str(p.value("note")));
        }
    }

    if (p.isSet("ai") && !aiText.empty()) {
        for (const auto& w : doc["warnings"])
            std::cerr << "warning: " << w.get<std::string>() << '\n';
        std::cout << aiText;
        for (auto it = outputs.begin(); it != outputs.end(); ++it)
            std::cerr << "wrote " << it.key() << ": " << it.value().get<std::string>() << '\n';
        if (p.isSet("copy") && !copyAndConfirm(aiText, error)) {
            std::cerr << "logictracer-cli capture: " << error << std::endl;
            return rc == ExitOk ? ExitError : rc;
        }
        return rc;
    }

    std::cout << "Capture " << lt::toString(result.status) << " on " << info.port << " (" << info.version.raw << "), "
              << result.elapsed.count() << " ms";
    if (!result.error.empty())
        std::cout << ": " << result.error;
    std::cout << '\n';
    for (const auto& w : doc["warnings"])
        std::cerr << "warning: " << w.get<std::string>() << '\n';
    if (result.buffer) {
        std::cout << result.buffer->size() << " samples @ " << lt::formatHz(spec.frequency) << " ("
                  << lt::formatTime(static_cast<double>(result.buffer->size()) / spec.frequency) << "), trigger "
                  << trigDesc << '\n';
        if (p.isSet("summary") && stats)
            printSummaryTable(*stats);
        if (outcome.document.latency) {
            printLatency(*outcome.document.latency);
        }
        if (doc.contains("assertions")) {
            std::cout << "Assertions: " << (outcome.document.expectationsPassed ? "PASS" : "FAIL") << '\n';
            printAssertionIssues(doc["assertions"]);
        }
    }
    if (result.lineDump) {
        std::cout << "Current line state:\n";
        printDiagnosisTable(lt::SignalAnalyzer{}.analyze(*result.lineDump));
    }
    for (auto it = outputs.begin(); it != outputs.end(); ++it)
        std::cout << "wrote " << it.key() << ": " << it.value().get<std::string>() << '\n';
    if (p.isSet("copy") && !aiText.empty() && !copyAndConfirm(aiText, error)) {
        std::cerr << "logictracer-cli capture: " << error << std::endl;
        return rc == ExitOk ? ExitError : rc;
    }
    return rc;
}

int cmdAnalyze(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    const auto helpOption = p.addHelpOption();
    p.setApplicationDescription(
        "Offline analysis, conversion and --expect verification of a saved capture (.json, .vcd, or legacy .lac).");
    p.addPositionalArgument("file", "Input capture file (.json, .vcd, or .lac).");
    p.addOptions({
        {"file", "Input capture file (alternative to positional argument).", "path"},
        {"json", "Machine-readable JSON output on stdout."},
        {"ai", "Print an AI-ready Markdown + ASCII report for the capture file."},
        {"ai-mode", "AI report detail: brief (default, chat-ready), cursors, or full.", "mode", "brief"},
        {"note", "Context or question prepended to the AI report.", "text"},
        {"meta", "Record test-setup metadata with the result, repeatable: --meta dut.board=PYNQ-Z1.", "key=value"},
        {"copy", "Copy the AI report (or JSON when --json is set) to the system clipboard."},
        {"board", "Optional board name for GPIO pin mapping (e.g. PICO, ZERO, PICO_2).", "name"},
        {"summary", "Include per-channel analytics in the output."},
        {"edges", "Include every edge timestamp in stdout JSON."},
        {"measure-latency", "Cross-channel latency STIM[:edge]->REACT[:edge], e.g. 1->2 or 24:falling->2:rising.", "spec"},
        {"expect", "Pass/fail assertions (exit 3 on failure), e.g. 'p99_ns<=60,drops==0'.", "expr"},
        {"out-json", "Convert/write lossless capture JSON v1 to a file.", "file"},
        {"out-vcd", "Convert/write VCD waveform to a file.", "file"},
        {"out-csv", "Write sample-by-sample CSV table.", "file"},
        {"out-events-csv", "Write paired STIM->REACT latency events CSV.", "file"},
    });
    if (!p.parse(args))
        return report(o, ExitUsage, str(p.errorText()));
    if (p.isSet(helpOption)) {
        std::cout << str(p.helpText());
        return ExitOk;
    }

    std::string inputPath = str(p.value("file"));
    const auto pos = p.positionalArguments();
    if (inputPath.empty() && !pos.isEmpty())
        inputPath = str(pos.first());
    if (inputPath.empty() || pos.size() > 1)
        return report(o, ExitUsage, "pass exactly one capture file (.json, .vcd, or .lac)");

    std::string error;
    const auto aiMode = parseAiModeOption(p, lt::CopyMode::Brief, error);
    if (!aiMode)
        return report(o, ExitUsage, error);

    lt::LoadedCapture loaded;
    if (!lt::loadCaptureFile(inputPath, loaded, &error))
        return report(o, ExitError, error);

    const auto& buf = loaded.buffer;
    std::optional<lt::cli::LatencyArg> latencyArg;
    if (p.isSet("measure-latency")) {
        latencyArg = lt::cli::parseLatencyArg(str(p.value("measure-latency")), kMaxProtocolChannels, &error);
        if (!latencyArg)
            return report(o, ExitUsage, error);
        auto inCapture = [&](uint8_t hwCh) {
            return std::any_of(buf.channels().begin(), buf.channels().end(),
                               [&](const lt::ChannelInfo& c) { return c.number == hwCh; });
        };
        if (!inCapture(latencyArg->stimChannel) || !inCapture(latencyArg->reactChannel))
            return report(o, ExitUsage, "--measure-latency channels are not present in " + inputPath);
    }
    if (p.isSet("out-events-csv") && !latencyArg)
        return report(o, ExitUsage, "--out-events-csv requires --measure-latency");

    std::optional<std::vector<lt::cli::ExpectationClause>> expectations;
    if (p.isSet("expect")) {
        expectations = lt::cli::parseExpectations(str(p.value("expect")), &error);
        if (!expectations)
            return report(o, ExitUsage, error);
    }
    const auto setup = parseMetaOptions(p, loaded.setup, error);
    if (!setup)
        return report(o, ExitUsage, error);

    lt::SignalAnalyzer::Options anOpts;
    anOpts.collectEdges = p.isSet("out-json") || p.isSet("edges");
    lt::CaptureDocument capDoc;
    capDoc.stats = lt::SignalAnalyzer(anOpts).analyze(buf);

    capDoc.doc = {
        {"schema_version", lt::kCaptureSchemaVersion},
        {"command", "analyze"},
        {"status", "OK"},
        {"source_file", inputPath},
        {"trigger_description", loaded.triggerDescription},
        {"warnings", json::array()},
        {"outputs", json::object()},
    };
    if (!setup->empty())
        capDoc.doc["setup"] = lt::toJson(*setup);
    if (p.isSet("summary") || p.isSet("edges") || o.json)
        capDoc.doc["capture"] = lt::captureToJson(buf, *capDoc.stats, p.isSet("edges"));

    if (latencyArg) {
        std::size_t stimIdx = 0, reactIdx = 0;
        for (std::size_t i = 0; i < buf.channels().size(); ++i) {
            if (buf.channels()[i].number == latencyArg->stimChannel) stimIdx = i;
            if (buf.channels()[i].number == latencyArg->reactChannel) reactIdx = i;
        }
        capDoc.latency = lt::SignalAnalyzer::analyzeLatency(
            buf, {stimIdx, latencyArg->stimFalling, reactIdx, latencyArg->reactFalling, std::nullopt},
            p.isSet("out-events-csv"));
        capDoc.doc["latency"] = lt::toJson(*capDoc.latency, false);
    }

    if (expectations) {
        const auto checks = lt::cli::evaluateExpectations(*expectations, *capDoc.stats, capDoc.latency);
        bool allOk = !checks.empty();
        auto arr = json::array();
        for (const auto& c : checks) {
            allOk = allOk && c.passed;
            arr.push_back({{"expression", c.expression},
                           {"passed", c.passed},
                           {"actual", c.actual ? json(*c.actual) : json(nullptr)},
                           {"detail", c.detail}});
        }
        capDoc.expectationsPassed = allOk;
        capDoc.doc["assertions"] = {{"passed", allOk}, {"checks", std::move(arr)}};
    }

    lt::CaptureResult fakeRes;
    fakeRes.status = lt::CaptureStatus::Ok;
    fakeRes.buffer = buf;
    lt::writeCaptureOutputs(
        capDoc.doc, fakeRes, capDoc,
        lt::OutputPaths{str(p.value("out-vcd")), str(p.value("out-json")), str(p.value("out-csv")),
                        str(p.value("out-events-csv"))});

    const int rc = capDoc.expectationsPassed ? ExitOk : ExitExpectFailed;
    if (o.json) {
        const std::string dumped = capDoc.doc.dump(2);
        std::cout << dumped << std::endl;
        if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
            return rc == ExitOk ? ExitError : rc;
        return rc;
    }

    std::string aiText;
    if (p.isSet("ai") || p.isSet("copy")) {
        const json* assertionsPtr = capDoc.doc.contains("assertions") ? &capDoc.doc["assertions"] : nullptr;
        aiText = buildBufferAiReport(buf, *aiMode, "LogicTracer analyze: " + inputPath, loaded.triggerDescription, {},
                                     str(p.value("board")), str(p.value("note")), latencyArg, loaded.annotations,
                                     assertionsPtr, *setup);
    }

    if (p.isSet("ai")) {
        std::cout << aiText;
        if (p.isSet("copy") && !copyAndConfirm(aiText, error)) {
            std::cerr << "logictracer-cli analyze: " << error << std::endl;
            return rc == ExitOk ? ExitError : rc;
        }
        return rc;
    }

    std::cout << "Analyze " << inputPath << ": " << buf.size() << " samples @ " << lt::formatHz(buf.frequency())
              << ", trigger_sample=" << buf.triggerIndex() << '\n';
    printSummaryTable(*capDoc.stats);
    if (capDoc.latency) {
        printLatency(*capDoc.latency);
    }
    if (capDoc.doc.contains("assertions")) {
        std::cout << "Assertions: " << (capDoc.expectationsPassed ? "PASS" : "FAIL") << '\n';
        printAssertionIssues(capDoc.doc["assertions"]);
    }
    if (p.isSet("copy") && !aiText.empty() && !copyAndConfirm(aiText, error)) {
        std::cerr << "logictracer-cli analyze: " << error << std::endl;
        return rc == ExitOk ? ExitError : rc;
    }
    return rc;
}

int cmdBootloader(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription("Reboot a connected RP2040/RP2350 capture device into USB UF2 bootloader mode (RPI-RP2).");
    p.addOptions({
        {"json", "Machine-readable JSON output."},
        {"port", "Serial port (default: first detected device).", "path"},
        {"timeout", "Handshake timeout in ms (default 3000).", "ms", "3000"},
        {"yes", "Confirm rebooting the device into UF2 bootloader mode."},
    });
    if (auto rc = parse(p, args, o))
        return *rc;
    if (!p.isSet("yes"))
        return report(o, ExitUsage, "pass --yes to confirm rebooting the device into UF2 bootloader mode");
    const auto timeout = parseMs(p.value("timeout"));
    if (!timeout)
        return report(o, ExitUsage, "invalid --timeout");

    std::string error;
    auto device = openDevice(p, *timeout, error);
    if (!device)
        return report(o, ExitError, error);
    lt::IDevice& conn = *device;
    const std::string port = conn.info().port;
    const std::string ver = conn.info().version.raw;
    if (!conn.enterBootloader())
        return report(o, ExitError, conn.lastError(), {{"port", port}});

    if (o.json) {
        std::cout << json{{"command", "bootloader"}, {"status", "OK"}, {"port", port}, {"previous_version", ver}}
                         .dump(2)
                  << std::endl;
    } else {
        std::cout << "Rebooted " << port << " (" << ver << ") into UF2 bootloader mode.\n";
    }
    return ExitOk;
}

// ---------------------------------------------------------------------------------------------
// Commands talking to a running GUI session.

// Prints a session reply (or error) and maps it to an exit code.
int finishSessionCall(const Output& o, const lt::session::CallResult& r, const std::function<void(const json&)>& text)
{
    if (!r.ok) {
        const ExitCode code = r.errorCode == lt::session::error::InvalidParams ? ExitUsage : ExitError;
        return report(o, code, r.error + (r.connected ? "" : " (start logictracer-gui first)"),
                      {{"session_connected", r.connected}});
    }
    if (o.json) {
        json doc = r.result;
        doc["command"] = o.command;
        doc["status"] = "OK";
        std::cout << doc.dump(2) << std::endl;
    } else {
        text(r.result);
    }
    return ExitOk;
}

int cmdAnnotate(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription(
        "Place a marker on the timeline of the running GUI (relative to the capture's trigger), so the\n"
        "engineer sees on screen what the analysis refers to.\n"
        "Example: annotate --time 50.12ms --label \"Jetson pulse start\" --color red");
    p.addOptions({
        {"json", "Machine-readable JSON output."},
        {"time", "Time relative to the trigger with a unit: 50.12ms, 200us, 1.5s, -20us.", "t"},
        {"label", "Marker text (1..200 characters).", "text"},
        {"color", "Colour name or #rrggbb (default red).", "color"},
        {"duration", "Highlight a span of this length starting at --time (e.g. 2ms).", "t"},
        {"author", "Who placed the marker (default: cli).", "name", "cli"},
        {"list", "List existing annotations instead of adding one."},
        {"clear", "Remove all annotations instead of adding one."},
        {"socket", "Session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"},
    });
    if (auto rc = parse(p, args, o))
        return *rc;
    const QString socket = socketPathFrom(p);

    if (p.isSet("list"))
        return finishSessionCall(o, lt::session::call(socket, "list_annotations"), [](const json& r) {
            for (const auto& a : r["annotations"])
                std::cout << "#" << a["id"].get<int>() << "  " << lt::formatTime(a["time_us"].get<double>() * 1e-6)
                          << "  [" << a["author"].get<std::string>() << "] " << a["label"].get<std::string>() << '\n';
            if (r["annotations"].empty())
                std::cout << "No annotations.\n";
        });
    if (p.isSet("clear"))
        return finishSessionCall(o, lt::session::call(socket, "clear_annotations"), [](const json& r) {
            std::cout << "Removed " << r["cleared"].get<int>() << " annotation(s).\n";
        });

    if (!p.isSet("time") || !p.isSet("label"))
        return report(o, ExitUsage, "--time and --label are required (or use --list / --clear)");
    const auto seconds = lt::cli::parseTimeSeconds(str(p.value("time")));
    if (!seconds)
        return report(o, ExitUsage, "invalid --time '" + str(p.value("time")) + "' (use a unit: 50.12ms, 200us, 1.5s)");
    json params = {{"time_s", *seconds}, {"label", str(p.value("label"))}, {"author", str(p.value("author"))}};
    if (p.isSet("color"))
        params["color"] = str(p.value("color"));
    if (p.isSet("duration")) {
        const auto d = lt::cli::parseTimeSeconds(str(p.value("duration")));
        if (!d || *d < 0)
            return report(o, ExitUsage, "invalid --duration '" + str(p.value("duration")) + "'");
        params["duration_us"] = *d * 1e6;
    }
    return finishSessionCall(o, lt::session::call(socket, "annotate", params), [&](const json& r) {
        std::cout << "Annotation #" << r["id"].get<int>() << " added at " << lt::formatTime(*seconds) << ": "
                  << r["label"].get<std::string>() << '\n';
    });
}

int cmdView(const QStringList& args, Output o)
{
    QCommandLineParser p;
    p.setApplicationDescription(
        "Read what the engineer currently sees in the GUI: the visible range, cursors A/B and annotations.\n"
        "--report returns the full 'Copy for AI' text (Markdown + ASCII timeline + JSON).");
    p.addOptions({
        {"json", "Machine-readable JSON output."},
        {"report", "Return the Copy-for-AI report for the current GUI view."},
        {"ai-mode", "Report detail: full, brief, or cursors (default: GUI's selected mode).", "mode"},
        {"note", "Override the Context/Question header in the report.", "text"},
        {"copy", "Copy the report (or JSON with --json) to the system clipboard."},
        {"socket", "Session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"},
    });
    if (auto rc = parse(p, args, o))
        return *rc;
    std::string error;
    if (p.isSet("ai-mode")) {
        if (!lt::parseCopyMode(str(p.value("ai-mode"))))
            return report(o, ExitUsage, "invalid --ai-mode '" + str(p.value("ai-mode")) + "' (expected brief, cursors, or full)");
    }
    const QString socket = socketPathFrom(p);

    if (p.isSet("report") || (p.isSet("copy") && !o.json)) {
        json params = json::object();
        if (p.isSet("ai-mode"))
            params["mode"] = str(p.value("ai-mode"));
        if (p.isSet("note"))
            params["note"] = str(p.value("note"));
        const auto sr = lt::session::call(socket, "get_report", params);
        if (!sr.ok) {
            const ExitCode code = sr.errorCode == lt::session::error::InvalidParams ? ExitUsage : ExitError;
            return report(o, code, sr.error + (sr.connected ? "" : " (start logictracer-gui first)"),
                          {{"session_connected", sr.connected}});
        }
        if (o.json) {
            json doc = sr.result;
            doc["command"] = o.command;
            doc["status"] = "OK";
            const std::string dumped = doc.dump(2);
            std::cout << dumped << std::endl;
            if (p.isSet("copy") && !copyAndConfirm(dumped + "\n", error))
                return report(o, ExitError, error);
        } else {
            const std::string rep = sr.result["report"].get<std::string>();
            if (p.isSet("report"))
                std::cout << rep;
            if (p.isSet("copy") && !copyAndConfirm(rep, error))
                return report(o, ExitError, error);
        }
        return ExitOk;
    }
    const auto sr = lt::session::call(socket, "get_state");
    if (p.isSet("copy") && sr.ok && o.json) {
        json doc = sr.result;
        doc["command"] = o.command;
        doc["status"] = "OK";
        const std::string dumped = doc.dump(2);
        std::cout << dumped << std::endl;
        if (!copyAndConfirm(dumped + "\n", error))
            return report(o, ExitError, error);
        return ExitOk;
    }
    return finishSessionCall(o, sr, [](const json& r) {
        std::cout << "GUI " << (r["connected"].get<bool>() ? "connected to a device" : "not connected") << ", "
                  << (r["has_capture"].get<bool>() ? "capture shown" : "no capture") << ", "
                  << r["annotation_count"].get<int>() << " annotation(s)\n";
        if (r.contains("visible") && !r["visible"]["start_us"].is_null())
            std::cout << "Visible: " << lt::formatTime(r["visible"]["start_us"].get<double>() * 1e-6) << " .. "
                      << lt::formatTime(r["visible"]["end_us"].get<double>() * 1e-6) << " (samples "
                      << r["visible"]["first_sample"].get<int>() << ".." << r["visible"]["end_sample"].get<int>()
                      << ")\n";
    });
}

void printUsage()
{
    std::cout << "logictracer-cli " LT_VERSION " - headless logic analysis for scripts and AI agents\n\n"
                 "Usage: logictracer-cli <command> [options]\n\n"
                 "Commands:\n"
                 "  detect      List connected devices and their capabilities\n"
                 "  probe       Snapshot current pin levels without a trigger (pull-up / stuck-at-0 check)\n"
                 "  capture     Capture with trigger, latency analysis, --expect assertions, and exports\n"
                 "  analyze     Analyze, verify (--expect), or convert saved .json / .vcd / .lac files\n"
                 "  annotate    Place a marker on the timeline of the running GUI (--list / --clear)\n"
                 "  view        Read what the GUI currently shows (visible range, cursors, annotations)\n"
                 "  bootloader  Reboot the RP2040 into USB UF2 bootloader mode (requires --yes)\n\n"
                 "Run 'logictracer-cli <command> --help' for command options.\n"
                 "Exit codes: 0 OK, 1 error, 2 timeout, 3 --expect failed, 64 usage error.\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("logictracer-cli");
    QCoreApplication::setApplicationVersion(LT_VERSION);

    QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) {
        printUsage();
        return ExitUsage;
    }
    const QString command = args.takeAt(1);
    const bool jsonRequested = command == "--json" || args.contains("--json");
    Output o{command == "--json" ? std::string() : str(command), jsonRequested};

    if (command == "detect")
        return cmdDetect(args, o);
    if (command == "probe")
        return cmdProbe(args, o);
    if (command == "capture")
        return cmdCapture(args, o);
    if (command == "analyze")
        return cmdAnalyze(args, o);
    if (command == "annotate")
        return cmdAnnotate(args, o);
    if (command == "view")
        return cmdView(args, o);
    if (command == "bootloader")
        return cmdBootloader(args, o);
    if (command == "-h" || command == "--help" || command == "help") {
        printUsage();
        return ExitOk;
    }
    if (command == "-v" || command == "--version") {
        std::cout << "logictracer-cli " LT_VERSION "\n";
        return ExitOk;
    }
    if (o.json) {
        return report(o, ExitUsage,
                      "missing or unknown command '" + str(command) +
                          "' (expected detect|probe|capture|analyze|annotate|view|bootloader)");
    }
    std::cerr << "logictracer-cli: unknown command '" << str(command) << "'\n\n";
    printUsage();
    return ExitUsage;
}

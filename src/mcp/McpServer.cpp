// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "mcp/McpServer.hpp"

#include "core/ArgParsing.hpp"
#include "core/Exporters.hpp"
#include "services/Operations.hpp"
#include "services/Session.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace lt::mcp {

using json = nlohmann::json;

namespace {

constexpr const char* kServerName = "logictracer-mcp";
#ifndef LT_VERSION
#define LT_VERSION "0.1.0"
#endif

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

const char* const kSupportedVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

json rpcError(const json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json textResult(const json& structured, bool isError)
{
    return {{"content", json::array({{{"type", "text"}, {"text", structured.dump(2)}}})},
            {"isError", isError},
            {"structuredContent", structured}};
}

json errorResult(const std::string& message)
{
    return textResult({{"status", "ERROR"}, {"error", message}}, true);
}

json prop(const char* type, const char* description)
{
    return {{"type", type}, {"description", description}};
}

// Quantities may be given as numbers or as strings with SI suffixes ("1M").
json quantity(const char* description)
{
    return {{"type", json::array({"integer", "string"})}, {"description", description}};
}

json schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

json tool(const char* name, const char* description, json inputSchema)
{
    return {{"name", name}, {"description", description}, {"inputSchema", std::move(inputSchema)}};
}

// A TIMEOUT is a valid answer (it carries the current line state); only real failures are errors.
bool isFailure(const json& doc)
{
    const auto status = doc.value("status", std::string("ERROR"));
    return status != "OK" && status != "TIMEOUT" && status != "NOT_FOUND";
}

}  // namespace

McpServer::McpServer(Options options) : m_options(std::move(options))
{
    if (m_options.socketPath.isEmpty())
        m_options.socketPath = session::defaultSocketPath();
}

json McpServer::toolList()
{
    const json channels = {{"description",
                            "1-based channels, e.g. \"1,2:REACT,24:JETSON_DO1\", \"1-8\" or [1,2,24]. Names are "
                            "optional labels."}};
    json capture = {
        {"port", prop("string", "Serial port; default: the GUI's device or the first capture device found.")},
        {"channels", channels},
        {"freq", quantity("Sample rate in Hz, plain or with k/M suffix, e.g. \"1M\" (default 1M).")},
        {"samples", quantity("Total samples (pre+post trigger), e.g. 10000 or \"100k\" (default 10k).")},
        {"pre_samples", quantity("Samples kept before the trigger (default 10% of samples).")},
        {"trigger", prop("string", "\"CH:rising\" or \"CH:falling\" (level-sensitive in firmware; CH 25 = external "
                                   "input); \"none\" = immediate capture (default).")},
        {"timeout_ms", prop("integer", "How long to wait for the trigger (default 5000). The firmware adds ~2.2 s.")},
        {"summary", prop("boolean", "Include per-channel analytics (default true).")},
        {"edges", prop("boolean", "Include every edge timestamp in the result (default false).")},
        {"latency", prop("string", "Optional cross-channel STIM->REACT latency spec, e.g. \"1->2\" or \"24:falling->2:rising\".")},
        {"expect", prop("string", "Optional pass/fail assertions, e.g. \"p99_ns<=60,drops==0,DO1:low_pulses>=1\".")},
        {"meta", prop("object", "Optional test-setup metadata recorded with the result, string values only, e.g. "
                                "{\"dut.board\": \"PYNQ-Z1\", \"dut.bitstream_sha256\": \"...\"}.")},
        {"out_vcd", prop("string", "Write the waveform to this .vcd file (confined to allowed output directory).")},
        {"out_json", prop("string", "Write the lossless capture JSON v1 (with edge_samples) to this .json file.")},
        {"out_csv", prop("string", "Write sample-by-sample CSV to this .csv file.")},
        {"out_events_csv", prop("string", "Write paired STIM->REACT latency events to this .csv file.")},
        {"probe_on_timeout", prop("boolean", "On TIMEOUT also report the current line levels (default true).")},
    };
    capture["channels"]["type"] = json::array({"string", "array"});

    json probe = {
        {"port", prop("string", "Serial port; default: the GUI's device or the first capture device found.")},
        {"channels", channels},
        {"window_ms", prop("integer", "Observation window in ms (default 10).")},
        {"rate", quantity("Sample rate (default 1M).")},
    };
    probe["channels"]["type"] = json::array({"string", "array"});

    return json::array({
        tool("lt_detect_devices",
             "List connected capture devices (USB 1209:3020) with firmware version, channel count, max "
             "sample rate and buffer limits.",
             schema({{"handshake", prop("boolean", "Open each port and query its limits (default true).")},
                     {"include_all", prop("boolean", "Also probe every /dev/ttyACM* port (default false).")},
                     {"timeout_ms", prop("integer", "Handshake timeout per device (default 3000).")}})),
        tool("lt_probe_inputs",
             "Snapshot the current logic level of the inputs without a trigger and classify each line: HIGH "
             "(pull-up ok / driven), LOW (stuck-at-0 or floating; RP2040 inputs default to pull-down), TOGGLING or "
             "NOISY. Takes ~2-8 s because of the firmware polling.",
             schema(probe)),
        tool("lt_configure_and_capture",
             "Run a capture with the given trigger and timeout and return compact per-channel analytics (edges, "
             "pulses, min/max pulse width, frequency, duty cycle, glitches). On TIMEOUT (status \"TIMEOUT\") the "
             "result includes line_dump with the current level of every channel.",
             schema(capture)),
        tool("lt_add_annotation",
             "Place a marker on the timeline of the engineer's open logictracer-gui window. Time is relative to "
             "the capture's trigger (t=0).",
             schema({{"time_us", prop("number", "Time in microseconds relative to the trigger (may be negative).")},
                     {"text", prop("string", "Marker text, 1..200 characters.")},
                     {"color", prop("string", "Colour name or #rrggbb (default red).")},
                     {"duration_us", prop("number", "Highlight a span of this length starting at time_us.")},
                     {"capture_id", prop("integer", "Capture id from lt_get_view / lt_configure_and_capture. The marker "
                                                    "is refused with StaleCapture if another capture is on screen now; "
                                                    "always pass it.")}},
                    {"time_us", "text"})),
        tool("lt_get_view",
             "Read what the engineer currently sees in the GUI: capture metadata, the visible time range, cursors "
             "A/B and annotations. With report=true also returns the 'Copy for AI' report (Markdown summary, causal "
             "chronological transition log, ASCII timeline, and optional JSON edge list) of that view. The result "
             "carries capture_id (pass it to lt_add_annotation) and display_state (live / armed / stale): when "
             "stale, the report is a failure diagnosis, not a description of the waveform on screen.",
             schema({{"capture_id", prop("integer", "Refuse with StaleCapture unless this capture is on screen.")},
                     {"report", prop("boolean", "Include the report for the visible range (default true).")},
                     {"mode", prop("string", "Report detail: 'brief' (compact, no raw JSON), 'cursors' (A..B only), or 'full'.")},
                     {"latency", prop("string", "Optional cross-channel STIM..REACT latency spec, e.g. '1..2' or '24:falling..2:rising'.")}})),
        tool("lt_set_view",
             "Steer the engineer's GUI view so they see what you describe: zoom to a window, fit the whole capture, "
             "place cursors A/B, toggle hiding of inactive channels. Times are microseconds relative to the trigger. "
             "Pass capture_id from lt_get_view; the call is refused with StaleCapture if another capture is on "
             "screen. Returns the updated view state.",
             schema({{"capture_id", prop("integer", "Capture id from lt_get_view; refused with StaleCapture on mismatch.")},
                     {"fit", prop("boolean", "Show the whole capture.")},
                     {"start_us", prop("number", "Window start (use with end_us).")},
                     {"end_us", prop("number", "Window end (use with start_us).")},
                     {"center_us", prop("number", "Window centre (use with span_us).")},
                     {"span_us", prop("number", "Window width (use with center_us).")},
                     {"cursor_a_us", prop("number", "Place Cursor A at this time.")},
                     {"cursor_b_us", prop("number", "Place Cursor B at this time.")},
                     {"clear_cursors", prop("boolean", "Remove both cursors first.")},
                     {"hide_inactive", prop("boolean", "Hide channels without transitions (display only).")}})),
        tool("lt_get_anomalies",
             "List the anomalies the analyzer found in the capture on screen (drops, spurious reactions, timing "
             "outliers, irregular periods), most severe first. Each incident has sample, time_us, severity "
             "(3 functional, 2 timing, 1 signal quality), kind and detail. Use lt_set_view / lt_add_annotation to "
             "show them to the engineer.",
             schema({{"capture_id", prop("integer", "Capture id from lt_get_view; refused with StaleCapture on mismatch.")},
                     {"latency", prop("string", "Optional STIM..REACT latency spec, e.g. '1..2' or '24:falling..2:rising'.")},
                     {"max_count", prop("integer", "Maximum incidents to return (default 20, max 1000).")}})),
        tool("lt_clear_annotations",
             "Remove every annotation from the GUI timeline of the capture on screen.",
             schema({{"capture_id", prop("integer", "Capture id from lt_get_view; refused with StaleCapture on mismatch, "
                                                    "so annotations of a newer capture are never wiped.")}})),
    });
}

bool McpServer::sessionHoldsDevice(const std::string& requestedPort) const
{
    if (!m_options.useSession)
        return false;
    const auto r = session::call(m_options.socketPath, "get_state", json::object(), 2000);
    if (!r.ok || !r.result.value("connected", false))
        return false;
    if (requestedPort.empty())
        return true;
    return r.result.contains("device") && r.result["device"].is_object() &&
           r.result["device"].value("port", std::string{}) == requestedPort;
}

json McpServer::sessionOrError(const std::string& method, const json& params)
{
    const auto r = session::call(m_options.socketPath, method, params, 60'000);
    if (r.ok)
        return textResult(r.result, false);
    std::string message = r.error;
    if (!r.connected)
        message += ". Start logictracer-gui (the engineer's window) so annotations and the live view are available.";
    json structured = {{"status", "ERROR"}, {"error", message}, {"code", r.errorCode}};
    if (r.errorCode == session::error::StaleCapture)
        structured["error_kind"] = "StaleCapture";
    if (!r.errorData.is_null())
        structured["details"] = r.errorData;
    return textResult(structured, true);
}

json McpServer::runDeviceTool(const std::string& tool, const json& args)
{
    const std::string requestedPort = args.value("port", std::string{});
    const bool forward = sessionHoldsDevice(requestedPort);

    if (tool == "lt_probe_inputs") {
        if (forward) {
            const auto r = session::call(m_options.socketPath, "probe", args, 60'000);
            if (!r.ok)
                return errorResult(r.error);
            return textResult(r.result, isFailure(r.result));
        }
        std::string error;
        const std::string port = requestedPort.empty() ? m_options.defaultPort : requestedPort;
        auto device = openDevice(port, std::chrono::milliseconds(3000), error);
        if (!device)
            return errorResult(error);
        IDevice& conn = *device;
        auto params = probeParamsFromJson(args, conn.info().channels, error);
        if (!params)
            return errorResult(error);
        if (params->channels.empty())
            for (uint8_t i = 0; i < conn.info().channels; ++i)
                params->channels.push_back({i, {}});
        const auto probe = performProbe(conn, params->channels, params->window, params->rate);
        return textResult(probe.doc, isFailure(probe.doc));
    }

    // Validate and sandbox all output paths up front (both for session forwarding and direct capture).
    std::string error;
    static constexpr std::array<std::string_view, 1> kVcdExt{".vcd"};
    static constexpr std::array<std::string_view, 1> kJsonExt{".json"};
    static constexpr std::array<std::string_view, 1> kCsvExt{".csv"};

    const auto vcdPath = resolveSafeOutputPath(args.value("out_vcd", std::string{}), kVcdExt, m_options.outputDir, &error);
    if (!vcdPath)
        return errorResult(error);
    const auto jsonPath = resolveSafeOutputPath(args.value("out_json", std::string{}), kJsonExt, m_options.outputDir, &error);
    if (!jsonPath)
        return errorResult(error);
    const auto csvPath = resolveSafeOutputPath(args.value("out_csv", std::string{}), kCsvExt, m_options.outputDir, &error);
    if (!csvPath)
        return errorResult(error);
    const auto eventsCsvPath =
        resolveSafeOutputPath(args.value("out_events_csv", std::string{}), kCsvExt, m_options.outputDir, &error);
    if (!eventsCsvPath)
        return errorResult(error);

    // lt_configure_and_capture
    if (forward) {
        json sanitized = args;
        if (!vcdPath->empty()) sanitized["out_vcd"] = *vcdPath;
        if (!jsonPath->empty()) sanitized["out_json"] = *jsonPath;
        if (!csvPath->empty()) sanitized["out_csv"] = *csvPath;
        if (!eventsCsvPath->empty()) sanitized["out_events_csv"] = *eventsCsvPath;

        int64_t ipcWaitMs = 120'000;
        std::string tmpErr;
        if (auto est = captureSpecFromJson(sanitized, 32, tmpErr)) {
            const int64_t capMs = static_cast<int64_t>((est->samples * 1000ull) / std::max<uint32_t>(1u, est->frequency));
            ipcWaitMs = std::clamp<int64_t>(est->timeout.count() + capMs + 20'000, 120'000, 3'700'000);
        }
        const auto r = session::call(m_options.socketPath, "capture", sanitized, static_cast<int>(ipcWaitMs));
        if (!r.ok)
            return errorResult(r.error);
        const bool expectFailed =
            r.result.contains("assertions") && !r.result["assertions"].value("passed", true);
        return textResult(r.result, isFailure(r.result) || expectFailed);
    }

    const std::string port = requestedPort.empty() ? m_options.defaultPort : requestedPort;
    auto device = openDevice(port, std::chrono::milliseconds(3000), error);
    if (!device)
        return errorResult(error);
    IDevice& conn = *device;
    auto spec = captureSpecFromJson(args, conn.info().channels, error);
    if (!spec)
        return errorResult(error);
    auto options = documentOptionsFromJson(args, true, conn.info().channels, spec->channels, error);
    if (!options)
        return errorResult(error);
    if (!eventsCsvPath->empty() && !options->latency)
        return errorResult("out_events_csv requires 'latency' (e.g. \"1->2\")");
    options->collectEdges = !jsonPath->empty();
    options->collectLatencyEvents = !eventsCsvPath->empty();
    auto outcome = performCapture(conn, *spec, *options);
    lt::writeCaptureOutputs(outcome.document.doc, outcome.result, outcome.document,
                            OutputPaths{*vcdPath, *jsonPath, *csvPath, *eventsCsvPath},
                            conn.info().version.board);
    const bool failed = outcome.usageError || isFailure(outcome.document.doc) || !outcome.document.expectationsPassed;
    return textResult(outcome.document.doc, failed);
}

json McpServer::callTool(const std::string& name, const json& arguments)
{
    const json args = arguments.is_null() ? json::object() : arguments;  // "arguments": null means none
    if (!args.is_object())
        return errorResult("arguments must be an object");
    try {
        return dispatchTool(name, args);
    } catch (const json::exception& e) {  // wrongly typed arguments
        return errorResult(std::string("invalid arguments: ") + e.what());
    }
}

json McpServer::dispatchTool(const std::string& name, const json& args)
{
    if (name == "lt_detect_devices") {
        std::chrono::milliseconds timeout{3000};
        if (args.contains("timeout_ms")) {
            const auto& v = args["timeout_ms"];
            if (!v.is_number() || !(v.get<double>() >= 1.0 && v.get<double>() <= 60000.0))
                return errorResult("timeout_ms must be a number in 1..60000");
            timeout = std::chrono::milliseconds(static_cast<int>(v.get<double>()));
        }
        const QString sock = m_options.useSession ? m_options.socketPath : QString{};
        return textResult(detectDocument(args.value("include_all", false), args.value("handshake", true), timeout, sock),
                          false);
    }
    if (name == "lt_probe_inputs" || name == "lt_configure_and_capture")
        return runDeviceTool(name, args);

    if (name == "lt_add_annotation") {
        json params = {{"author", "ai"}};
        if (args.contains("time_us"))
            params["time_us"] = args["time_us"];
        else if (args.contains("time"))
            params["time"] = args["time"];
        params["label"] = args.contains("text") ? args["text"] : args.value("label", json(""));
        for (const char* key : {"color", "duration_us", "capture_id"})
            if (args.contains(key))
                params[key] = args[key];
        return sessionOrError("annotate", params);
    }
    if (name == "lt_get_view") {
        json params = json::object();
        if (args.contains("capture_id"))
            params["capture_id"] = args["capture_id"];
        if (!args.value("report", true))
            return sessionOrError("get_state", params);
        for (const char* key : {"mode", "latency", "note"})
            if (args.contains(key))
                params[key] = args[key];
        return sessionOrError("get_report", params);
    }
    if (name == "lt_set_view" || name == "lt_get_anomalies") {
        json params = json::object();
        for (const char* key : {"capture_id", "fit", "start_us", "end_us", "center_us", "span_us", "cursor_a_us",
                                "cursor_b_us", "clear_cursors", "hide_inactive", "latency", "max_count"})
            if (args.contains(key))
                params[key] = args[key];
        return sessionOrError(name == "lt_set_view" ? "set_view" : "get_anomalies", params);
    }
    if (name == "lt_clear_annotations") {
        json params = json::object();
        if (args.contains("capture_id"))
            params["capture_id"] = args["capture_id"];
        return sessionOrError("clear_annotations", params);
    }

    return errorResult("unknown tool '" + name + "'");
}

json McpServer::handleRequest(const json& req)
{
    const json id = req.value("id", json(nullptr));
    if (!req.contains("method") || !req["method"].is_string())
        return rpcError(id, kInvalidRequest, "missing method");
    const std::string method = req["method"].get<std::string>();
    const json params = req.contains("params") ? req["params"] : json::object();

    if (method == "initialize") {
        std::string version = kSupportedVersions[0];
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
            const auto requested = params["protocolVersion"].get<std::string>();
            if (std::find(std::begin(kSupportedVersions), std::end(kSupportedVersions), requested) !=
                std::end(kSupportedVersions))
                version = requested;
        }
        return {{"jsonrpc", "2.0"},
                {"id", id},
                {"result",
                 {{"protocolVersion", version},
                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", kServerName}, {"version", LT_VERSION}}},
                  {"instructions",
                   "Tools for LogicTracer (RP2040 capture device) used for hardware-in-the-loop testing. Times are relative "
                   "to the trigger. Use lt_probe_inputs to check line levels before a capture; use lt_get_view to "
                   "see what the engineer sees and lt_add_annotation to mark findings on their screen."}}}};
    }
    if (method == "ping")
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    if (method == "tools/list")
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", toolList()}}}};
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string())
            return rpcError(id, kInvalidParams, "tools/call needs a tool name");
        const std::string name = params["name"].get<std::string>();
        bool known = false;
        for (const auto& t : toolList())
            known = known || t["name"] == name;
        if (!known)
            return rpcError(id, kInvalidParams, "unknown tool '" + name + "'");
        return {{"jsonrpc", "2.0"},
                {"id", id},
                {"result", callTool(name, params.contains("arguments") ? params["arguments"] : json::object())}};
    }
    return rpcError(id, kMethodNotFound, "method not found: " + method);
}

std::optional<json> McpServer::handle(const json& message)
{
    if (message.is_array()) {
        if (message.empty())
            return rpcError(nullptr, kInvalidRequest, "empty batch");
        json out = json::array();
        for (const auto& m : message)
            if (auto r = handle(m))
                out.push_back(std::move(*r));
        return out.empty() ? std::nullopt : std::optional<json>(out);
    }
    if (!message.is_object())
        return rpcError(nullptr, kInvalidRequest, "a JSON-RPC message must be an object");
    // Notifications (no id) never get a response.
    if (!message.contains("id")) {
        return std::nullopt;
    }
    return handleRequest(message);
}

void McpServer::run(std::istream& in, std::ostream& out)
{
    constexpr std::size_t kMaxLineBytes = 2 * 1024 * 1024;
    std::string line;
    char ch = '\0';
    bool overflow = false;

    auto processLine = [&]() {
        if (overflow) {
            out << rpcError(nullptr, kInvalidRequest, "JSON-RPC message exceeds 2 MiB limit").dump() << '\n'
                << std::flush;
        } else if (line.find_first_not_of(" \t\r") != std::string::npos) {
            const json message = json::parse(line, nullptr, false);
            const auto response = message.is_discarded()
                                      ? std::optional<json>(rpcError(nullptr, kParseError, "invalid JSON"))
                                      : handle(message);
            if (response)
                out << response->dump() << '\n' << std::flush;
        }
        line.clear();
        overflow = false;
    };

    while (in.get(ch)) {
        if (ch == '\n') {
            processLine();
        } else if (!overflow) {
            if (line.size() >= kMaxLineBytes)
                overflow = true;
            else
                line.push_back(ch);
        }
    }
    if (!line.empty() || overflow)
        processLine();
}

}  // namespace lt::mcp

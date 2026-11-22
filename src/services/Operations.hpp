// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// High-level, front-end independent operations. The CLI, the GUI session server and the MCP server
// all go through these, so every front-end produces the same JSON for the same request.

#include "core/ArgParsing.hpp"
#include "core/Invariants.hpp"
#include "core/ProtocolDecoder.hpp"
#include "core/Segments.hpp"
#include "device/IDevice.hpp"
#include "device/PicoConnection.hpp"
#include "core/SignalAnalyzer.hpp"

#include <QString>

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace lt {

nlohmann::json channelBrief(const ChannelInfo& channel, std::string_view board = {});
// Level/diagnosis record of one channel of a probe or line dump.
nlohmann::json diagnosisJson(const ChannelStats& stats, std::string_view board = {});

// {"command":"detect","status":"OK"|"NOT_FOUND","count":N,"devices":[...]}
// When `sessionSocketPath` is non-empty, a busy port held by the running GUI session is reported
// using the session's cached DeviceInfo instead of failing with EBUSY.
nlohmann::json detectDocument(bool includeAllAcm, bool handshake, std::chrono::milliseconds timeout,
                              const QString& sessionSocketPath = {});

// What the user asked for, before device limits are applied (channels are 0-based internally).
struct CaptureRequestSpec {
    std::vector<ChannelInfo> channels;
    uint32_t frequency = 1'000'000;
    uint64_t samples = 10'000;  // pre + post for triggered captures, post only for immediate ones
    std::optional<uint64_t> preSamples;
    std::optional<EdgeTrigger> trigger;  // nullopt = immediate
    std::chrono::milliseconds timeout = 5000ms;
    bool probeOnTimeout = true;
    uint32_t segments = 1;  // trigger windows (CaptureConfig::segments); `samples` is then one window
};

// Splits samples into pre/post and applies the device limits. On failure returns nullopt and a message
// in `error`; non-fatal remarks are appended to `warnings`.
std::optional<CaptureConfig> buildCaptureConfig(const DeviceInfo& info, const CaptureRequestSpec& spec,
                                                std::vector<std::string>& warnings, std::string& error,
                                                CaptureField* field = nullptr);

struct DocumentOptions {
    bool collectEdges = false;  // keep every edge timestamp in the analysis (needed for edge output)
    bool summary = false;       // include per-channel analytics under "capture"
    bool summaryEdges = false;  // ... with edge timestamps
    std::optional<cli::LatencyArg> latency;
    bool collectLatencyEvents = false;
    std::optional<std::vector<cli::ExpectationClause>> expectations;
    std::optional<std::vector<InvariantRule>> invariants;  // protocol rules ("invariants" in the document)
    std::vector<DecoderSpec> decoders;                     // bus protocol decoders ("decoded" in the document)
    SetupInfo setup;  // recorded as "setup" in the document and the lossless capture file
};

struct CaptureDocument {
    nlohmann::json doc;
    std::optional<std::vector<ChannelStats>> stats;  // analysis of result.buffer
    std::optional<LatencyStats> latency;
    std::optional<std::vector<DecodedBus>> decoded;
    bool expectationsPassed = true;  // --expect assertions and --invariant rules
    std::optional<SegmentAnalysis> segments;  // window-by-window analysis of a segmented capture
    nlohmann::json fileOnly = nlohmann::json::object();  // extra keys of the saved --out-json file (not of stdout)
};

inline constexpr const char* kSegmentedTimelineError =
    "VCD/CSV cannot represent a segmented capture (the time between windows is unknown); use out_json";
// True when `params` request more than one segment together with a VCD/CSV output.
[[nodiscard]] bool segmentedWithTimelineOutput(const nlohmann::json& params, bool timelineOutput);

// Fills the analysis part of a document: "capture" (summary), "latency", "assertions", "invariants" and, for a
// segmented capture, "segments". `out.stats` always describes the stored samples (lossless export); for a
// segmented capture the summary, latency, assertions and invariants come from the windows.
void analyzeIntoDocument(CaptureDocument& out, const CaptureBuffer& buffer, const std::optional<SegmentLayout>& segments,
                         const DocumentOptions& options, std::string_view board = {});

// {"command":"capture","status":...,"config":...} without file outputs.
CaptureDocument captureDocument(const DeviceInfo& info, const CaptureRequestSpec& spec, const CaptureConfig& config,
                                const CaptureResult& result, const std::vector<std::string>& warnings,
                                const DocumentOptions& options);

struct OutputPaths {
    std::string vcdPath;
    std::string jsonPath;
    std::string csvPath;
    std::string eventsCsvPath;
    bool confined = false;  // paths came from resolveSafeOutputPath: refuse symlinks at write time
};

// Writes the optional VCD / lossless-JSON / CSV / events-CSV files (empty path = skip) and records
// them under doc["outputs"]; failures are appended to doc["warnings"].
// Returns false when a requested file could not be written.
bool writeCaptureOutputs(nlohmann::json& doc, const CaptureResult& result,
                         const CaptureDocument& document, const OutputPaths& paths,
                         std::string_view board = {});

// A message when two requested outputs resolve to the same file (the second write would replace the first).
[[nodiscard]] std::optional<std::string> duplicateOutputPath(const OutputPaths& paths);
// Backwards-compatible 2-path overload.
bool writeCaptureOutputs(nlohmann::json& doc, const CaptureResult& result,
                         const std::optional<std::vector<ChannelStats>>& stats, const std::string& vcdPath,
                         const std::string& jsonPath);

struct CaptureOutcome {
    CaptureDocument document;
    CaptureResult result;
    bool usageError = false;  // rejected before touching the device (bad limits, bad channels)
};

// Validates, runs the capture on an open connection and describes it.
CaptureOutcome performCapture(IDevice& connection, const CaptureRequestSpec& spec,
                              const DocumentOptions& options, std::vector<std::string> warnings = {});

struct ProbeDocument {
    nlohmann::json doc;
    std::optional<std::vector<ChannelStats>> stats;
};

// {"command":"probe",...}; status/error are filled for failed probes. `requestedRate`/`requestedWindow`
// are compared with what the device could do and reported as warnings.
ProbeDocument probeDocument(const DeviceInfo& info, const ProbeResult& result, uint32_t requestedRate,
                            std::chrono::milliseconds requestedWindow);
ProbeDocument performProbe(IDevice& connection, const std::vector<ChannelInfo>& channels,
                           std::chrono::milliseconds window, uint32_t rate);

// JSON request parameters (MCP tools, GUI session). Numbers may also be given as strings with SI
// suffixes ("1M", "10k"); channels are 1-based ("1,2:NAME", "1-8" or an array of numbers/strings).
// Recognised capture keys: channels, freq|rate, samples, pre_samples, trigger ("24:falling"|"none"),
// timeout_ms, probe_on_timeout, segments (trigger windows, needs a trigger).
std::optional<CaptureRequestSpec> captureSpecFromJson(const nlohmann::json& params, int deviceChannels,
                                                      std::string& error);
// Keys: summary (default `defaultSummary`), edges, latency ("1->2"), expect ("p99_ns<=60,drops==0"),
// invariant ("never(A & B); within(A, B, 100ns)").
// Returns nullopt and sets `error` if `latency`, `expect` or `invariant` is malformed or references uncaptured channels.
std::optional<DocumentOptions> documentOptionsFromJson(const nlohmann::json& params, bool defaultSummary,
                                                       int deviceChannels,
                                                       const std::vector<ChannelInfo>& capturedChannels,
                                                       std::string& error);
DocumentOptions documentOptionsFromJson(const nlohmann::json& params, bool defaultSummary);

struct ProbeParams {
    std::vector<ChannelInfo> channels;  // empty = every channel of the device
    std::chrono::milliseconds window{10};
    uint32_t rate = 1'000'000;
};
// Keys: channels, window_ms, rate.
std::optional<ProbeParams> probeParamsFromJson(const nlohmann::json& params, int deviceChannels, std::string& error);

// Opens `port` (a serial port or "replay:<file>"), or the first capture device found when empty.
// Returns nullptr and fills `error` on failure.
[[nodiscard]] std::unique_ptr<IDevice> openDevice(const std::string& port, std::chrono::milliseconds timeout,
                                                  std::string& error);

}  // namespace lt

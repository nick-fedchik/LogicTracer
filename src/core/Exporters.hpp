// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/AiReport.hpp"
#include "core/CaptureBuffer.hpp"
#include "core/CaptureTypes.hpp"
#include "core/DeviceProtocol.hpp"
#include "core/SignalAnalyzer.hpp"

#include <nlohmann/json.hpp>

#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

inline constexpr int kCaptureSchemaVersion = 1;

[[nodiscard]] nlohmann::json toJson(const DeviceInfo& info);
// Per-channel analytics (times in microseconds, relative to the trigger).
[[nodiscard]] nlohmann::json toJson(const ChannelStats& stats, bool includeEdges = false,
                                    std::string_view board = {});
// Cross-channel STIM -> REACT propagation latency summary (nanoseconds).
[[nodiscard]] nlohmann::json toJson(const LatencyStats& latency, bool includeEvents = false);
// Capture metadata + analytics for every channel (lossless when includeEdges == true).
[[nodiscard]] nlohmann::json captureToJson(const CaptureBuffer& buffer, const std::vector<ChannelStats>& stats,
                                           bool includeEdges = false, std::string_view board = {});

// Value Change Dump for GTKWave / PulseView. Time 0 is the first sample; the trigger position is noted.
void writeVcd(std::ostream& out, const CaptureBuffer& buffer);
// File writers are atomic (temp file + rename) and report flush/close errors. `confined` (for paths returned
// by resolveSafeOutputPath) refuses any symlink in the directory path at write time.
bool writeVcdFile(const std::string& path, const CaptureBuffer& buffer, std::string* error = nullptr,
                  bool confined = false);
bool writeJsonFile(const std::string& path, const nlohmann::json& doc, std::string* error = nullptr,
                   bool confined = false);

// Raw sample table (SampleIndex,TimeRelativeNs,<ch1>,...) and paired STIM->REACT events CSV.
void writeCsv(std::ostream& out, const CaptureBuffer& buffer);
bool writeCsvFile(const std::string& path, const CaptureBuffer& buffer, std::string* error = nullptr,
                  bool confined = false);
void writeEventsCsv(std::ostream& out, const std::vector<LatencyEvent>& events);
bool writeEventsCsvFile(const std::string& path, const std::vector<LatencyEvent>& events, std::string* error = nullptr,
                        bool confined = false);

struct LoadedCapture {
    CaptureBuffer buffer;
    std::string triggerDescription;
    std::vector<Annotation> annotations;
    SetupInfo setup;
    // Identity of the device that produced the capture, when the file records it ("device" object).
    std::string deviceVersion;
    std::string board;
    // Non-fatal import problems the user must see (e.g. unknown x/z levels, coarsened time base).
    std::vector<std::string> warnings;
    // Segmented capture ("segments.layout" in a CLI/MCP envelope): analyse window by window.
    std::optional<SegmentLayout> segments;
};

// SetupInfo <-> JSON object {"key": "value", ...}. Invalid entries are skipped when reading.
[[nodiscard]] nlohmann::json toJson(const SetupInfo& setup);
[[nodiscard]] SetupInfo setupFromJson(const nlohmann::json& object);

// Loads a lossless JSON capture (direct captureToJson or CLI/MCP envelope) or a VCD waveform.
bool readCaptureJson(const nlohmann::json& doc, LoadedCapture& out, std::string* error = nullptr);
bool readVcd(std::istream& in, LoadedCapture& out, std::string* error = nullptr);
bool loadCaptureFile(const std::string& path, LoadedCapture& out, std::string* error = nullptr);

// Validates an untrusted output path (MCP / IPC session): enforces allowed file extension, rejects
// '..' traversal, hidden/dot segments and symlinks, and confines to `sandboxDir` (or cwd + temp dir
// when `sandboxDir` is empty). Returns the normalized path or nullopt on rejection.
[[nodiscard]] std::optional<std::string> resolveSafeOutputPath(const std::string& rawPath,
                                                               std::span<const std::string_view> allowedExtensions,
                                                               const std::string& sandboxDir,
                                                               std::string* error = nullptr);

}  // namespace lt

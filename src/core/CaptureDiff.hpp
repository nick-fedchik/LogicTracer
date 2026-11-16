// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Compares two captures of the same test (baseline A, candidate B): per-channel activity, the STIM -> REACT
// latency distribution and the anomaly incidents. The verdict separates real changes from differences that
// the +-1 sample quantization of each capture can explain.

#include "core/CaptureBuffer.hpp"
#include "core/SignalAnalyzer.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lt {

struct DiffOptions {
    // Latency pair by hardware channel number (0-based), resolved in each capture independently.
    struct Latency {
        uint8_t stimChannel = 0;
        bool stimFalling = false;
        uint8_t reactChannel = 1;
        bool reactFalling = false;
    };
    std::optional<Latency> latency;
};

// Machine-readable comparison: {"a": ..., "b": ..., "channels": [...], "latency": {...}, "anomalies": {...},
// "regressions": [...], "verdict": "..."}. Channels are matched by hardware number.
[[nodiscard]] nlohmann::json diffCaptures(const CaptureBuffer& a, const CaptureBuffer& b, const DiffOptions& options);

// Short Markdown rendering of diffCaptures() for people and chat agents.
[[nodiscard]] std::string diffToMarkdown(const nlohmann::json& diff, const std::string& nameA, const std::string& nameB);

}  // namespace lt

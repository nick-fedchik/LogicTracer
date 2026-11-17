// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Product identity shown by `logictracer-cli about` and the GUI About dialog.

namespace lt::about {

inline constexpr const char* kProduct = "LogicTracer";
inline constexpr const char* kSummary =
    "Logic analyzer host and RP2040 firmware for hardware-in-the-loop verification: engineers and AI agents see "
    "the same data, every timing figure comes with its measurement bounds.";
inline constexpr const char* kAuthor = "Mykola Fedchyk";
inline constexpr const char* kRepository = "https://github.com/nick-fedchik/LogicTracer";
inline constexpr const char* kIssues = "https://github.com/nick-fedchik/LogicTracer/issues";
inline constexpr const char* kLicense = "GPL-3.0-or-later (firmware: GPL-3.0)";
inline constexpr const char* kUpstream =
    "Firmware derived from LogicAnalyzer by Agustin Gimenez Bernad (https://github.com/gusmanb/logicanalyzer); "
    "not affiliated with that project.";

}  // namespace lt::about

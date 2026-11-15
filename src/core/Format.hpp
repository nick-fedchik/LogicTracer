// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace lt {

// Human-readable time with an automatically chosen unit ("1.5 us", "20 ms").
// Locale-independent number text: QApplication installs the user's C locale, which would turn
// std::to_string(1.31) into "1,310000".
inline std::string formatNumber(double value, int precision = 4)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(precision) << value;
    return ss.str();
}

inline std::string formatTime(double seconds)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(4);
    const double a = std::abs(seconds);
    if (a == 0.0)
        ss << "0 s";
    else if (a < 1e-6)
        ss << seconds * 1e9 << " ns";
    else if (a < 1e-3)
        ss << seconds * 1e6 << " us";
    else if (a < 1.0)
        ss << seconds * 1e3 << " ms";
    else
        ss << seconds << " s";
    return ss.str();
}

inline std::string formatHz(double hz)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(6);
    if (hz >= 1e6)
        ss << hz / 1e6 << " MHz";
    else if (hz >= 1e3)
        ss << hz / 1e3 << " kHz";
    else
        ss << hz << " Hz";
    return ss.str();
}

}  // namespace lt

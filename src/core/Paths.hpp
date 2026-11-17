// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Paths travel through the program as UTF-8 std::string (Qt's toStdString, JSON, command lines). On
// Windows std::filesystem::path(std::string) would read them in the ANSI code page, and path::string()
// can throw for characters outside it, so conversions go through these helpers.

#include <filesystem>
#include <string>
#include <string_view>

namespace lt {

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8)
{
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

[[nodiscard]] inline std::string utf8FromPath(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const std::u8string u = path.u8string();
    return std::string(u.begin(), u.end());
#else
    return path.string();
#endif
}

}  // namespace lt

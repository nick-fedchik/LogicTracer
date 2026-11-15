// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <string>
#include <string_view>

namespace lt {

// Copies UTF-8 text to the desktop clipboard from a headless CLI process.
// Uses wl-copy on Wayland and xclip/xsel on X11 (redirecting stdout/stderr to /dev/null so
// forked clipboard daemons do not block). For hermetic tests, setting
// LOGICTRACER_CLIPBOARD_FILE writes the text to that file instead.
bool copyTextToClipboard(std::string_view text, std::string* backendUsed = nullptr, std::string* error = nullptr);

}  // namespace lt

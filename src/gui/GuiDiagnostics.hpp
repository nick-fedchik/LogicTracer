// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <QString>

#include <memory>

namespace lt::gui {

// Owns the GUI process's stderr tee and Qt message hook. Install before QApplication, not in
// the CLI or MCP server. The only read API returns a sanitized, bounded in-memory snapshot.
class GuiDiagnostics {
public:
    static constexpr int MaxLines = 100;
    static constexpr int MaxBytes = 16 * 1024;

    struct Preview {
        QString text;
        bool omitted = false;
    };

    GuiDiagnostics();
    ~GuiDiagnostics();
    GuiDiagnostics(const GuiDiagnostics&) = delete;
    GuiDiagnostics& operator=(const GuiDiagnostics&) = delete;

    [[nodiscard]] Preview preview() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace lt::gui

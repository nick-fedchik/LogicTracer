// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "gui/GuiDiagnostics.hpp"

#include <QDialog>

class QCheckBox;
class QPlainTextEdit;

namespace lt::gui {

class DiagnosticsDialog : public QDialog {
    Q_OBJECT

public:
    DiagnosticsDialog(GuiDiagnostics& diagnostics, QWidget* parent);
    void openPreview();
    [[nodiscard]] const GuiDiagnostics::Preview* sharedPreview() const;

Q_SIGNALS:
    void copyWithPreview(const QString& text);
    void sharingChanged(bool shared);

private:
    void refresh();
    GuiDiagnostics& m_diagnostics;
    GuiDiagnostics::Preview m_preview;
    QPlainTextEdit* m_text;
    QCheckBox* m_share;
};

}  // namespace lt::gui

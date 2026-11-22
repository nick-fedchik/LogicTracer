// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/DiagnosticsDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lt::gui {

DiagnosticsDialog::DiagnosticsDialog(GuiDiagnostics& diagnostics, QWidget* parent)
    : QDialog(parent), m_diagnostics(diagnostics), m_text(new QPlainTextEdit), m_share(new QCheckBox)
{
    setWindowTitle(tr("GUI diagnostics for Ask AI"));
    resize(680, 360);
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Recent stderr from this GUI only, not capture status. Review before sharing: "
                               "common credentials and addresses are masked, but other private text may remain."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_text->setObjectName("guiDiagnosticsPreview");
    m_text->setReadOnly(true);
    m_text->setPlaceholderText(tr("No recent GUI diagnostics."));
    layout->addWidget(m_text);
    m_share->setObjectName("shareGuiDiagnostics");
    m_share->setText(tr("Allow GUI-session / MCP clients to read this preview"));
    m_share->setToolTip(tr("Only this snapshot is shared, until unchecked, refreshed, or the GUI closes."));
    layout->addWidget(m_share);
    connect(m_share, &QCheckBox::toggled, this, &DiagnosticsDialog::sharingChanged);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* refreshButton = buttons->addButton(tr("Refresh preview"), QDialogButtonBox::ActionRole);
    auto* copy = buttons->addButton(tr("Copy report + this preview"), QDialogButtonBox::ActionRole);
    copy->setObjectName("copyReportWithDiagnostics");
    refreshButton->setObjectName("refreshGuiDiagnostics");
    connect(refreshButton, &QPushButton::clicked, this, &DiagnosticsDialog::refresh);
    connect(copy, &QPushButton::clicked, this, [this] { Q_EMIT copyWithPreview(m_preview.text); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void DiagnosticsDialog::refresh()
{
    m_share->setChecked(false);  // never grant consent for text the operator hasn't seen
    m_preview = m_diagnostics.preview();
    m_text->setPlainText(m_preview.text);
}

void DiagnosticsDialog::openPreview()
{
    if (!m_share->isChecked())
        refresh();
    show();
    raise();
    activateWindow();
}

const GuiDiagnostics::Preview* DiagnosticsDialog::sharedPreview() const
{
    return m_share->isChecked() ? &m_preview : nullptr;
}

}  // namespace lt::gui

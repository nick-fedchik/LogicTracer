// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "gui/CaptureFields.hpp"

#include <QFutureWatcher>
#include <QStringList>
#include <QWidget>
#include <functional>

class QPushButton;

namespace lt::gui {

class ProfileCombo;

class ProfileSelector : public QWidget {
    Q_OBJECT

public:
    ProfileSelector(std::function<CaptureFields()> read, std::function<void(const CaptureFields&)> apply,
                    QWidget* parent);
    [[nodiscard]] QString selectedPath() const { return m_path; }
    void setSelectedPath(const QString& path);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    void applySelection();
    void loadProfile(const QString& nameOrPath);
    void askName(bool save);
    void deleteSelected();
    void showError(const QString& error);

    std::function<CaptureFields()> m_read;
    std::function<void(const CaptureFields&)> m_apply;
    ProfileCombo* m_combo;
    QPushButton* m_delete;
    QString m_path;
    QFutureWatcher<QStringList> m_scan;
    bool m_scanning = false;
    bool m_rescan = false;
};

}  // namespace lt::gui

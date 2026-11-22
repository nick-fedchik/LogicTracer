// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/CaptureBuffer.hpp"

#include <QDialog>
#include <nlohmann/json_fwd.hpp>

#include <array>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTabBar;
class QToolButton;

namespace lt::gui {

struct PicoWiring {
    std::string board;
    uint64_t captureId = 0;  // zero = next capture setup
    bool offlineReference = false;
    bool simulated = false;
    int inputCount = 24;
    std::vector<ChannelInfo> channels;
    QString error;

    [[nodiscard]] nlohmann::json document() const;
    [[nodiscard]] QString description() const;
};

class PicoPinout : public QDialog {
    Q_OBJECT

public:
    explicit PicoPinout(QWidget* settings, QWidget* parent);
    [[nodiscard]] bool showsCapture() const;
    void selectSource(bool capture);
    [[nodiscard]] uint64_t captureId() const { return m_wiring.captureId; }
    void setWiring(PicoWiring wiring, bool busy, bool deviceSettings);
    void setTriggerText(const QString& text);
    bool focusTrigger();
    [[nodiscard]] std::vector<ChannelInfo> editedChannels() const;

Q_SIGNALS:
    void sourceChanged();
    void applyRequested();
    void triggerEdited(const QString& text);

private:
    void validate();
    QWidget* makeTriggerButtons(int index);
    std::array<std::array<QToolButton*, 4>, 25> m_triggerButtons{};
    std::array<QWidget*, 25> m_triggerGroups{};
    QTabBar* m_source;
    QLabel* m_context;
    QWidget* m_settings;
    QLabel* m_orientation;
    QLabel* m_safety;
    QScrollArea* m_scroll;
    std::array<QPushButton*, 24> m_inputs{};
    std::array<QLineEdit*, 24> m_names{};
    QLabel* m_hint;
    QPushButton* m_apply;
    QPushButton* m_copy;
    PicoWiring m_wiring;
    bool m_loading = false;
    bool m_busy = false;
};

}  // namespace lt::gui

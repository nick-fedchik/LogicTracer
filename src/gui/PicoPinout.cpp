// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/PicoPinout.hpp"
#include "core/ArgParsing.hpp"
#include "gui/Builders.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace lt::gui {
namespace {

constexpr char kOrientation[] = "Component side up, USB at the top: pin 1 is top left, pin 40 top right.";
constexpr char kSafety[] = "Use 3.3 V logic and a common GND (e.g. pin 3). Never probe 5 V, CAN_H or CAN_L directly.";

const ChannelInfo* findInput(const std::vector<ChannelInfo>& channels, uint8_t number)
{
    const auto it = std::find_if(channels.begin(), channels.end(), [number](const auto& c) { return c.number == number; });
    return it == channels.end() ? nullptr : &*it;
}

}  // namespace

QString PicoWiring::description() const
{
    if (!error.isEmpty())
        return error;
    if (captureId)
        return QObject::tr("Displayed capture #%1 (%2). This describes the recording, not the next setup.")
            .arg(captureId).arg(QString::fromStdString(board));
    if (simulated)
        return QObject::tr("Demo Pico (simulated). No physical device or wiring is involved.");
    if (offlineReference)
        return QObject::tr("Offline Pico reference. Confirm your board matches this header before connecting wires.");
    return QObject::tr("Next capture on connected board %1. These assignments are a plan, not verified connections.")
        .arg(QString::fromStdString(board));
}

nlohmann::json PicoWiring::document() const
{
    auto pins = nlohmann::json::array();
    for (const auto& pin : picoHeaderPins(board)) {
        nlohmann::json row{{"pin", pin.pin}};
        if (pin.gpio)
            row["gpio"] = *pin.gpio;
        if (!pin.function.empty())
            row["function"] = pin.function;
        if (pin.channel && *pin.channel < inputCount) {
            row["channel"] = *pin.channel + 1;
            const auto* ch = findInput(channels, *pin.channel);
            row["selected"] = ch != nullptr;
            if (ch && !ch->name.empty() && validChannelName(ch->name))
                row["name"] = ch->name;
        }
        pins.push_back(std::move(row));
    }
    nlohmann::json doc{{"board", board}, {"source", captureId ? "capture" : offlineReference ? "offline_reference" : "setup"},
                       {"orientation", kOrientation}, {"safety", kSafety}, {"pins", std::move(pins)}};
    if (captureId)
        doc["capture_id"] = captureId;
    return doc;
}

PicoPinout::PicoPinout(QWidget* settings, QWidget* parent) : QDialog(parent), m_settings(settings)
{
    setObjectName("picoPinout");
    setWindowTitle(tr("Pico pinout and wiring"));
    auto* layout = new QVBoxLayout(this);
    m_source = new QTabBar;
    m_source->setObjectName("pinoutSource");
    m_source->setAccessibleName(tr("Pinout view"));
    m_source->setExpanding(false);
    m_source->setUsesScrollButtons(false);
    m_source->addTab(tr("Next capture setup"));
    m_source->addTab(tr("Displayed capture"));
    layout->addWidget(m_source);
    m_context = new QLabel;
    m_context->setObjectName("wiringContext");
    m_context->setTextFormat(Qt::PlainText);
    m_context->setWordWrap(true);
    layout->addWidget(m_context);
    layout->addWidget(m_settings);
    m_orientation = new QLabel;
    m_orientation->setWordWrap(true);
    layout->addWidget(m_orientation);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    auto* contents = new QWidget;
    auto* grid = new QGridLayout(contents);
    grid->setSpacing(2);
    enum Column { LeftTrigger, LeftName, LeftChannel, LeftPad, Center, RightPad, RightChannel, RightName, RightTrigger };
    contents->setStyleSheet(QStringLiteral(
        "QFrame#picoBoard { background: #215343; border-radius: 9px; }"
        "QLabel#picoUsb { background: #bec7c9; color: #182323; padding: 2px 8px; border-radius: 2px; }"
        "QLabel#picoMcu { background: #252c30; color: white; padding: 10px 5px; border-radius: 3px; }"
        "QLabel[fixedContact=\"true\"] { color: #efdeaf; padding: 0 5px; }"
        "QPushButton { background: #254438; color: #fff2cb; border: 1px solid #b6a77f;"
        "              border-radius: 4px; padding: 2px 5px; margin: 0 2px; }"
        "QPushButton:disabled { color: #b4c1b9; border-color: #70867a; }"
        "QPushButton:checked { background: #74edbd; color: #10291e; border-color: #bcffdf; }"
        "QPushButton:focus { border-style: dashed; }"));
    auto* face = new QFrame;
    face->setObjectName("picoBoard");
    face->setAccessibleName(tr("Pico header, component side, USB at top"));
    face->setAttribute(Qt::WA_TransparentForMouseEvents);
    grid->addWidget(face, 0, LeftPad, 20, 3);
    auto* usb = new QLabel("USB");
    usb->setObjectName("picoUsb");
    grid->addWidget(usb, 0, Center, Qt::AlignHCenter | Qt::AlignTop);
    auto* chip = new QLabel("MCU");
    chip->setObjectName("picoMcu");
    grid->addWidget(chip, 9, Center, 2, 1, Qt::AlignCenter);
    int padWidth = 0;
    for (const auto& pin : picoHeaderPins("PICO")) {
        const bool left = pin.pin <= 20;
        const int row = left ? pin.pin - 1 : 40 - pin.pin;
        grid->setRowStretch(row, 1);
        const QString role = QString::fromLatin1(pin.function.data(), qsizetype(pin.function.size()));
        const QString pinName = pin.gpio ? QString("GP%1").arg(*pin.gpio) : role;
        const QString number = QString::number(pin.pin);
        const QString padText = left ? number + " | " + pinName : pinName + " | " + number;
        const auto outerEdge = Qt::AlignVCenter | (left ? Qt::AlignLeft : Qt::AlignRight);
        const auto innerEdge = Qt::AlignVCenter | (left ? Qt::AlignRight : Qt::AlignLeft);
        if (!pin.channel) {
            auto* label = new QLabel(padText);
            label->setProperty("fixedContact", true);
            grid->addWidget(label, row, left ? LeftPad : RightPad, outerEdge);
            padWidth = std::max(padWidth, label->sizeHint().width());
            if (pin.gpio && !role.isEmpty())
                grid->addWidget(new QLabel(role), row, left ? LeftName : RightChannel, 1, 2, innerEdge);
            if (pin.function == "EXT TRIG")
                grid->addWidget(makeTriggerButtons(24), row, LeftTrigger);
            continue;
        }
        const uint8_t ch = *pin.channel;
        const QString contact = tr("Header pin %1, %2, channel %3").arg(pin.pin).arg(pinName).arg(ch + 1);
        auto* pad = m_inputs[ch] = new QPushButton(padText);
        pad->setObjectName(QString("pinPad%1").arg(pin.pin));
        pad->setCheckable(true);
        pad->setAutoDefault(false);
        pad->setAccessibleName(contact);
        pad->setToolTip(contact);
        grid->addWidget(pad, row, left ? LeftPad : RightPad, outerEdge);
        padWidth = std::max(padWidth, pad->sizeHint().width());
        grid->addWidget(new QLabel(QString("CH%1").arg(ch + 1)), row, left ? LeftChannel : RightChannel, innerEdge);
        auto* name = m_names[ch] = new QLineEdit;
        name->setObjectName(QString("pinName%1").arg(pin.pin));
        name->setAccessibleName(tr("Signal name for %1").arg(contact));
        name->setToolTip(QString::fromLatin1(kChannelNameRule.data()));
        grid->addWidget(name, row, left ? LeftName : RightName);
        grid->addWidget(makeTriggerButtons(ch), row, left ? LeftTrigger : RightTrigger);
        connect(pad, &QPushButton::toggled, this, &PicoPinout::validate);
        connect(name, &QLineEdit::textChanged, this, &PicoPinout::validate);
    }
    grid->setColumnMinimumWidth(LeftPad, padWidth);
    grid->setColumnMinimumWidth(RightPad, padWidth);
    grid->setColumnStretch(LeftName, 1);
    grid->setColumnStretch(RightName, 1);
    m_scroll->setWidget(contents);
    layout->addWidget(m_scroll, 1);
    m_safety = new QLabel(tr(kSafety));
    m_safety->setWordWrap(true);
    layout->addWidget(m_safety);
    m_hint = new QLabel;
    m_hint->setTextFormat(Qt::PlainText);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);
    auto* buttons = new QHBoxLayout;
    m_copy = new QPushButton(tr("Copy wiring for AI"));
    m_copy->setObjectName("copyWiring");
    m_apply = new QPushButton(tr("Apply pin edits"));
    m_apply->setObjectName("applyWiring");
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(m_copy);
    buttons->addStretch(1);
    buttons->addWidget(m_apply);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(m_source, &QTabBar::currentChanged, this, &PicoPinout::sourceChanged);
    connect(m_apply, &QPushButton::clicked, this, &PicoPinout::applyRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QStringList lines{m_wiring.description(), tr(kOrientation), tr(kSafety)};
        const auto doc = m_wiring.document();
        for (const auto& pin : doc.at("pins"))
            if (pin.value("selected", false))
                lines << tr("Pin %1 | GP%2 | CH%3 | %4").arg(pin.at("pin").get<int>()).arg(pin.at("gpio").get<int>())
                             .arg(pin.at("channel").get<int>()).arg(QString::fromStdString(pin.value("name", std::string{})));
        QApplication::clipboard()->setText(lines.join('\n'));
    });
    resize(750, 720);
}

bool PicoPinout::showsCapture() const { return m_source->currentIndex() == 1; }

QWidget* PicoPinout::makeTriggerButtons(int index)
{
    auto* group = m_triggerGroups[index] = new QWidget;
    auto* row = new QHBoxLayout(group);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    const QStringList labels{QStringLiteral("↑"), QStringLiteral("↓"), "0", "1"};
    const QStringList actions{tr("rising edge"), tr("falling edge"), tr("pattern low"), tr("pattern high")};
    const auto source = index == 24 ? tr("EXT TRIG") : QString("CH%1").arg(index + 1);
    for (int action = 0; action < (index < 16 ? 4 : 2); ++action) {
        auto* button = m_triggerButtons[index][action] = new QToolButton;
        button->setObjectName(QString("pinTrigger%1_%2").arg(index + 1).arg(action));
        button->setText(labels[action]);
        button->setAccessibleName(tr("%1 trigger: %2").arg(source, actions[action]));
        button->setToolTip(button->accessibleName());
        button->setCheckable(true);
        row->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, index, action](bool checked) {
            if (action < 2) {
                const int channel = index == 24 ? m_wiring.inputCount + 1 : index + 1;
                Q_EMIT triggerEdited(checked ? composeTrigger(channel, action == 1) : QStringLiteral("none"));
                return;
            }
            m_triggerButtons[index][action == 2 ? 3 : 2]->setChecked(false);
            QStringList levels;
            for (int i = 0; i < std::min(16, m_wiring.inputCount); ++i)
                for (int level = 0; level <= 1; ++level)
                    if (m_triggerButtons[i][2 + level]->isChecked())
                        levels << QString("%1=%2").arg(i + 1).arg(level);
            Q_EMIT triggerEdited(levels.isEmpty() ? QStringLiteral("none") : levels.join(','));
        });
    }
    return group;
}

void PicoPinout::setTriggerText(const QString& text)
{
    for (const auto& row : m_triggerButtons)
        for (auto* button : row)
            if (button)
                button->setChecked(false);
    // A pattern can have a gap while the operator is selecting it. Keep both sides visible
    // and let the shared parser explain the gap rather than silently discarding either pin.
    for (const auto& clause : text.split(',')) {
        const auto value = cli::parseTrigger(clause.toStdString(), 25, nullptr);
        if (!value || value->none)
            continue;
        const auto& t = value->trigger;
        if (t.isPattern()) {
            for (int i = 0; i < t.bitCount; ++i)
                m_triggerButtons[t.channel + i][2 + ((t.pattern >> i) & 1)]->setChecked(true);
        } else {
            const int index = t.channel == m_wiring.inputCount ? 24 : t.channel;
            m_triggerButtons[index][t.inverted ? 1 : 0]->setChecked(true);
        }
    }
}

bool PicoPinout::focusTrigger()
{
    for (const auto& row : m_triggerButtons)
        for (auto* button : row)
            if (button && button->isVisible() && button->isEnabled()) {
                m_scroll->ensureWidgetVisible(button);
                button->setFocus(Qt::OtherFocusReason);
                return true;
            }
    return false;
}

void PicoPinout::selectSource(bool capture) { m_source->setCurrentIndex(capture ? 1 : 0); }

void PicoPinout::setWiring(PicoWiring wiring, bool busy, bool deviceSettings)
{
    m_wiring = std::move(wiring);
    m_busy = busy;
    m_context->setText(m_wiring.description());
    m_settings->setVisible(deviceSettings && !showsCapture());
    m_settings->setEnabled(!busy);
    const bool hasHeader = !picoHeaderPins(m_wiring.board).empty();
    setWindowTitle(!showsCapture() && deviceSettings && !hasHeader ? tr("Device capture setup") : tr("Pico pinout and wiring"));
    m_orientation->setText(tr(kOrientation) + '\n' + (showsCapture()
        ? tr("Green pins belong to this recording.")
        : tr("Click a numbered pin to select or deselect it. Green pins are selected; Apply pin edits updates Channels.")));
    m_orientation->setVisible(hasHeader);
    m_safety->setVisible(hasHeader);
    m_apply->setVisible(hasHeader);
    m_copy->setVisible(hasHeader);
    m_scroll->setVisible(m_wiring.error.isEmpty());
    const bool plan = hasHeader && deviceSettings && !showsCapture();
    for (int i = 0; i <= 24; ++i) {
        m_triggerGroups[i]->setVisible(plan && (i == 24 ? !m_wiring.simulated : i < m_wiring.inputCount));
        m_triggerGroups[i]->setEnabled(!busy);
    }
    m_loading = true;
    for (uint8_t i = 0; i < m_inputs.size(); ++i) {
        const auto* ch = findInput(m_wiring.channels, i);
        m_inputs[i]->setChecked(ch != nullptr);
        m_inputs[i]->setEnabled(!busy && !showsCapture() && i < m_wiring.inputCount);
        const bool legacy = ch && !validChannelName(ch->name);
        m_names[i]->setText(ch && !legacy ? QString::fromStdString(ch->name) : QString{});
        m_names[i]->setPlaceholderText(legacy ? tr("Legacy label") : tr("Signal name"));
    }
    m_loading = false;
    validate();
}

std::vector<ChannelInfo> PicoPinout::editedChannels() const
{
    std::vector<ChannelInfo> out;
    // Renaming must not reorder the sampled bits. Append newly enabled channels after existing ones.
    for (auto ch : m_wiring.channels) {
        if (!m_inputs[ch.number]->isChecked())
            continue;
        auto* name = m_names[ch.number];
        if (validChannelName(ch.name) || name->isModified() || !name->text().isEmpty())
            ch.name = name->text().toStdString();
        out.push_back(std::move(ch));
    }
    for (uint8_t i = 0; i < m_inputs.size(); ++i)
        if (m_inputs[i]->isChecked() && !findInput(m_wiring.channels, i))
            out.push_back({i, m_names[i]->text().toStdString()});
    return out;
}

void PicoPinout::validate()
{
    if (m_loading)
        return;
    QString problem;
    bool any = false;
    bool dirty = false;
    for (uint8_t i = 0; i < m_names.size(); ++i) {
        const bool enabled = m_inputs[i]->isChecked();
        m_names[i]->setEnabled(enabled && !m_busy);
        const auto* ch = findInput(m_wiring.channels, i);
        const auto name = m_names[i]->text().toStdString();
        if (enabled && !validChannelName(name))
            problem = tr("Invalid CH%1 name. ").arg(i + 1) + QString::fromLatin1(kChannelNameRule.data());
        any |= enabled;
        dirty |= enabled != (ch != nullptr) || (enabled && ch &&
                 (validChannelName(ch->name) ? name != ch->name : !name.empty() || m_names[i]->isModified()));
    }
    if (!any && problem.isEmpty())
        problem = tr("Select at least one capture input.");
    m_hint->setText(!m_wiring.error.isEmpty() ? QString{} :
                    !problem.isEmpty() ? problem : QString::fromLatin1(kChannelNameRule.data()));
    m_apply->setEnabled(m_wiring.error.isEmpty() && !m_busy && dirty && problem.isEmpty());
    m_copy->setEnabled(m_wiring.error.isEmpty() && !dirty && problem.isEmpty());
}

}  // namespace lt::gui

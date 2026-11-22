// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/Builders.hpp"

#include "core/ArgParsing.hpp"
#include "core/DeviceProtocol.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>

namespace lt::gui {

namespace {

constexpr int kNoChannel = 0;

QFrame* makePopup(QWidget* anchor, const char* objectName)
{
    auto* frame = new QFrame(anchor->window(), Qt::Popup);
    frame->setObjectName(objectName);
    frame->setAttribute(Qt::WA_DeleteOnClose);
    frame->setFrameShape(QFrame::StyledPanel);
    return frame;
}

void placeBelow(QFrame* frame, QWidget* anchor)
{
    frame->adjustSize();
    frame->move(anchor->mapToGlobal(QPoint(0, anchor->height())));
    frame->show();
}

QPushButton* addApply(QVBoxLayout* layout, QFrame* frame, const std::function<void()>& apply)
{
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* cancel = new QPushButton(QObject::tr("Cancel"));
    cancel->setObjectName("cancel");
    auto* ok = new QPushButton(QObject::tr("Apply"));
    ok->setObjectName("apply");
    ok->setDefault(true);
    row->addWidget(cancel);
    row->addWidget(ok);
    layout->addLayout(row);
    QObject::connect(cancel, &QPushButton::clicked, frame, &QWidget::close);
    QObject::connect(ok, &QPushButton::clicked, frame, [frame, apply] {
        apply();
        frame->close();
    });
    return ok;
}

QComboBox* edgeCombo(bool falling)
{
    auto* c = new QComboBox;
    c->addItem(QObject::tr("rising"), false);
    c->addItem(QObject::tr("falling"), true);
    c->setCurrentIndex(falling ? 1 : 0);
    return c;
}

QComboBox* channelCombo(const std::vector<int>& channels, int current)
{
    auto* c = new QComboBox;
    for (const int ch : channels)
        c->addItem(QString("CH%1").arg(ch), ch);
    const int idx = c->findData(current);
    if (idx >= 0)
        c->setCurrentIndex(idx);
    return c;
}

}  // namespace

QString composeChannelList(const std::vector<std::pair<int, QString>>& channels)
{
    QStringList tokens;
    for (std::size_t i = 0; i < channels.size();) {
        if (!channels[i].second.isEmpty()) {
            tokens << QString("%1:%2").arg(channels[i].first).arg(channels[i].second);
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < channels.size() && channels[j + 1].second.isEmpty() &&
               channels[j + 1].first == channels[j].first + 1)
            ++j;
        if (j - i >= 2)
            tokens << QString("%1-%2").arg(channels[i].first).arg(channels[j].first);
        else
            for (std::size_t k = i; k <= j; ++k)
                tokens << QString::number(channels[k].first);
        i = j + 1;
    }
    return tokens.join(',');
}

QString composeTrigger(int channel, bool falling)
{
    if (channel == kNoChannel)
        return "none";
    return QString("%1:%2").arg(channel).arg(falling ? "falling" : "rising");
}

QString composeLatency(int stimChannel, bool stimFalling, int reactChannel, bool reactFalling)
{
    if (!stimFalling && !reactFalling)
        return QString("%1..%2").arg(stimChannel).arg(reactChannel);
    return QString("%1:%2..%3:%4")
        .arg(stimChannel)
        .arg(stimFalling ? "falling" : "rising")
        .arg(reactChannel)
        .arg(reactFalling ? "falling" : "rising");
}

QFrame* showChannelsBuilder(QWidget* anchor, QLineEdit* target, const std::string& board, int maxChannels)
{
    auto* frame = makePopup(anchor, "channelsBuilder");
    auto* layout = new QVBoxLayout(frame);
    layout->addWidget(new QLabel(QObject::tr("Pick the channels to capture and optionally name them.")));

    std::map<int, QString> initial;
    if (const auto parsed = lt::cli::parseChannelList(target->text().toStdString(), maxChannels, nullptr))
        for (const auto& ch : *parsed)
            initial[ch.number + 1] = QString::fromStdString(ch.name);

    auto* grid = new QGridLayout;
    constexpr int kPerRow = 4;
    std::vector<QCheckBox*> boxes;
    std::vector<QLineEdit*> names;
    for (int n = 1; n <= maxChannels; ++n) {
        QString title = QString("CH%1").arg(n);
        if (!board.empty())
            if (const auto gp = lt::gpioForChannel(board, static_cast<uint8_t>(n - 1)))
                title += QString(" (GP%1)").arg(static_cast<int>(*gp));
        auto* box = new QCheckBox(title);
        box->setObjectName(QString("ch%1").arg(n));
        box->setChecked(initial.count(n) != 0);
        auto* name = new QLineEdit(initial.count(n) ? initial[n] : QString{});
        name->setObjectName(QString("name%1").arg(n));
        name->setPlaceholderText(QObject::tr("name"));
        name->setToolTip(QString::fromLatin1(kChannelNameRule.data()));
        name->setMaximumWidth(90);
        name->setEnabled(box->isChecked());
        QObject::connect(box, &QCheckBox::toggled, name, &QWidget::setEnabled);
        const int row = (n - 1) / kPerRow, col = ((n - 1) % kPerRow) * 2;
        grid->addWidget(box, row, col);
        grid->addWidget(name, row, col + 1);
        boxes.push_back(box);
        names.push_back(name);
    }
    layout->addLayout(grid);

    auto* quick = new QHBoxLayout;
    auto* all = new QPushButton(QObject::tr("All"));
    auto* none = new QPushButton(QObject::tr("Clear"));
    quick->addWidget(all);
    quick->addWidget(none);
    quick->addStretch(1);
    layout->addLayout(quick);
    QObject::connect(all, &QPushButton::clicked, frame, [boxes] {
        for (auto* b : boxes)
            b->setChecked(true);
    });
    QObject::connect(none, &QPushButton::clicked, frame, [boxes] {
        for (auto* b : boxes)
            b->setChecked(false);
    });

    const QPointer<QLineEdit> field(target);
    auto* hint = new QLabel(QString::fromLatin1(kChannelNameRule.data()));
    layout->addWidget(hint);
    auto* apply = addApply(layout, frame, [field, boxes, names] {
        std::vector<std::pair<int, QString>> picked;
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i]->isChecked())
                picked.emplace_back(static_cast<int>(i) + 1, names[i]->text());
        if (field && !picked.empty())
            field->setText(composeChannelList(picked));
    });
    const auto validate = [apply, boxes, names] {
        bool any = false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (!boxes[i]->isChecked())
                continue;
            any = true;
            if (!validChannelName(names[i]->text().toStdString())) {
                apply->setEnabled(false);
                return;
            }
        }
        apply->setEnabled(any);
    };
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        QObject::connect(boxes[i], &QCheckBox::toggled, frame, validate);
        QObject::connect(names[i], &QLineEdit::textChanged, frame, validate);
    }
    validate();
    placeBelow(frame, anchor);
    return frame;
}

QFrame* showLatencyBuilder(QWidget* anchor, QLineEdit* target, const std::vector<int>& channels)
{
    auto* frame = makePopup(anchor, "latencyBuilder");
    auto* layout = new QVBoxLayout(frame);

    int stim = channels.empty() ? 1 : channels.front();
    int react = channels.size() > 1 ? channels[1] : stim;
    bool stimFalling = false, reactFalling = false;
    if (const auto parsed = lt::cli::parseLatencyArg(target->text().toStdString(), 32, nullptr)) {
        stim = parsed->stimChannel + 1;
        react = parsed->reactChannel + 1;
        stimFalling = parsed->stimFalling;
        reactFalling = parsed->reactFalling;
    }
    auto* grid = new QGridLayout;
    auto* stimCh = channelCombo(channels, stim);
    stimCh->setObjectName("stimChannel");
    auto* stimEdge = edgeCombo(stimFalling);
    stimEdge->setObjectName("stimEdge");
    auto* reactCh = channelCombo(channels, react);
    reactCh->setObjectName("reactChannel");
    auto* reactEdge = edgeCombo(reactFalling);
    reactEdge->setObjectName("reactEdge");
    grid->addWidget(new QLabel(QObject::tr("Stimulus")), 0, 0);
    grid->addWidget(stimCh, 0, 1);
    grid->addWidget(stimEdge, 0, 2);
    grid->addWidget(new QLabel(QObject::tr("Reaction")), 1, 0);
    grid->addWidget(reactCh, 1, 1);
    grid->addWidget(reactEdge, 1, 2);
    layout->addLayout(grid);

    const QPointer<QLineEdit> field(target);
    auto* ok = addApply(layout, frame, [field, stimCh, stimEdge, reactCh, reactEdge] {
        if (field)
            field->setText(composeLatency(stimCh->currentData().toInt(), stimEdge->currentData().toBool(),
                                          reactCh->currentData().toInt(), reactEdge->currentData().toBool()));
    });
    ok->setEnabled(!channels.empty());
    placeBelow(frame, anchor);
    return frame;
}

QFrame* showSetupEditor(QWidget* parent, const lt::SetupInfo& current,
                        const std::function<void(const lt::SetupInfo&)>& apply)
{
    auto* frame = new QFrame(parent->window(), Qt::Dialog);
    frame->setObjectName("setupEditor");
    frame->setWindowTitle(QObject::tr("Setup Metadata"));
    frame->setAttribute(Qt::WA_DeleteOnClose);
    frame->setWindowModality(Qt::NonModal);
    auto* layout = new QVBoxLayout(frame);
    layout->addWidget(new QLabel(QObject::tr("Describe the test setup. These pairs are attached to every AI report "
                                             "and exported capture.")));
    auto* table = new QTableWidget(0, 2);
    table->setObjectName("setupTable");
    table->setHorizontalHeaderLabels({QObject::tr("Key"), QObject::tr("Value")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setMinimumSize(420, 200);
    layout->addWidget(table, 1);
    auto* status = new QLabel;
    status->setObjectName("setupStatus");
    status->setStyleSheet("color: #c0392b;");
    layout->addWidget(status);

    const auto addRow = [table](const QString& key, const QString& value) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(key));
        table->setItem(r, 1, new QTableWidgetItem(value));
    };
    for (const auto& [k, v] : current)
        addRow(QString::fromStdString(k), QString::fromStdString(v));

    auto* quick = new QHBoxLayout;
    quick->addWidget(new QLabel(QObject::tr("Quick add:")));
    for (const char* key : {"dut.board", "dut.firmware", "wiring", "pullups"}) {
        auto* b = new QPushButton(QString::fromLatin1(key));
        b->setObjectName(QString("template_%1").arg(key));
        QObject::connect(b, &QPushButton::clicked, frame, [table, addRow, key] {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0) && table->item(r, 0)->text() == key) {
                    table->setCurrentCell(r, 1);
                    return;
                }
            addRow(QString::fromLatin1(key), {});
            table->setCurrentCell(table->rowCount() - 1, 1);
            table->editItem(table->item(table->rowCount() - 1, 1));
        });
        quick->addWidget(b);
    }
    auto* addBlank = new QPushButton(QObject::tr("Add row"));
    addBlank->setObjectName("addRow");
    auto* remove = new QPushButton(QObject::tr("Remove row"));
    remove->setObjectName("removeRow");
    quick->addStretch(1);
    quick->addWidget(addBlank);
    quick->addWidget(remove);
    layout->addLayout(quick);
    QObject::connect(addBlank, &QPushButton::clicked, frame, [addRow] { addRow({}, {}); });
    QObject::connect(remove, &QPushButton::clicked, frame, [table] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* close = new QPushButton(QObject::tr("Close"));
    close->setObjectName("cancel");
    auto* ok = new QPushButton(QObject::tr("Apply"));
    ok->setObjectName("apply");
    ok->setDefault(true);
    row->addWidget(close);
    row->addWidget(ok);
    layout->addLayout(row);
    QObject::connect(close, &QPushButton::clicked, frame, &QWidget::close);
    QObject::connect(ok, &QPushButton::clicked, frame, [frame, table, status, apply] {
        lt::SetupInfo out;
        for (int r = 0; r < table->rowCount(); ++r) {
            const std::string key = table->item(r, 0) ? table->item(r, 0)->text().trimmed().toStdString() : "";
            const std::string value = table->item(r, 1) ? table->item(r, 1)->text().trimmed().toStdString() : "";
            if (key.empty() && value.empty())
                continue;  // blank rows are ignored
            std::string err;
            if (!lt::cli::isValidSetupEntry(key, value, &err)) {
                status->setText(QObject::tr("Row %1: %2").arg(r + 1).arg(QString::fromStdString(err)));
                table->setCurrentCell(r, 0);
                return;
            }
            lt::cli::setSetupEntry(out, key, value);
        }
        apply(out);
        frame->close();
    });
    frame->resize(520, 340);
    frame->show();
    return frame;
}

}  // namespace lt::gui

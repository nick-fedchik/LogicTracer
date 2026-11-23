// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/EventRecorder.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <atomic>
#include <memory>

namespace lt::gui {
namespace {

class EventRecorder final : public QDialog {
public:
    EventRecorder(QWidget* parent, nlohmann::json initial) : QDialog(parent)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Native event recorder"));
        resize(760, 500);
        auto* layout = new QVBoxLayout(this);
        auto* formWidget = new QWidget(this);
        auto* form = new QFormLayout(formWidget);
        auto* source = new QComboBox;
        source->addItems({tr("SocketCAN frames"), tr("GPIO edges")});
        auto* device = new QLineEdit;
        auto* line = new QLineEdit;
        line->setPlaceholderText(tr("Verified line offset (zero-based)"));
        auto* clock = new QComboBox;
        clock->addItems({tr("Kernel monotonic (software)"), tr("Hardware HTE (no fallback)")});
        auto* duration = new QSpinBox;
        duration->setRange(1, events::maxDurationMs); duration->setValue(1000); duration->setSuffix(tr(" ms"));
        auto* limit = new QSpinBox;
        limit->setRange(1, events::maxEvents); limit->setValue(1000);
        form->addRow(tr("Source"), source);
        form->addRow(tr("Interface / chip"), device);
        form->addRow(tr("GPIO line"), line);
        form->addRow(tr("GPIO clock"), clock);
        form->addRow(tr("Duration"), duration);
        form->addRow(tr("Event limit"), limit);
        layout->addWidget(formWidget);
        auto update = [=](int index) {
            device->clear();
            device->setPlaceholderText(index ? "/dev/gpiochipN" : tr("Existing CAN interface, e.g. can0"));
            line->setEnabled(index != 0); clock->setEnabled(index != 0);
        };
        update(0);
        connect(source, &QComboBox::currentIndexChanged, this, update);
        auto* note = new QLabel(tr("Events only: no sampling rate, trigger or level probe. CAN reception does not set "
                                   "controller listen-only mode. Request only a verified GPIO input."), this);
        note->setWordWrap(true);
        layout->addWidget(note);
        m_text = new QPlainTextEdit(this);
        m_text->setObjectName("nativeEventsText");
        m_text->setReadOnly(true);
        m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(m_text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* start = buttons->addButton(tr("Record"), QDialogButtonBox::ActionRole);
        auto* stop = buttons->addButton(tr("Cancel recording"), QDialogButtonBox::ActionRole);
        m_save = buttons->addButton(QDialogButtonBox::Save);
        stop->setEnabled(false);
        m_save->setEnabled(false);
#ifndef Q_OS_LINUX
        start->setEnabled(false);
        start->setToolTip(tr("Live acquisition requires Linux. Saved event recordings can still be opened."));
#endif
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(stop, &QPushButton::clicked, this, [this] { m_cancel->store(true); });
        connect(start, &QPushButton::clicked, this, [=, this] {
            events::Request r;
            r.source = source->currentIndex() ? events::Source::Gpio : events::Source::Can;
            r.device = device->text().trimmed().toStdString();
            r.durationMs = duration->value(); r.limit = limit->value();
            if (r.source == events::Source::Gpio) {
                bool ok = false;
                r.line = line->text().toUInt(&ok);
                if (!ok || line->text().trimmed().startsWith('-')) {
                    m_text->setPlainText(tr("Enter a nonnegative GPIO line offset.")); m_save->setEnabled(false); return;
                }
                r.hte = clock->currentIndex() == 1;
            }
            m_save->setEnabled(false);
            if (const auto why = events::validate(r); !why.empty()) {
                m_text->setPlainText(QString::fromStdString(why)); return;
            }
            formWidget->setEnabled(false); start->setEnabled(false); stop->setEnabled(true);
            m_text->setPlainText(tr("Waiting for events..."));
            m_cancel->store(false);
            m_watcher.setFuture(QtConcurrent::run([r, token = m_cancel] {
                return events::record(r, [token] { return token->load(); });
            }));
        });
        connect(&m_watcher, &QFutureWatcher<events::Result>::finished, this, [=, this] {
            formWidget->setEnabled(true); start->setEnabled(true); stop->setEnabled(false);
            auto result = m_watcher.result();
            if (result.status == events::Status::Ok)
                display(std::move(result.recording));
            else
                m_text->setPlainText(QString::fromStdString(result.message));
        });
        connect(m_save, &QPushButton::clicked, this, [this] {
            auto* picker = new QFileDialog(this, tr("Save event recording"), {}, tr("Event recording (*.ltevents)"));
            picker->setAcceptMode(QFileDialog::AcceptSave);
            picker->setDefaultSuffix("ltevents");
            picker->setAttribute(Qt::WA_DeleteOnClose);
            connect(picker, &QFileDialog::fileSelected, this, [this](const QString& path) {
                QString err;
                if (!events::save(path, m_recording, err))
                    m_text->appendPlainText(tr("Save failed: %1").arg(err));
            });
            picker->open();
        });
        if (!initial.is_null())
            display(std::move(initial));
    }
    ~EventRecorder() override { m_cancel->store(true); }

private:
    void display(nlohmann::json document)
    {
        m_recording = std::move(document);
        m_text->setPlainText(QString::fromStdString(events::describe(m_recording)));
        m_save->setEnabled(true);
    }
    nlohmann::json m_recording;
    QPlainTextEdit* m_text;
    QPushButton* m_save;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    QFutureWatcher<events::Result> m_watcher;
};
}  // namespace

void showEventRecorder(QWidget* parent, nlohmann::json recording)
{
    (new EventRecorder(parent, std::move(recording)))->open();
}
}  // namespace lt::gui

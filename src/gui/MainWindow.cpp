// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/MainWindow.hpp"

#include "core/ArgParsing.hpp"
#include "core/Exporters.hpp"
#include "services/Operations.hpp"
#include "core/Format.hpp"
#include "core/SignalAnalyzer.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <QColor>
#include <QCoreApplication>

namespace lt::gui {

namespace {

constexpr int kMaxProtocolChannels = 32;

std::string str(const QString& s) { return s.toStdString(); }

QString describeTrigger(const std::optional<lt::EdgeTrigger>& t)
{
    return t ? QString("CH%1 %2").arg(t->channel + 1).arg(t->inverted ? "falling" : "rising") : QString();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("LogicTracer"));
    buildUi();
    buildMenus();

    connect(&m_device, &lt::AsyncDevice::opened, this, &MainWindow::onOpened);
    connect(&m_device, &lt::AsyncDevice::captureFinished, this, &MainWindow::onCaptureFinished);
    connect(&m_device, &lt::AsyncDevice::probeFinished, this, &MainWindow::onProbeFinished);
    connect(&m_device, &lt::AsyncDevice::progress, this, &MainWindow::onProgress);
    connect(&m_device, &lt::AsyncDevice::busyChanged, this, &MainWindow::onBusyChanged);
    connect(&m_device, &lt::AsyncDevice::closed, this, [this] { updateControls(); });

    m_reportTimer = new QTimer(this);
    m_reportTimer->setSingleShot(true);
    m_reportTimer->setInterval(120);
    connect(m_reportTimer, &QTimer::timeout, this, &MainWindow::updateReport);
    connect(m_waveform, &WaveformWidget::viewChanged, m_reportTimer, qOverload<>(&QTimer::start));
    connect(m_waveform, &WaveformWidget::cursorsChanged, m_reportTimer, qOverload<>(&QTimer::start));
    connect(m_waveform, &WaveformWidget::annotationsChanged, m_reportTimer, qOverload<>(&QTimer::start));
    connect(m_waveform, &WaveformWidget::hoverChanged, this, &MainWindow::onHover);
    connect(m_waveform, &WaveformWidget::contextMenuRequested, this, &MainWindow::showWaveformContextMenu);
    connect(m_aiNote, &QLineEdit::textChanged, m_reportTimer, qOverload<>(&QTimer::start));
    connect(m_latencySpec, &QLineEdit::textChanged, m_reportTimer, qOverload<>(&QTimer::start));
    connect(m_copyMode, &QComboBox::currentIndexChanged, this, [this](int) { updateReport(); });
    for (auto* field : {m_channels, m_rate, m_samples, m_trigger, m_latencySpec})
        connect(field, &QLineEdit::textChanged, this, [this] { updatePreflight(); });
    connect(m_timeout, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updatePreflight(); });
    connect(&m_device, &lt::AsyncDevice::opened, this, [this](bool, const QString&) { updatePreflight(); });
    connect(m_waveform, &WaveformWidget::captureChanged, this, [this] { updatePreflight(); });

    refreshPorts();
    updateControls();
    updatePreflight();
    updateReport();
    showStatus(tr("Not connected"));
    resize(1180, 760);
}

void MainWindow::buildUi()
{
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);

    // Connection and capture settings.
    auto* grid = new QGridLayout;
    m_port = new QComboBox;
    m_port->setEditable(true);
    m_port->setMinimumWidth(220);
    m_refresh = new QPushButton(tr("Refresh"));
    m_connect = new QPushButton(tr("Connect"));
    m_channels = new QLineEdit("1-8");
    m_channels->setToolTip(tr("1-based channels with optional names: 1:STIM,2:REACT,24:DO1 or 1-8"));
    m_rate = new QLineEdit("1M");
    m_rate->setToolTip(tr("Sample rate in Hz: 1M, 250k, 100000"));
    m_samples = new QLineEdit("10k");
    m_samples->setToolTip(tr("Total samples (pre + post trigger)"));
    m_trigger = new QLineEdit("none");
    m_trigger->setToolTip(tr("CH:rising|falling (level sensitive in firmware), 'none' = immediate"));
    m_port->setObjectName("port");
    m_channels->setObjectName("channels");
    m_rate->setObjectName("rate");
    m_samples->setObjectName("samples");
    m_trigger->setObjectName("trigger");
    m_timeout = new QSpinBox;
    m_timeout->setRange(0, 600000);
    m_timeout->setValue(5000);
    m_timeout->setSuffix(" ms");
    m_capture = new QPushButton(tr("Capture"));
    m_probe = new QPushButton(tr("Probe"));
    m_probe->setToolTip(tr("Snapshot of the current input levels (~10 ms, no trigger)"));
    m_cancel = new QPushButton(tr("Cancel"));

    int col = 0;
    grid->addWidget(new QLabel(tr("Port")), 0, col++);
    grid->addWidget(m_port, 0, col++);
    grid->addWidget(m_refresh, 0, col++);
    grid->addWidget(m_connect, 0, col++);
    grid->addWidget(new QLabel(tr("Channels")), 0, col++);
    grid->addWidget(m_channels, 0, col++);
    grid->addWidget(new QLabel(tr("Rate")), 0, col++);
    grid->addWidget(m_rate, 0, col++);
    grid->addWidget(new QLabel(tr("Samples")), 0, col++);
    grid->addWidget(m_samples, 0, col++);
    grid->addWidget(new QLabel(tr("Trigger")), 0, col++);
    grid->addWidget(m_trigger, 0, col++);
    grid->addWidget(new QLabel(tr("Timeout")), 0, col++);
    grid->addWidget(m_timeout, 0, col++);
    grid->addWidget(m_capture, 0, col++);
    grid->addWidget(m_probe, 0, col++);
    grid->addWidget(m_cancel, 0, col++);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(5, 3);
    root->addLayout(grid);

    m_deviceInfo = new QLabel;
    m_prev = new QPushButton(tr("Prev"));
    m_prev->setObjectName("historyPrev");
    m_prev->setToolTip(tr("Show the previous capture (Alt+Left)"));
    m_next = new QPushButton(tr("Next"));
    m_next->setObjectName("historyNext");
    m_next->setToolTip(tr("Show the next capture (Alt+Right)"));
    m_historyLabel = new QLabel;
    m_historyLabel->setObjectName("historyLabel");
    auto* infoRow = new QHBoxLayout;
    infoRow->setContentsMargins(0, 0, 0, 0);
    infoRow->addWidget(m_deviceInfo, 1);
    infoRow->addWidget(m_prev);
    infoRow->addWidget(m_historyLabel);
    infoRow->addWidget(m_next);
    root->addLayout(infoRow);
    m_preflight = new QLabel;
    m_preflight->setObjectName("preflight");
    m_preflight->setWordWrap(true);
    m_preflight->setTextFormat(Qt::PlainText);
    root->addWidget(m_preflight);

    // Waveform on top, AI grounding bar + live AI report preview below it.
    auto* splitter = new QSplitter(Qt::Vertical);
    m_waveform = new WaveformWidget;
    splitter->addWidget(m_waveform);

    auto* aiPane = new QWidget;
    auto* aiLayout = new QVBoxLayout(aiPane);
    aiLayout->setContentsMargins(0, 2, 0, 0);
    aiLayout->setSpacing(4);

    auto* aiBar = new QHBoxLayout;
    aiBar->setContentsMargins(0, 0, 0, 0);
    m_aiNote = new QLineEdit;
    m_aiNote->setObjectName("aiNote");
    m_aiNote->setClearButtonEnabled(true);
    m_aiNote->setPlaceholderText(
        tr("Context / Question for AI (e.g. J501 DO1 -> GP28 via 4.7k pull-up; why is pulse 12 us?)"));
    m_notePin = new QCheckBox(tr("Pin"));
    m_notePin->setObjectName("notePin");
    m_notePin->setToolTip(tr("Keep this note for the next captures. Unpinned notes are cleared when a new capture arrives."));
    m_latencySpec = new QLineEdit;
    m_latencySpec->setObjectName("latencySpec");
    m_latencySpec->setClearButtonEnabled(true);
    m_latencySpec->setMaximumWidth(170);
    m_latencySpec->setPlaceholderText(tr("Latency: 1..2"));
    m_latencySpec->setToolTip(tr("Optional paired STIM..REACT latency in report, e.g. 1..2 or 24:falling..2:rising"));
    m_copyMode = new QComboBox;
    m_copyMode->setObjectName("copyMode");
    m_copyMode->addItem(tr("Full (MD + ASCII + JSON)"), static_cast<int>(lt::CopyMode::Full));
    m_copyMode->addItem(tr("Brief (Chat: MD + ASCII)"), static_cast<int>(lt::CopyMode::Brief));
    m_copyMode->addItem(tr("Cursors A..B only"), static_cast<int>(lt::CopyMode::CursorsOnly));
    m_copyMode->setToolTip(tr("Choose how much detail Copy for AI puts on the clipboard"));
    m_reportSize = new QLabel;
    m_reportSize->setMinimumWidth(105);
    m_copyButton = new QPushButton(tr("Copy for AI"));
    m_copyButton->setObjectName("copyButton");
    m_copyButton->setToolTip(tr("Copy the report below to the clipboard (Ctrl+Shift+C)"));

    aiBar->addWidget(new QLabel(tr("Ask AI:")));
    aiBar->addWidget(m_aiNote, 1);
    aiBar->addWidget(m_notePin);
    aiBar->addWidget(m_latencySpec);
    aiBar->addWidget(new QLabel(tr("Mode:")));
    aiBar->addWidget(m_copyMode);
    aiBar->addWidget(m_reportSize);
    aiBar->addWidget(m_copyButton);
    aiLayout->addLayout(aiBar);

    m_report = new QPlainTextEdit;
    m_report->setReadOnly(true);
    m_report->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_report->setToolTip(tr("Exact text that 'Copy for AI' puts on the clipboard for the current view and mode"));
    aiLayout->addWidget(m_report, 1);

    splitter->addWidget(aiPane);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);
    setCentralWidget(central);

    m_status = new QLabel;
    m_hover = new QLabel;
    m_hover->setMinimumWidth(260);
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_hover);

    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectOrDisconnect);
    connect(m_capture, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(m_probe, &QPushButton::clicked, this, &MainWindow::startProbe);
    connect(m_cancel, &QPushButton::clicked, &m_device, &lt::AsyncDevice::requestCancel);
    connect(m_copyButton, &QPushButton::clicked, this, &MainWindow::copyForAi);
    connect(m_prev, &QPushButton::clicked, this, &MainWindow::showPreviousCapture);
    connect(m_next, &QPushButton::clicked, this, &MainWindow::showNextCapture);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open Capture..."), QKeySequence::Open, this, &MainWindow::openCapture);
    file->addSeparator();
    auto* vcd = file->addAction(tr("Save &VCD..."), this, &MainWindow::saveVcd);
    vcd->setToolTip(tr("Waveform for GTKWave / PulseView"));
    file->addAction(tr("Save &JSON..."), QKeySequence::Save, this, &MainWindow::saveJson);
    file->addAction(tr("Save &CSV..."), this, &MainWindow::saveCsv);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    m_copyAction = edit->addAction(tr("Copy for &AI"), this, &MainWindow::copyForAi);
    m_copyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyAction->setToolTip(tr("Copy the report using the selected Copy Mode"));
    auto* copyBrief = edit->addAction(tr("Copy &Brief for Chat"), this, &MainWindow::copyBriefForAi);
    copyBrief->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    copyBrief->setToolTip(tr("Copy compact Markdown + causal transitions + ASCII (without raw JSON)"));
    auto* copyAb = edit->addAction(tr("Copy Cursors &A..B for AI"), this, &MainWindow::copyCursorsForAi);
    copyAb->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    copyAb->setToolTip(tr("Copy report zoomed strictly to the span between Cursors A and B"));

    edit->addSeparator();
    edit->addAction(tr("Clear Setup &Metadata"), this, &MainWindow::clearSetupMetadata)
        ->setToolTip(tr("Forget the test-setup metadata (DUT, wiring, ...) attached to reports"));

    auto* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("&Previous capture"), QKeySequence(Qt::ALT | Qt::Key_Left), this, &MainWindow::showPreviousCapture);
    view->addAction(tr("&Next capture"), QKeySequence(Qt::ALT | Qt::Key_Right), this, &MainWindow::showNextCapture);
    view->addSeparator();
    view->addAction(tr("Zoom &in"), QKeySequence(Qt::Key_Plus), m_waveform, &WaveformWidget::zoomIn);
    view->addAction(tr("Zoom &out"), QKeySequence(Qt::Key_Minus), m_waveform, &WaveformWidget::zoomOut);
    view->addAction(tr("Zoom to &cursors A..B"), QKeySequence(Qt::Key_Z), m_waveform, &WaveformWidget::zoomToCursors);
    view->addAction(tr("&Fit all"), QKeySequence(Qt::Key_Home), m_waveform, &WaveformWidget::fitAll);
    view->addSeparator();
    m_snapAction = view->addAction(tr("&Snap cursors to edges"));
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(m_waveform->snapToEdges());
    connect(m_snapAction, &QAction::toggled, m_waveform, &WaveformWidget::setSnapToEdges);
    view->addAction(tr("&Clear cursors"), m_waveform, &WaveformWidget::clearCursors);
    view->addAction(tr("Clear &annotations"), m_waveform, &WaveformWidget::clearAnnotations);
}

void MainWindow::showStatus(const QString& text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? "color: #d04040;" : QString());
}

void MainWindow::updateControls()
{
    const bool busy = m_device.isBusy();
    const bool connected = m_device.isConnected();
    m_port->setEnabled(!connected && !busy);
    m_refresh->setEnabled(!connected && !busy);
    m_connect->setEnabled(!busy || connected);
    m_connect->setText(connected ? tr("Disconnect") : tr("Connect"));
    m_capture->setEnabled(connected && !busy);
    m_probe->setEnabled(connected && !busy);
    m_cancel->setEnabled(busy);
    for (QWidget* w : {static_cast<QWidget*>(m_channels), static_cast<QWidget*>(m_rate),
                       static_cast<QWidget*>(m_samples), static_cast<QWidget*>(m_trigger),
                       static_cast<QWidget*>(m_timeout)})
        w->setEnabled(!busy);
    updateHistoryUi();
    m_deviceInfo->setText(connected ? QString("%1  |  %2 channels, max %3, buffer %4 KiB")
                                          .arg(QString::fromStdString(m_device.info().version.raw))
                                          .arg(m_device.info().channels)
                                          .arg(QString::fromStdString(lt::formatHz(m_device.info().maxFrequency)))
                                          .arg(m_device.info().bufferSize / 1024)
                                    : tr("No device connected"));
}

void MainWindow::refreshPorts()
{
    const QString current = m_port->currentText();
    m_port->clear();
    int preferred = -1;
    for (const auto& p : lt::PicoConnection::enumerate(true)) {
        m_port->addItem(QString("%1  (%2)").arg(QString::fromStdString(p.port), QString::fromStdString(p.description)),
                        QString::fromStdString(p.port));
        if (p.vidPidMatch && preferred < 0)
            preferred = m_port->count() - 1;
    }
    if (preferred >= 0)
        m_port->setCurrentIndex(preferred);
    else if (!current.isEmpty())
        m_port->setEditText(current);
}

void MainWindow::setPortText(const QString& port)
{
    m_port->setEditText(port);
}

void MainWindow::connectOrDisconnect()
{
    if (m_device.isConnected()) {
        m_device.close();
        showStatus(tr("Disconnected"));
        updateControls();
        return;
    }
    // Items carry the bare path as data; a typed path is used as is.
    QString port = m_port->currentData().toString();
    if (port.isEmpty() || m_port->currentText() != m_port->itemText(m_port->currentIndex()))
        port = m_port->currentText().trimmed();
    if (port.isEmpty()) {
        showStatus(tr("Select a serial port first"), true);
        return;
    }
    showStatus(tr("Connecting to %1...").arg(port));
    m_device.open(port);
}

void MainWindow::onOpened(bool ok, const QString& error)
{
    updateControls();
    if (ok) {
        const std::string board = m_device.info().version.board;
        if (!m_lastBoard.empty() && board != m_lastBoard)
            clearSetupMetadata();  // setup text describes wiring of another board
        m_lastBoard = board;
        showStatus(tr("Connected: %1").arg(QString::fromStdString(m_device.info().version.raw)));
    } else {
        showStatus(tr("Connection failed: %1").arg(error), true);
    }
}

bool MainWindow::buildSpec(lt::CaptureRequestSpec& spec, QString& error) const
{
    std::string err;
    const int maxCh = std::min<int>(kMaxProtocolChannels, m_device.info().channels);
    const auto channels = lt::cli::parseChannelList(str(m_channels->text()), maxCh, &err);
    if (!channels) {
        error = QString::fromStdString(err);
        return false;
    }
    const auto rate = lt::cli::parseQuantity(str(m_rate->text()));
    const auto samples = lt::cli::parseQuantity(str(m_samples->text()));
    if (!rate || *rate == 0 || *rate > UINT32_MAX) {
        error = tr("Invalid sample rate '%1'").arg(m_rate->text());
        return false;
    }
    if (!samples || *samples < 4 || *samples > UINT32_MAX) {
        error = tr("Invalid sample count '%1'").arg(m_samples->text());
        return false;
    }
    const auto trigger = lt::cli::parseTrigger(str(m_trigger->text()), maxCh + 1, &err);
    if (!trigger) {
        error = QString::fromStdString(err);
        return false;
    }
    spec = {};
    spec.channels = *channels;
    spec.frequency = static_cast<uint32_t>(*rate);
    spec.samples = *samples;
    if (!trigger->none)
        spec.trigger = trigger->trigger;
    spec.timeout = std::chrono::milliseconds(m_timeout->value());
    return true;
}

bool MainWindow::buildConfig(lt::CaptureConfig& config, QString& triggerText, QString& error,
                             std::vector<std::string>* warningsOut) const
{
    lt::CaptureRequestSpec spec;
    if (!buildSpec(spec, error))
        return false;
    std::vector<std::string> warnings;
    std::string err;
    const auto cfg = lt::buildCaptureConfig(m_device.info(), spec, warnings, err);
    if (warningsOut)
        *warningsOut = std::move(warnings);
    if (!cfg) {
        error = QString::fromStdString(err);
        return false;
    }
    config = *cfg;
    triggerText = describeTrigger(config.trigger);
    return true;
}

void MainWindow::startCapture()
{
    lt::CaptureConfig config;
    QString error;
    QString trigger;
    std::vector<std::string> warnings;
    if (!buildConfig(config, trigger, error, &warnings)) {
        showStatus(error, true);
        return;
    }
    m_pendingTriggerDescription = trigger;
    showStatus(tr("Starting capture..."));
    if (!m_device.startCapture(config)) {
        m_pendingTriggerDescription.clear();
        showStatus(tr("Device is busy or not connected"), true);
        return;
    }
    m_pendingWarnings = warnings;
    if (config.trigger)
        setArmed();
    if (!warnings.empty())
        showStatus(tr("Starting capture... Warning: %1").arg(QString::fromStdString(warnings.front())));
}

void MainWindow::setFieldError(QLineEdit* field, const QString& message)
{
    if (!field->property("baseTip").isValid())
        field->setProperty("baseTip", field->toolTip());
    const QString base = field->property("baseTip").toString();
    if (message.isEmpty()) {
        field->setStyleSheet({});
        field->setToolTip(base);
    } else {
        field->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #c0392b; }"));
        field->setToolTip(base.isEmpty() ? message : base + "\n\n" + message);
    }
}

void MainWindow::updatePreflight()
{
    const int maxCh = m_device.isConnected() ? std::min<int>(kMaxProtocolChannels, m_device.info().channels) : static_cast<int>(kMaxProtocolChannels);
    std::string err;
    QString text;
    bool bad = false;

    const auto channels = lt::cli::parseChannelList(str(m_channels->text()), maxCh, &err);
    setFieldError(m_channels, channels ? QString{} : QString::fromStdString(err));
    bad |= !channels;

    const auto rate = lt::cli::parseQuantity(str(m_rate->text()));
    const bool rateOk = rate && *rate > 0 && *rate <= UINT32_MAX;
    setFieldError(m_rate, rateOk ? QString{} : tr("Invalid sample rate (e.g. 1M, 500k)"));
    const auto samples = lt::cli::parseQuantity(str(m_samples->text()));
    const bool samplesOk = samples && *samples >= 4 && *samples <= UINT32_MAX;
    setFieldError(m_samples, samplesOk ? QString{} : tr("Invalid sample count (at least 4)"));
    const auto trigger = lt::cli::parseTrigger(str(m_trigger->text()), maxCh + 1, &err);
    setFieldError(m_trigger, trigger ? QString{} : QString::fromStdString(err));
    bad |= !rateOk || !samplesOk || !trigger;

    QString latencyError;
    if (const QString lat = m_latencySpec->text().trimmed(); !lat.isEmpty()) {
        if (const auto parsed = lt::cli::parseLatencyArg(str(lat), kMaxProtocolChannels, &err)) {
            if (m_waveform->hasCapture()) {
                bool stim = false;
                bool react = false;
                for (const auto& ch : m_waveform->capture().channels()) {
                    stim |= ch.number == parsed->stimChannel;
                    react |= ch.number == parsed->reactChannel;
                }
                if (!stim || !react)
                    latencyError = tr("Latency channel is not in the capture on screen");
            }
        } else {
            latencyError = QString::fromStdString(err);
        }
    }
    setFieldError(m_latencySpec, latencyError);

    if (bad) {
        m_preflight->setText(tr("Pre-flight: fix the highlighted fields"));
        return;
    }
    const double step = 1.0 / static_cast<double>(*rate);
    text = tr("Window %1 | Step %2")
               .arg(QString::fromStdString(lt::formatTime(step * static_cast<double>(*samples))),
                    QString::fromStdString(lt::formatTime(step)));
    if (m_device.isConnected()) {
        lt::CaptureConfig config;
        QString trig;
        QString error;
        std::vector<std::string> warnings;
        if (buildConfig(config, trig, error, &warnings)) {
            text += tr(" | Pre %1 (%2)")
                        .arg(config.trigger ? config.preSamples : 0)
                        .arg(QString::fromStdString(lt::formatTime(
                            step * static_cast<double>(config.trigger ? config.preSamples : 0))));
            for (const auto& w : warnings)
                text += "\n" + tr("Warning: %1").arg(QString::fromStdString(w));
        } else {
            text += "\n" + tr("Error: %1").arg(error);
        }
    } else {
        text += tr(" | connect a device for Pre and limits");
    }
    m_preflight->setText(text);
}

void MainWindow::startProbe()
{
    std::string err;
    const int maxCh = std::min<int>(kMaxProtocolChannels, m_device.info().channels);
    const auto channels = lt::cli::parseChannelList(str(m_channels->text()), maxCh, &err);
    if (!channels) {
        showStatus(QString::fromStdString(err), true);
        return;
    }
    m_pendingTriggerDescription = tr("none (probe)");
    showStatus(tr("Probing..."));
    if (!m_device.startProbe(*channels))
        showStatus(tr("Device is busy or not connected"), true);
}

void MainWindow::onProgress(int stage)
{
    switch (static_cast<lt::CaptureStage>(stage)) {
    case lt::CaptureStage::Starting: showStatus(tr("Starting capture...")); break;
    case lt::CaptureStage::WaitingForTrigger: showStatus(tr("Waiting for trigger... (Cancel to abort)")); break;
    case lt::CaptureStage::Downloading: showStatus(tr("Downloading samples...")); break;
    case lt::CaptureStage::Done: break;
    }
}

void MainWindow::onBusyChanged(bool)
{
    updateControls();
}

void MainWindow::onCaptureFinished(const lt::CaptureResult& r)
{
    updateControls();
    const auto pending = std::exchange(m_pending, std::nullopt);
    const QString candidateTrigger = std::exchange(m_pendingTriggerDescription, QString{});
    auto pendingWarnings = std::exchange(m_pendingWarnings, {});
    const bool fromSession = pending && !pending->isProbe;
    switch (r.status) {
    case lt::CaptureStatus::Ok:
        consumeNote();
        m_failure.reset();
        showNewCapture(*r.buffer, candidateTrigger, fromSession ? pending->options.setup : m_setup,
                       fromSession ? pending->warnings : std::move(pendingWarnings), false);
        showStatus(tr("Capture complete: %1 samples in %2 ms").arg(r.buffer->size()).arg(r.elapsed.count()));
        break;
    case lt::CaptureStatus::Cancelled:
        restoreIdleState();
        showStatus(tr("Capture cancelled"));
        break;
    case lt::CaptureStatus::Timeout:
    case lt::CaptureStatus::Error: {
        QString statusMsg = QString::fromStdString(r.error);
        if (r.lineDump) {  // TIMEOUT carries the current line state
            QString summary;
            for (const auto& st : lt::SignalAnalyzer{}.analyze(*r.lineDump)) {
                if (!summary.isEmpty())
                    summary += ", ";
                summary += QString("%1=%2").arg(QString::fromStdString(st.channel.label()),
                                                QString::fromStdString(std::string(lt::toString(lt::SignalAnalyzer::diagnose(st).state))));
            }
            statusMsg += tr(" [line state: %1]").arg(summary);
        }
        m_failure = FailureInfo{r.lineDump ? *r.lineDump : lt::CaptureBuffer{}, std::string(lt::toString(r.status)),
                                r.error, candidateTrigger};
        restoreIdleState();
        updateReport();
        showStatus(statusMsg, true);
        break;
    }
    }

    if (fromSession) {  // the request came over the session socket
        auto doc = lt::captureDocument(m_device.info(), pending->spec, pending->config, r, pending->warnings,
                                       pending->options);
        if (r.status == lt::CaptureStatus::Ok)
            doc.doc["capture_id"] = m_currentCaptureId;
        lt::writeCaptureOutputs(doc.doc, r, doc, pending->outputs, m_device.info().version.board);
        pending->reply.ok(doc.doc);
    }
}

void MainWindow::onProbeFinished(const lt::ProbeResult& r)
{
    updateControls();
    const auto pending = std::exchange(m_pending, std::nullopt);
    const QString candidateTrigger = std::exchange(m_pendingTriggerDescription, QString{});
    const bool ok = r.status == lt::CaptureStatus::Ok && r.buffer;
    if (ok) {
        consumeNote();
        m_failure.reset();
        showNewCapture(*r.buffer, candidateTrigger, m_setup, {}, true);
        showStatus(tr("Probe complete (%1 ms)").arg(r.elapsed.count()));
    } else if (r.status == lt::CaptureStatus::Cancelled) {
        showStatus(tr("Probe cancelled"));
    } else {
        showStatus(QString::fromStdString(r.error), true);
    }
    if (pending && pending->isProbe) {
        auto doc = lt::probeDocument(m_device.info(), r, pending->probeRate, pending->probeWindow).doc;
        if (ok)
            doc["capture_id"] = m_currentCaptureId;
        pending->reply.ok(doc);
    }
}

void MainWindow::setCapture(const lt::CaptureBuffer& buffer, const QString& triggerDescription, lt::SetupInfo setup)
{
    m_failure.reset();
    showNewCapture(buffer, triggerDescription, std::move(setup), {}, false);
}

void MainWindow::setHistoryLimits(std::size_t entries, std::size_t bytes)
{
    m_maxHistoryEntries = std::max<std::size_t>(1, entries);
    m_maxHistoryBytes = bytes;
    trimHistory();
    if (!m_history.empty())
        showEntry(std::min(m_viewIndex, m_history.size() - 1));
}

void MainWindow::showNewCapture(const lt::CaptureBuffer& buffer, const QString& trigger, lt::SetupInfo setup,
                                std::vector<std::string> warnings, bool isProbe)
{
    saveViewState();
    CaptureEntry entry;
    entry.id = ++m_nextCaptureId;
    entry.buffer = buffer;
    entry.trigger = trigger;
    entry.setup = std::move(setup);
    entry.warnings = std::move(warnings);
    entry.isProbe = isProbe;
    m_history.push_back(std::move(entry));
    trimHistory();
    showEntry(m_history.size() - 1);
}

void MainWindow::saveViewState()
{
    if (m_viewIndex >= m_history.size())
        return;
    auto& e = m_history[m_viewIndex];
    e.annotations = m_waveform->annotations();
    e.cursorA = m_waveform->cursorA();
    e.cursorB = m_waveform->cursorB();
    e.setup = m_setup;
}

void MainWindow::trimHistory()
{
    const auto bytes = [this] {
        std::size_t total = 0;
        for (const auto& e : m_history)
            total += e.buffer.size() * sizeof(uint32_t);
        return total;
    };
    while (m_history.size() > 1 && (m_history.size() > m_maxHistoryEntries || bytes() > m_maxHistoryBytes)) {
        m_history.pop_front();
        if (m_viewIndex > 0)
            --m_viewIndex;
    }
}

void MainWindow::showEntry(std::size_t index)
{
    if (index >= m_history.size())
        return;
    m_viewIndex = index;
    const auto& e = m_history[index];
    m_triggerDescription = e.trigger;
    m_setup = e.setup;
    m_warnings = e.warnings;
    m_currentCaptureId = e.id;
    m_waveform->setCapture(e.buffer);
    m_waveform->setAnnotations(e.annotations);
    if (e.cursorA)
        m_waveform->setCursorA(*e.cursorA);
    if (e.cursorB)
        m_waveform->setCursorB(*e.cursorB);
    restoreIdleState();
    updateHistoryUi();
    updateReport();
}

void MainWindow::showPreviousCapture()
{
    if (m_device.isBusy() || m_viewIndex == 0 || m_history.empty())
        return;
    saveViewState();
    showEntry(m_viewIndex - 1);
    showStatus(tr("Showing capture #%1 (%2 of %3)").arg(m_currentCaptureId).arg(m_viewIndex + 1).arg(m_history.size()));
}

void MainWindow::showNextCapture()
{
    if (m_device.isBusy() || m_viewIndex + 1 >= m_history.size())
        return;
    saveViewState();
    showEntry(m_viewIndex + 1);
    showStatus(tr("Showing capture #%1 (%2 of %3)").arg(m_currentCaptureId).arg(m_viewIndex + 1).arg(m_history.size()));
}

void MainWindow::updateHistoryUi()
{
    if (!m_prev)
        return;
    const bool idle = !m_device.isBusy();
    m_prev->setEnabled(idle && m_viewIndex > 0 && !m_history.empty());
    m_next->setEnabled(idle && m_viewIndex + 1 < m_history.size());
    m_historyLabel->setText(m_history.empty() ? tr("No captures")
                                              : tr("Capture #%1 (%2/%3)")
                                                    .arg(m_currentCaptureId)
                                                    .arg(m_viewIndex + 1)
                                                    .arg(m_history.size()));
}

void MainWindow::setArmed()
{
    m_waveform->setDisplayState(WaveformWidget::DisplayState::Armed,
                                m_waveform->hasCapture()
                                    ? tr("ARMED - waiting for trigger; showing previous capture #%1").arg(m_currentCaptureId)
                                    : tr("ARMED - waiting for trigger"));
    updateReport();
}

void MainWindow::restoreIdleState()
{
    const bool latest = m_history.empty() || m_viewIndex + 1 >= m_history.size();
    if (m_failure && latest)
        m_waveform->setDisplayState(WaveformWidget::DisplayState::Stale,
                                    m_waveform->hasCapture()
                                        ? tr("STALE - capture failed; showing old capture #%1").arg(m_currentCaptureId)
                                        : tr("STALE - capture failed"));
    else
        m_waveform->setDisplayState(WaveformWidget::DisplayState::Live);
}

void MainWindow::consumeNote()
{
    if (!m_notePin->isChecked() && !m_consumedNote.isEmpty() && m_aiNote->text() == m_consumedNote)
        m_aiNote->clear();
    m_consumedNote = m_aiNote->text();
}

void MainWindow::clearSetupMetadata()
{
    m_setup.clear();
    if (m_viewIndex < m_history.size())
        m_history[m_viewIndex].setup.clear();
    updateReport();
    showStatus(tr("Setup metadata cleared"));
}

QString MainWindow::failureReport() const
{
    if (!m_failure)
        return {};
    const std::string board = m_device.isConnected() ? m_device.info().version.board : m_lastBoard;
    const QString note = m_aiNote ? m_aiNote->text().trimmed() : QString{};
    return QString::fromStdString(lt::timeoutForAi(m_failure->lineDump, m_failure->status, m_failure->error,
                                                   str(m_failure->trigger), board, str(note),
                                                   m_waveform->hasCapture() ? m_currentCaptureId : 0));
}

void MainWindow::onHover(bool has, double seconds, int channel, int level)
{
    if (!has) {
        m_hover->clear();
        return;
    }
    QString text = QString("t = %1").arg(QString::fromStdString(lt::formatTime(seconds)));
    if (channel > 0)
        text += QString("   CH%1 = %2").arg(channel).arg(level);
    m_hover->setText(text);
}

lt::CopyMode MainWindow::copyMode() const
{
    if (!m_copyMode)
        return lt::CopyMode::Full;
    return static_cast<lt::CopyMode>(m_copyMode->currentData().toInt());
}

void MainWindow::setCopyMode(lt::CopyMode mode)
{
    if (!m_copyMode)
        return;
    const int idx = m_copyMode->findData(static_cast<int>(mode));
    if (idx >= 0)
        m_copyMode->setCurrentIndex(idx);
}

void MainWindow::setAiNote(const QString& note)
{
    if (m_aiNote)
        m_aiNote->setText(note);
}

void MainWindow::setLatencySpec(const QString& spec)
{
    if (m_latencySpec)
        m_latencySpec->setText(spec);
}

lt::ReportOptions MainWindow::buildReportOptions(std::optional<lt::CopyMode> modeOverride) const
{
    auto options = m_waveform->reportOptions();
    options.mode = modeOverride ? *modeOverride : copyMode();
    options.triggerDescription = str(m_triggerDescription);
    options.setup = m_setup;
    options.warnings = m_warnings;
    options.captureId = m_currentCaptureId;
    if (m_waveform->displayState() == WaveformWidget::DisplayState::Armed)
        options.banner = "ARMED: waiting for the trigger - this report describes the PREVIOUS capture #" +
                         std::to_string(m_currentCaptureId) + ", not the new one";
    else if (m_viewIndex + 1 < m_history.size())
        options.banner = "HISTORY: capture #" + std::to_string(m_currentCaptureId) + " (" +
                         std::to_string(m_viewIndex + 1) + " of " + std::to_string(m_history.size()) +
                         "), not the newest capture #" + std::to_string(m_history.back().id);
    if (m_device.isConnected()) {
        options.deviceVersion = m_device.info().version.raw;
        options.board = m_device.info().version.board;
    }
    if (m_aiNote) {
        const QString note = m_aiNote->text().trimmed();
        if (!note.isEmpty())
            options.userNote = str(note);
    }
    if (m_latencySpec && m_waveform->hasCapture()) {
        const QString latText = m_latencySpec->text().trimmed();
        if (!latText.isEmpty()) {
            std::string err;
            if (const auto parsed = lt::cli::parseLatencyArg(str(latText), kMaxProtocolChannels, &err)) {
                const auto& chs = m_waveform->capture().channels();
                std::optional<std::size_t> sIdx, rIdx;
                for (std::size_t i = 0; i < chs.size(); ++i) {
                    if (chs[i].number == parsed->stimChannel)
                        sIdx = i;
                    if (chs[i].number == parsed->reactChannel)
                        rIdx = i;
                }
                if (sIdx && rIdx)
                    options.latency = lt::LatencyPairSpec{*sIdx, parsed->stimFalling, *rIdx, parsed->reactFalling, {}};
            }
        }
    }
    return options;
}

QString MainWindow::aiReportForMode(lt::CopyMode mode) const
{
    if (m_failure && m_waveform->displayState() == WaveformWidget::DisplayState::Stale)
        return failureReport();
    if (!m_waveform->hasCapture())
        return tr("No capture yet.");
    return QString::fromStdString(lt::copyForAi(m_waveform->capture(), buildReportOptions(mode)));
}

QString MainWindow::aiReport() const
{
    return aiReportForMode(copyMode());
}

void MainWindow::updateReport()
{
    m_reportTimer->stop();
    const QString text = aiReport();
    m_report->setPlainText(text);
    if (m_reportSize) {
        if (!m_waveform->hasCapture() && !m_failure) {
            m_reportSize->clear();
        } else {
            const int chars = text.size();
            const int approxTok = (chars + 3) / 4;
            m_reportSize->setText(tr("%1 ch (~%2 tok)").arg(chars).arg(approxTok));
        }
    }
}

void MainWindow::copyWithMode(lt::CopyMode mode)
{
    const QString text = aiReportForMode(mode);
    QGuiApplication::clipboard()->setText(text);
    const int approxTok = (text.size() + 3) / 4;
    showStatus(tr("Copied %1 chars (~%2 tokens) for AI [%3]")
                   .arg(text.size())
                   .arg(approxTok)
                   .arg(QString::fromUtf8(lt::toString(mode).data(), static_cast<qsizetype>(lt::toString(mode).size()))));
}

void MainWindow::copyForAi()
{
    updateReport();
    copyWithMode(copyMode());
}

void MainWindow::copyBriefForAi()
{
    copyWithMode(lt::CopyMode::Brief);
}

void MainWindow::copyCursorsForAi()
{
    copyWithMode(lt::CopyMode::CursorsOnly);
}

void MainWindow::showWaveformContextMenu(const QPoint& globalPos)
{
    QMenu menu(this);
    menu.addAction(tr("Copy Brief for Chat (MD + ASCII)"), this, &MainWindow::copyBriefForAi);
    auto* copyAb = menu.addAction(tr("Copy Cursors A..B for AI"), this, &MainWindow::copyCursorsForAi);
    copyAb->setEnabled(m_waveform->cursorA().has_value() && m_waveform->cursorB().has_value());
    menu.addAction(tr("Copy Full Report (MD + ASCII + JSON)"), this, [this] { copyWithMode(lt::CopyMode::Full); });
    menu.addSeparator();
    auto* zoomAb = menu.addAction(tr("Zoom to Cursors A..B"), m_waveform, &WaveformWidget::zoomToCursors);
    zoomAb->setEnabled(m_waveform->cursorA().has_value() && m_waveform->cursorB().has_value());
    menu.addAction(tr("Fit All"), m_waveform, &WaveformWidget::fitAll);
    menu.addAction(tr("Clear Cursors"), m_waveform, &WaveformWidget::clearCursors);
    menu.addSeparator();
    if (m_snapAction)
        menu.addAction(m_snapAction);
    menu.exec(globalPos);
}

bool MainWindow::openCaptureFile(const QString& path, QString* error)
{
    lt::LoadedCapture loaded;
    std::string err;
    if (!lt::loadCaptureFile(str(path), loaded, &err)) {
        if (error)
            *error = QString::fromStdString(err);
        showStatus(QString::fromStdString(err), true);
        return false;
    }
    setCapture(loaded.buffer, QString::fromStdString(loaded.triggerDescription), loaded.setup);
    if (!loaded.annotations.empty())
        m_waveform->setAnnotations(std::move(loaded.annotations));
    updateReport();
    showStatus(tr("Opened %1 (%2 samples, %3 channels)")
                   .arg(path)
                   .arg(m_waveform->capture().size())
                   .arg(m_waveform->capture().channels().size()));
    return true;
}

void MainWindow::openCapture()
{
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Open Capture"), QString{},
        tr("Captures (*.json *.vcd *.lac);;JSON (*.json);;VCD (*.vcd);;Legacy LAC (*.lac);;All files (*)"));
    if (!path.isEmpty())
        openCaptureFile(path);
}

void MainWindow::saveVcd()
{
    if (!m_waveform->hasCapture()) {
        showStatus(tr("Nothing to save"), true);
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, tr("Save VCD"), "capture.vcd", tr("VCD (*.vcd)"));
    if (path.isEmpty())
        return;
    std::string err;
    if (lt::writeVcdFile(str(path), m_waveform->capture(), &err))
        showStatus(tr("Saved %1").arg(path));
    else
        showStatus(QString::fromStdString(err), true);
}

void MainWindow::saveJson()
{
    if (!m_waveform->hasCapture()) {
        showStatus(tr("Nothing to save"), true);
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, tr("Save JSON"), "capture.json", tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    lt::SignalAnalyzer::Options opts;
    opts.collectEdges = true;
    const auto& buf = m_waveform->capture();
    const auto stats = lt::SignalAnalyzer(opts).analyze(buf);
    const std::string board = m_device.isConnected() ? m_device.info().version.board : std::string{};
    nlohmann::json doc = lt::captureToJson(buf, stats, true, board);
    if (!m_triggerDescription.isEmpty())
        doc["trigger_description"] = str(m_triggerDescription);
    if (!m_setup.empty())
        doc["setup"] = lt::toJson(m_setup);
    if (!m_waveform->annotations().empty()) {
        auto notes = nlohmann::json::array();
        for (const auto& a : m_waveform->annotations()) {
            notes.push_back({{"id", a.id},
                             {"time_us", a.timeSeconds * 1e6},
                             {"duration_us", a.durationSeconds * 1e6},
                             {"label", a.label},
                             {"color", a.color},
                             {"author", a.author}});
        }
        doc["annotations"] = std::move(notes);
    }
    std::string err;
    if (lt::writeJsonFile(str(path), doc, &err))
        showStatus(tr("Saved %1").arg(path));
    else
        showStatus(QString::fromStdString(err), true);
}

void MainWindow::saveCsv()
{
    if (!m_waveform->hasCapture()) {
        showStatus(tr("Nothing to save"), true);
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, tr("Save CSV"), "capture.csv", tr("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    std::string err;
    if (lt::writeCsvFile(str(path), m_waveform->capture(), &err))
        showStatus(tr("Saved %1").arg(path));
    else
        showStatus(QString::fromStdString(err), true);
}

void MainWindow::loadDemo()
{
    // 4 channels at 1 MHz: a 50 kHz clock, a delayed echo, a burst, and an idle-high line.
    std::vector<uint32_t> samples(4000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const bool clk = (i / 10) % 2;
        const bool echo = i >= 205 && ((i - 205) / 10) % 2;
        const bool burst = i >= 1000 && i < 1400 && ((i / 4) % 2);
        samples[i] = (clk ? 1u : 0u) | (echo ? 2u : 0u) | (burst ? 4u : 0u) | 8u;
    }
    setCapture(lt::CaptureBuffer({{0, "CLK"}, {1, "ECHO"}, {2, "BURST"}, {3, "IDLE_HI"}}, 1'000'000, 200, samples),
               "CH1 rising");
    showStatus(tr("Demo capture (no hardware)"));
}

// ---------------------------------------------------------------------------------------------
// Session (IPC) endpoint

namespace {

using json = nlohmann::json;
namespace sess = lt::session;

const char* displayStateName(WaveformWidget::DisplayState state)
{
    switch (state) {
    case WaveformWidget::DisplayState::Armed: return "armed";
    case WaveformWidget::DisplayState::Stale: return "stale";
    case WaveformWidget::DisplayState::Live: break;
    }
    return "live";
}

std::optional<double> secondsFromParams(const json& p, std::string& error)
{
    int given = 0;
    std::optional<double> value;
    if (p.contains("time_us")) {
        ++given;
        if (!p["time_us"].is_number()) {
            error = "time_us must be a number";
            return std::nullopt;
        }
        value = p["time_us"].get<double>() * 1e-6;
    }
    if (p.contains("time_s")) {
        ++given;
        if (!p["time_s"].is_number()) {
            error = "time_s must be a number";
            return std::nullopt;
        }
        value = p["time_s"].get<double>();
    }
    if (p.contains("time")) {
        ++given;
        value = p["time"].is_string() ? lt::cli::parseTimeSeconds(p["time"].get<std::string>()) : std::nullopt;
        if (!value) {
            error = "time must be a string with a unit, e.g. \"50.12ms\", \"200us\"";
            return std::nullopt;
        }
    }
    if (given != 1) {
        error = "give exactly one of time_us, time_s, time";
        return std::nullopt;
    }
    if (!std::isfinite(*value)) {
        error = "time must be finite";
        return std::nullopt;
    }
    return value;
}

}  // namespace

bool MainWindow::startSession(const QString& path, QString* error)
{
    m_session.setHandler([this](const std::string& method, const json& params, sess::Responder reply) {
        handleSession(method, params, std::move(reply));
    });
    const QString target = path.isEmpty() ? sess::defaultSocketPath() : path;
    QString err;
    const bool ok = m_session.listen(target, &err);
    if (error)
        *error = err;
    showStatus(ok ? tr("Session endpoint: %1").arg(target) : tr("No session endpoint: %1").arg(err), !ok);
    return ok;
}

json MainWindow::stateJson() const
{
    json j = {{"app", "logictracer-gui"},
              {"connected", m_device.isConnected()},
              {"busy", m_device.isBusy()},
              {"has_capture", m_waveform->hasCapture()},
              {"capture_id", m_currentCaptureId},
              {"display_state", displayStateName(m_waveform->displayState())},
              {"history_size", m_history.size()},
              {"annotation_count", m_waveform->annotations().size()}};
    if (m_device.isConnected())
        j["device"] = lt::toJson(m_device.info());
    if (m_waveform->hasCapture()) {
        auto options = m_waveform->reportOptions();
        options.triggerDescription = str(m_triggerDescription);
        auto meta = lt::jsonMetadata(m_waveform->capture(), options);
        j["capture"] = {{"sample_rate_hz", meta["sample_rate_hz"]},
                        {"total_samples", meta["total_samples"]},
                        {"trigger_sample", meta["trigger_sample"]},
                        {"trigger", meta["trigger"]}};
        j["visible"] = meta["visible"];
        j["cursors"] = meta["cursors"];
    }
    return j;
}

void MainWindow::handleSession(const std::string& method, const json& params, sess::Responder reply)
{
    if (method == "ping") {
        reply.ok({{"app", "logictracer-gui"},
                  {"version", QCoreApplication::applicationVersion().toStdString()},
                  {"pid", QCoreApplication::applicationPid()}});
    } else if (method == "get_state") {
        reply.ok(stateJson());
    } else if (method == "get_report") {
        if (m_failure && m_waveform->displayState() == WaveformWidget::DisplayState::Stale) {
            reply.ok({{"report", failureReport().toStdString()},
                      {"metadata",
                       {{"display_state", "stale"},
                        {"capture_id", m_currentCaptureId},
                        {"status", m_failure->status},
                        {"error", m_failure->error}}}});
            return;
        }
        if (!m_waveform->hasCapture()) {
            reply.fail(sess::error::NoCapture, "no capture is shown in the GUI");
            return;
        }
        std::optional<lt::CopyMode> modeOverride;
        if (params.contains("mode")) {
            if (!params["mode"].is_string()) {
                reply.fail(sess::error::InvalidParams, "mode must be 'brief', 'cursors', or 'full'");
                return;
            }
            modeOverride = lt::parseCopyMode(params["mode"].get<std::string>());
            if (!modeOverride) {
                reply.fail(sess::error::InvalidParams, "unknown mode (expected 'brief', 'cursors', or 'full')");
                return;
            }
        }
        auto options = buildReportOptions(modeOverride);
        if (params.contains("note") && params["note"].is_string())
            options.userNote = params["note"].get<std::string>();
        if (params.contains("latency") && params["latency"].is_string() && !params["latency"].get<std::string>().empty()) {
            std::string err;
            const auto parsed = lt::cli::parseLatencyArg(params["latency"].get<std::string>(), kMaxProtocolChannels, &err);
            if (!parsed) {
                reply.fail(sess::error::InvalidParams, err);
                return;
            }
            const auto& chs = m_waveform->capture().channels();
            std::optional<std::size_t> sIdx, rIdx;
            for (std::size_t i = 0; i < chs.size(); ++i) {
                if (chs[i].number == parsed->stimChannel)
                    sIdx = i;
                if (chs[i].number == parsed->reactChannel)
                    rIdx = i;
            }
            if (!sIdx || !rIdx) {
                reply.fail(sess::error::InvalidParams, "latency channels are not present in the current capture");
                return;
            }
            options.latency = lt::LatencyPairSpec{*sIdx, parsed->stimFalling, *rIdx, parsed->reactFalling, {}};
        }
        auto metadata = lt::jsonMetadata(m_waveform->capture(), options);
        metadata["display_state"] = displayStateName(m_waveform->displayState());
        metadata["viewing_history"] = m_viewIndex + 1 < m_history.size();
        reply.ok({{"report", lt::copyForAi(m_waveform->capture(), options)}, {"metadata", metadata}});
    } else if (method == "annotate") {
        sessionAnnotate(params, std::move(reply));
    } else if (method == "list_annotations") {
        auto list = json::array();
        for (const auto& a : m_waveform->annotations())
            list.push_back({{"id", a.id}, {"time_us", a.timeSeconds * 1e6}, {"duration_us", a.durationSeconds * 1e6},
                            {"label", a.label}, {"color", a.color}, {"author", a.author}});
        reply.ok({{"annotations", list}});
    } else if (method == "clear_annotations") {
        const auto n = m_waveform->annotations().size();
        m_waveform->clearAnnotations();
        reply.ok({{"cleared", n}});
    } else if (method == "capture") {
        sessionCapture(params, std::move(reply));
    } else if (method == "probe") {
        sessionProbe(params, std::move(reply));
    } else {
        reply.fail(sess::error::MethodNotFound, "unknown method '" + method + "'");
    }
}

void MainWindow::sessionAnnotate(const json& p, sess::Responder reply)
{
    if (p.contains("capture_id")) {
        if (!p["capture_id"].is_number_unsigned()) {
            reply.fail(sess::error::InvalidParams, "capture_id must be a positive integer");
            return;
        }
        const auto wanted = p["capture_id"].get<uint64_t>();
        if (wanted != m_currentCaptureId || !m_waveform->hasCapture()) {
            reply.fail(sess::error::StaleCapture,
                       "capture #" + std::to_string(wanted) + " is not on screen; the current capture is #" +
                           std::to_string(m_waveform->hasCapture() ? m_currentCaptureId : 0) +
                           ". Re-read the view (lt_get_view) and retry.",
                       {{"requested_capture_id", wanted},
                        {"current_capture_id", m_waveform->hasCapture() ? m_currentCaptureId : 0}});
            return;
        }
    }
    if (!m_waveform->hasCapture()) {
        reply.fail(sess::error::NoCapture, "no capture is shown in the GUI; annotations are relative to its trigger");
        return;
    }
    std::string err;
    const auto seconds = secondsFromParams(p, err);
    if (!seconds) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }
    lt::Annotation a;
    a.timeSeconds = *seconds;
    const std::string label = p.contains("label") && p["label"].is_string() ? p["label"].get<std::string>() : "";
    const QString trimmed = QString::fromStdString(label).trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200) {
        reply.fail(sess::error::InvalidParams, "label is required (1..200 characters)");
        return;
    }
    a.label = str(trimmed);
    if (p.contains("color")) {
        const std::string color = p["color"].is_string() ? p["color"].get<std::string>() : "";
        if (!QColor(QString::fromStdString(color)).isValid()) {
            reply.fail(sess::error::InvalidParams, "color must be a colour name or #rrggbb");
            return;
        }
        a.color = color;
    }
    if (p.contains("duration_us")) {
        if (!p["duration_us"].is_number() || p["duration_us"].get<double>() < 0) {
            reply.fail(sess::error::InvalidParams, "duration_us must be a non-negative number");
            return;
        }
        a.durationSeconds = p["duration_us"].get<double>() * 1e-6;
    }
    if (p.contains("author") && p["author"].is_string() && !p["author"].get<std::string>().empty())
        a.author = p["author"].get<std::string>().substr(0, 40);

    const auto id = m_waveform->addAnnotation(a);
    showStatus(tr("Annotation #%1 by %2: %3").arg(id).arg(QString::fromStdString(a.author), trimmed));
    reply.ok({{"id", id}, {"time_us", a.timeSeconds * 1e6}, {"label", a.label}, {"color", a.color},
              {"capture_id", m_currentCaptureId}});
}

void MainWindow::sessionCapture(const json& p, sess::Responder reply)
{
    if (!m_device.isConnected()) {
        reply.fail(sess::error::NoDevice, "the GUI is not connected to a device");
        return;
    }
    if (m_device.isBusy() || m_pending) {
        reply.fail(sess::error::Busy, "the device is busy with another operation");
        return;
    }
    std::string err;
    auto spec = lt::captureSpecFromJson(p, m_device.info().channels, err);
    if (!spec) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }
    auto opts = lt::documentOptionsFromJson(p, true, m_device.info().channels, spec->channels, err);
    if (!opts) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }

    static constexpr std::array<std::string_view, 1> kVcdExt{".vcd"};
    static constexpr std::array<std::string_view, 1> kJsonExt{".json"};
    static constexpr std::array<std::string_view, 1> kCsvExt{".csv"};
    const auto vcd = lt::resolveSafeOutputPath(p.value("out_vcd", std::string()), kVcdExt, {}, &err);
    const auto jsn = vcd ? lt::resolveSafeOutputPath(p.value("out_json", std::string()), kJsonExt, {}, &err) : std::nullopt;
    const auto csv = jsn ? lt::resolveSafeOutputPath(p.value("out_csv", std::string()), kCsvExt, {}, &err) : std::nullopt;
    const auto evCsv =
        csv ? lt::resolveSafeOutputPath(p.value("out_events_csv", std::string()), kCsvExt, {}, &err) : std::nullopt;
    if (!vcd || !jsn || !csv || !evCsv) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }
    if (!evCsv->empty() && !opts->latency) {
        reply.fail(sess::error::InvalidParams, "out_events_csv requires 'latency'");
        return;
    }

    Pending pending;
    pending.options = std::move(*opts);
    pending.outputs = lt::OutputPaths{*vcd, *jsn, *csv, *evCsv};
    pending.options.collectEdges = !pending.outputs.jsonPath.empty();
    pending.options.collectLatencyEvents = !pending.outputs.eventsCsvPath.empty();
    const auto cfg = lt::buildCaptureConfig(m_device.info(), *spec, pending.warnings, err);
    if (!cfg) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }
    pending.reply = std::move(reply);
    pending.spec = *spec;
    pending.config = *cfg;
    m_pendingTriggerDescription = describeTrigger(cfg->trigger);
    m_pending = std::move(pending);
    showStatus(tr("Capture requested over the session socket..."));
    if (!m_device.startCapture(*cfg)) {
        const auto failed = std::exchange(m_pending, std::nullopt);
        m_pendingTriggerDescription.clear();
        failed->reply.fail(sess::error::Busy, "could not start the capture");
        return;
    }
    if (cfg->trigger)
        setArmed();
}

void MainWindow::sessionProbe(const json& p, sess::Responder reply)
{
    if (!m_device.isConnected()) {
        reply.fail(sess::error::NoDevice, "the GUI is not connected to a device");
        return;
    }
    if (m_device.isBusy() || m_pending) {
        reply.fail(sess::error::Busy, "the device is busy with another operation");
        return;
    }
    std::string err;
    auto params = lt::probeParamsFromJson(p, m_device.info().channels, err);
    if (!params) {
        reply.fail(sess::error::InvalidParams, err);
        return;
    }
    if (params->channels.empty())
        for (uint8_t i = 0; i < m_device.info().channels; ++i)
            params->channels.push_back({i, {}});
    Pending pending;
    pending.isProbe = true;
    pending.reply = std::move(reply);
    pending.probeRate = params->rate;
    pending.probeWindow = params->window;
    m_pendingTriggerDescription = tr("none (probe)");
    m_pending = std::move(pending);
    showStatus(tr("Probe requested over the session socket..."));
    if (!m_device.startProbe(params->channels, static_cast<int>(params->window.count()), params->rate)) {
        const auto failed = std::exchange(m_pending, std::nullopt);
        m_pendingTriggerDescription.clear();
        failed->reply.fail(sess::error::Busy, "could not start the probe");
    }
}

}  // namespace lt::gui

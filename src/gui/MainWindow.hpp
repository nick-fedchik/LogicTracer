// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "device/AsyncDevice.hpp"
#include "services/Operations.hpp"
#include "services/Session.hpp"
#include "gui/WaveformWidget.hpp"

#include <QMainWindow>

#include <deque>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace lt::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    [[nodiscard]] WaveformWidget* waveform() noexcept { return m_waveform; }
    [[nodiscard]] lt::AsyncDevice& device() noexcept { return m_device; }

    // Shows a capture (from a device or from tests) and refreshes the AI report.
    // `setup` is the test-setup metadata recorded with the capture (shown in reports, saved to JSON).
    void setCapture(const lt::CaptureBuffer& buffer, const QString& triggerDescription, lt::SetupInfo setup = {});
    // Loads a saved lossless .json or .vcd capture into the viewer.
    bool openCaptureFile(const QString& path, QString* error = nullptr);
    // The text "Copy for AI" puts on the clipboard: the report for the current view and cursors.
    [[nodiscard]] QString aiReport() const;
    [[nodiscard]] QString aiReportForMode(lt::CopyMode mode) const;
    [[nodiscard]] lt::CopyMode copyMode() const;
    void setCopyMode(lt::CopyMode mode);
    void setAiNote(const QString& note);
    void setLatencySpec(const QString& spec);
    // Identifies the capture on screen; strictly increasing, never 0 once a capture exists.
    [[nodiscard]] uint64_t captureId() const noexcept { return m_currentCaptureId; }
    [[nodiscard]] WaveformWidget::DisplayState displayState() const noexcept { return m_waveform->displayState(); }
    [[nodiscard]] std::size_t historySize() const noexcept { return m_history.size(); }
    // Index of the history entry on screen (0 = oldest); historySize() when nothing is shown.
    [[nodiscard]] std::size_t historyIndex() const noexcept { return m_viewIndex; }
    [[nodiscard]] const lt::SetupInfo& setup() const noexcept { return m_setup; }
    // History keeps at most `entries` captures / `bytes` of samples, but always the newest one.
    void setHistoryLimits(std::size_t entries, std::size_t bytes);
    // Synthetic capture so the UI can be explored without hardware.
    void loadDemo();

    void setPortText(const QString& port);

    // Starts the local IPC endpoint (default path: lt::session::defaultSocketPath()) used by
    // `logictracer-cli annotate|view` and the MCP server. Returns false with a message if another
    // session owns the socket.
    bool startSession(const QString& path = {}, QString* error = nullptr);
    [[nodiscard]] lt::session::Server& session() noexcept { return m_session; }

public Q_SLOTS:
    void refreshPorts();
    void connectOrDisconnect();
    void startCapture();
    void startProbe();
    void showPreviousCapture();
    void showNextCapture();
    void clearSetupMetadata();
    // Moves the view to the next (+1) / previous (-1) anomaly of the whole capture, wrapping around.
    void jumpToAnomaly(int direction);
    void copyForAi();
    void copyBriefForAi();
    void copyCursorsForAi();
    void openCapture();
    void saveVcd();
    void saveJson();
    void saveCsv();

private Q_SLOTS:
    void onOpened(bool ok, const QString& error);
    void onCaptureFinished(const lt::CaptureResult& result);
    void onProbeFinished(const lt::ProbeResult& result);
    void onProgress(int stage);
    void onBusyChanged(bool busy);
    void onHover(bool has, double seconds, int channel, int level);
    void showWaveformContextMenu(const QPoint& globalPos);
    void updateReport();

private:
    struct Pending {
        lt::session::Responder reply;
        bool isProbe = false;
        lt::CaptureRequestSpec spec;
        lt::CaptureConfig config;
        lt::DocumentOptions options;
        std::vector<std::string> warnings;
        lt::OutputPaths outputs;
        uint32_t probeRate = 0;
        std::chrono::milliseconds probeWindow{0};
    };

    struct CaptureEntry {
        uint64_t id = 0;
        lt::CaptureBuffer buffer;
        QString trigger;
        lt::SetupInfo setup;
        std::vector<lt::Annotation> annotations;
        std::optional<std::size_t> cursorA, cursorB;
        std::vector<std::string> warnings;
        bool isProbe = false;
    };
    // The last failed attempt; its line levels explain why the waveform on screen is stale.
    struct FailureInfo {
        lt::CaptureBuffer lineDump;
        std::string status;
        std::string error;
        QString trigger;
    };

    void showNewCapture(const lt::CaptureBuffer& buffer, const QString& trigger, lt::SetupInfo setup,
                        std::vector<std::string> warnings, bool isProbe);
    void showEntry(std::size_t index);
    void saveViewState();
    void trimHistory();
    void updateHistoryUi();
    void setArmed();
    void restoreIdleState();
    void consumeNote();
    [[nodiscard]] QString failureReport() const;
    void handleSession(const std::string& method, const nlohmann::json& params, lt::session::Responder reply);
    void sessionAnnotate(const nlohmann::json& params, lt::session::Responder reply);
    void sessionCapture(const nlohmann::json& params, lt::session::Responder reply);
    void sessionProbe(const nlohmann::json& params, lt::session::Responder reply);
    [[nodiscard]] nlohmann::json stateJson() const;
    void buildUi();
    void buildMenus();
    void updateControls();
    void showStatus(const QString& text, bool error = false);
    [[nodiscard]] lt::ReportOptions buildReportOptions(std::optional<lt::CopyMode> modeOverride = std::nullopt) const;
    void copyWithMode(lt::CopyMode mode);
    [[nodiscard]] bool buildSpec(lt::CaptureRequestSpec& spec, QString& error) const;
    [[nodiscard]] bool buildConfig(lt::CaptureConfig& config, QString& triggerText, QString& error,
                                   std::vector<std::string>* warnings = nullptr) const;
    void updatePreflight();
    void setFieldError(QLineEdit* field, const QString& message);

    lt::AsyncDevice m_device;
    WaveformWidget* m_waveform = nullptr;
    QPlainTextEdit* m_report = nullptr;
    QLineEdit* m_aiNote = nullptr;
    QLineEdit* m_latencySpec = nullptr;
    QComboBox* m_copyMode = nullptr;
    QPushButton* m_copyButton = nullptr;
    QLabel* m_reportSize = nullptr;
    QTimer* m_reportTimer = nullptr;

    QComboBox* m_port = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_connect = nullptr;
    QLineEdit* m_channels = nullptr;
    QLineEdit* m_rate = nullptr;
    QLineEdit* m_samples = nullptr;
    QLineEdit* m_trigger = nullptr;
    QSpinBox* m_timeout = nullptr;
    QPushButton* m_capture = nullptr;
    QPushButton* m_probe = nullptr;
    QPushButton* m_cancel = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_hover = nullptr;
    QLabel* m_deviceInfo = nullptr;
    QLabel* m_preflight = nullptr;
    std::vector<std::string> m_pendingWarnings;
    std::vector<std::string> m_warnings;  // device-layer warnings for the capture on screen
    QCheckBox* m_notePin = nullptr;
    QPushButton* m_prev = nullptr;
    QPushButton* m_next = nullptr;
    QLabel* m_historyLabel = nullptr;
    std::deque<CaptureEntry> m_history;
    std::size_t m_viewIndex = 0;
    std::size_t m_maxHistoryEntries = 8;
    std::size_t m_maxHistoryBytes = std::size_t{64} << 20;
    uint64_t m_nextCaptureId = 0;
    uint64_t m_currentCaptureId = 0;
    std::optional<FailureInfo> m_failure;
    QString m_consumedNote;  // note text already attached to the previous device capture
    std::string m_lastBoard;
    std::optional<std::size_t> m_lastAnomaly;  // sample of the anomaly F3 last jumped to

    lt::session::Server m_session;
    std::optional<Pending> m_pending;
    QString m_triggerDescription;
    QString m_pendingTriggerDescription;
    lt::SetupInfo m_setup;
    QAction* m_copyAction = nullptr;
    QAction* m_snapAction = nullptr;
};

}  // namespace lt::gui

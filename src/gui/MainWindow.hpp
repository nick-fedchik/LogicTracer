// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "device/AsyncDevice.hpp"
#include "services/Operations.hpp"
#include "services/Session.hpp"
#include "gui/WaveformWidget.hpp"

#include <QMainWindow>

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

    lt::session::Server m_session;
    std::optional<Pending> m_pending;
    QString m_triggerDescription;
    QString m_pendingTriggerDescription;
    lt::SetupInfo m_setup;
    QAction* m_copyAction = nullptr;
    QAction* m_snapAction = nullptr;
};

}  // namespace lt::gui

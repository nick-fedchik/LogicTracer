// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "device/AsyncDevice.hpp"
#include "gui/CaptureFields.hpp"
#include "gui/CaptureProfiles.hpp"
#include "services/Operations.hpp"
#include "services/SelfTest.hpp"
#include "services/Session.hpp"
#include "gui/WaveformWidget.hpp"

#include <QMainWindow>

#include <array>
#include <deque>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace lt::gui {

class DiagnosticsDialog;
class GuiDiagnostics;
class PicoPinout;
class ProfileSelector;
class SerialPortPicker;
struct PicoWiring;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Rich text of Help > About (version, author, licence, links).
    [[nodiscard]] static QString aboutText();
    // The optional process stderr recorder outlives this window.
    explicit MainWindow(QWidget* parent = nullptr, GuiDiagnostics* diagnostics = nullptr);

    [[nodiscard]] WaveformWidget* waveform() noexcept { return m_waveform; }
    [[nodiscard]] lt::AsyncDevice& device() noexcept { return m_device; }

    // Shows a capture (from a device or from tests) and refreshes the AI report.
    // `setup` is the test-setup metadata recorded with the capture (shown in reports, saved to JSON).
    // `warnings`, `deviceVersion` and `board` describe a loaded capture's import caveats and source device.
    void setCapture(const lt::CaptureBuffer& buffer, const QString& triggerDescription, lt::SetupInfo setup = {},
                    std::vector<std::string> warnings = {}, std::string deviceVersion = {}, std::string board = {},
                    std::optional<lt::SegmentLayout> segments = {});
    // Loads a saved lossless .json or .vcd capture into the viewer.
    bool openCaptureFile(const QString& path, QString* error = nullptr);
    // The text "Copy for AI" puts on the clipboard: the report for the current view and cursors.
    [[nodiscard]] QString aiReport() const;
    [[nodiscard]] QString aiReportForMode(lt::CopyMode mode) const;
    [[nodiscard]] lt::CopyMode copyMode() const;
    void setCopyMode(lt::CopyMode mode);
    void setAiNote(const QString& note);
    void setLatencySpec(const QString& spec);
    void setDecodeSpec(const QString& spec);
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
    void applyStartupProfile(const profiles::Profile& profile);

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
    void editSetupMetadata();
    void setSetupMetadata(const lt::SetupInfo& setup);
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
    void showWaveformContextMenu(const QPoint& globalPos, std::size_t sample, uint64_t annotationId);
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

    // Which device produced a capture; empty for demo captures and files.
    struct DeviceStamp {
        std::string version;
        std::string board;
    };

    struct CaptureEntry {
        uint64_t id = 0;
        DeviceStamp stamp;
        lt::CaptureBuffer buffer;
        QString trigger;
        lt::SetupInfo setup;
        std::vector<lt::Annotation> annotations;
        std::optional<std::size_t> cursorA, cursorB;
        std::vector<std::string> warnings;
        bool isProbe = false;
        std::optional<lt::SegmentLayout> segments;  // segmented capture: analysed window by window
    };
    // The last failed attempt; its line levels explain why the waveform on screen is stale.
    struct FailureInfo {
        lt::CaptureBuffer lineDump;
        std::string status;
        std::string error;
        QString trigger;
    };

    void showNewCapture(const lt::CaptureBuffer& buffer, const QString& trigger, lt::SetupInfo setup,
                        std::vector<std::string> warnings, bool isProbe, DeviceStamp stamp = {},
                        std::optional<lt::SegmentLayout> segments = {});
    // Anomalies over the whole shown capture (window by window for a segmented one).
    [[nodiscard]] std::vector<lt::AnomalyIncident> findAllAnomalies(const lt::ReportOptions& options) const;
    // AI report of the shown capture (segmentsForAi for a segmented one).
    [[nodiscard]] std::string reportFor(const lt::ReportOptions& options) const;
    std::optional<lt::SegmentLayout> m_segments;  // of the shown capture
    [[nodiscard]] DeviceStamp liveStamp() const;
    // Remembers the failed operation, returns to the newest capture and marks the display stale.
    void recordFailure(FailureInfo failure);
    void rearmRepeat(uint32_t generation);
    // Non-blocking file dialog; `onChosen` runs only if the capture on screen is still the one it was opened for.
    void askFile(bool save, const QString& title, const QString& name, const QString& filter,
                 std::function<void(const QString&)> onChosen);
    void showEntry(std::size_t index);
    void saveViewState();
    void trimHistory();
    void updateHistoryUi();
    void setArmed();
    void restoreIdleState();
    void consumeNote();
    // `noteOverride` replaces the GUI note (a session request may carry its own).
    [[nodiscard]] QString failureReport(const std::optional<QString>& noteOverride = std::nullopt) const;
    void handleSession(const std::string& method, const nlohmann::json& params, lt::session::Responder reply);
    void sessionAnnotate(const nlohmann::json& params, lt::session::Responder reply);
    void sessionSetView(const nlohmann::json& params, lt::session::Responder reply);
    void sessionGetAnomalies(const nlohmann::json& params, lt::session::Responder reply);
    // Validates the optional "capture_id"; replies with InvalidParams/StaleCapture and returns false on mismatch.
    bool checkCaptureId(const nlohmann::json& params, lt::session::Responder& reply);
    void sessionCapture(const nlohmann::json& params, lt::session::Responder reply);
    void sessionProbe(const nlohmann::json& params, lt::session::Responder reply);
    // Runs the self-test on the worker thread; answers `reply` (session) or shows the result (menu).
    void startSelfTest(const lt::SelfTestOptions& options, std::optional<lt::session::Responder> reply);
    void onTaskFinished();
    [[nodiscard]] nlohmann::json stateJson() const;
    void buildUi();
    QWidget* buildCaptureFields();
    QWidget* buildProfileBar(QWidget* parent);
    CaptureFields switchCapturePlan();
    void buildMenus();
    void showPinout(bool capture);
    void refreshPinout();
    void applyPinout();
    [[nodiscard]] PicoWiring picoWiring(bool capture) const;
    void showAbout();
    void updateControls();
    void updatePortControls();
    bool demoSelected() const;
    int setupChannelLimit() const;
    void updateDeviceSetup();
    void updateMetadataButton();
    void stopRepeat(const QString& why = {});
    QCheckBox* m_repeat = nullptr;
    QPushButton* m_metadataButton = nullptr;
    QWidget* withBuilder(QLineEdit* field, const char* buttonName, const std::function<void()>& open);
    std::vector<int> builderChannels() const;
    void showStatus(const QString& text, bool error = false);
    [[nodiscard]] lt::ReportOptions buildReportOptions(std::optional<lt::CopyMode> modeOverride = std::nullopt) const;
    void copyWithMode(lt::CopyMode mode);
    [[nodiscard]] bool buildSpec(lt::CaptureRequestSpec& spec, QString& error, CaptureField& field) const;
    [[nodiscard]] bool buildConfig(const CaptureRequestSpec& spec, CaptureConfig& config, QString& error,
                                   std::vector<std::string>& warnings, CaptureField& field) const;
    void updatePreflight();
    void setFieldError(QWidget* field, const QString& message);
    QWidget* captureControl(CaptureField field) const;
    [[nodiscard]] CaptureFields captureFields() const;
    void setCaptureFields(const CaptureFields& fields);

    lt::AsyncDevice m_device;
    WaveformWidget* m_waveform = nullptr;
    PicoPinout* m_pinout = nullptr;
    QPlainTextEdit* m_report = nullptr;
    GuiDiagnostics* m_diagnostics;
    DiagnosticsDialog* m_diagnosticsDialog = nullptr;
    QLineEdit* m_aiNote = nullptr;
    QLineEdit* m_latencySpec = nullptr;
    QLineEdit* m_decodeSpec = nullptr;
    QComboBox* m_copyMode = nullptr;
    QPushButton* m_copyButton = nullptr;
    QLabel* m_reportSize = nullptr;
    QTimer* m_reportTimer = nullptr;

    SerialPortPicker* m_port = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_connect = nullptr;
    QPushButton* m_deviceSetup = nullptr;
    uint64_t m_demoPreviewId = 0;
    bool m_usingDemoFields = false;
    CaptureFields m_otherFields = CaptureFields::defaults(true);
    ProfileSelector* m_profiles = nullptr;
    QString m_otherProfilePath;
    std::optional<profiles::Profile> m_disconnectedProfileBase;
    QLineEdit* m_channels = nullptr;
    QLineEdit* m_rate = nullptr;
    QLineEdit* m_samples = nullptr;
    QLineEdit* m_trigger = nullptr;
    QLabel* m_triggerSummary = nullptr;
    QPushButton* m_fixSetup = nullptr;
    CaptureField m_invalidField = CaptureField::Channels;
    std::array<QLabel*, 5> m_fieldHints{};
    QSpinBox* m_timeout = nullptr;
    QSpinBox* m_windows = nullptr;  // segmented capture: trigger windows per run
    QPushButton* m_capture = nullptr;
    QPushButton* m_probe = nullptr;
    QPushButton* m_cancel = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_hover = nullptr;
    QLabel* m_deviceInfo = nullptr;
    QLabel* m_preflight = nullptr;
    QLabel* m_settingsPreflight = nullptr;
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
    // The latest finished capture or probe attempt: a report bound to an attempt id never describes another one.
    struct LastAttempt {
        enum class Kind { None, Ok, Failed, Cancelled };
        uint64_t id = 0;
        Kind kind = Kind::None;
        uint64_t captureId = 0;
    };
    uint64_t m_attemptCounter = 0;
    LastAttempt m_lastAttempt;
    bool bindAttempt(const nlohmann::json& params, nlohmann::json& bound, lt::session::Responder& reply);
    QString m_consumedNote;  // note text already attached to the previous device capture
    std::string m_lastBoard;
    std::optional<std::size_t> m_lastAnomaly;  // sample of the anomaly F3 last jumped to

    lt::session::Server m_session;
    std::optional<Pending> m_pending;
    struct SelfTestRun {
        std::shared_ptr<lt::SelfTestOutcome> outcome;
        std::optional<lt::session::Responder> reply;
    };
    std::optional<SelfTestRun> m_selfTest;
    QString m_triggerDescription;
    QString m_pendingTriggerDescription;
    lt::SetupInfo m_setup;      // setup of the entry on screen (editable)
    lt::SetupInfo m_nextSetup;  // setup new captures start with; changed only by the engineer
    uint32_t m_repeatGeneration = 0;  // invalidates pending Repeat retries when Repeat is toggled
    DeviceStamp m_stamp;  // device that produced the capture on screen
    QAction* m_copyAction = nullptr;
    QAction* m_hideInactiveAction = nullptr;
    QAction* m_snapAction = nullptr;
};

}  // namespace lt::gui

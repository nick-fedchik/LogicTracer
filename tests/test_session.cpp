// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/Exporters.hpp"
#include "core/SignalAnalyzer.hpp"
#include "FakeDevice.hpp"
#include "gui/MainWindow.hpp"
#include "services/Session.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTimer>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>

using namespace std::chrono_literals;
using json = nlohmann::json;
namespace sess = lt::session;

namespace {

// The server lives on the test thread's event loop, so every client runs elsewhere while we spin events.
template <typename F>
auto awaitOffThread(F&& f)
{
    auto fut = std::async(std::launch::async, std::forward<F>(f));
    QElapsedTimer timer;
    timer.start();
    while (fut.wait_for(0ms) != std::future_status::ready && timer.elapsed() < 15000)
        QTest::qWait(10);
    return fut.get();
}

struct CliRun {
    int exitCode = -1;
    QString out;
    QString err;
    json doc;
};

CliRun runCli(const QStringList& args, const QString& socket)
{
    QProcess p;
    p.start(LT_CLI_PATH, QStringList{args} << "--socket" << socket);
    CliRun r;
    for (int i = 0; i < 300 && p.state() != QProcess::NotRunning; ++i)
        QTest::qWait(50);  // keep the GUI's event loop alive while the CLI talks to it
    p.waitForFinished(1000);
    r.exitCode = p.exitCode();
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    r.doc = json::parse(r.out.toStdString(), nullptr, false);
    return r;
}

}  // namespace

// The GUI answers on this thread's event loop, so the blocking client call runs on another thread.
lt::session::CallResult callInThread(const QString& socketPath, const std::string& method,
                                     const nlohmann::json& params = nlohmann::json::object())
{
    auto future = std::async(std::launch::async,
                             [=] { return lt::session::call(socketPath, method, params, 5000); });
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        QCoreApplication::processEvents();
    return future.get();
}

class SessionTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString socket(const char* name = "s.sock") const { return m_dir.filePath(name); }

private Q_SLOTS:
    void serverAnswersAndRejectsBadInput()
    {
        sess::Server server;
        server.setHandler([](const std::string& method, const json& params, sess::Responder reply) {
            if (method == "echo")
                reply.ok(params);
            else if (method == "later")
                QTimer::singleShot(50, [reply] { reply.ok({{"late", true}}); });
            else
                reply.fail(sess::error::MethodNotFound, "unknown " + method);
        });
        QVERIFY(server.listen(socket()));

        const auto path = socket();
        auto r = awaitOffThread([path] { return sess::call(path, "echo", {{"a", 1}}); });
        QVERIFY(r.ok);
        QCOMPARE(r.result["a"].get<int>(), 1);

        r = awaitOffThread([path] { return sess::call(path, "later"); });  // answered asynchronously
        QVERIFY(r.ok && r.result["late"].get<bool>());

        r = awaitOffThread([path] { return sess::call(path, "nope"); });
        QVERIFY(!r.ok && r.connected);
        QCOMPARE(r.errorCode, sess::error::MethodNotFound);

        // Garbage on the wire is answered with a parse error, not a crash or a hang.
        auto raw = awaitOffThread([path] {
            QLocalSocket s;
            s.connectToServer(path);
            s.waitForConnected(1000);
            s.write("this is not json\n{\"id\":7}\n");
            s.waitForBytesWritten(1000);
            QByteArray all;
            while (all.count('\n') < 2 && s.waitForReadyRead(2000))
                all += s.readAll();
            return all;
        });
        QVERIFY(raw.contains("-32700"));
        QVERIFY(raw.contains("-32600"));

        // A live session cannot be hijacked; once closed, the path can be reused.
        sess::Server second;
        QString err;
        QVERIFY(!second.listen(socket(), &err));
        QVERIFY(err.contains("already"));
        server.close();
        QVERIFY(second.listen(socket(), &err));
    }

    void noSessionIsReportedCleanly()
    {
        const auto r = sess::call(socket("missing.sock"), "ping");
        QVERIFY(!r.connected && !r.ok);
        QVERIFY(!r.error.empty());
    }

    void annotationsFlowFromCliIntoTheGui()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QString err;
        QVERIFY2(win.startSession(socket("gui.sock"), &err), qPrintable(err));
        const QString s = socket("gui.sock");

        auto r = runCli({"annotate", "--time", "205us", "--label", "ECHO rises", "--color", "orange", "--json"}, s);
        QVERIFY2(r.exitCode == 0, qPrintable(r.out));
        QCOMPARE(r.doc["id"].get<int>(), 1);
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{1});
        const auto& a = win.waveform()->annotations().front();
        QCOMPARE(a.label, std::string("ECHO rises"));
        QCOMPARE(a.author, std::string("cli"));
        QVERIFY(std::abs(a.timeSeconds - 205e-6) < 1e-12);

        r = runCli({"annotate", "--time", "1ms", "--label", "span", "--duration", "100us", "--json"}, s);
        QCOMPARE(r.exitCode, 0);

        // The marker is part of what an AI reads back.
        QVERIFY(win.aiReport().contains("#1 t = 205 us [cli] \"ECHO rises\""));
        r = runCli({"view", "--report"}, s);
        QVERIFY(r.out.contains("Annotations (2 total, 2 in the visible range)"));
        QVERIFY(r.out.contains("ASCII timeline"));

        r = runCli({"view", "--json"}, s);
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.doc["has_capture"].get<bool>());
        QCOMPARE(r.doc["annotation_count"].get<int>(), 2);
        QCOMPARE(r.doc["capture"]["trigger"].get<std::string>(), std::string("CH1 rising"));

        r = runCli({"annotate", "--list", "--json"}, s);
        QCOMPARE(r.doc["annotations"].size(), std::size_t{2});
        r = runCli({"annotate", "--clear", "--json"}, s);
        QCOMPARE(r.doc["cleared"].get<int>(), 2);
        QVERIFY(win.waveform()->annotations().empty());
    }

    void annotationCanBePinnedToACaptureId()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QVERIFY(win.startSession(socket("id.sock")));
        const QString s = socket("id.sock");

        auto r = runCli({"view", "--json"}, s);
        const auto id = r.doc["capture_id"].get<std::uint64_t>();
        QVERIFY(id > 0);

        r = runCli({"annotate", "--time", "5us", "--label", "ok", "--capture-id", QString::number(id), "--json"}, s);
        QVERIFY2(r.exitCode == 0, qPrintable(r.out));

        r = runCli({"annotate", "--time", "6us", "--label", "late", "--capture-id", QString::number(id + 7), "--json"}, s);
        QVERIFY(r.exitCode != 0);
        QCOMPARE(r.doc["error_kind"].get<std::string>(), std::string("StaleCapture"));
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{1});

        QCOMPARE(runCli({"annotate", "--time", "5us", "--label", "x", "--capture-id", "0", "--json"}, s).exitCode, 64);
    }

    // Reads and destructive calls honour capture_id too, so a newer capture is never wiped or misreported.
    void readsAndClearsHonourTheCaptureId()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QVERIFY(win.startSession(socket("guard.sock")));
        const QString s = socket("guard.sock");
        const auto id = runCli({"view", "--json"}, s).doc["capture_id"].get<std::uint64_t>();
        QVERIFY(runCli({"annotate", "--time", "5us", "--label", "keep", "--json"}, s).exitCode == 0);

        const auto wrong = QString::number(id + 5);
        auto r = runCli({"view", "--report", "--capture-id", wrong, "--json"}, s);
        QVERIFY(r.exitCode != 0);
        QCOMPARE(r.doc["error_kind"].get<std::string>(), std::string("StaleCapture"));
        r = runCli({"annotate", "--clear", "--capture-id", wrong, "--json"}, s);
        QCOMPARE(r.doc["error_kind"].get<std::string>(), std::string("StaleCapture"));
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{1});  // nothing was wiped
        QCOMPARE(runCli({"view", "--report", "--capture-id", QString::number(id), "--json"}, s).exitCode, 0);
    }

    // Wrongly typed or non-UTF-8-safe parameters are answered with an error and never terminate the GUI.
    void malformedRequestsDoNotCrashTheSession()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QVERIFY(win.startSession(socket("fuzz.sock")));
        const QString s = socket("fuzz.sock");

        std::string author(39, 'x');
        author += "\xD0\x90";  // a 2-byte character that byte-wise truncation to 40 would split
        auto r = callInThread(s, "annotate", {{"time_us", 5}, {"label", "n"}, {"author", author}});
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(callInThread(s, "list_annotations").ok);
        QVERIFY(callInThread(s, "get_report").ok);

        r = callInThread(s, "annotate", {{"time_s", 1e308}, {"label", "far"}});
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, lt::session::error::InvalidParams);
        r = callInThread(s, "get_report", {{"latency", 5}});
        QCOMPARE(r.errorCode, lt::session::error::InvalidParams);
        r = callInThread(s, "capture", {{"out_vcd", 12345}});
        QVERIFY(!r.ok);
        QVERIFY(callInThread(s, "ping").ok);  // still alive
    }

    // A stale-state report honours the request's own note, like a normal report does.
    void failureReportHonoursTheRequestNote()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QVERIFY(win.startSession(socket("failnote.sock")));
        const QString s = socket("failnote.sock");
        lt::CaptureResult timeout;
        timeout.status = lt::CaptureStatus::Timeout;
        timeout.error = "Trigger timeout";
        Q_EMIT win.device().captureFinished(timeout);
        QVERIFY(win.displayState() == lt::gui::WaveformWidget::DisplayState::Stale);
        const auto r = callInThread(s, "get_report", {{"note", "is the pull-up fitted?"}, {"mode", "brief"}});
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(r.result["report"].get<std::string>().find("is the pull-up fitted?") != std::string::npos);
        QCOMPARE(r.result["metadata"]["ignored_params"][0].get<std::string>(), std::string("mode"));
        QCOMPARE(callInThread(s, "get_report", {{"mode", "fancy"}}).errorCode, lt::session::error::InvalidParams);
    }

    void viewCanBeSteeredAndAnomaliesListed()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        QVERIFY(win.startSession(socket("steer.sock")));
        const QString s = socket("steer.sock");
        const auto id = runCli({"view", "--json"}, s).doc["capture_id"].get<std::uint64_t>();

        auto r = runCli({"view", "--start", "100us", "--end", "500us", "--cursor-a", "150us", "--cursor-b", "250us",
                         "--capture-id", QString::number(id), "--json"}, s);
        QVERIFY2(r.exitCode == 0, qPrintable(r.out));
        QCOMPARE(win.waveform()->visibleFirst(), std::size_t{300});
        QCOMPARE(win.waveform()->visibleEnd(), std::size_t{700});
        QCOMPARE(win.waveform()->cursorA().value(), std::size_t{350});
        QCOMPARE(win.waveform()->cursorB().value(), std::size_t{450});
        QCOMPARE(r.doc["cursors"]["a"]["us"].get<double>(), 150.0);

        r = runCli({"view", "--hide-inactive", "--clear-cursors", "--json"}, s);
        QVERIFY(r.doc["hide_inactive"].get<bool>());
        QVERIFY(!win.waveform()->cursorA());
        QVERIFY(win.waveform()->visibleChannels().size() < 4);
        r = runCli({"view", "--show-inactive", "--fit", "--json"}, s);
        QVERIFY(!r.doc["hide_inactive"].get<bool>());
        QCOMPARE(win.waveform()->visibleFirst(), std::size_t{0});

        r = runCli({"view", "--fit", "--capture-id", QString::number(id + 3), "--json"}, s);
        QVERIFY(r.exitCode != 0);
        QCOMPARE(r.doc["error_kind"].get<std::string>(), std::string("StaleCapture"));
        QCOMPARE(runCli({"view", "--start", "1ms", "--json"}, s).exitCode, 64);  // unpaired
        QVERIFY(runCli({"view", "--cursor-a", "9s", "--json"}, s).exitCode != 0);  // outside the capture

        r = runCli({"view", "--anomalies", "--max-count", "3", "--capture-id", QString::number(id), "--json"}, s);
        QVERIFY2(r.exitCode == 0, qPrintable(r.out));
        QCOMPARE(r.doc["capture_id"].get<std::uint64_t>(), id);
        QVERIFY(r.doc.contains("display_state"));
        QVERIFY(r.doc["incidents"].size() <= 3);
        for (const auto& i : r.doc["incidents"])
            for (const char* key : {"sample", "time_us", "severity", "kind", "detail"})
                QVERIFY(i.contains(key));
        QCOMPARE(runCli({"view", "--anomalies", "--capture-id", QString::number(id + 3), "--json"}, s).doc["error_kind"].get<std::string>(),
                 std::string("StaleCapture"));
    }

    void invalidAnnotationsAreRejected()
    {
        lt::gui::MainWindow win;
        win.loadDemo();
        QVERIFY(win.startSession(socket("bad.sock")));
        const QString s = socket("bad.sock");

        QCOMPARE(runCli({"annotate", "--time", "5", "--label", "x", "--json"}, s).exitCode, 64);  // unit missing
        QCOMPARE(runCli({"annotate", "--time", "5us", "--label", "x", "--color", "notacolour", "--json"}, s).exitCode, 64);
        QCOMPARE(runCli({"annotate", "--time", "5us", "--label", "   ", "--json"}, s).exitCode, 64);
        QCOMPARE(runCli({"annotate", "--label", "x", "--json"}, s).exitCode, 64);
        QVERIFY(win.waveform()->annotations().empty());

        // Without any capture there is nothing to anchor a marker to.
        lt::gui::MainWindow empty;
        QVERIFY(empty.startSession(socket("empty.sock")));
        const auto r = runCli({"annotate", "--time", "5us", "--label", "x", "--json"}, socket("empty.sock"));
        QCOMPARE(r.exitCode, 1);
        QVERIFY(QString::fromStdString(r.doc["error"].get<std::string>()).contains("no capture"));
    }

    void cliWithoutGuiExplainsWhatToDo()
    {
        const auto r = runCli({"annotate", "--time", "5us", "--label", "x", "--json"}, socket("nobody.sock"));
        QCOMPARE(r.exitCode, 1);
        QVERIFY(QString::fromStdString(r.doc["error"].get<std::string>()).contains("logictracer-gui"));
        QVERIFY(!r.doc["session_connected"].get<bool>());
    }

    // One effective setup everywhere: the engineer's setup overridden per key by the request's meta, which is
    // used for that capture only.
    void sessionMetaOverridesPerKeyWithoutPersisting()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("V6_5", true));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        LT_REQUIRE_PTY(dev);
        lt::gui::MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        QVERIFY(win.startSession(socket("meta.sock")));
        const QString path = socket("meta.sock");
        win.setPortText(QString::fromStdString(dev.port()));
        for (auto* b : win.findChildren<QPushButton*>())
            if (b->text() == "Connect")
                b->click();
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        win.setSetupMetadata({{"dut.board", "engineer"}, {"wiring", "stable"}});

        const json withMeta = {{"channels", "1"}, {"samples", 100}, {"meta", {{"dut.board", "ONE-OFF"}, {"extra", "session"}}}};
        auto r = awaitOffThread([path, withMeta] { return sess::call(path, "capture", withMeta, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["setup"]["dut.board"].get<std::string>(), std::string("ONE-OFF"));
        QCOMPARE(r.result["setup"]["wiring"].get<std::string>(), std::string("stable"));  // merged, not wiped
        QCOMPARE(r.result["setup"]["extra"].get<std::string>(), std::string("session"));
        QCOMPARE(win.setup().size(), std::size_t{3});  // the entry on screen shows the same effective setup

        const json plain = {{"channels", "1"}, {"samples", 100}};
        r = awaitOffThread([path, plain] { return sess::call(path, "capture", plain, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["setup"]["dut.board"].get<std::string>(), std::string("engineer"));  // one-off not kept
        QVERIFY(!r.result["setup"].contains("extra"));
    }

    void captureAndProbeOverTheSocket()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("V6_5", true));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        LT_REQUIRE_PTY(dev);
        lt::gui::MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        QVERIFY(win.startSession(socket("cap.sock")));
        const QString path = socket("cap.sock");

        // Not connected yet.
        auto r = awaitOffThread([path] { return sess::call(path, "capture", {{"channels", "1,2"}}); });
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, sess::error::NoDevice);
        r = awaitOffThread([path] { return sess::call(path, "selftest", json::object()); });
        QCOMPARE(r.errorCode, sess::error::NoDevice);
        r = awaitOffThread([path] { return sess::call(path, "selftest", {{"a", 0}}); });
        QCOMPARE(r.errorCode, sess::error::InvalidParams);

        win.setPortText(QString::fromStdString(dev.port()));
        for (auto* b : win.findChildren<QPushButton*>())
            if (b->text() == "Connect")
                b->click();
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        QTemporaryDir out;
        const auto vcd = out.filePath("c.vcd").toStdString();
        const json params = {{"channels", "1:A,2:B"}, {"samples", 300}, {"out_vcd", vcd}};
        r = awaitOffThread([path, params] { return sess::call(path, "capture", params, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(r.result["capture"]["samples"].get<int>(), 300);
        QCOMPARE(r.result["capture"]["channels"][0]["name"].get<std::string>(), std::string("A"));
        QVERIFY(QFile::exists(QString::fromStdString(vcd)));
        // The session reports the validated (canonical) path; macOS temp dirs sit behind /var -> /private/var.
        QCOMPARE(QFileInfo(QString::fromStdString(r.result["outputs"]["vcd"].get<std::string>())).canonicalFilePath(),
                 QFileInfo(QString::fromStdString(vcd)).canonicalFilePath());
        // The engineer sees the capture the agent asked for.
        QCOMPARE(win.waveform()->capture().size(), std::size_t{300});

        r = awaitOffThread([path] { return sess::call(path, "capture", {{"rate", "fast"}}); });
        QCOMPARE(r.errorCode, sess::error::InvalidParams);

        // Segmented capture through the session: analysed window by window, seams shown in the GUI.
        const json seg = {{"channels", "1:A,2:B"}, {"samples", 50},    {"pre_samples", 10},
                          {"trigger", "1:rising"}, {"segments", 3}, {"latency", "1->2"}};
        r = awaitOffThread([path, seg] { return sess::call(path, "capture", seg, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["config"]["segments"].get<int>(), 3);
        QCOMPARE(r.result["segments"]["windows"].size(), std::size_t{3});
        QCOMPARE(win.waveform()->capture().size(), std::size_t{130});
        QCOMPARE(win.waveform()->segmentStarts().size(), std::size_t{3});
        r = awaitOffThread([path] { return sess::call(path, "get_report", json::object()); });
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(r.result["report"].get<std::string>().find("**3 trigger windows**") != std::string::npos);
        QCOMPARE(r.result["metadata"]["segments"]["count"].get<int>(), 3);
        json segVcd = seg;
        segVcd["out_vcd"] = vcd;
        r = awaitOffThread([path, segVcd] { return sess::call(path, "capture", segVcd, 10000); });
        QCOMPARE(r.errorCode, sess::error::InvalidParams);

        r = awaitOffThread([path] { return sess::call(path, "probe", {{"channels", "1,2"}, {"window_ms", 2}}, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(r.result["channels"].size(), std::size_t{2});

        // CLI probe & capture transparently proxy through the GUI session when it holds the port (A-03).
        const auto cliProbe = runCli({"probe", "--port", QString::fromStdString(dev.port()), "--channels", "1,2", "--json"}, path);
        QVERIFY2(cliProbe.exitCode == 0, qPrintable(cliProbe.out));
        QCOMPARE(cliProbe.doc["status"].get<std::string>(), std::string("OK"));

        // Self-test through the GUI: this firmware has no generator, which the answer must say; the GUI stays usable.
        r = awaitOffThread([path] { return sess::call(path, "selftest", {{"freq", "10M"}}, 10000); });
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, sess::error::Failed);
        QVERIFY2(r.error.find("test-signal generator") != std::string::npos, r.error.c_str());
        const auto cliSelf = runCli({"selftest", "--port", QString::fromStdString(dev.port()), "--json"}, path);
        QCOMPARE(cliSelf.exitCode, 1);
        QVERIFY(cliSelf.doc["error"].get<std::string>().find("test-signal generator") != std::string::npos);
        QVERIFY(!win.device().isBusy());
        r = awaitOffThread([path] { return sess::call(path, "probe", {{"channels", "1"}, {"window_ms", 2}}, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
    }

    // A GUI-routed capture behaves like a direct one: usage errors exit 64 without disturbing the screen, results
    // and failed checks are printed, and a report bound to an attempt never describes another attempt.
    void sessionCapturesMatchDirectOnesAndBindReportsToTheAttempt()
    {
        QTemporaryDir rec;
        const QString file = rec.filePath("rec.json");
        std::vector<uint32_t> samples(700, 0u);
        for (std::size_t t0 : {100u, 300u, 500u})
            for (std::size_t w = 0; w < 20; ++w)
                samples[t0 + w] |= 1u;  // CH1 toggles, CH2 never does
        const lt::CaptureBuffer buf({{0, "STIM"}, {1, "IDLE"}}, 1'000'000, 0, samples);
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        QVERIFY(lt::writeJsonFile(file.toStdString(), lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true)));

        lt::gui::MainWindow win;
        win.show();
        const QString path = socket("attempt.sock");
        QVERIFY(win.startSession(path));
        const QString port = "replay:" + file;
        win.setPortText(port);
        for (auto* b : win.findChildren<QPushButton*>())
            if (b->text() == "Connect")
                b->click();
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        const QString output = rec.filePath("capture.csv");
        const QStringList args = {"capture", "--port", port, "--freq", "1M", "--samples", "400", "--channels", "1:STIM,2:IDLE",
                                  "--summary", "--measure-latency", "1:rising..1:falling", "--expect", "1:transitions>100",
                                  "--invariant", "never(STIM)", "--out-csv", output, "--pull-up", "1"};
        const auto direct = runCli(QStringList(args) << "--no-session", path);
        QCOMPARE(direct.exitCode, 3);
        QVERIFY(QFile::remove(output));
        auto cli = runCli(args, path);
        QCOMPARE(cli.exitCode, 3);
        QVERIFY(QFile::exists(output));
        for (const auto* expected : {"STIM", "histogram", "Assertions: FAIL", "Invariants: FAIL", "wrote csv:"})
            QVERIFY2(cli.out.contains(expected), qPrintable(cli.out));
        QVERIFY(cli.err.contains("warning:") && cli.err.contains("1:transitions>100"));
        QCOMPARE(cli.err, direct.err);
        const auto resultLines = [](const QString& text) {
            return text.trimmed().split('\n').mid(1);  // first line names the transport
        };
        auto proxyLines = resultLines(cli.out);
        auto directLines = resultLines(direct.out);
        const QString prefix = "wrote csv: ";
        for (auto* lines : {&proxyLines, &directLines}) {
            QVERIFY(!lines->isEmpty());
            const auto written = lines->takeLast().trimmed();
            QVERIFY2(written.startsWith(prefix), qPrintable(written));
            // The GUI canonicalizes paths; /var and /private/var can identify the same file.
            QCOMPARE(QFileInfo(written.mid(prefix.size())).canonicalFilePath(), QFileInfo(output).canonicalFilePath());
        }
        QCOMPARE(proxyLines, directLines);

        // An invalid request is a usage error, like on the direct path, and keeps the capture on screen live.
        const auto idBefore = runCli({"view", "--json"}, path).doc["capture_id"].get<std::uint64_t>();
        cli = runCli({"capture", "--port", port, "--freq", "3M", "--samples", "8", "--json"}, path);
        QCOMPARE(cli.exitCode, 64);
        const auto state = runCli({"view", "--json"}, path).doc;
        QCOMPARE(state["capture_id"].get<std::uint64_t>(), idBefore);
        QCOMPARE(state["display_state"].get<std::string>(), std::string("live"));

        // Reports follow their own attempt, also when it failed.
        const json ok = {{"channels", "1:STIM,2:IDLE"}, {"samples", 50}};
        const json failing = {{"channels", "1:STIM,2:IDLE"}, {"samples", 50}, {"trigger", "2:rising"},
                              {"timeout_ms", 200}, {"probe_on_timeout", false}};
        auto r = awaitOffThread([path, ok] { return sess::call(path, "capture", ok, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        const auto first = r.result["attempt_id"].get<std::uint64_t>();
        const auto firstCapture = r.result.at("capture_id");
        r = awaitOffThread([path, failing] { return sess::call(path, "capture", failing, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["status"].get<std::string>(), std::string("TIMEOUT"));
        const auto second = r.result["attempt_id"].get<std::uint64_t>();
        QVERIFY(second > first);
        const auto guarded = callInThread(path, "get_report", {{"capture_id", firstCapture}});
        QCOMPARE(guarded.errorCode, sess::error::StaleCapture);

        auto report = [&](std::uint64_t attempt) {
            return awaitOffThread([path, attempt] { return sess::call(path, "get_report", {{"attempt_id", attempt}}); });
        };
        auto rep = report(first);  // a later failure must not replace the report of the earlier capture
        QVERIFY(!rep.ok);
        QCOMPARE(rep.errorCode, sess::error::StaleCapture);
        rep = report(second);
        QVERIFY2(rep.ok, rep.error.c_str());
        QCOMPARE(rep.result["metadata"]["status"].get<std::string>(), std::string("TIMEOUT"));

        r = awaitOffThread([path, ok] { return sess::call(path, "capture", ok, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        const auto third = r.result["attempt_id"].get<std::uint64_t>();
        QCOMPARE(report(second).errorCode, sess::error::StaleCapture);
        rep = report(third);
        QVERIFY2(rep.ok, rep.error.c_str());
        QCOMPARE(rep.result["metadata"]["capture_id"].get<std::uint64_t>(), r.result["capture_id"].get<std::uint64_t>());
    }

    void cancelledAttemptCannotReturnPreviousWaveformReport()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send(fake::idReply("V6_5", true));
            else if (payload[0] == 1)
                d.send("CAPTURE_STARTED\n");  // stay armed until the host cancels
        });
        LT_REQUIRE_PTY(dev);
        lt::gui::MainWindow win;
        const QString path = socket("cancel.sock");
        QVERIFY(win.startSession(path));
        QVERIFY(win.device().open(QString::fromStdString(dev.port())));
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        win.loadDemo();
        const auto before = callInThread(path, "get_report");
        QVERIFY2(before.ok, before.error.c_str());
        const auto previousId = before.result.at("metadata").at("capture_id");

        connect(&win.device(), &lt::AsyncDevice::progress, &win, [&](int stage) {
            if (stage == static_cast<int>(lt::CaptureStage::WaitingForTrigger))
                win.device().requestCancel();
        });
        const auto cancelled = callInThread(path, "capture", {{"channels", "1"}, {"trigger", "1:rising"},
                                                               {"samples", 300}, {"timeout_ms", 3000}});
        QVERIFY2(cancelled.ok, cancelled.error.c_str());
        QCOMPARE(cancelled.result.at("status").get<std::string>(), std::string("CANCELLED"));
        QVERIFY(!cancelled.result.contains("capture_id"));
        const auto attempt = cancelled.result.at("attempt_id").get<uint64_t>();
        QVERIFY(attempt > 0);

        const auto bound = callInThread(path, "get_report", {{"attempt_id", attempt}});
        QVERIFY(!bound.ok);
        QCOMPARE(bound.errorCode, sess::error::StaleCapture);
        QVERIFY(bound.error.find("cancelled") != std::string::npos);
        const auto current = callInThread(path, "get_report");
        QVERIFY2(current.ok, current.error.c_str());
        QVERIFY(current.result.at("metadata").at("capture_id") == previousId);
    }

    void cliAiAndCopyWriteToClipboardBackend()
    {
        QTemporaryDir dir;
        const QString capFile = dir.filePath("trace.json");
        const QString clipFile = dir.filePath("clip.md");

        lt::gui::MainWindow win;
        win.loadDemo();
        win.waveform()->setCursorA(200);
        win.waveform()->setCursorB(240);
        const QString sock = socket("copy.sock");
        QVERIFY(win.startSession(sock));

        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        const auto& buf = win.waveform()->capture();
        auto doc = lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true, "PICO");
        doc["trigger_description"] = "CH1 rising";
        QVERIFY(lt::writeJsonFile(capFile.toStdString(), doc));

        // 1. Offline `analyze --ai --copy` using hermetic LOGICTRACER_CLIPBOARD_FILE.
        {
            QProcess p;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("LOGICTRACER_CLIPBOARD_FILE", clipFile);
            p.setProcessEnvironment(env);
            p.start(LT_CLI_PATH, {"analyze", capFile, "--ai", "--board", "PICO", "--measure-latency", "1..2",
                                  "--note", "Verify 5 us echo", "--copy"});
            QVERIFY(p.waitForFinished(5000));
            QCOMPARE(p.exitCode(), 0);
            QFile f(clipFile);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString clipped = QString::fromUtf8(f.readAll());
            QVERIFY(clipped.contains("Verify 5 us echo"));
            QVERIFY(clipped.contains("GPIO map: CH1=GP2, CH2=GP3"));
            QVERIFY(clipped.contains("Propagation latency: CH1 (CLK rising) -> CH2 (ECHO rising)"));
            QVERIFY(!clipped.contains("```json"));
        }

        // 2. Live GUI `view --ai-mode cursors --note ... --copy` over session socket.
        {
            QProcess p;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("LOGICTRACER_CLIPBOARD_FILE", clipFile);
            p.setProcessEnvironment(env);
            p.start(LT_CLI_PATH,
                    {"view", "--ai-mode", "cursors", "--note", "Inspect A..B", "--copy", "--socket", sock});
            for (int i = 0; i < 100 && p.state() != QProcess::NotRunning; ++i)
                QTest::qWait(20);
            p.waitForFinished(1000);
            QCOMPARE(p.exitCode(), 0);
            QFile f(clipFile);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString clipped = QString::fromUtf8(f.readAll());
            QVERIFY(clipped.contains("Inspect A..B"));
            QVERIFY(clipped.contains("Cursor range (A..B): samples [199, 241)"));
        }
    }

    void secondGuiCannotTakeTheSocket()
    {
        lt::gui::MainWindow a, b;
        QVERIFY(a.startSession(socket("one.sock")));
        QString err;
        QVERIFY(!b.startSession(socket("one.sock"), &err));
        QVERIFY(!err.isEmpty());
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    SessionTests t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_session.moc"

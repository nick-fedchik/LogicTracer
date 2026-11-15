// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/Exporters.hpp"
#include "FakeDevice.hpp"
#include "gui/MainWindow.hpp"
#include "services/Session.hpp"

#include <QApplication>
#include <QElapsedTimer>
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
    r.doc = json::parse(r.out.toStdString(), nullptr, false);
    return r;
}

}  // namespace

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
        lt::gui::MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        QVERIFY(win.startSession(socket("cap.sock")));
        const QString path = socket("cap.sock");

        // Not connected yet.
        auto r = awaitOffThread([path] { return sess::call(path, "capture", {{"channels", "1,2"}}); });
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, sess::error::NoDevice);

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
        QCOMPARE(r.result["outputs"]["vcd"].get<std::string>(), vcd);
        QVERIFY(QFile::exists(QString::fromStdString(vcd)));
        // The engineer sees the capture the agent asked for.
        QCOMPARE(win.waveform()->capture().size(), std::size_t{300});

        r = awaitOffThread([path] { return sess::call(path, "capture", {{"rate", "fast"}}); });
        QCOMPARE(r.errorCode, sess::error::InvalidParams);

        r = awaitOffThread([path] { return sess::call(path, "probe", {{"channels", "1,2"}, {"window_ms", 2}}, 10000); });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.result["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(r.result["channels"].size(), std::size_t{2});

        // CLI probe & capture transparently proxy through the GUI session when it holds the port (A-03).
        const auto cliProbe = runCli({"probe", "--port", QString::fromStdString(dev.port()), "--channels", "1,2", "--json"}, path);
        QVERIFY2(cliProbe.exitCode == 0, qPrintable(cliProbe.out));
        QCOMPARE(cliProbe.doc["status"].get<std::string>(), std::string("OK"));
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

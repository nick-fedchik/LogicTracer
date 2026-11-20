// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "FakeDevice.hpp"
#include "core/Exporters.hpp"
#include "gui/MainWindow.hpp"
#include "mcp/McpServer.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <set>
#include <sstream>

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

// Drives logictracer-mcp over stdio. Keeps spinning the event loop so an in-process GUI stays alive.
class McpClient {
public:
    explicit McpClient(const QStringList& args)
    {
        m_process.start(LT_MCP_PATH, args);
        m_process.waitForStarted(5000);
    }
    ~McpClient()
    {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(3000))
            m_process.kill();
    }

    json request(const std::string& method, const json& params = json::object())
    {
        const int id = ++m_id;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 30000) {
            m_buffer += m_process.readAllStandardOutput();
            while (m_buffer.contains('\n')) {
                const auto line = m_buffer.left(m_buffer.indexOf('\n'));
                m_buffer.remove(0, line.size() + 1);
                const json msg = json::parse(line.toStdString(), nullptr, false);
                if (msg.is_object() && msg.value("id", -1) == id)
                    return msg;
            }
            QTest::qWait(20);
        }
        return json();
    }

    json tool(const std::string& name, const json& arguments)
    {
        return request("tools/call", {{"name", name}, {"arguments", arguments}});
    }

    void send(const json& message) { m_process.write(QByteArray::fromStdString(message.dump()) + '\n'); }

private:
    QProcess m_process;
    QByteArray m_buffer;
    int m_id = 0;
};

fake::Device::Handler emulatedDevice()
{
    return [](fake::Device& d, const std::vector<uint8_t>& payload) {
        if (payload[0] == 0) {
            d.send(fake::idReply("0.2.0", true));
        } else if (payload[0] == 1) {
            const auto r = fake::parseRequest(payload);
            d.recordRequest(r);
            d.send("CAPTURE_STARTED\n");
            d.send(fake::captureData(r, 0x01));
        }
    };
}

}  // namespace

class McpTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void protocolBasics()
    {
        lt::mcp::McpServer server;
        const auto init = server.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                         {"params", {{"protocolVersion", "2024-11-05"}}}});
        QVERIFY(init);
        QCOMPARE((*init)["result"]["protocolVersion"].get<std::string>(), std::string("2024-11-05"));
        QVERIFY((*init)["result"]["capabilities"].contains("tools"));
        QCOMPARE((*init)["result"]["serverInfo"]["name"].get<std::string>(), std::string("logictracer-mcp"));

        const auto future = server.handle({{"id", 2}, {"method", "initialize"}, {"params", {{"protocolVersion", "2099-01-01"}}}});
        QCOMPARE((*future)["result"]["protocolVersion"].get<std::string>(), std::string("2025-06-18"));

        QVERIFY(!server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));  // no reply
        QCOMPARE((*server.handle({{"id", 3}, {"method", "ping"}}))["result"], json::object());

        const auto err = server.handle({{"id", 4}, {"method", "resources/list"}});
        QCOMPARE((*err)["error"]["code"].get<int>(), -32601);
        QCOMPARE((*err)["id"].get<int>(), 4);

        const auto batch = server.handle(json::array({{{"id", 5}, {"method", "ping"}}, {{"id", 6}, {"method", "ping"}}}));
        QVERIFY(batch && batch->is_array());
        QCOMPARE(batch->size(), std::size_t{2});
    }

    void toolsAreListedWithSchemas()
    {
        lt::mcp::McpServer server;
        const auto list = (*server.handle({{"id", 1}, {"method", "tools/list"}}))["result"]["tools"];
        std::set<std::string> names;
        for (const auto& t : list) {
            names.insert(t["name"].get<std::string>());
            QCOMPARE(t["inputSchema"]["type"].get<std::string>(), std::string("object"));
            QVERIFY(!t["description"].get<std::string>().empty());
        }
        const std::set<std::string> expected{"lt_detect_devices",
                                             "lt_probe_inputs",
                                             "lt_configure_and_capture",
                                             "lt_analyze_file",
                                             "lt_diff_captures",
                                             "lt_add_annotation",
                                             "lt_get_view",
                                             "lt_clear_annotations",
                                             "lt_set_view",
                                             "lt_get_anomalies",
                                             "lt_self_test"};
        QVERIFY(names == expected);
    }

    void badCallsAreRejectedProperly()
    {
        lt::mcp::McpServer server;
        auto r = server.handle({{"id", 1}, {"method", "tools/call"}, {"params", {{"name", "lt_nope"}}}});
        QCOMPARE((*r)["error"]["code"].get<int>(), -32602);
        r = server.handle({{"id", 2}, {"method", "tools/call"}, {"params", json::object()}});
        QCOMPARE((*r)["error"]["code"].get<int>(), -32602);

        // Wrongly typed arguments are a tool error (isError), not a crash.
        const auto bad = server.callTool("lt_detect_devices", {{"timeout_ms", "soon"}});
        QVERIFY(bad["isError"].get<bool>());
        QVERIFY(server.callTool("lt_probe_inputs", json::array())["isError"].get<bool>());
    }

    void streamLoopSurvivesGarbage()
    {
        lt::mcp::McpServer server;
        std::istringstream in("not json\n\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n[]\n");
        std::ostringstream out;
        server.run(in, out);
        std::istringstream lines(out.str());
        std::string a, b, c;
        std::getline(lines, a);
        std::getline(lines, b);
        std::getline(lines, c);
        QCOMPARE(json::parse(a)["error"]["code"].get<int>(), -32700);
        QCOMPARE(json::parse(b)["id"].get<int>(), 9);
        QCOMPARE(json::parse(c)["error"]["code"].get<int>(), -32600);
    }

    void directDeviceAccessOverStdio()
    {
        fake::Device dev(emulatedDevice());
        LT_REQUIRE_PTY(dev);
        McpClient client({"--no-session", "--port", QString::fromStdString(dev.port())});
        const auto init = client.request("initialize", {{"protocolVersion", "2025-06-18"}});
        QCOMPARE(init["result"]["serverInfo"]["name"].get<std::string>(), std::string("logictracer-mcp"));
        client.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

        const auto cap = client.tool("lt_configure_and_capture",
                                     {{"channels", "1:A,2:B"}, {"freq", "1M"}, {"samples", 200}});
        QVERIFY(cap.contains("result"));
        QVERIFY(!cap["result"]["isError"].get<bool>());
        const auto& doc = cap["result"]["structuredContent"];
        QCOMPARE(doc["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(doc["capture"]["samples"].get<int>(), 200);
        QCOMPARE(doc["capture"]["channels"][0]["name"].get<std::string>(), std::string("A"));
        // The text content carries the same document for clients that ignore structuredContent.
        QCOMPARE(json::parse(cap["result"]["content"][0]["text"].get<std::string>())["status"].get<std::string>(),
                 std::string("OK"));
        QCOMPARE(dev.requests().size(), std::size_t{1});

        const auto probe = client.tool("lt_probe_inputs", {{"channels", json::array({1, 2})}, {"window_ms", 2}});
        QVERIFY(!probe["result"]["isError"].get<bool>());
        QCOMPARE(probe["result"]["structuredContent"]["channels"][0]["state"].get<std::string>(), std::string("HIGH"));

        const auto seg = client.tool("lt_configure_and_capture", {{"channels", "1:A,2:B"},
                                                                  {"samples", 50},
                                                                  {"pre_samples", 10},
                                                                  {"trigger", "1:rising"},
                                                                  {"segments", 3}});
        QVERIFY2(!seg["result"]["isError"].get<bool>(), seg.dump().c_str());
        QCOMPARE(seg["result"]["structuredContent"]["segments"]["windows"].size(), std::size_t{3});
        QCOMPARE(dev.requests().back().loop, 2u);
        const auto segVcd = client.tool("lt_configure_and_capture", {{"channels", "1"}, {"trigger", "1:rising"},
                                                                     {"segments", 3}, {"out_vcd", "x.vcd"}});
        QVERIFY(segVcd["result"]["isError"].get<bool>());

        const auto badSelf = client.tool("lt_self_test", {{"a", 0}});
        QVERIFY(badSelf["result"]["isError"].get<bool>());
        QVERIFY(badSelf["result"]["content"][0]["text"].get<std::string>().find("channel number") != std::string::npos);

        const auto bad = client.tool("lt_configure_and_capture", {{"channels", "1"}, {"samples", "lots"}});
        QVERIFY(bad["result"]["isError"].get<bool>());
        QVERIFY(bad["result"]["content"][0]["text"].get<std::string>().find("invalid samples") != std::string::npos);

        // No GUI is running, so there is nobody to annotate for: say so plainly.
        const auto note = client.tool("lt_add_annotation", {{"time_us", 10}, {"text", "x"}});
        QVERIFY(note["result"]["isError"].get<bool>());
        QVERIFY(note["result"]["content"][0]["text"].get<std::string>().find("logictracer-gui") != std::string::npos);
    }

    void agentAndEngineerShareOneWindow()
    {
        fake::Device dev(emulatedDevice());
        LT_REQUIRE_PTY(dev);
        QTemporaryDir dir;
        const QString socket = dir.filePath("mcp.sock");

        lt::gui::MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        QVERIFY(win.startSession(socket));
        win.setPortText(QString::fromStdString(dev.port()));
        for (auto* b : win.findChildren<QPushButton*>())
            if (b->text() == "Connect")
                b->click();
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        McpClient client({"--socket", socket});
        client.request("initialize", {{"protocolVersion", "2025-06-18"}});

        // The agent captures through the GUI: the device stays with the window, the engineer sees the result.
        const auto cap = client.tool("lt_configure_and_capture", {{"channels", "1:STIM,2:REACT"}, {"samples", 400}});
        QVERIFY2(!cap["result"]["isError"].get<bool>(), cap.dump().c_str());
        QCOMPARE(cap["result"]["structuredContent"]["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(win.waveform()->capture().size(), std::size_t{400});

        const auto note = client.tool("lt_add_annotation", {{"time_us", 120.5}, {"text", "Jetson pulse start"}, {"color", "red"}});
        QVERIFY2(!note["result"]["isError"].get<bool>(), note.dump().c_str());
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{1});
        QCOMPARE(win.waveform()->annotations().front().author, std::string("ai"));

        // ... and reads back what the engineer sees, annotation included.
        win.waveform()->setCursorA(10);
        win.waveform()->setCursorB(60);
        const auto view = client.tool("lt_get_view", {});
        QVERIFY2(!view["result"]["isError"].get<bool>(), view.dump().c_str());
        const auto report = view["result"]["structuredContent"]["report"].get<std::string>();
        QVERIFY(report.find("Jetson pulse start") != std::string::npos);
        QVERIFY(report.find("Δt = 50 us") != std::string::npos);
        const auto state = client.tool("lt_get_view", {{"report", false}});
        QCOMPARE(state["result"]["structuredContent"]["annotation_count"].get<int>(), 1);

        const auto id = state["result"]["structuredContent"]["capture_id"].get<std::uint64_t>();
        QVERIFY(id > 0);
        const auto stale = client.tool("lt_add_annotation", {{"time_us", 5}, {"text", "x"}, {"capture_id", id + 1}});
        QVERIFY(stale["result"]["isError"].get<bool>());
        QVERIFY(stale.dump().find("StaleCapture") != std::string::npos);
        const auto steered = client.tool("lt_set_view", {{"capture_id", id}, {"center_us", 300}, {"span_us", 200}, {"cursor_a_us", 250}});
        QVERIFY2(!steered["result"]["isError"].get<bool>(), steered.dump().c_str());
        QCOMPARE(steered["result"]["structuredContent"]["capture_id"].get<std::uint64_t>(), id);
        QVERIFY(win.waveform()->visibleEnd() - win.waveform()->visibleFirst() <= 201);
        QVERIFY(win.waveform()->cursorA().has_value());
        const auto staleSteer = client.tool("lt_set_view", {{"capture_id", id + 1}, {"fit", true}});
        QVERIFY(staleSteer["result"]["isError"].get<bool>());
        QVERIFY(staleSteer.dump().find("StaleCapture") != std::string::npos);
        const auto anomalies = client.tool("lt_get_anomalies", {{"capture_id", id}, {"max_count", 5}});
        QVERIFY2(!anomalies["result"]["isError"].get<bool>(), anomalies.dump().c_str());
        QCOMPARE(anomalies["result"]["structuredContent"]["capture_id"].get<std::uint64_t>(), id);
        QVERIFY(anomalies["result"]["structuredContent"]["incidents"].is_array());
        QVERIFY(client.tool("lt_get_anomalies", {{"capture_id", id + 1}}).dump().find("StaleCapture") != std::string::npos);
        const auto fresh = client.tool("lt_add_annotation", {{"time_us", 5}, {"text", "y"}, {"capture_id", id}});
        QVERIFY2(!fresh["result"]["isError"].get<bool>(), fresh.dump().c_str());
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{2});
        QVERIFY(client.tool("lt_clear_annotations", {{"capture_id", id + 1}}).dump().find("StaleCapture") != std::string::npos);
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{2});  // a stale clear wipes nothing
        QVERIFY(client.tool("lt_get_view", {{"capture_id", id + 1}}).dump().find("StaleCapture") != std::string::npos);
        QVERIFY(client.tool("lt_detect_devices", {{"timeout_ms", 1e100}})["result"]["isError"].get<bool>());
        QVERIFY(client.tool("lt_detect_devices", {{"timeout_ms", -5}})["result"]["isError"].get<bool>());
        QVERIFY(!client.tool("lt_clear_annotations", {})["result"]["isError"].get<bool>());
        const auto cleared = client.tool("lt_clear_annotations", {});
        QVERIFY(!cleared["result"]["isError"].get<bool>());
        QVERIFY(win.waveform()->annotations().empty());

        const auto badNote = client.tool("lt_add_annotation", {{"time_us", 5}, {"text", "x"}, {"color", "nonsense"}});
        QVERIFY(badNote["result"]["isError"].get<bool>());

        // Security check (A-04): path traversal, hidden files and wrong extensions are rejected.
        const auto traversal = client.tool("lt_configure_and_capture",
                                           {{"channels", "1"}, {"samples", 100}, {"out_vcd", "../escape.vcd"}});
        QVERIFY(traversal["result"]["isError"].get<bool>());
        const auto badExt = client.tool("lt_configure_and_capture",
                                        {{"channels", "1"}, {"samples", 100}, {"out_json", "/tmp/evil.sh"}});
        QVERIFY(badExt["result"]["isError"].get<bool>());
    }

    void offlineAnalyzeAndDiffTools()
    {
        // Build two saved capture files in the current working directory (within the MCP sandbox).
        const QString fileA = "mcp_test_a.json";
        const QString fileB = "mcp_test_b.json";
        const QString outVcd = "mcp_test_out.vcd";
        std::vector<uint32_t> sA(200, 1u);
        // Put a valid UART byte 0x55 @ 100 kbaud (10 samples/bit) on CH1 and a delayed edge on CH2.
        for (int k = 0; k < 10; ++k)
            sA[20 + k] = 0u;  // start bit
        for (int b = 0; b < 8; ++b)
            for (int k = 0; k < 10; ++k)
                sA[30 + b * 10 + k] = ((0x55 >> b) & 1) ? 1u : 0u;
        for (int k = 100; k < 150; ++k)
            sA[k] |= 1u;
        for (int k = 102; k < 152; ++k)
            sA[k] |= 2u;  // CH2 rises 2 samples after CH1

        std::vector<uint32_t> sB = sA;
        for (int k = 102; k < 110; ++k)
            sB[k] &= ~2u;  // shift CH2 rising edge to sample 110 (8 us latency instead of 2 us)

        const lt::CaptureBuffer bufA({{0, "TX"}, {1, "REACT"}}, 1'000'000, 0, sA);
        const lt::CaptureBuffer bufB({{0, "TX"}, {1, "REACT"}}, 1'000'000, 0, sB);
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        QVERIFY(lt::writeJsonFile(fileA.toStdString(), lt::captureToJson(bufA, lt::SignalAnalyzer(opts).analyze(bufA), true)));
        QVERIFY(lt::writeJsonFile(fileB.toStdString(), lt::captureToJson(bufB, lt::SignalAnalyzer(opts).analyze(bufB), true)));

        lt::mcp::McpServer server;
        const auto analyzed = server.callTool(
            "lt_analyze_file",
            {{"file", fileA.toStdString()},
             {"decode", "uart:tx=1,baud=100k"},
             {"expect", "uart:frames>=1,uart:errors==0"},
             {"out_vcd", outVcd.toStdString()},
             {"ai", true}});
        QVERIFY2(!analyzed["isError"].get<bool>(), analyzed.dump().c_str());
        const auto& docA = analyzed["structuredContent"];
        QCOMPARE(docA["command"].get<std::string>(), std::string("analyze"));
        QVERIFY(docA["assertions"]["passed"].get<bool>());
        QVERIFY(docA["decoded"][0]["frames"].get<int>() >= 1);
        QVERIFY(docA["report"].get<std::string>().find("Decoded bus protocols") != std::string::npos);
        QVERIFY(QFile::exists(outVcd));

        // Failing --expect sets isError=true while returning the structured analyze document.
        const auto failedExpect = server.callTool(
            "lt_analyze_file", {{"file", fileA.toStdString()}, {"decode", "uart:tx=1,baud=100k"}, {"expect", "uart:frames==99"}});
        QVERIFY(failedExpect["isError"].get<bool>());
        QCOMPARE(failedExpect["structuredContent"]["command"].get<std::string>(), std::string("analyze"));
        QVERIFY(!failedExpect["structuredContent"]["assertions"]["passed"].get<bool>());

        // lt_diff_captures compares the two files; strict=true sets isError=true on regression.
        const auto diffOk = server.callTool(
            "lt_diff_captures", {{"a", fileA.toStdString()}, {"b", fileB.toStdString()}, {"latency", "1..2"}});
        QVERIFY2(!diffOk["isError"].get<bool>(), diffOk.dump().c_str());
        QCOMPARE(diffOk["structuredContent"]["verdict"].get<std::string>(), std::string("regression"));

        const auto diffStrict = server.callTool(
            "lt_diff_captures",
            {{"a", fileA.toStdString()}, {"b", fileB.toStdString()}, {"latency", "1..2"}, {"strict", true}});
        QVERIFY(diffStrict["isError"].get<bool>());
        QCOMPARE(diffStrict["structuredContent"]["verdict"].get<std::string>(), std::string("regression"));

        // Missing latency channel in lt_diff_captures is rejected as a tool error.
        QVERIFY(server.callTool("lt_diff_captures",
                                {{"a", fileA.toStdString()}, {"b", fileB.toStdString()}, {"latency", "1..9"}})["isError"]
                    .get<bool>());

        // Output path traversal is rejected by the MCP sandbox.
        QVERIFY(server.callTool("lt_analyze_file",
                                {{"file", fileA.toStdString()}, {"out_vcd", "../outside.vcd"}})["isError"]
                    .get<bool>());

        QFile::remove(fileA);
        QFile::remove(fileB);
        QFile::remove(outVcd);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    McpTests t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_mcp.moc"

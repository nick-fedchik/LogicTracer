// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "FakeDevice.hpp"
#include "gui/MainWindow.hpp"
#include "gui/ViewTransform.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/Exporters.hpp"

#include <chrono>

using namespace std::chrono_literals;
using lt::gui::MainWindow;
using lt::gui::ViewTransform;
using lt::gui::WaveformWidget;

namespace {

constexpr int kGutter = WaveformWidget::kGutterWidth;

QPoint plotPoint(const WaveformWidget& w, double sample, int y = 60)
{
    return {static_cast<int>(kGutter + w.view().xAt(sample + 0.5)), y};
}

QPushButton* button(MainWindow& w, const QString& text)
{
    for (auto* b : w.findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

QLineEdit* field(MainWindow& w, const char* name)  // channels, rate, samples, trigger
{
    return w.findChild<QLineEdit*>(name);
}

}  // namespace

class GuiTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void viewFitsAndNeverScrollsPastTheCapture()
    {
        ViewTransform v;
        v.setTotalSamples(10'000);
        v.setWidth(1000);
        v.fitAll();
        QCOMPARE(v.samplesPerPixel(), 10.0);
        QCOMPARE(v.firstVisible(), std::size_t{0});
        QCOMPARE(v.endVisible(), std::size_t{10'000});
        QVERIFY(v.isFit());

        v.zoomAt(1.0 / 4, 500);  // zooming out of a fitted view is a no-op
        QCOMPARE(v.samplesPerPixel(), 10.0);
        v.panByPixels(-300);
        QCOMPARE(v.start(), 0.0);
    }

    void zoomKeepsTheSampleUnderThePointer()
    {
        ViewTransform v;
        v.setTotalSamples(10'000);
        v.setWidth(1000);
        v.fitAll();
        const double before = v.sampleAt(250);
        v.zoomAt(4.0, 250);
        QCOMPARE(v.samplesPerPixel(), 2.5);
        QVERIFY(std::abs(v.sampleAt(250) - before) < 1e-9);
        QVERIFY(!v.isFit());

        v.panByPixels(100);  // dragging right reveals earlier samples, clamped at 0
        QVERIFY(v.start() >= 0.0);
        v.panByPixels(-1e9);
        QCOMPARE(v.endVisible(), std::size_t{10'000});  // clamped at the end
        v.zoomAt(1e9, 500);
        QCOMPARE(v.samplesPerPixel(), 1.0 / ViewTransform::kMaxPixelsPerSample);
    }

    void viewHandlesEmptyAndTinyCaptures()
    {
        ViewTransform v;
        v.setWidth(800);
        v.fitAll();
        QCOMPARE(v.endVisible(), std::size_t{0});
        v.setTotalSamples(3);
        v.fitAll();
        QCOMPARE(v.endVisible(), std::size_t{3});
        QCOMPARE(v.firstVisible(), std::size_t{0});
    }

    void clicksPlaceCursorsAndWheelZooms()
    {
        MainWindow win;
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        QSignalSpy cursors(w, &WaveformWidget::cursorsChanged);
        QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, plotPoint(*w, 400));
        QVERIFY(w->cursorA());
        QVERIFY(std::abs(static_cast<long>(*w->cursorA()) - 400) <= 4);
        QTest::mouseClick(w, Qt::LeftButton, Qt::ShiftModifier, plotPoint(*w, 1200));
        QVERIFY(w->cursorB());
        QVERIFY(std::abs(static_cast<long>(*w->cursorB()) - 1200) <= 4);
        QCOMPARE(cursors.count(), 2);
        w->clearCursors();
        QTest::mouseClick(w, Qt::RightButton, Qt::NoModifier, plotPoint(*w, 800));
        QVERIFY(!w->cursorA() && w->cursorB());

        const auto fullSpan = w->visibleEnd() - w->visibleFirst();
        QPointF anchor = plotPoint(*w, 2000);
        QWheelEvent zoom(anchor, w->mapToGlobal(anchor), {}, {0, 120}, Qt::NoButton, Qt::ControlModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &zoom);
        QVERIFY(w->visibleEnd() - w->visibleFirst() < fullSpan);
        QVERIFY(w->visibleFirst() <= 2000 && w->visibleEnd() >= 2000);  // still around the anchor

        QTest::mouseDClick(w, Qt::LeftButton, Qt::NoModifier, plotPoint(*w, 100));
        QCOMPARE(w->visibleEnd() - w->visibleFirst(), fullSpan);
    }

    void draggingPansTheView()
    {
        MainWindow win;
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        const QPointF anchor = plotPoint(*w, 2000);
        QWheelEvent zoom(anchor, w->mapToGlobal(anchor), {}, {0, 480}, Qt::NoButton, Qt::ControlModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &zoom);
        const auto firstBefore = w->visibleFirst();

        QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, {kGutter + 400, 60});
        QTest::mouseMove(w, {kGutter + 300, 60});
        QTest::mouseMove(w, {kGutter + 200, 60});
        QTest::mouseRelease(w, Qt::LeftButton, Qt::NoModifier, {kGutter + 200, 60});
        QVERIFY2(w->visibleFirst() > firstBefore, "dragging left must reveal later samples");
        QVERIFY2(!w->cursorA(), "a drag is not a click");
    }

    void copyForAiDescribesTheCurrentView()
    {
        MainWindow win;
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        w->setCursorA(205);
        w->setCursorB(405);
        const auto text = win.aiReport();
        QVERIFY(text.contains("CH1 rising, t = 0 at sample 200"));
        QVERIFY(text.contains("Δt = 200 us (200 samples)"));
        QVERIFY(text.contains("| 1 | CLK |"));
        QVERIFY(text.contains("ASCII timeline"));
        QVERIFY(text.contains("\"edge_times_us\""));
        QVERIFY(text.contains("IDLE_HI: no activity, constant HIGH"));

        QAction* copy = nullptr;
        for (auto* a : win.findChildren<QAction*>())
            if (a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C))
                copy = a;
        QVERIFY2(copy, "Ctrl+Shift+C must be bound");
        copy->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), win.aiReport());

        // Zooming narrows the range the report (and the clipboard) covers.
        const auto before = w->visibleEnd() - w->visibleFirst();
        w->zoomIn();
        w->zoomIn();
        QVERIFY(win.aiReport().contains(QString("Visible range: samples [%1, %2)").arg(w->visibleFirst()).arg(w->visibleEnd())));
        QVERIFY(w->visibleEnd() - w->visibleFirst() < before);
    }

    void rendersTheDemoCapture()
    {
        MainWindow win;
        win.resize(1180, 760);
        win.show();
        win.loadDemo();
        win.waveform()->setCursorA(205);
        win.waveform()->setCursorB(405);
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTest::qWait(250);  // let the debounced report refresh

        const QImage img = win.grab().toImage();
        QVERIFY(!img.isNull());
        const auto out = qEnvironmentVariable("LT_GUI_SCREENSHOT");
        if (!out.isEmpty())
            QVERIFY(img.save(out));

        // The plot must contain drawn waveform pixels in the channel colour (not just background).
        const QImage plot = win.waveform()->grab().toImage();
        int coloured = 0;
        for (int y = 0; y < plot.height(); ++y)
            for (int x = kGutter; x < plot.width(); ++x)
                coloured += plot.pixelColor(x, y).green() > 150 && plot.pixelColor(x, y).red() < 120;
        QVERIFY(coloured > 500);
    }

    void capturesThroughTheUiAgainstAnEmulatedDevice()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("V6_5", true));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.recordRequest(r);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setPortText(QString::fromStdString(dev.port()));
        auto* connect = button(win, "Connect");
        auto* capture = button(win, "Capture");
        QVERIFY(connect && capture);
        QVERIFY(!capture->isEnabled());

        QTest::mouseClick(connect, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        QTRY_VERIFY(capture->isEnabled());
        QCOMPARE(connect->text(), QString("Disconnect"));

        field(win, "channels")->setText("1,2");
        field(win, "samples")->setText("200");
        QTest::mouseClick(capture, Qt::LeftButton);
        QVERIFY(!capture->isEnabled());  // busy while the capture runs
        QTRY_VERIFY_WITH_TIMEOUT(win.waveform()->hasCapture(), 5000);
        QCOMPARE(win.waveform()->capture().size(), std::size_t{200});
        QCOMPARE(win.waveform()->capture().channels().size(), std::size_t{2});
        QTRY_VERIFY(capture->isEnabled());

        // An invalid request is reported, not sent to the device.
        field(win, "rate")->setText("fast");
        QTest::mouseClick(capture, Qt::LeftButton);
        QVERIFY(!win.device().isBusy());
        QCOMPARE(dev.requests().size(), std::size_t{1});

        QTest::mouseClick(connect, Qt::LeftButton);  // disconnect
        QTRY_VERIFY(!win.device().isConnected());
    }

    void preflightShowsWindowAndValidatesFieldsInline()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send(fake::idReply("V6_5", true));
        });
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setPortText(QString::fromStdString(dev.port()));
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        auto* preflight = win.findChild<QLabel*>("preflight");
        QVERIFY(preflight);
        field(win, "rate")->setText("1M");
        field(win, "samples")->setText("1000");
        field(win, "trigger")->setText("1:rising");
        QVERIFY(preflight->text().contains("Window 1 ms"));
        QVERIFY(preflight->text().contains("Step 1 us"));
        QVERIFY(preflight->text().contains("Pre 100"));
        QVERIFY(field(win, "rate")->styleSheet().isEmpty());

        field(win, "rate")->setText("fast");
        QVERIFY(!field(win, "rate")->styleSheet().isEmpty());
        QVERIFY(preflight->text().contains("fix"));
        field(win, "rate")->setText("1M");
        field(win, "channels")->setText("99");
        QVERIFY(!field(win, "channels")->styleSheet().isEmpty());
        field(win, "channels")->setText("1,2");

        // A latency pair naming a channel that is not on screen is flagged.
        win.loadDemo();
        field(win, "latencySpec")->setText("1..9");
        QVERIFY(!field(win, "latencySpec")->styleSheet().isEmpty());
        field(win, "latencySpec")->setText("1..2");
        QVERIFY(field(win, "latencySpec")->styleSheet().isEmpty());
    }

    void failedCaptureKeepsOldTriggerAndProbeIgnoresSamples()
    {
        // Regression for A-06 and A-09.
        bool rejectNext = false;
        fake::Device dev([&](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("V6_5", true));
            } else if (payload[0] == 1) {
                if (rejectNext) {
                    d.send("CAPTURE_ERROR\n");
                } else {
                    const auto r = fake::parseRequest(payload);
                    d.send("CAPTURE_STARTED\n");
                    d.send(fake::captureData(r, 0x01));
                }
            }
        });
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.loadDemo();  // sets trigger description to "CH1 rising"
        QVERIFY(win.aiReport().contains("CH1 rising"));

        win.setPortText(QString::fromStdString(dev.port()));
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        // Start a capture with a different trigger that fails -> old waveform must keep "CH1 rising".
        rejectNext = true;
        field(win, "channels")->setText("1,2");
        field(win, "trigger")->setText("2:falling");
        QTest::mouseClick(button(win, "Capture"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!win.device().isBusy(), 5000);
        QVERIFY(win.aiReport().contains("CH1 rising"));
        QVERIFY(!win.aiReport().contains("CH2 falling"));

        // Probe must succeed even when Samples holds garbage (A-09).
        rejectNext = false;
        field(win, "samples")->setText("not_a_number");
        QTest::mouseClick(button(win, "Probe"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!win.device().isBusy(), 5000);
        QVERIFY(win.aiReport().contains("none (probe)"));
    }

    void openCaptureFileLoadsJsonAndVcd()
    {
        QTemporaryDir dir;
        const QString jsonFile = dir.filePath("saved.json");
        const QString vcdFile = dir.filePath("saved.vcd");

        MainWindow writer;
        writer.loadDemo();
        lt::Annotation note;
        note.timeSeconds = 50e-6;
        note.label = "Test marker";
        writer.waveform()->addAnnotation(note);

        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        const auto& buf = writer.waveform()->capture();
        auto doc = lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true, "PICO");
        doc["trigger_description"] = "CH1 rising";
        doc["annotations"] = nlohmann::json::array({{{"id", 1}, {"time_us", 50.0}, {"label", "Test marker"}, {"color", "red"}, {"author", "ai"}}});
        QVERIFY(lt::writeJsonFile(jsonFile.toStdString(), doc));
        QVERIFY(lt::writeVcdFile(vcdFile.toStdString(), buf));

        MainWindow reader;
        QString err;
        QVERIFY2(reader.openCaptureFile(jsonFile, &err), qPrintable(err));
        QCOMPARE(reader.waveform()->capture().samples(), buf.samples());
        QCOMPARE(reader.waveform()->annotations().size(), std::size_t{1});
        QVERIFY(reader.aiReport().contains("CH1 rising"));

        QVERIFY2(reader.openCaptureFile(vcdFile, &err), qPrintable(err));
        QCOMPARE(reader.waveform()->capture().samples(), buf.samples());
    }

    void snapToEdgeAndAiCopyModesWork()
    {
        MainWindow win;
        win.resize(1180, 760);
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        // Zoom in strongly around sample 400 so 1 sample spans several pixels, then click 4 px off the edge.
        for (int i = 0; i < 6; ++i) {
            const QPointF anchor = plotPoint(*w, 400);
            QWheelEvent zoom(anchor, w->mapToGlobal(anchor), {}, {0, 120}, Qt::NoButton, Qt::ControlModifier,
                             Qt::NoScrollPhase, false);
            QApplication::sendEvent(w, &zoom);
        }
        const QPoint edgePt = plotPoint(*w, 400);
        const QPoint nearEdge(edgePt.x() + 4, WaveformWidget::kRulerHeight + 10);  // row 0 = CLK (toggles at sample 400)
        QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, nearEdge);
        QVERIFY(w->cursorA().has_value());
        QCOMPARE(*w->cursorA(), std::size_t{400});  // snapped onto the exact transition

        // Holding Alt bypasses edge snapping when an exact sample between transitions is desired.
        const QPoint midPulse = plotPoint(*w, 405);
        QTest::mouseClick(w, Qt::LeftButton, Qt::AltModifier, QPoint(midPulse.x(), WaveformWidget::kRulerHeight + 10));
        QVERIFY(*w->cursorA() >= 404 && *w->cursorA() <= 406);  // mid-pulse, not snapped to 400 or 410

        // AI grounding bar: note + paired latency + Brief / Cursors A..B modes.
        w->fitAll();
        w->setCursorA(200);
        w->setCursorB(230);
        win.setAiNote("Check CLK -> ECHO propagation delay");
        win.setLatencySpec("1..2");
        win.setCopyMode(lt::CopyMode::Brief);

        const QString brief = win.aiReport();
        QVERIFY(brief.contains("Context / Question:** Check CLK -> ECHO propagation delay"));
        QVERIFY(brief.contains("Propagation latency: CH1 (CLK rising) -> CH2 (ECHO rising)"));
        QVERIFY(brief.contains("Chronological transitions"));
        QVERIFY(!brief.contains("```json"));

        win.copyBriefForAi();
        QCOMPARE(QGuiApplication::clipboard()->text(), brief);

        win.copyCursorsForAi();
        const QString ab = QGuiApplication::clipboard()->text();
        QVERIFY(ab.contains("Cursor range (A..B): samples [199, 231)"));
        QVERIFY(ab.contains("```json"));
    }
};

QTEST_MAIN(GuiTests)
#include "test_gui.moc"

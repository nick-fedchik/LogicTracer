// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "FakeDevice.hpp"
#include "gui/Builders.hpp"
#include "gui/MainWindow.hpp"
#include "gui/ViewTransform.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QImage>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QWheelEvent>
#include <QToolButton>
#include <QMenu>
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

lt::CaptureBuffer toggling(std::size_t n = 400)
{
    std::vector<uint32_t> samples(n);
    for (std::size_t i = 0; i < n; ++i)
        samples[i] = (i / 10) % 2;
    return lt::CaptureBuffer({{0, "CLK"}}, 1'000'000, 20, samples);
}

lt::CaptureResult okResult(const lt::CaptureBuffer& b)
{
    lt::CaptureResult r;
    r.status = lt::CaptureStatus::Ok;
    r.buffer = b;
    return r;
}

int brightPixels(const QImage& img)  // pixels clearly brighter than the dark background
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            n += qGray(img.pixel(x, y)) > 100;
    return n;
}

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
        QSignalSpy menuRequests(w, &WaveformWidget::contextMenuRequested);
        QTest::mouseClick(w, Qt::RightButton, Qt::NoModifier, plotPoint(*w, 800));
        QVERIFY2(!w->cursorA() && !w->cursorB(), "a plain right click must not place a cursor");
        QCOMPARE(menuRequests.count(), 1);
        const auto menu = win.findChild<QMenu*>("waveformMenu");  // opened without blocking the event loop
        QVERIFY(menu);
        QVERIFY(menu->isVisible());
        const auto setA = [&](const QString& t) {
            for (auto* act : menu->actions())
                if (act->text() == t)
                    return act;
            return static_cast<QAction*>(nullptr);
        };
        QVERIFY(setA("Set Cursor B Here"));
        setA("Set Cursor B Here")->trigger();
        QVERIFY(w->cursorB() && std::abs(static_cast<long>(*w->cursorB()) - 800) <= 4);
        menu->close();

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

    void draggingACursorLineMovesItWithoutPanning()
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
        w->setCursorA(w->visibleFirst() + 20);
        const auto first = w->visibleFirst();
        const auto before = *w->cursorA();

        const int y = WaveformWidget::kPlotTop + 5;
        const QPoint onLine(static_cast<int>(kGutter + w->view().xAt(static_cast<double>(before) + 0.5)) + 3, y);
        QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, onLine);
        QTest::mouseMove(w, onLine + QPoint(30, 0));
        QTest::mouseMove(w, onLine + QPoint(60, 0));
        QTest::mouseRelease(w, Qt::LeftButton, Qt::AltModifier, onLine + QPoint(60, 0));
        QCOMPARE(w->visibleFirst(), first);
        QVERIFY2(*w->cursorA() > before, "the cursor follows the drag");
        QVERIFY(!w->cursorB());
    }

    void hideInactiveChannelsKeepsBufferIndices()
    {
        MainWindow win;
        win.show();
        auto* w = win.waveform();
        std::vector<uint32_t> samples(300);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = ((i / 10) & 1u) | 2u;  // CH1 toggles, CH2 is constant high
        w->setCapture(lt::CaptureBuffer({{0, "A"}, {1, "B"}}, 1'000'000, 0, samples));
        QVERIFY(w->channelActive(0));
        QVERIFY(!w->channelActive(1));
        QCOMPARE(w->visibleChannels().size(), std::size_t{2});

        QAction* hide = nullptr;
        for (auto* act : win.findChildren<QAction*>())
            if (act->text() == "&Hide Inactive Channels")
                hide = act;
        QVERIFY(hide);
        QCOMPARE(hide->shortcut(), QKeySequence(Qt::Key_H));
        hide->trigger();
        QVERIFY(w->hideInactive());
        QCOMPARE(w->visibleChannels(), std::vector<std::size_t>{0});
        QCOMPARE(w->capture().channels().size(), std::size_t{2});  // buffer untouched
        QVERIFY(win.aiReport().contains("| 2 | B |"));             // reports still cover every channel
        hide->trigger();
        QCOMPARE(w->visibleChannels().size(), std::size_t{2});

        // Nothing active: never leave the view empty.
        hide->trigger();
        w->setCapture(lt::CaptureBuffer({{0, "A"}}, 1'000'000, 0, std::vector<uint32_t>(50, 1)));
        QCOMPARE(w->visibleChannels().size(), std::size_t{1});
    }

    void overviewClickJumpsTheView()
    {
        MainWindow win;
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        const QPointF anchor = plotPoint(*w, 100);
        QWheelEvent zoom(anchor, w->mapToGlobal(anchor), {}, {0, 480}, Qt::NoButton, Qt::ControlModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &zoom);
        const auto span = w->visibleEnd() - w->visibleFirst();
        QVERIFY(w->visibleFirst() < w->capture().size() / 4);

        const int x = kGutter + (w->width() - kGutter) * 3 / 4;
        QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, QPoint(x, 5));
        const auto centre = (w->visibleFirst() + w->visibleEnd()) / 2;
        QVERIFY(std::abs(static_cast<long>(centre) - static_cast<long>(w->capture().size() * 3 / 4)) < static_cast<long>(span));
        QVERIFY2(!w->cursorA(), "an overview click navigates, it does not place a cursor");
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

    void timeoutKeepsOldWaveformButReportsLineState()
    {
        MainWindow win;
        win.show();
        win.setCapture(toggling(), "CH1 rising");
        const auto firstId = win.captureId();
        QVERIFY(firstId != 0);

        lt::CaptureResult timeout;
        timeout.status = lt::CaptureStatus::Timeout;
        timeout.error = "Trigger timeout";
        timeout.lineDump = lt::CaptureBuffer({{0, "CLK"}}, 1'000'000, 0, std::vector<uint32_t>(16, 1));
        Q_EMIT win.device().captureFinished(timeout);

        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Stale);
        QCOMPARE(win.waveform()->capture().size(), std::size_t{400});  // old waveform stays visible
        QCOMPARE(win.captureId(), firstId);
        const QString report = win.aiReport();
        QVERIFY(report.contains("TIMEOUT"));
        QVERIFY(report.contains(QString("previous capture #%1").arg(firstId)));
        QVERIFY(!report.contains("Chronological transitions"));  // not a report about the old waveform
        win.copyForAi();
        QCOMPARE(QGuiApplication::clipboard()->text(), report);

        Q_EMIT win.device().captureFinished(okResult(toggling(300)));
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Live);
        QVERIFY(win.captureId() > firstId);
        QVERIFY(!win.aiReport().contains("TIMEOUT"));
    }

    void f3JumpsBetweenAnomaliesAndWraps()
    {
        lt::gui::MainWindow win;
        win.show();
        win.loadDemo();
        win.jumpToAnomaly(+1);
        const QString first = win.findChild<QLabel*>("status")->text();
        QVERIFY2(first.startsWith("Anomaly ") || first.startsWith("No anomalies"), qPrintable(first));
        if (first.startsWith("No anomalies"))
            return;
        win.jumpToAnomaly(-1);  // wraps to the last one
        QVERIFY(win.findChild<QLabel*>("status")->text().startsWith("Anomaly "));
    }

    void armedStateDimsTheOldWaveformAndLabelsTheReport()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send(fake::idReply("V6_5", true));  // never answers the capture: it stays armed
        });
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setCapture(toggling(), "CH1 rising");
        const auto lowerHalf = [&] {  // below the banner, where only the waveform lines can differ
            const QImage img = win.waveform()->grab().toImage();
            return brightPixels(img.copy(0, 70, img.width(), img.height() - 70));
        };
        const int live = lowerHalf();
        win.setPortText(QString::fromStdString(dev.port()));
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);

        field(win, "trigger")->setText("1:rising");
        QTest::mouseClick(button(win, "Capture"), Qt::LeftButton);
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Armed);
        QVERIFY(win.aiReport().contains("ARMED"));
        QVERIFY(win.aiReport().contains("PREVIOUS capture"));
        QVERIFY(live > 100);
        QVERIFY(lowerHalf() < live);

        QTest::mouseClick(button(win, "Cancel"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!win.device().isBusy(), 8000);
        // The silent device never acknowledged the capture, so the attempt ends as a failure, not as ARMED.
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Stale);
        QVERIFY(!win.aiReport().contains("ARMED"));
    }

    void historyRingKeepsRecentCapturesAndAnnotations()
    {
        MainWindow win;
        win.setHistoryLimits(3, std::size_t{1} << 20);
        for (int i = 0; i < 5; ++i) {
            win.setCapture(toggling(100 + static_cast<std::size_t>(i)), "CH1 rising");
            if (i == 3) {
                lt::Annotation a;
                a.label = "marker";
                win.waveform()->addAnnotation(a);
            }
        }
        QCOMPARE(win.historySize(), std::size_t{3});
        QCOMPARE(win.captureId(), uint64_t{5});
        QCOMPARE(win.waveform()->capture().size(), std::size_t{104});

        auto* prev = button(win, "Prev");
        auto* next = button(win, "Next");
        QVERIFY(prev->isEnabled() && !next->isEnabled());
        QTest::mouseClick(prev, Qt::LeftButton);
        QCOMPARE(win.captureId(), uint64_t{4});
        QCOMPARE(win.waveform()->annotations().size(), std::size_t{1});  // kept with its capture
        QVERIFY(win.aiReport().contains("HISTORY: capture #4"));
        QTest::mouseClick(prev, Qt::LeftButton);
        QCOMPARE(win.captureId(), uint64_t{3});
        QVERIFY(!prev->isEnabled());
        QTest::mouseClick(next, Qt::LeftButton);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(win.captureId(), uint64_t{5});
        QVERIFY(!win.aiReport().contains("HISTORY:"));

        win.setHistoryLimits(8, 2 * 100 * sizeof(uint32_t));  // memory cap keeps only what fits (and the newest)
        QCOMPARE(win.historySize(), std::size_t{1});
        QCOMPARE(win.captureId(), uint64_t{5});
    }

    void setupSurvivesDeviceCapturesAndClearsOnDemand()
    {
        MainWindow win;
        win.setCapture(toggling(), "CH1 rising", {{"dut", "board A"}});
        QCOMPARE(win.setup().size(), std::size_t{1});
        Q_EMIT win.device().captureFinished(okResult(toggling(200)));  // a GUI capture carries no new setup
        QCOMPARE(win.setup().size(), std::size_t{1});
        QVERIFY(win.aiReport().contains("dut=board A"));

        for (auto* a : win.findChildren<QAction*>())
            if (a->text().contains("Clear Setup"))
                a->trigger();
        QVERIFY(win.setup().empty());
        QVERIFY(!win.aiReport().contains("dut=board A"));
    }

    void unpinnedNoteIsClearedByTheNextCapture()
    {
        MainWindow win;
        auto* note = field(win, "aiNote");
        auto* pin = win.findChild<QCheckBox*>("notePin");
        QVERIFY(note && pin && !pin->isChecked());

        note->setText("first question");
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        QCOMPARE(note->text(), QString("first question"));  // written for this capture
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        QVERIFY(note->text().isEmpty());  // stale: it was already sent with the previous capture

        note->setText("kept");
        pin->setChecked(true);
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        QCOMPARE(note->text(), QString("kept"));
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
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Stale);
        QVERIFY(win.aiReport().contains("capture failed"));
        QVERIFY(win.aiReport().contains("previous capture #1"));
        QCOMPARE(win.waveform()->capture().size(), std::size_t{4000});  // old waveform kept on screen

        // Probe must succeed even when Samples holds garbage (A-09).
        rejectNext = false;
        field(win, "samples")->setText("not_a_number");
        QTest::mouseClick(button(win, "Probe"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!win.device().isBusy(), 5000);
        QVERIFY(win.aiReport().contains("none (probe)"));
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Live);

        // The probe did not destroy the earlier triggered capture: it is one Prev away, trigger intact.
        QCOMPARE(win.historySize(), std::size_t{2});
        QTest::mouseClick(button(win, "Prev"), Qt::LeftButton);
        QVERIFY(win.aiReport().contains("CH1 rising"));
        QVERIFY(!win.aiReport().contains("CH2 falling"));
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
        const QPoint nearEdge(edgePt.x() + 4, WaveformWidget::kPlotTop + 10);  // row 0 = CLK (toggles at sample 400)
        QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, nearEdge);
        QVERIFY(w->cursorA().has_value());
        QCOMPARE(*w->cursorA(), std::size_t{400});  // snapped onto the exact transition

        // Holding Alt bypasses edge snapping when an exact sample between transitions is desired.
        const QPoint midPulse = plotPoint(*w, 405);
        QTest::mouseClick(w, Qt::LeftButton, Qt::AltModifier, QPoint(midPulse.x(), WaveformWidget::kPlotTop + 10));
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

    void emptyCardLoadsTheDemoAndMenuHasShortcut()
    {
        MainWindow win;
        win.resize(1000, 700);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto* card = win.waveform()->findChild<QWidget*>("emptyCard");
        QVERIFY(card && card->isVisible());
        QVERIFY(card->findChild<QPushButton*>("openButton"));
        bool found = false;
        for (auto* act : win.findChildren<QAction*>())
            if (act->text().contains("Demo Capture")) {
                QCOMPARE(act->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_D));
                found = true;
            }
        QVERIFY(found);
        QTest::mouseClick(card->findChild<QPushButton*>("demoButton"), Qt::LeftButton);
        QVERIFY(win.waveform()->hasCapture());
        QVERIFY(!card->isVisible());
    }

    void buildersComposeFieldsFromPopups()
    {
        QCOMPARE(lt::gui::composeChannelList({{1, "CLK"}, {2, ""}, {3, ""}, {4, ""}, {6, ""}, {7, ""}}),
                 QString("1:CLK,2-4,6,7"));
        QCOMPARE(lt::gui::composeTrigger(0, false), QString("none"));
        QCOMPARE(lt::gui::composeTrigger(3, true), QString("3:falling"));
        QCOMPARE(lt::gui::composeLatency(1, false, 2, false), QString("1..2"));
        QCOMPARE(lt::gui::composeLatency(1, true, 2, false), QString("1:falling..2:rising"));

        MainWindow win;
        win.resize(1200, 700);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto* channels = win.findChild<QLineEdit*>("channels");
        auto* trigger = win.findChild<QLineEdit*>("trigger");
        auto* latency = win.findChild<QLineEdit*>("latencySpec");

        QTest::mouseClick(win.findChild<QToolButton*>("channelsBuilderButton"), Qt::LeftButton);
        auto* cb = win.findChild<QFrame*>("channelsBuilder");
        QVERIFY(cb && cb->isVisible());
        for (auto* box : cb->findChildren<QCheckBox*>())
            box->setChecked(false);
        cb->findChild<QCheckBox*>("ch1")->setChecked(true);
        cb->findChild<QCheckBox*>("ch2")->setChecked(true);
        cb->findChild<QLineEdit*>("name1")->setText("STIM");
        cb->findChild<QPushButton*>("apply")->click();
        QCOMPARE(channels->text(), QString("1:STIM,2"));

        QTest::mouseClick(win.findChild<QToolButton*>("triggerBuilderButton"), Qt::LeftButton);
        auto* tb = win.findChild<QFrame*>("triggerBuilder");
        QVERIFY(tb);
        tb->findChild<QComboBox*>("channel")->setCurrentIndex(tb->findChild<QComboBox*>("channel")->findData(2));
        tb->findChild<QComboBox*>("edge")->setCurrentIndex(1);
        tb->findChild<QPushButton*>("apply")->click();
        QCOMPARE(trigger->text(), QString("2:falling"));

        QTest::mouseClick(win.findChild<QToolButton*>("latencyBuilderButton"), Qt::LeftButton);
        auto* lb = win.findChild<QFrame*>("latencyBuilder");
        QVERIFY(lb);
        lb->findChild<QComboBox*>("reactChannel")->setCurrentIndex(1);
        lb->findChild<QPushButton*>("apply")->click();
        QCOMPARE(latency->text(), QString("1..2"));
        QTest::qWait(50);
        QVERIFY(latency->styleSheet().isEmpty());
    }

    void anomalyMarkersAreSeparateFromAnnotationsAndTogglable()
    {
        MainWindow win;
        win.resize(1200, 600);
        win.show();
        std::vector<uint32_t> s(1000 * 500 + 100, 0u);
        for (std::size_t k = 0; k < 1000; ++k) {
            const std::size_t t0 = 50 + k * 500;
            for (std::size_t i = 0; i < 200; ++i)
                s[t0 + i] |= 1u;
            if (k == 910)
                continue;  // stimulus without a reaction
            for (std::size_t i = 0; i < 200; ++i)
                s[t0 + 2 + i] |= 2u;
        }
        auto* w = win.waveform();
        w->setCapture(lt::CaptureBuffer({{0, "STIM"}, {1, "REACT"}}, 100'000'000, 50, s));
        field(win, "latencySpec")->setText("1..2");
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTRY_VERIFY(!w->anomalies().empty());  // debounced report refresh computes the incidents
        QVERIFY(w->showAnomalyMarkers());
        QVERIFY(w->annotations().empty());

        const auto markerPixels = [&] {
            const QImage img = win.grab().toImage();
            const QPoint origin = w->mapTo(&win, QPoint(0, 0));
            int count = 0;
            for (int y = origin.y() + WaveformWidget::kPlotTop - 8; y < origin.y() + WaveformWidget::kPlotTop; ++y)
                for (int x = origin.x() + kGutter; x < origin.x() + w->width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    count += (c.red() > 200 && c.green() < 100 && c.blue() < 100) ||
                             (c.red() > 220 && c.green() > 150 && c.green() < 190 && c.blue() < 70);
                }
            return count;
        };
        QTest::qWait(100);
        QVERIFY(markerPixels() > 0);
        QAction* toggle = nullptr;
        for (auto* act : win.findChildren<QAction*>())
            if (act->text().contains("Anomaly"))
                toggle = act;
        QVERIFY(toggle && toggle->isChecked());
        toggle->setChecked(false);
        QVERIFY(!w->showAnomalyMarkers());
        QTest::qWait(100);
        QCOMPARE(markerPixels(), 0);
        QVERIFY(w->annotations().empty());
    }

    void setupEditorSyncsWithSetupAndReport()
    {
        MainWindow win;
        win.resize(1200, 700);
        win.show();
        win.loadDemo();
        auto* btn = win.findChild<QPushButton*>("setupButton");
        QVERIFY(btn);
        QCOMPARE(btn->text(), QString("Setup (0)"));
        QTest::mouseClick(btn, Qt::LeftButton);
        auto* ed = win.findChild<QFrame*>("setupEditor");
        QVERIFY(ed && ed->isVisible());
        ed->findChild<QPushButton*>("template_dut.board")->click();
        auto* table = ed->findChild<QTableWidget*>("setupTable");
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("dut.board"));
        table->item(0, 1)->setText("ESP32");
        ed->findChild<QPushButton*>("addRow")->click();
        table->item(1, 0)->setText("bad key!");
        table->item(1, 1)->setText("x");
        ed->findChild<QPushButton*>("apply")->click();
        QVERIFY(ed->isVisible());  // invalid row blocks Apply
        QVERIFY(!ed->findChild<QLabel*>("setupStatus")->text().isEmpty());
        table->item(1, 0)->setText("wiring");
        table->item(1, 1)->setText("GP0-GP3");
        ed->findChild<QPushButton*>("apply")->click();

        QCOMPARE(win.setup().size(), std::size_t{2});
        QCOMPARE(btn->text(), QString("Setup (2)"));
        QTest::qWait(250);
        QVERIFY(win.findChild<QPlainTextEdit*>() ? win.findChild<QPlainTextEdit*>()->toPlainText().contains("ESP32") : false);
        win.clearSetupMetadata();
        QCOMPARE(btn->text(), QString("Setup (0)"));
    }

    void shiftWheelScrollsRowsWhileHeaderStaysPinned()
    {
        MainWindow win;
        win.resize(1200, 500);
        win.show();
        win.loadDemo();
        auto* w = win.waveform();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QCOMPARE(w->scrollY(), 0.0);
        w->setRowHeight(w->height());  // far taller than the viewport allows for 4 rows
        w->setRowHeight(60);
        w->setRowHeight(w->height() / 2.0);
        QVERIFY(w->maxScrollY() > 0);
        QVERIFY(w->maxScrollY() > 0);
        QVERIFY(w->rowAt(60) && *w->rowAt(60) == 0);

        QWheelEvent down(QPointF(400, 100), QPointF(), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &down);
        QVERIFY(w->scrollY() > 0);
        QVERIFY(!w->rowAt(10));  // overview / ruler are not rows
        QVERIFY(!w->rowAt(WaveformWidget::kPlotTop - 1));
        const auto row = w->rowAt(WaveformWidget::kPlotTop + 2);
        QVERIFY(row && *row >= 1);  // scrolled past the first row

        const double h = w->effectiveRowHeight();
        QWheelEvent zoom(QPointF(400, 100), QPointF(), QPoint(), QPoint(0, 120), Qt::NoButton,
                         Qt::ShiftModifier | Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &zoom);
        QVERIFY(w->effectiveRowHeight() > h);
        w->setRowHeight(0);
        QCOMPARE(w->scrollY(), 0.0);  // auto-fit never needs scrolling for 4 rows
    }

    void repeatRearmsUntilCancelOrDisconnect()
    {
        int served = 0;
        fake::Device dev([&served](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("V6_5", true));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.recordRequest(r);
                d.send("CAPTURE_STARTED\n");
                if (++served <= 3)  // then stay armed so Cancel can be pressed while busy
                    d.send(fake::captureData(r, 0x01));
            }
        });
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setPortText(QString::fromStdString(dev.port()));
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        field(win, "channels")->setText("1,2");
        field(win, "samples")->setText("100");
        auto* repeat = win.findChild<QCheckBox*>("repeat");
        QVERIFY(repeat && !repeat->isChecked());
        repeat->setChecked(true);
        QTest::mouseClick(button(win, "Capture"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dev.requests().size() >= 4, 8000);
        QVERIFY(repeat->isChecked());
        QTRY_VERIFY(win.device().isBusy());

        QTest::mouseClick(button(win, "Cancel"), Qt::LeftButton);
        QVERIFY(!repeat->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(!win.device().isBusy(), 5000);
        const auto count = dev.requests().size();
        QTest::qWait(300);
        QVERIFY(dev.requests().size() <= count + 1);

        repeat->setChecked(true);
        QTest::mouseClick(button(win, "Disconnect"), Qt::LeftButton);
        QTRY_VERIFY(!win.device().isConnected());
        QVERIFY(!repeat->isChecked());
    }
};

QTEST_MAIN(GuiTests)
#include "test_gui.moc"

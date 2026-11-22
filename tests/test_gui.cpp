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
#include <QTabBar>
#include <QTableWidget>
#include <QWheelEvent>
#include <QToolButton>
#include <QMenu>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSerialPort>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QMessageBox>
#include <QTimer>
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

#ifdef Q_OS_LINUX
bool holdPort(QProcess& holder, const QString& port)
{
    holder.setChildProcessModifier([path = QFile::encodeName(port)] {
        const int fd = ::open(path.constData(), O_RDONLY | O_NOCTTY);
        if (fd < 0 || ::dup2(fd, STDIN_FILENO) < 0)
            ::_exit(1);
        if (fd != STDIN_FILENO)
            ::close(fd);
    });
    holder.start("sleep", {"60"});
    return holder.waitForStarted();
}
#endif

}  // namespace

class GuiTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void nativeRecordingOpensWithoutCreatingAWaveform()
    {
        MainWindow win;
        QTemporaryDir tmp;
        QFile file(tmp.filePath("edges.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"format":"logictracer-events-v1","schema_version":1,"source":{"kind":"gpio","chip":"/dev/gpiochip1","line":7,"clock":"hte","initial_level":null},"events":[{"timestamp_ns":9007199254740993,"sequence":3,"lost_before":2,"edge":"falling"}]})");
        file.close();
        QString err;
        QVERIFY2(win.openCaptureFile(file.fileName(), &err), qPrintable(err));
        auto* rows = win.findChild<QPlainTextEdit*>("nativeEventsText");
        QVERIFY(rows);
        QVERIFY(rows->toPlainText().contains("9007199254740993 ns  falling"));
        QVERIFY(rows->toPlainText().contains("gap: 2") && rows->toPlainText().contains("initial level: unknown"));
        QVERIFY(!win.waveform()->hasCapture());
    }

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
        const QPoint onLine(static_cast<int>(kGutter + w->view().xAt(static_cast<double>(before))) + 3, y);
        QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, onLine);
        QTest::mouseMove(w, onLine + QPoint(30, 0));
        QTest::mouseMove(w, onLine + QPoint(60, 0));
        QTest::mouseRelease(w, Qt::LeftButton, Qt::AltModifier, onLine + QPoint(60, 0));
        QCOMPARE(w->visibleFirst(), first);
        QVERIFY2(*w->cursorA() > before, "the cursor follows the drag");
        QVERIFY(!w->cursorB());
    }

    void aboutNamesTheAuthorAndRepository()
    {
        MainWindow win;
        QAction* about = nullptr;
        for (auto* act : win.findChildren<QAction*>())
            if (act->text() == "&About LogicTracer")
                about = act;
        QVERIFY(about);
        QCOMPARE(about->menuRole(), QAction::AboutRole);
        const QString text = MainWindow::aboutText();
        QVERIFY(text.contains("Mykola Fedchyk"));
        QVERIFY(text.contains("https://github.com/nick-fedchik/LogicTracer"));
        QVERIFY(text.contains(LT_VERSION));
    }

    void selfTestMenuNeedsADevice()
    {
        MainWindow win;
        QAction* self = nullptr;
        for (auto* act : win.findChildren<QAction*>())
            if (act->text() == "&Self-test")
                self = act;
        QVERIFY(self);
        self->trigger();
        bool told = false;
        for (auto* label : win.findChildren<QLabel*>())
            told = told || label->text().contains("connect a device");
        QVERIFY(told);
        QVERIFY(!win.device().isBusy());
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

    void askAiRestoresTheDefaultSplitWithoutLosingTheNote()
    {
        MainWindow win;
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto* split = win.findChild<QSplitter*>();
        auto* pane = split->widget(1);
        auto* hide = pane->findChild<QToolButton*>("hideAskAi");
        QVERIFY(hide && hide->text() == "_");
        auto* restore = button(win, "Show Ask AI");
        QVERIFY(restore && win.statusBar()->isAncestorOf(restore));
        auto* note = field(win, "aiNote");
        note->setText("keep this question");
        const auto original = split->sizes();
        split->setSizes({300, 600});
        QVERIFY(split->sizes() != original);
        QTest::mouseClick(hide, Qt::LeftButton);
        QVERIFY(!pane->isVisible() && win.waveform()->isVisible() && restore->isVisible());
        QTest::mouseClick(restore, Qt::LeftButton);
        QVERIFY(pane->isVisible() && !restore->isVisible());
        QTRY_COMPARE(split->sizes(), original);
        QCOMPARE(note->text(), QString("keep this question"));
    }

    void demoSetupConfiguresTheNextCaptureNotTheRecording()
    {
        MainWindow win;
        win.show();
        auto* setup = button(win, "Setup");
        QVERIFY(setup && !setup->isEnabled());
        win.setCapture(toggling(), {}, {}, {}, {}, "PICO");
        QVERIFY(!setup->isEnabled());  // an imported Pico recording is not a connected device
        win.loadDemo();
        const auto preview = win.captureId();
        QVERIFY(setup->isEnabled() && !win.device().isConnected());
        win.findChild<QAction*>("showPinout")->trigger();
        auto* dialog = win.findChild<QDialog*>("picoPinout");
        auto* source = dialog->findChild<QTabBar*>("pinoutSource");
        QVERIFY(source);
        QCOMPARE(source->currentIndex(), 1);
        setup->click();
        QCOMPARE(source->currentIndex(), 0);
        for (const auto* name : {"channels", "rate", "samples", "windows", "triggerSummary", "timeout"}) {
            auto* editor = dialog->findChild<QWidget*>(name);
            QVERIFY2(editor && editor->isVisible() && editor->isEnabled(), name);
            QVERIFY(!win.centralWidget()->findChild<QWidget*>(name));
        }
        field(win, "channels")->setText("2:ECHO_RX,1:CLK");
        field(win, "rate")->setText("500k");
        field(win, "samples")->setText("50");
        field(win, "trigger")->setText("1:rising");
        dialog->findChild<QSpinBox*>("windows")->setValue(2);
        dialog->findChild<QSpinBox*>("timeout")->setValue(1234);
        source->setCurrentIndex(1);
        QVERIFY(!field(win, "rate")->isVisible());
        QCOMPARE(win.captureId(), preview);
        QCOMPARE(win.waveform()->capture().frequency(), 1'000'000u);
        QCOMPARE(win.waveform()->capture().channels()[0].name, std::string("CLK"));
        dialog->close();

        button(win, "Connect")->click();  // explicitly connect to the selected simulator
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        QVERIFY(win.device().info().version.family == lt::FirmwareFamily::Replay);
        setup->click();
        QCOMPARE(source->currentIndex(), 0);
        QCOMPARE(dialog->findChild<QSpinBox*>("timeout")->value(), 1234);
        dialog->close();
        button(win, "Capture")->click();
        QTRY_VERIFY_WITH_TIMEOUT(win.captureId() != preview, 5000);
        const auto& buf = win.waveform()->capture();
        QCOMPARE(buf.frequency(), 500'000u);
        QCOMPARE(buf.channels().size(), std::size_t{2});
        QCOMPARE(buf.channels()[0].name, std::string("ECHO_RX"));
        QCOMPARE(buf.channels()[0].number, 1);
        QCOMPARE(buf.size(), std::size_t{95});  // 5 pre + 2 windows of 45 post
        QCOMPARE(win.waveform()->segmentStarts().size(), std::size_t{2});
        button(win, "Disconnect")->click();
        QTRY_VERIFY(!setup->isEnabled());
        QCOMPARE(field(win, "channels")->text(), QString("1-8"));  // not the simulator's plan
    }

    void triggerPinsChooseWhatFiresNotWhatIsRecorded()
    {
        MainWindow win;
        win.show();
        win.loadDemo();
        button(win, "Connect")->click();
        QTRY_VERIFY(win.device().isConnected());
        button(win, "Setup")->click();
        field(win, "channels")->setText("1:CLK,2:ECHO");
        field(win, "samples")->setText("50");
        auto* capture = button(win, "Capture");
        auto* notice = win.findChild<QLabel*>("preflight");
        auto* ch3Rise = win.findChild<QToolButton*>("pinTrigger3_0");
        auto* ch3Low = win.findChild<QToolButton*>("pinTrigger3_2");
        auto* ch4High = win.findChild<QToolButton*>("pinTrigger4_3");
        auto* ext = win.findChild<QToolButton*>("pinTrigger25_0");
        QVERIFY(ch3Rise && ch3Low && ch4High && ext && !ext->isVisible());
        auto* outsideDemo = win.findChild<QPushButton*>("pinPad32");
        QVERIFY(outsideDemo && !outsideDemo->isEnabled());
        QSignalSpy completed(&win.device(), &lt::AsyncDevice::captureFinished);
        auto run = [&](const QString& trigger) {
            QVERIFY(capture->isEnabled());
            capture->click();
            QTRY_COMPARE(completed.count(), 1);
            const auto r = completed.takeFirst().at(0).value<lt::CaptureResult>();
            QVERIFY2(r.buffer.has_value(), r.error.c_str());
            QCOMPARE(r.triggerUsed ? QString::fromStdString(r.triggerUsed->describe()) : QString{}, trigger);
            QCOMPARE(r.buffer->channels().size(), std::size_t{2});
            QCOMPARE(r.buffer->channels()[0].number, 0);
            QCOMPARE(r.buffer->channels()[1].number, 1);
        };
        ch3Rise->click();
        run("CH3 rising");
        ch3Low->click();
        ch4High->click();
        QVERIFY(!ch3Rise->isChecked());
        auto* gap = win.findChild<QToolButton*>("pinTrigger1_2");
        gap->click();
        QVERIFY(!capture->isEnabled() && notice->text().contains("gap"));
        gap->click();
        run("CH3-CH4==01");
        win.findChild<QPushButton*>("clearTrigger")->click();
        run({});
    }

    void setupGuidesOperatorAndResetsAnInvalidDemoPlan()
    {
        MainWindow win;
        win.show();
        field(win, "channels")->setText("5:OTHER_PICO");
        win.loadDemo();
        const auto preview = win.captureId();
        button(win, "Connect")->click();
        QTRY_VERIFY(win.device().isConnected());
        button(win, "Setup")->click();
        auto* dialog = win.findChild<QDialog*>("picoPinout");
        auto* windows = win.findChild<QSpinBox*>("windows");
        auto* timeout = win.findChild<QSpinBox*>("timeout");
        auto* capture = button(win, "Capture");
        auto* error = win.findChild<QLabel*>("preflight");
        auto* fix = win.findChild<QPushButton*>("fixCaptureSetup");
        field(win, "rate")->setText("500k");
        field(win, "samples")->setText("3000");
        QVERIFY(!capture->isEnabled() && button(win, "Probe")->isEnabled());
        QVERIFY(error->text().contains("3000") && error->text().contains("maximum 2000") && error->text().contains("4 ms"));
        QVERIFY(dialog->findChild<QLabel*>("samplesError")->isVisible());
        field(win, "samples")->setText("50k");
        QVERIFY(error->text().contains("maximum 2000"));  // not the simulated device's larger byte buffer
        field(win, "samples")->setText("2000");
        QVERIFY(capture->isEnabled());
        field(win, "samples")->setText("50");
        windows->setValue(3);
        dialog->close();
        QVERIFY(fix->isVisible() && !capture->isEnabled());
        QVERIFY(error->text().contains("Windows to 1") && error->text().contains("rising or falling"));
        error->setSelection(0, static_cast<int>(error->text().size()));
        QTest::keySequence(error, QKeySequence::Copy);
        QCOMPARE(QApplication::clipboard()->text(), error->text());
        fix->click();
        auto* edge = win.findChild<QToolButton*>("pinTrigger1_0");
        QTRY_VERIFY(edge->hasFocus());
        auto* setupError = dialog->findChild<QLabel*>("setupPreflight");
        QVERIFY(setupError && setupError->isVisible());
        QCOMPARE(setupError->text(), error->text());
        QApplication::clipboard()->clear();
        setupError->setSelection(0, static_cast<int>(setupError->text().size()));
        QTest::keySequence(setupError, QKeySequence::Copy);
        QCOMPARE(QApplication::clipboard()->text(), setupError->text());
        edge->click();
        QVERIFY(capture->isEnabled() && !fix->isVisible());
        QVERIFY(!error->text().contains("Open Setup") && !setupError->text().contains("Open Setup"));
        win.findChild<QPushButton*>("clearTrigger")->click();
        windows->setValue(1);
        QVERIFY(capture->isEnabled());
        QCOMPARE(setupError->text(), error->text());
        QVERIFY(!setupError->text().contains("Open Setup"));

        field(win, "channels")->setText("2:OTHER");
        win.findChild<QToolButton*>("pinTrigger2_1")->click();
        windows->setValue(2);
        timeout->setValue(7);
        field(win, "rate")->setText("invalid");
        win.findChild<QPushButton*>("resetCaptureSetup")->click();
        QCOMPARE(field(win, "channels")->text(), QString("1:CLK,2:ECHO,3:BURST,4:IDLE_HI"));
        QCOMPARE(field(win, "rate")->text(), QString("1M"));
        QCOMPARE(field(win, "samples")->text(), QString("1k"));
        QCOMPARE(field(win, "trigger")->text(), QString("none"));
        QCOMPARE(windows->value(), 1);
        QCOMPARE(timeout->value(), 5000);
        QCOMPARE(field(win, "pinName5")->text(), QString("ECHO"));
        QVERIFY(capture->isEnabled() && !fix->isVisible());
        QCOMPARE(win.captureId(), preview);
        win.connectOrDisconnect();
        QTRY_COMPARE(field(win, "channels")->text(), QString("5:OTHER_PICO"));
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
        LT_REQUIRE_PTY(dev);
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setPortText(QString::fromStdString(dev.port()));
        auto* connect = button(win, "Connect");
        auto* capture = button(win, "Capture");
        auto* setup = button(win, "Setup");
        QVERIFY(connect && capture);
        QVERIFY(setup && !setup->isEnabled() && !capture->isEnabled());

        QTest::mouseClick(connect, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        QTRY_VERIFY(capture->isEnabled());
        QCOMPARE(connect->text(), QString("Disconnect"));
        QVERIFY(setup->isEnabled());
        setup->click();
        QVERIFY(field(win, "channels")->isVisible());
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
        QVERIFY(!setup->isEnabled() && !field(win, "channels")->isVisible());
    }

#ifdef Q_OS_LINUX
    void occupiedPortIsIdentifiedAndRefreshAllowsReconnection()
    {
        std::atomic<int> handshakes{0};
        fake::Device dev([&](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload.front() == 0) {
                ++handshakes;
                d.send(fake::idReply("V6_5", true));
            }
        });
        const auto port = QString::fromStdString(dev.port());
        QProcess holder;
        QVERIFY(holdPort(holder, port));
        const auto cleanup = qScopeGuard([&] {
            if (holder.state() != QProcess::NotRunning) {
                holder.terminate();
                holder.waitForFinished();
            }
        });
        const auto pid = QString::number(holder.processId());
        MainWindow win;
        win.show();
        auto* ports = win.findChild<QComboBox*>("port");
        auto* connect = button(win, "Connect");
        ports->addItem("Emulated Pico", port);
        ports->setCurrentIndex(ports->count() - 1);
        QTRY_VERIFY_WITH_TIMEOUT(ports->currentText().contains("in use", Qt::CaseInsensitive), 2000);
        auto* info = win.findChild<QLabel*>("deviceInfo");
        QVERIFY(info && info->text().contains(port) && info->text().contains(pid));
        QVERIFY(info->text().contains("sleep") && info->text().contains("Refresh"));
        QVERIFY(!connect->isEnabled());
        QCOMPARE(handshakes.load(), 0);

        QTemporaryDir aliases;
        const auto alias = aliases.filePath("pico");
        QVERIFY(QFile::link(port, alias));
        win.setPortText(alias);
        QTRY_VERIFY(info->text().contains(alias) && info->text().contains(pid));
        ports->setCurrentIndex(ports->findData("demo:pico"));
        QVERIFY(connect->isEnabled() && !info->text().contains(pid));
        win.setPortText(port);
        holder.terminate();
        QVERIFY(holder.waitForFinished());
        button(win, "Refresh")->click();
        QCOMPARE(ports->currentText(), port);  // Refresh must preserve a manually entered path.
        QTRY_VERIFY(connect->isEnabled() && !info->text().contains(pid));
        QCOMPARE(handshakes.load(), 0);
        connect->click();
        QTRY_VERIFY(win.device().isConnected());
        QCOMPARE(handshakes.load(), 1);
    }

    void connectRechecksHolderBeforeSendingTheHandshake()
    {
        std::atomic<int> handshakes{0};
        fake::Device dev([&](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload.front() == 0) {
                ++handshakes;
                d.send(fake::idReply("V6_5", true));
            }
        });
        const auto port = QString::fromStdString(dev.port());
        MainWindow win;
        win.setPortText(port);
        QProcess holder;
        QVERIFY(holdPort(holder, port));
        const QString pid = QString::number(holder.processId());
        QSignalSpy opened(&win.device(), &lt::AsyncDevice::opened);
        win.connectOrDisconnect();  // Same path as --port, without waiting for a picker scan.
        QTRY_COMPARE(opened.count(), 1);
        holder.terminate();
        QVERIFY(holder.waitForFinished());
        QVERIFY(!win.device().isConnected());
        QVERIFY(opened.takeFirst().at(1).toString().contains(pid));
        QCOMPARE(handshakes.load(), 0);
        QCOMPARE(dev.cancelCount(), 0);

        win.setPortText("/dev/pts/nonexistent-logictracer");
        win.connectOrDisconnect();
        QTRY_COMPARE(opened.count(), 1);
        const QString error = opened.takeFirst().at(1).toString();
        QVERIFY(error.contains("cannot open") && !error.contains("in use", Qt::CaseInsensitive));
    }

    void portPermissionErrorIsNotAnOccupancyReport()
    {
        if (::geteuid() == 0)
            QSKIP("root can open a PTY even with all permission bits cleared");
        fake::Device dev([](fake::Device&, const auto&) {});
        const auto port = QString::fromStdString(dev.port());
        const auto permissions = QFile::permissions(port);
        QVERIFY(QFile::setPermissions(port, {}));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(port, permissions); });
        QSerialPort native(port);
        QVERIFY(!native.open(QIODevice::ReadWrite));
        QCOMPARE(native.error(), QSerialPort::PermissionError);
        MainWindow win;
        win.setPortText(port);
        QSignalSpy opened(&win.device(), &lt::AsyncDevice::opened);
        win.connectOrDisconnect();
        QTRY_COMPARE(opened.count(), 1);
        QCOMPARE(opened.takeFirst().at(1).toString(), QString("cannot open %1: %2").arg(port, native.errorString()));
        QVERIFY(button(win, "Connect")->isEnabled());
    }
#endif

    void unsupportedDeviceNeverOffersPicoWiring()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send("LOGIC_ANALYZER_ZERO_V6_5\nFREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nCHANNELS:24\n");
        });
        LT_REQUIRE_PTY(dev);
        MainWindow win;
        win.show();
        win.setPortText(QString::fromStdString(dev.port()));
        win.connectOrDisconnect();
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        QVERIFY(!button(win, "Setup")->isEnabled());
        win.findChild<QAction*>("showPinout")->trigger();
        QVERIFY(!win.findChild<QWidget*>("picoBoard")->isVisible());
        QVERIFY(field(win, "rate")->isVisible());  // generic capture settings remain accessible
    }

    void preflightShowsWindowAndValidatesFieldsInline()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send(fake::idReply("V6_5", true));
            else if (payload[0] == 1)
                d.recordRequest(fake::parseRequest(payload));
        });
        LT_REQUIRE_PTY(dev);
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
        button(win, "Setup")->click();
        field(win, "rate")->setText("101M");
        QVERIFY(!button(win, "Capture")->isEnabled());
        QVERIFY(preflight->text().contains("100000000"));
        field(win, "rate")->setText("1M");
        field(win, "channels")->setText("24");
        field(win, "samples")->setText("40k");
        QVERIFY(!button(win, "Capture")->isEnabled());
        QVERIFY(preflight->text().contains("32768"));
        QVERIFY(button(win, "Probe")->isEnabled());
        button(win, "Capture")->click();
        QVERIFY(!win.device().isBusy());
        QCOMPARE(dev.requests().size(), std::size_t{0});
        auto* ext = win.findChild<QToolButton*>("pinTrigger25_1");
        QVERIFY(ext->isVisible());
        ext->click();
        QCOMPARE(field(win, "trigger")->text(), QString("25:falling"));
        win.findChild<QPushButton*>("resetCaptureSetup")->click();
        QCOMPARE(field(win, "channels")->text(), QString("1-8"));
        QCOMPARE(field(win, "samples")->text(), QString("10k"));
        QVERIFY(button(win, "Capture")->isEnabled());
        win.findChild<QToolButton*>("pinTrigger2_2")->click();
        QVERIFY(preflight->text().contains("GP0") && preflight->text().contains("GP1"));

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
        LT_REQUIRE_PTY(dev);
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
        LT_REQUIRE_PTY(dev);
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

    // The Windows control records a segmented capture (here from a replayed recording, so it runs everywhere).
    void windowsControlRecordsSegments()
    {
        QTemporaryDir dir;
        std::vector<uint32_t> s(700, 0u);
        for (std::size_t t0 : {100u, 300u, 500u})
            for (std::size_t w = 0; w < 20; ++w) {
                s[t0 + w] |= 1u;
                s[t0 + 2 + w] |= 2u;
            }
        const lt::CaptureBuffer rec({{0, "STIM"}, {1, "REACT"}}, 1'000'000, 0, s);
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        const QString file = dir.filePath("rec.json");
        QVERIFY(lt::writeJsonFile(file.toStdString(), lt::captureToJson(rec, lt::SignalAnalyzer(opts).analyze(rec), true)));

        MainWindow win;
        win.show();
        win.setPortText("replay:" + file);
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        button(win, "Setup")->click();
        QVERIFY(field(win, "channels")->isVisible());
        QVERIFY(!field(win, "pinName4")->isVisible());  // no board provenance in this recording
        field(win, "channels")->setText("1,2");
        field(win, "samples")->setText("50");
        field(win, "trigger")->setText("1:rising");
        auto* windows = win.findChild<QSpinBox*>("windows");
        QVERIFY(windows);
        windows->setValue(3);
        auto* preflight = win.findChild<QLabel*>("preflight");
        QVERIFY(preflight->text().contains("3 windows"));
        QVERIFY(preflight->text().contains("samples stored"));
        QTest::mouseClick(button(win, "Capture"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.waveform()->hasCapture(), 5000);
        QCOMPARE(win.waveform()->segmentStarts().size(), std::size_t{3});
        QVERIFY(win.aiReport().contains("**3 trigger windows**"));
        QVERIFY(win.aiReport().contains("Trigger-to-trigger spacing"));  // replay knows the exact times

        field(win, "trigger")->setText("none");  // windows need a trigger: refused before reaching the device
        QVERIFY(preflight->text().contains("needs a trigger"));
    }

    // A saved segmented capture opens with window seams and a window-by-window report and anomaly list.
    void segmentedCaptureFileShowsWindows()
    {
        QTemporaryDir dir;
        const QString jsonFile = dir.filePath("seg.json");
        std::vector<uint32_t> s = {0, 0, 1, 1, 3, 3, 3, 3};
        for (int k = 0; k < 2; ++k)
            s.insert(s.end(), {1, 1, 3, 3, 3, 3});
        const lt::CaptureBuffer buf({{0, "A"}, {1, "B"}}, 1'000'000, 2, s);
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        auto doc = lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true);
        doc["segments"] = {{"layout", lt::toJson(lt::SegmentLayout{3, 2, 6, uint8_t{0}, false, {}, 0.0})}};
        QVERIFY(lt::writeJsonFile(jsonFile.toStdString(), doc));

        MainWindow win;
        QString err;
        QVERIFY2(win.openCaptureFile(jsonFile, &err), qPrintable(err));
        QCOMPARE(win.waveform()->segmentStarts(), (std::vector<std::size_t>{0, 8, 14}));
        QVERIFY(win.aiReport().contains("**3 trigger windows**"));
        for (const auto& a : win.waveform()->anomalies())
            QVERIFY(a.detail.rfind("window ", 0) == 0);

        win.loadDemo();  // a continuous capture clears the seams
        QVERIFY(win.waveform()->segmentStarts().empty());
    }

    // Cursors in different windows: the stored-sample distance is not time, and a cursor slice of window 2
    // describes window 2 (not an empty slice of window 1).
    void segmentedCursorsAreNotStitchedTime()
    {
        QTemporaryDir dir;
        const QString jsonFile = dir.filePath("seg.json");
        std::vector<uint32_t> s = {0, 0, 1, 1, 3, 3, 3, 3};
        for (int k = 0; k < 2; ++k)
            s.insert(s.end(), {1, 1, 3, 3, 3, 3});
        const lt::CaptureBuffer buf({{0, "A"}, {1, "B"}}, 1'000'000, 2, s);
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        auto doc = lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true);
        doc["segments"] = {{"layout", lt::toJson(lt::SegmentLayout{3, 2, 6, uint8_t{0}, false, {0.0, 1e-3, 2e-3}, 0.0})}};
        QVERIFY(lt::writeJsonFile(jsonFile.toStdString(), doc));

        MainWindow win;
        QString err;
        QVERIFY2(win.openCaptureFile(jsonFile, &err), qPrintable(err));
        auto* w = win.waveform();
        win.setCopyMode(lt::CopyMode::CursorsOnly);

        w->setCursorA(2);  // trigger of window 1
        w->setCursorB(8);  // trigger of window 2, 1 ms later
        auto options = w->reportOptions();
        QVERIFY(options.crossWindow);
        QCOMPARE(options.crossWindow->windowA, 1u);
        QCOMPARE(options.crossWindow->windowB, 2u);
        QVERIFY(options.crossWindow->elapsedSeconds);
        QVERIFY(std::abs(*options.crossWindow->elapsedSeconds - 1e-3) < 1e-9);
        const auto meta = lt::jsonMetadata(w->capture(), options);
        QVERIFY(std::abs(meta["cursors"]["delta_us"].get<double>() - 1000.0) < 1e-6);
        QVERIFY(meta["cursors"].contains("cross_window"));
        QVERIFY(!meta["cursors"].contains("between"));  // no edges counted across the seam
        const QString crossReport = win.aiReport();
        QVERIFY(crossReport.contains("different windows"));
        QVERIFY(!crossReport.contains("in detail"));

        w->setCursorA(9);  // both in window 2 (stored samples 8..13)
        w->setCursorB(11);
        QVERIFY(!w->reportOptions().crossWindow);
        const QString sliceReport = win.aiReport();
        QVERIFY2(sliceReport.contains("Window 2 of 3 in detail"), qPrintable(sliceReport));
        QVERIFY(sliceReport.contains("A = sample 1 "));  // offset in the recorded window
        QVERIFY(!sliceReport.contains("(0 samples"));

        w->setCursorA(8);
        w->setCursorB(9);
        const QString boundary = win.aiReport();
        QVERIFY2(boundary.contains("Window 2 of 3 in detail") && boundary.contains("Cursor range (A..B): samples [0, 2)"),
                 qPrintable(boundary));

        // Without recorded start times the elapsed time is unknown, never the stitched distance.
        doc["segments"] = {{"layout", lt::toJson(lt::SegmentLayout{3, 2, 6, uint8_t{0}, false, {}, 0.0})}};
        QVERIFY(lt::writeJsonFile(jsonFile.toStdString(), doc));
        QVERIFY2(win.openCaptureFile(jsonFile, &err), qPrintable(err));
        w->setCursorA(2);
        w->setCursorB(8);
        QVERIFY(!w->reportOptions().crossWindow->elapsedSeconds);
        QVERIFY(lt::jsonMetadata(w->capture(), w->reportOptions())["cursors"]["delta_us"].is_null());
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
        win.loadDemo();
        button(win, "Setup")->click();
        auto* channels = win.findChild<QLineEdit*>("channels");
        auto* trigger = win.findChild<QLineEdit*>("trigger");
        auto* latency = win.findChild<QLineEdit*>("latencySpec");
        channels->setText("1-4");

        QTest::mouseClick(win.findChild<QToolButton*>("channelsBuilderButton"), Qt::LeftButton);
        auto* cb = win.findChild<QFrame*>("channelsBuilder");
        QVERIFY(cb && cb->isVisible());
        for (auto* box : cb->findChildren<QCheckBox*>())
            box->setChecked(false);
        cb->findChild<QCheckBox*>("ch1")->setChecked(true);
        cb->findChild<QCheckBox*>("ch2")->setChecked(true);
        cb->findChild<QLineEdit*>("name1")->setText("STIM,2:BAD");
        QVERIFY(!cb->findChild<QPushButton*>("apply")->isEnabled());
        cb->findChild<QLineEdit*>("name1")->setText("STIM");
        cb->findChild<QPushButton*>("apply")->click();
        QCOMPARE(channels->text(), QString("1:STIM,2"));

        win.findChild<QToolButton*>("pinTrigger2_1")->click();
        QCOMPARE(trigger->text(), QString("2:falling"));

        win.findChild<QDialog*>("picoPinout")->close();
        QTest::mouseClick(win.findChild<QToolButton*>("latencyBuilderButton"), Qt::LeftButton);
        auto* lb = win.findChild<QFrame*>("latencyBuilder");
        QVERIFY(lb);
        lb->findChild<QComboBox*>("reactChannel")->setCurrentIndex(1);
        lb->findChild<QPushButton*>("apply")->click();
        QCOMPARE(latency->text(), QString("1..2"));
        QTest::qWait(50);
        QVERIFY(latency->styleSheet().isEmpty());
    }

    void pinoutEditsPlansAndCapturesIndependently()
    {
        MainWindow win;
        win.show();
        auto* channels = field(win, "channels");
        channels->setText("24:OLD,1");
        win.findChild<QAction*>("showPinout")->trigger();
        auto* pinout = win.findChild<QDialog*>("picoPinout");
        QVERIFY(pinout && pinout->isVisible() && !pinout->isModal());
        QVERIFY(QTest::qWaitForWindowExposed(pinout));
        auto* source = pinout->findChild<QTabBar*>("pinoutSource");
        QVERIFY(source && source->isVisible());
        QVERIFY(source->rect().contains(source->tabRect(0)));
        QVERIFY(source->rect().contains(source->tabRect(1)));
        auto* name = field(win, "pinName34");  // pin 34, GP28, CH24
        auto* apply = pinout->findChild<QPushButton*>("applyWiring");
        auto* copy = pinout->findChild<QPushButton*>("copyWiring");
        QCOMPARE(name->text(), QString("OLD"));
        QVERIFY(!field(win, "pinName40"));  // VBUS isn't a capture input
        QVERIFY(!field(win, "pinName2"));   // neither is EXT TRIG
        name->setText("BAD\nLABEL");
        QVERIFY(!apply->isEnabled() && !copy->isEnabled());
        QCOMPARE(channels->text(), QString("24:OLD,1"));
        name->setText("CAN_RX");
        auto* left = pinout->findChild<QPushButton*>("pinPad9");
        auto* right = pinout->findChild<QPushButton*>("pinPad32");
        QVERIFY(left && right);
        QCOMPARE(left->text(), QString("9 | GP6"));
        QCOMPARE(right->text(), QString("GP27 | 32"));
        QVERIFY(!pinout->findChild<QAbstractButton*>("pinPad40"));  // VBUS is never a capture input
        QVERIFY(!pinout->findChild<QAbstractButton*>("pinPad2"));   // EXT TRIG isn't one either
        const auto hitPin = [&](QPushButton* pin) {
            const auto point = pin->mapTo(pinout, pin->rect().center());
            QCOMPARE(pinout->childAt(point), static_cast<QWidget*>(pin));
            QTest::mouseClick(pin, Qt::LeftButton);
        };
        const auto padFill = [](QPushButton* pin) {
            const QImage image = pin->grab().toImage();
            return image.pixelColor(qRound(5 * image.devicePixelRatio()), image.height() / 2);
        };
        const auto unselected = padFill(right);
        hitPin(left);
        hitPin(right);
        QVERIFY(left->isChecked() && right->isChecked());
        QVERIFY(padFill(right) != unselected);
        QVERIFY(field(win, "pinName32")->isEnabled());
        hitPin(right);
        QVERIFY(!right->isChecked() && !field(win, "pinName32")->isEnabled());
        QCOMPARE(padFill(right), unselected);
        right->setFocus();
        QTest::keyClick(right, Qt::Key_Space);
        apply->click();
        QCOMPARE(channels->text(), QString("24:CAN_RX,1,5,23"));  // retain selection order
        copy->click();
        const QString note = QApplication::clipboard()->text();
        QVERIFY(note.contains("Offline Pico reference") && note.contains("Pin 34 | GP28 | CH24 | CAN_RX"));
        QVERIFY(note.contains("GND") && note.contains("3.3 V"));

        const lt::CaptureBuffer sparse({{23, "MISO"}, {0, "CLK"}}, 1'000'000, 1, {0, 1, 3, 2, 0});
        win.setCapture(sparse, {}, {}, {}, {}, "PICO");
        win.waveform()->setCursorA(1);
        QTest::mouseClick(source, Qt::LeftButton, {}, source->tabRect(1).center());
        QCOMPARE(name->text(), QString("MISO"));
        auto* recorded = pinout->findChild<QPushButton*>("pinPad34");
        QVERIFY(recorded && recorded->isChecked() && !recorded->isEnabled());  // recorded set is immutable
        QVERIFY(!left->isChecked() && !right->isChecked());
        name->setText("BUS.MISO-1");
        apply->click();
        QCOMPARE(win.waveform()->capture().channels()[0].name, std::string("BUS.MISO-1"));
        QCOMPARE(win.waveform()->capture().samples(), sparse.samples());
        QCOMPARE(win.waveform()->cursorA(), std::optional<std::size_t>(1));
        QVERIFY(win.aiReport().contains("BUS.MISO-1"));
        QCOMPARE(channels->text(), QString("24:CAN_RX,1,5,23"));
        win.setCapture(sparse, {}, {}, {}, {}, "ZERO");
        QVERIFY(!name->isVisible() && !copy->isEnabled());
        win.showPreviousCapture();
        QCOMPARE(name->text(), QString("BUS.MISO-1"));
        QTest::mouseClick(source, Qt::LeftButton, {}, source->tabRect(0).center());
        channels->setText("1:new:bad");
        QVERIFY(!name->isVisible() && !copy->isEnabled());
        channels->setText("24:CIPO");
        QCOMPARE(name->text(), QString("CIPO"));  // live sync with direct Channels edits
    }

    // Cursors, annotations, the trigger and edges share one x for the same sample (no half-sample offset).
    void sampleAnchoredMarksShareOneAxis()
    {
        auto w = std::make_unique<WaveformWidget>();
        w->resize(900, 300);
        std::vector<uint32_t> s(40, 0);
        for (std::size_t i = 20; i < 40; ++i)
            s[i] = 1;
        w->setCapture(lt::CaptureBuffer({{0, "A"}}, 1'000'000, 20, s));
        w->showSampleRange(10, 30);  // 20 samples over the plot: ~40 px per sample
        w->setCursorA(20);
        lt::Annotation a;
        a.timeSeconds = w->capture().timeOfSample(20);
        a.label = "edge";
        w->addAnnotation(a);
        const double cursorX = w->xOfSample(20.0);
        QCOMPARE(w->annotationNear(cursorX), w->annotations().front().id);
        QCOMPARE(w->cursorNear(cursorX), 1);
        QCOMPARE(cursorX, kGutter + w->view().xAt(20.0));  // the drawn edge into sample 20 and the trigger
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
        auto* btn = win.findChild<QPushButton*>("metadataButton");
        QVERIFY(btn);
        QCOMPARE(btn->text(), QString("Metadata (0)"));
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
        QCOMPARE(btn->text(), QString("Metadata (2)"));
        QTest::qWait(250);
        QVERIFY(win.findChild<QPlainTextEdit*>() ? win.findChild<QPlainTextEdit*>()->toPlainText().contains("ESP32") : false);
        win.clearSetupMetadata();
        QCOMPARE(btn->text(), QString("Metadata (0)"));
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

    void noteCopiedAfterACaptureIsNotCarriedToTheNext()
    {
        MainWindow win;
        auto* note = field(win, "aiNote");
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        note->setText("about capture one");
        win.copyForAi();
        Q_EMIT win.device().captureFinished(okResult(toggling()));
        QVERIFY2(note->text().isEmpty(), "a question already sent about #1 must not ride into #2");
    }

    void failureStartedFromHistoryIsStillReported()
    {
        MainWindow win;
        win.show();
        win.setCapture(toggling(), "CH1 rising");
        win.setCapture(toggling(300), "CH1 rising");
        win.showPreviousCapture();
        QCOMPARE(win.historyIndex(), std::size_t{0});

        lt::CaptureResult timeout;
        timeout.status = lt::CaptureStatus::Timeout;
        timeout.error = "Trigger timeout";
        timeout.lineDump = lt::CaptureBuffer({{0, "CLK"}}, 1'000'000, 0, std::vector<uint32_t>(16, 0));
        Q_EMIT win.device().captureFinished(timeout);
        QCOMPARE(win.displayState(), WaveformWidget::DisplayState::Stale);
        QCOMPARE(win.historyIndex(), std::size_t{1});  // back on the newest capture
        QVERIFY(win.aiReport().contains("TIMEOUT"));
    }

    void failedProbeReplacesTheOlderFailure()
    {
        MainWindow win;
        win.show();
        win.setCapture(toggling(), "CH1 rising");
        lt::CaptureResult timeout;
        timeout.status = lt::CaptureStatus::Timeout;
        timeout.error = "Trigger timeout";
        Q_EMIT win.device().captureFinished(timeout);
        QVERIFY(win.aiReport().contains("Trigger timeout"));

        lt::ProbeResult failed;
        failed.status = lt::CaptureStatus::Error;
        failed.error = "usb cable unplugged";
        Q_EMIT win.device().probeFinished(failed);
        const QString report = win.aiReport();
        QVERIFY(report.contains("usb cable unplugged"));
        QVERIFY(!report.contains("Trigger timeout"));  // the older failure is no longer presented as current
    }

    void reportsNameTheCapturesDeviceNotTheConnectedOne()
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
        MainWindow win;
        win.show();
        win.device().setPollLatency(50ms);
        win.setPortText(QString::fromStdString(dev.port()));
        QTest::mouseClick(button(win, "Connect"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.device().isConnected(), 5000);
        field(win, "channels")->setText("1");
        field(win, "samples")->setText("100");
        QTest::mouseClick(button(win, "Capture"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(win.waveform()->hasCapture(), 5000);
        QVERIFY(win.aiReport().contains("GPIO map"));

        win.loadDemo();  // a synthetic capture must not inherit the connected board's pin map
        QVERIFY(!win.aiReport().contains("GPIO map"));
        QVERIFY(!win.aiReport().contains("LOGIC_ANALYZER"));
    }

    // Correcting an old capture's setup must not become the wiring recorded on new captures.
    void editingAnOldEntrysSetupDoesNotChangeTheNextCapture()
    {
        MainWindow win;
        win.setSetupMetadata({{"wiring", "OLD"}});
        Q_EMIT win.device().captureFinished(okResult(toggling()));     // #1 records OLD
        Q_EMIT win.device().captureFinished(okResult(toggling(300)));  // #2 inherits OLD
        win.setSetupMetadata({{"wiring", "NEW"}});                    // newest entry: also the next capture's setup
        win.showPreviousCapture();
        QVERIFY(win.setup().size() == 1 && win.setup().front().second == "OLD");
        win.setSetupMetadata({{"wiring", "OLD-corrected"}});         // history edit only
        win.showNextCapture();
        Q_EMIT win.device().captureFinished(okResult(toggling(350)));  // #3
        QCOMPARE(QString::fromStdString(win.setup().front().second), QString("NEW"));

        // Loading a file without a setup must not erase the pending one either.
        win.loadDemo();
        Q_EMIT win.device().captureFinished(okResult(toggling(360)));
        QCOMPARE(QString::fromStdString(win.setup().front().second), QString("NEW"));
    }

    void loadedFilesKeepTheirDeviceAndImportWarnings()
    {
        QTemporaryDir dir;
        MainWindow writer;
        writer.loadDemo();
        const auto& buf = writer.waveform()->capture();
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        auto doc = lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true, "PICO_2");
        doc["device"] = {{"version", "LOGICTRACER_PICO_2_0.3.0"}, {"board", "PICO_2"}};
        const QString jsonFile = dir.filePath("dev.json");
        QVERIFY(lt::writeJsonFile(jsonFile.toStdString(), doc));

        MainWindow reader;
        QString err;
        QVERIFY2(reader.openCaptureFile(jsonFile, &err), qPrintable(err));
        QVERIFY(reader.aiReport().contains("LOGICTRACER_PICO_2_0.3.0"));
        QVERIFY(reader.aiReport().contains("GPIO map"));

        const QString vcdFile = dir.filePath("xz.vcd");
        QFile f(vcdFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("$timescale 1ns $end\n$var wire 1 ! S $end\n$enddefinitions $end\n#0\n1!\n#1\nz!\n#2\n0!\n");
        f.close();
        QVERIFY2(reader.openCaptureFile(vcdFile, &err), qPrintable(err));
        QVERIFY2(reader.aiReport().contains("x/z value change"), "import caveats must reach the AI report");
    }

    void contextMenuActionsRefuseAReplacedCapture()
    {
        MainWindow win;
        win.show();
        win.setCapture(toggling(), "CH1 rising");
        Q_EMIT win.waveform()->contextMenuRequested(QPoint(10, 10), 12, 0);
        auto* menu = win.findChild<QMenu*>("waveformMenu");
        QVERIFY(menu);
        win.setCapture(toggling(300), "CH1 rising");  // the capture changes while the menu is open
        for (auto* a : menu->actions())
            if (a->text().contains("Set Cursor A"))
                a->trigger();
        QVERIFY(!win.waveform()->cursorA());
        menu->close();
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
        LT_REQUIRE_PTY(dev);
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

    void decodeFieldUpdatesWaveformOverlayAndAnomalies()
    {
        MainWindow win;
        win.resize(1200, 700);
        win.show();
        // Build a 1 MHz capture on CH1 with one valid UART byte (0x55 @ 100 kbaud) and one framing error.
        std::vector<uint32_t> s(300, 1u);
        auto putBit = [&](std::size_t bitIdx, std::size_t startSample, bool high) {
            for (std::size_t k = 0; k < 10; ++k) {
                if (high)
                    s[startSample + bitIdx * 10 + k] |= 1u;
                else
                    s[startSample + bitIdx * 10 + k] &= ~1u;
            }
        };
        putBit(0, 20, false);
        for (int b = 0; b < 8; ++b)
            putBit(static_cast<std::size_t>(b + 1), 20, ((0x55 >> b) & 1) != 0);
        putBit(9, 20, true);
        for (int b = 0; b <= 9; ++b)
            putBit(static_cast<std::size_t>(b), 150, false);

        win.setCapture(lt::CaptureBuffer({{0, "TX"}}, 1'000'000, 0, s), "CH1 falling");
        auto* dec = field(win, "decodeSpec");
        QVERIFY(dec);
        win.setDecodeSpec("uart:tx=1,baud=100k");
        QTRY_VERIFY(!win.waveform()->decodedBuses().empty());
        QCOMPARE(win.waveform()->decodedBuses()[0].frameCount, std::size_t{2});
        QCOMPARE(win.waveform()->decodedBuses()[0].errorCount, std::size_t{1});
        QVERIFY(win.aiReport().contains("Decoded bus protocols"));
        QTRY_VERIFY(!win.waveform()->anomalies().empty());
        QVERIFY(win.waveform()->anomalies().front().kind.find("uart_framing_error") != std::string::npos);

        // Invalid decoder spec marks the field without crashing.
        win.setDecodeSpec("uart:tx=99");
        QTRY_VERIFY(win.waveform()->decodedBuses().empty());
        QVERIFY(!dec->styleSheet().isEmpty());
    }
};

QTEST_MAIN(GuiTests)
#include "test_gui.moc"

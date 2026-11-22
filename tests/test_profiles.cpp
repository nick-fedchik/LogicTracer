// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "FakeDevice.hpp"
#include "gui/CaptureProfiles.hpp"
#include "gui/MainWindow.hpp"
#include "gui/ProfileSelector.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

using namespace lt::gui;

namespace {

const CaptureFields i2c{"4:SCL,3:SDA", "2M", "300", "4:rising", 3, 1200};
const CaptureFields uart{"1:TX,2:RX", "500k", "1k", "1=0,2=1", 1, 7000};

void comparePlan(const CaptureFields& actual, const CaptureFields& expected)
{
    QCOMPARE(actual.channels, expected.channels);
    QCOMPARE(actual.rate, expected.rate);
    QCOMPARE(actual.samples, expected.samples);
    QCOMPARE(actual.trigger, expected.trigger);
    QCOMPARE(actual.windows, expected.windows);
    QCOMPARE(actual.timeout, expected.timeout);
}

CaptureFields plan(MainWindow& w)
{
    const auto text = [&](const char* name) { return w.findChild<QLineEdit*>(name)->text(); };
    return {text("channels"), text("rate"), text("samples"), text("trigger"),
            w.findChild<QSpinBox*>("windows")->value(), w.findChild<QSpinBox*>("timeout")->value()};
}

void savePlan(MainWindow& w, const CaptureFields& f, const QString& name)
{
    w.findChild<QLineEdit*>("channels")->setText(f.channels);
    w.findChild<QLineEdit*>("rate")->setText(f.rate);
    w.findChild<QLineEdit*>("samples")->setText(f.samples);
    w.findChild<QLineEdit*>("trigger")->setText(f.trigger);
    w.findChild<QSpinBox*>("windows")->setValue(f.windows);
    w.findChild<QSpinBox*>("timeout")->setValue(f.timeout);
    w.findChild<QPushButton*>("saveProfile")->click();
    auto* dialog = w.findChild<QInputDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    dialog->setTextValue(name);
    dialog->accept();
    QTRY_VERIFY(QFile::exists(QDir(profiles::directory()).filePath(name + ".ltprofile")));
    QTRY_VERIFY(!w.findChild<QInputDialog*>());
}

void openList(QComboBox* combo)
{
    combo->showPopup();
    QTRY_VERIFY(combo->view()->isVisible());
}

void pick(QComboBox* combo, const QString& filename)
{
    openList(combo);
    const int row = combo->findData(filename);
    QVERIFY2(row >= 0, qPrintable(filename));
    const auto item = combo->model()->index(row, 0);
    combo->view()->scrollTo(item);
    QTest::mouseClick(combo->view()->viewport(), Qt::LeftButton, Qt::NoModifier, combo->view()->visualRect(item).center());
    QTRY_VERIFY(!combo->view()->isVisible());
}

void confirmDelete(QWidget& w, const QString& filename, QMessageBox::StandardButton answer)
{
    auto* button = w.findChild<QPushButton*>("deleteProfile");
    QVERIFY(button->isEnabled());
    button->click();
    auto* dialog = w.findChild<QMessageBox*>();
    QVERIFY(dialog && dialog->isVisible());
    QVERIFY(dialog->text().contains(QDir(profiles::directory()).filePath(filename)));
    dialog->button(answer)->click();
    QTRY_VERIFY(!w.findChild<QMessageBox*>());
}

bool write(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

}  // namespace

class ProfileTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        QDir store(profiles::directory());
        QVERIFY(!store.exists() || store.removeRecursively());
    }

    void cleanup() { QThreadPool::globalInstance()->waitForDone(); }

    void profileSaveRestartSelectOnSamePort()
    {
        fake::Device dev([](fake::Device& d, const std::vector<uint8_t>& payload) {
            if (payload.front() == 0)
                d.send(fake::idReply("0.3.0", true,
                       "FREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nCHANNELS:24\nFEATURES:3\n"));
            else if (payload.front() == 1)
                d.recordRequest(fake::parseRequest(payload));
        });
        LT_REQUIRE_PTY(dev);
        const auto connectPico = [&](MainWindow& w) {
            w.show();
            w.setPortText(QString::fromStdString(dev.port()));
            w.connectOrDisconnect();
            QTRY_VERIFY(w.device().isConnected());
            w.findChild<QPushButton*>("deviceSetup")->click();
        };
        {
            MainWindow w;
            connectPico(w);
            savePlan(w, i2c, "I2C");
            savePlan(w, uart, "UART");
        }
        {
            MainWindow w;
            w.loadDemo();
            const auto recording = w.captureId();
            connectPico(w);
            auto* combo = w.findChild<QComboBox*>("profileSelector");
            pick(combo, "I2C.ltprofile");
            comparePlan(plan(w), i2c);
            QCOMPARE(w.findChild<QLineEdit*>("pinName7")->text(), "SCL");
            QCOMPARE(w.findChild<QLineEdit*>("pinName6")->text(), "SDA");
            pick(combo, "UART.ltprofile");
            comparePlan(plan(w), uart);
            w.findChild<QPushButton*>("resetCaptureSetup")->click();
            // Even choosing the already selected item must reload the saved (not reset) values.
            pick(combo, "UART.ltprofile");
            comparePlan(plan(w), uart);
            confirmDelete(w, "UART.ltprofile", QMessageBox::Yes);
            comparePlan(plan(w), uart);
            QCOMPARE(w.captureId(), recording);
            QCOMPARE(w.historySize(), std::size_t{1});
            QVERIFY(dev.requests().empty());
        }
        {
            MainWindow restarted;
            connectPico(restarted);
            auto* combo = restarted.findChild<QComboBox*>("profileSelector");
            pick(combo, "I2C.ltprofile");
            comparePlan(plan(restarted), i2c);
            QCOMPARE(combo->findData("UART.ltprofile"), -1);
        }
    }

    void profileListLimitsMetadataAndExactDeletionKeys()
    {
        QString error;
        const QString prefix = QStringLiteral("e\u0301").repeated(32);
        const QString a = prefix + "-A.ltprofile", b = prefix + "-B.ltprofile";
        QVERIFY2(profiles::save(a, i2c, error), qPrintable(error));
        QVERIFY(profiles::save(b, uart, error));
        const QDir dir(profiles::directory());
        for (int i = 0; i < 3; ++i)
            QVERIFY(write(dir.filePath(QString::number(i) + ".ltprofile"), "not JSON"));
        QVERIFY(write(dir.filePath("ignore.tmp"), {}));

        CaptureFields current = CaptureFields::defaults(false);
        ProfileSelector selector([&] { return current; }, [&](const CaptureFields& f) { current = f; }, nullptr);
        selector.show();
        auto* combo = selector.findChild<QComboBox*>("profileSelector");
        openList(combo);
        QCOMPARE(combo->count(), 5);
        QVERIFY(!combo->view()->verticalScrollBar()->isVisible());
        QCOMPARE(combo->itemText(combo->findData(a)), prefix.chopped(2) + QChar(0x2026));
        QCOMPARE(combo->itemText(combo->findData(b)), combo->itemText(combo->findData(a)));
        combo->hidePopup();

        QVERIFY(QFile::copy(dir.filePath("0.ltprofile"), dir.filePath("copy.ltprofile")));
        openList(combo);
        QCOMPARE(combo->count(), 6);
        QVERIFY(combo->view()->verticalScrollBar()->isVisible());
        QVERIFY(combo->view()->visualRect(combo->model()->index(5, 0)).top() >= combo->view()->viewport()->height());
        combo->hidePopup();
        QVERIFY(QFile::rename(dir.filePath("copy.ltprofile"), dir.filePath("renamed.ltprofile")));
        openList(combo);
        QCOMPARE(combo->findData("copy.ltprofile"), -1);
        QVERIFY(combo->findData("renamed.ltprofile") >= 0);
        combo->hidePopup();

        pick(combo, a);
        comparePlan(current, i2c);
        pick(combo, b);
        comparePlan(current, uart);
        confirmDelete(selector, b, QMessageBox::Cancel);
        QVERIFY(QFile::exists(dir.filePath(a)) && QFile::exists(dir.filePath(b)));
        confirmDelete(selector, b, QMessageBox::Yes);
        QVERIFY(!QFile::exists(dir.filePath(b)));
        openList(combo);
        QCOMPARE(combo->findData(b), -1);
        QVERIFY(!combo->view()->verticalScrollBar()->isVisible());
        combo->hidePopup();
        const auto unchanged = profiles::load(a, error);
        QVERIFY(unchanged);
        comparePlan(unchanged->fields, i2c);
        const QString external = QDir::current().filePath(a);
        QVERIFY(QFile::copy(dir.filePath(a), external));
        selector.findChild<QPushButton*>("loadProfile")->click();
        auto* dialog = selector.findChild<QInputDialog*>();
        dialog->setTextValue(external);
        dialog->accept();
        auto* remove = selector.findChild<QPushButton*>("deleteProfile");
        QVERIFY(!remove->isEnabled() && remove->toolTip().contains("External"));
        QVERIFY(QFile::exists(external) && QFile::exists(dir.filePath(a)));
    }

    void profileNewestHundredAreNotTheFirstHundred()
    {
        QString error;
        QVERIFY(profiles::save("oldest", uart, error));
        const QDir dir(profiles::directory());
        // Create newest first, oldest last by mtime. Names 098/099 tie at the newest timestamp.
        const auto stamp = [&](const QString& file, int seconds) {
            QFile f(dir.filePath(file));
            return f.open(QIODevice::ReadWrite)
                && f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + seconds), QFileDevice::FileModificationTime);
        };
        for (int n = 99; n >= 0; --n) {
            const QString name = QString("%1.ltprofile").arg(n, 3, 10, QChar('0'));
            QVERIFY(write(dir.filePath(name), {}));
            QVERIFY(stamp(name, std::min(n, 98) + 1));
        }
        QVERIFY(stamp("oldest.ltprofile", 0));
        CaptureFields current = i2c;
        ProfileSelector selector([&] { return current; }, [&](const CaptureFields& f) { current = f; }, nullptr);
        selector.show();
        auto* combo = selector.findChild<QComboBox*>("profileSelector");
        openList(combo);
        QCOMPARE(combo->count(), 100);
        QCOMPARE(combo->itemData(0).toString(), "098.ltprofile");
        QCOMPARE(combo->itemData(1).toString(), "099.ltprofile");
        QCOMPARE(combo->findData("oldest.ltprofile"), -1);
        combo->view()->scrollToBottom();
        const auto last = combo->model()->index(99, 0);
        QCOMPARE(last.data(Qt::UserRole).toString(), "000.ltprofile");
        QVERIFY(combo->view()->viewport()->rect().contains(combo->view()->visualRect(last).center()));
        combo->hidePopup();
        selector.findChild<QPushButton*>("loadProfile")->click();
        auto* dialog = selector.findChild<QInputDialog*>();
        dialog->setTextValue("oldest");
        dialog->accept();
        comparePlan(current, uart);
        confirmDelete(selector, "oldest.ltprofile", QMessageBox::Yes);
        QVERIFY(!QFile::exists(dir.filePath("oldest.ltprofile")));
    }

    void profileRefusesMalformedAndUnsafeStorage()
    {
        QString error;
        QVERIFY(!profiles::save("../escape", i2c, error));
        QVERIFY(!QDir(profiles::directory()).exists());
        QVERIFY(profiles::save("good", i2c, error));
        const QDir dir(profiles::directory());
        const QString malformed = dir.filePath("broken.ltprofile");
        QFile valid(dir.filePath("good.ltprofile"));
        QVERIFY(valid.open(QIODevice::ReadOnly));
        const auto original = nlohmann::json::parse(valid.readAll().toStdString());
        QFile schemaFile(QFINDTESTDATA("../docs/schema/profile.schema.json"));
        QVERIFY(schemaFile.open(QIODevice::ReadOnly));
        const auto schema = nlohmann::json::parse(schemaFile.readAll().toStdString());
        QVERIFY(original == schema.at("examples").at(0));
        for (const auto& [key, value] : std::vector<std::pair<std::string, nlohmann::json>>{
                 {"windows", 0}, {"windows", 65537}, {"windows", 1.5}, {"windows", 4294967297LL},
                 {"timeout_ms", -1}, {"timeout_ms", 600001}, {"channels", nlohmann::json::array()},
                 {"channels", "4:SCL,4:SDA"}, {"rate", "not a rate"}, {"samples", "1"},
                 {"trigger", "1=0,3=1"}, {"schema_version", 2}}) {
            auto doc = original;
            doc[key] = value;
            QVERIFY(write(malformed, QByteArray::fromStdString(doc.dump())));
            QVERIFY(!profiles::load(malformed, error));
            QVERIFY(error.contains(malformed));
        }
        QVERIFY(!profiles::remove("../good.ltprofile", error));
        QVERIFY(!profiles::load("./good.ltprofile", error)); // explicit missing path never falls back to the store
        const auto saved = profiles::load("good", error);
        QVERIFY(saved);
        comparePlan(saved->fields, i2c);
#ifdef Q_OS_UNIX
        const QString link = dir.filePath("link.ltprofile");
        QVERIFY(QFile::link(valid.fileName(), link));
        QVERIFY(!profiles::save("link", uart, error));
        QVERIFY(!profiles::remove("link.ltprofile", error));
        QVERIFY(!profiles::recentFiles().contains("link.ltprofile"));
        QVERIFY(QFileInfo(link).isSymLink());
        const auto kept = profiles::load("good", error);
        QVERIFY(kept);
        comparePlan(kept->fields, i2c);
#endif
    }

    void profileDisconnectedStartupFollowsFirstDeviceOnly()
    {
        MainWindow w;
        auto tooLarge = uart;
        tooLarge.samples = "3000";
        QString error;
        QVERIFY(profiles::save("bench", tooLarge, error));
        const auto profile = profiles::load("bench", error);
        QVERIFY(profile);
        w.applyStartupProfile(*profile);
        comparePlan(plan(w), tooLarge);
        w.setPortText("demo:pico");
        w.connectOrDisconnect();
        QTRY_VERIFY(w.device().isConnected());
        comparePlan(plan(w), tooLarge);
        const auto preflight = w.findChild<QLabel*>("preflight")->text();
        QVERIFY(preflight.contains("3000") && preflight.contains("2000"));
        QCOMPARE(w.historySize(), std::size_t{0});
        w.connectOrDisconnect();
        QTRY_COMPARE(plan(w).channels, CaptureFields::defaults(false).channels);
        comparePlan(plan(w), CaptureFields::defaults(false));
    }

    void profileCommandLineNamesPathsAndOpen()
    {
        QString error;
        QVERIFY(profiles::save("I2C", i2c, error));
        QVERIFY(profiles::save("UART", uart, error));
        const QDir home = QDir::current();
        QVERIFY(QDir().mkpath(home.filePath("bench")));
        QVERIFY(QFile::copy(QDir(profiles::directory()).filePath("UART.ltprofile"), home.filePath("bench/I2C.ltprofile")));
        QVERIFY(write(home.filePath("tiny.vcd"),
                      "$timescale 1 us $end\n$scope module test $end\n$var wire 1 ! A $end\n"
                      "$upscope $end\n$enddefinitions $end\n#0\n0!\n#1\n1!\n#2\n0!\n"));
        const auto launch = [&](QStringList args, const char* expected, int ch, bool recording) {
            QProcess process;
            process.setWorkingDirectory(home.path());
            const QString socket = home.filePath("gui.sock");
            process.start(LT_GUI_PATH, args << "--socket" << socket);
            QVERIFY(process.waitForStarted());
            QTRY_VERIFY(lt::session::call(socket, "ping", nlohmann::json::object(), 200).ok);
            if (args.contains("--port")) {
                QTRY_VERIFY(lt::session::call(socket, "get_state").result.value("connected", false));
            }
            const auto pins = lt::session::call(socket, "get_pinout");
            QVERIFY2(pins.ok, pins.error.c_str());
            bool found = false;
            for (const auto& pin : pins.result.at("pins"))
                found |= pin.value("channel", 0) == ch && pin.value("name", std::string{}) == expected;
            QVERIFY(found);
            QCOMPARE(lt::session::call(socket, "get_state").result.at("has_capture").get<bool>(), recording);
            process.kill();
            QVERIFY(process.waitForFinished());
        };
        launch({"I2C"}, "SCL", 4, false);
        launch({"I2C.ltprofile", "--demo"}, "SDA", 3, true);
        launch({"./bench/I2C.ltprofile", "--port", "demo:pico", "--open", "tiny.vcd"}, "TX", 1, true);
        launch({home.filePath("bench/I2C.ltprofile")}, "RX", 2, false);
        for (const auto& args : {QStringList{"./missing/I2C.ltprofile"}, QStringList{"./tiny.vcd"},
                                 QStringList{"tiny.vcd", "I2C"}}) {
            QProcess process;
            process.setWorkingDirectory(home.path());
            process.start(LT_GUI_PATH, args);
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitCode(), 2);
            const auto stderrText = process.readAllStandardError();
            QVERIFY2(stderrText.contains(args.size() == 1 ? "Cannot load profile" : "at most one"), stderrText.constData());
        }
    }
};

int main(int argc, char** argv)
{
    const QString cwd = QDir::currentPath();
    QTemporaryDir home(QDir::current().filePath(QStringLiteral("profiles-home-XXXXXX")));
    if (!home.isValid())
        return 1;
    const QDir profileHome(home.filePath("profile-store"));
    if (!profileHome.mkpath("."))
        return 1;
    const QByteArray path = QFile::encodeName(home.path());
    for (const char* key : {"HOME", "USERPROFILE", "TMPDIR", "TEMP", "TMP"})
        qputenv(key, path);
    qputenv("LOGICTRACER_DENY_HARDWARE", "1");
    qputenv("LOGICTRACER_TEST_PROFILE_HOME", QFile::encodeName(profileHome.path()));
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QApplication app(argc, argv);
    if (profiles::directory() != profileHome.filePath(".logictracer/profiles") || !QDir::setCurrent(home.path()))
        qFatal("Refusing to run profile tests outside their temporary home");
    ProfileTests tests;
    const int status = QTest::qExec(&tests, argc, argv);
    QDir::setCurrent(cwd);
    return status;
}

#include "test_profiles.moc"

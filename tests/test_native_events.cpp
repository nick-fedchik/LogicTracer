// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/NativeEvents.hpp"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <linux/can.h>
#include <linux/gpio.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace lt::events;

class NativeEventsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void datagramsBecomeTimestampedFramesNotSamples()
    {
        int fd[2];
        QVERIFY(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) == 0);
        const auto cleanup = qScopeGuard([&] { ::close(fd[0]); ::close(fd[1]); });
        const int on = 1;
        QVERIFY(::setsockopt(fd[0], SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) == 0);
        can_frame classic{};
        classic.can_id = 0x121; classic.can_dlc = 2; classic.data[0] = 0; classic.data[1] = 255;
        QCOMPARE(::send(fd[1], &classic, sizeof(classic), 0), ssize_t(CAN_MTU));
        classic.can_id |= CAN_RTR_FLAG;
        QCOMPARE(::send(fd[1], &classic, sizeof(classic), 0), ssize_t(CAN_MTU));
        canfd_frame extended{};
        extended.can_id = CAN_EFF_FLAG | 0x1010101; extended.len = 12; extended.flags = CANFD_BRS | CANFD_ESI;
        extended.data[11] = 0xac;
        QCOMPARE(::send(fd[1], &extended, sizeof(extended), 0), ssize_t(CANFD_MTU));

        Request r; r.device = "fixture"; r.limit = 3;
        const auto result = collectNativeEvents(fd[0], r, {});
        QCOMPARE(result.status, Status::Ok);
        const auto& rows = result.recording.at("events");
        QVERIFY(rows.at(0).at("timestamp_ns").get<uint64_t>() > 0);
        QVERIFY(rows.at(0).at("data_hex") == "00ff" && rows.at(0).at("extended") == false && rows.at(0).at("fd") == false);
        QVERIFY(rows.at(1).at("rtr") == true && rows.at(1).at("length") == 2 && rows.at(1).at("data_hex") == "");
        QVERIFY(rows.at(2).at("extended") == true && rows.at(2).at("id") == 0x1010101 && rows.at(2).at("fd") == true);
        QVERIFY(rows.at(2).at("esi") == true && rows.at(2).at("brs") == true && rows.at(2).at("length") == 12);
        QVERIFY(rows.at(2).at("data_hex") == "0000000000000000000000ac");
        QTemporaryDir tmp;
        const auto path = tmp.filePath("frames.ltevents");
        QString err;
        nlohmann::json reopened;
        QVERIFY2(save(path, result.recording, err) && load(path, reopened, err), qPrintable(err));
        QVERIFY(reopened == result.recording);
    }

    void missingTimestampIsNotReplacedByUserspaceTime()
    {
        int fd[2];
        QVERIFY(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) == 0);
        const auto cleanup = qScopeGuard([&] { ::close(fd[0]); ::close(fd[1]); });
        can_frame frame{};
        QCOMPARE(::send(fd[1], &frame, sizeof(frame), 0), ssize_t(CAN_MTU));
        Request r; r.device = "fixture"; r.limit = 1;
        const auto result = collectNativeEvents(fd[0], r, {});
        QCOMPARE(result.status, Status::Error);
        QVERIFY(result.message.find("without a kernel receive timestamp") != std::string::npos);
        QVERIFY(result.recording.is_null());
    }

#ifdef GPIO_V2_GET_LINE_IOCTL
    void sparseEdgesRetainNanosecondsPolarityAndGaps()
    {
        int fd[2];
        QVERIFY(::pipe(fd) == 0);
        const auto cleanup = qScopeGuard([&] { ::close(fd[0]); ::close(fd[1]); });
        gpio_v2_line_event edges[2]{};
        edges[0].timestamp_ns = 9007199254740993ull; edges[0].offset = 9;
        edges[0].id = GPIO_V2_LINE_EVENT_RISING_EDGE; edges[0].seqno = 2;
        edges[1] = edges[0]; edges[1].timestamp_ns += 97;
        edges[1].id = GPIO_V2_LINE_EVENT_FALLING_EDGE; edges[1].seqno = 5;
        QCOMPARE(::write(fd[1], edges, sizeof(edges)), ssize_t(sizeof(edges)));
        Request r; r.source = Source::Gpio; r.device = "/dev/gpiochip1"; r.line = 9; r.hte = true; r.limit = 2;
        const auto result = collectNativeEvents(fd[0], r, {});
        QCOMPARE(result.status, Status::Ok);
        const auto& doc = result.recording;
        QVERIFY(doc.at("source").at("clock") == "hte" && doc.at("source").at("line") == 9);
        QVERIFY(doc.at("source").at("initial_level").is_null());
        QVERIFY(!doc.contains("sample_rate_hz") && !doc.contains("exact_timing"));
        const auto& rows = doc.at("events");
        QVERIFY(rows.at(0).at("lost_before") == 1 && rows.at(0).at("edge") == "rising");
        QVERIFY(rows.at(1).at("lost_before") == 2 && rows.at(1).at("edge") == "falling");
        QVERIFY(rows.at(1).at("timestamp_ns").get<uint64_t>() == edges[1].timestamp_ns);
        QTemporaryDir tmp;
        QString err;
        nlohmann::json reopened;
        const auto path = tmp.filePath("edges.ltevents");
        QVERIFY2(save(path, doc, err) && load(path, reopened, err), qPrintable(err));
        QVERIFY(reopened == doc);
        r.hte = false; r.limit = 1;
        QCOMPARE(::write(fd[1], edges, sizeof(edges[0])), ssize_t(sizeof(edges[0])));
        QVERIFY(collectNativeEvents(fd[0], r, {}).recording.at("source").at("clock") == "monotonic");
    }
#endif

    void emptyInputAndCancellationAreDifferentOutcomes()
    {
        int fd[2];
        QVERIFY(::pipe(fd) == 0);
        const auto cleanup = qScopeGuard([&] { ::close(fd[0]); ::close(fd[1]); });
        Request r; r.source = Source::Gpio; r.device = "/dev/gpiochip1"; r.durationMs = 5;
        QCOMPARE(collectNativeEvents(fd[0], r, {}).status, Status::Timeout);
        r.durationMs = 1000;
        int checks = 0;
        QCOMPARE(collectNativeEvents(fd[0], r, [&] { return ++checks == 2; }).status, Status::Cancelled);
    }
};

QTEST_GUILESS_MAIN(NativeEventsTests)
#include "test_native_events.moc"

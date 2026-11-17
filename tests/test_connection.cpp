// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/ArgParsing.hpp"
#include "device/AsyncDevice.hpp"
#include "FakeDevice.hpp"
#include "device/PicoConnection.hpp"
#include "device/PynqConnection.hpp"
#include "device/ReplayDevice.hpp"
#include "core/Exporters.hpp"
#include "services/Operations.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using fake::Device;

namespace {

// Handler for a well-behaved device. `onCapture` decides how to answer a capture request.
Device::Handler device(const std::string& version, bool extended,
                       std::function<void(Device&, const fake::Request&)> onCapture = {})
{
    return [=](Device& d, const std::vector<uint8_t>& payload) {
        if (payload[0] == 0) {
            d.send(fake::idReply(version, extended));
        } else if (payload[0] == 1) {
            const auto req = fake::parseRequest(payload);
            d.recordRequest(req);
            if (onCapture)
                onCapture(d, req);
        }
    };
}

// Answers every capture with CAPTURE_STARTED followed by a constant-value dump.
std::function<void(Device&, const fake::Request&)> answerWith(uint8_t value)
{
    return [value](Device& d, const fake::Request& r) {
        d.send("CAPTURE_STARTED\n");
        d.send(fake::captureData(r, value));
    };
}

lt::CaptureConfig config(uint32_t pre, uint32_t post, std::chrono::milliseconds timeout = 100ms)
{
    lt::CaptureConfig c;
    c.channels = {{0, "A"}, {1, "B"}};
    c.frequency = 1'000'000;
    c.preSamples = pre;
    c.postSamples = post;
    c.timeout = timeout;
    c.probeOnTimeout = false;
    return c;
}

}  // namespace

class ConnectionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void handshakeV65UsesWideRequest()
    {
        Device dev(device("V6_5", true, answerWith(0x01)));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QVERIFY(conn.info().generation == lt::ProtocolGeneration::V6);
        QCOMPARE(conn.info().version.minor, 5);
        QCOMPARE(conn.info().maxFrequency, 100'000'000u);
        QCOMPARE(conn.info().bufferSize, 131072u);
        QCOMPARE(conn.info().channels, uint8_t{24});

        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, false};
        const auto r = conn.capture(cfg);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(dev.requests().at(0).size, std::size_t{56});
        QCOMPARE(r.buffer->size(), std::size_t{110});
        QCOMPARE(r.buffer->preSamples(), 10u);
        QCOMPARE(r.buffer->samples()[5], 0x1u);
    }

    void handshakeV60UsesLegacyRequest()
    {
        Device dev(device("V6_0", true, answerWith(0x02)));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QVERIFY(conn.info().generation == lt::ProtocolGeneration::V5);
        QCOMPARE(conn.info().blastFrequency, 200'000'000u);

        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, true};
        const auto r = conn.capture(cfg);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        const auto req = dev.requests().at(0);
        QCOMPARE(req.size, std::size_t{48});
        QVERIFY(req.inverted);
        QCOMPARE(req.pre, 10u);
        QCOMPARE(req.post, 100u);
    }

    // LogicTracer firmware is SemVer 0.x: it must still be asked for its real limits.
    void handshakeLogicTracerZeroVersionReadsCapabilities()
    {
        Device dev([](Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0)
                d.send(fake::idReply("0.3.0", true, "FREQ:200000000\nBLASTFREQ:400000000\nBUFFER:393216\nCHANNELS:28\n"));
        });
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QCOMPARE(conn.info().maxFrequency, 200'000'000u);
        QCOMPARE(conn.info().blastFrequency, 400'000'000u);
        QCOMPARE(conn.info().bufferSize, 393'216u);
        QCOMPARE(conn.info().channels, uint8_t{28});
    }

    void denyHardwareGuardBlocksSerialAccess()
    {
        // Restore whatever the caller had: an audit may run the whole suite with the guard exported.
        const bool hadGuard = qEnvironmentVariableIsSet("LOGICTRACER_DENY_HARDWARE");
        const QByteArray previous = qgetenv("LOGICTRACER_DENY_HARDWARE");
        qputenv("LOGICTRACER_DENY_HARDWARE", "1");
        lt::PicoConnection conn;
        const bool denied = !conn.open("/dev/ttyACM9");  // refused before the path is even touched
        const std::string message = conn.lastError();
        Device dev(device("V6_5", true, answerWith(0x01)));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection emulated;
        const bool ptyAllowed = emulated.open(dev.port());  // pseudo-terminals are not hardware
        if (hadGuard)
            qputenv("LOGICTRACER_DENY_HARDWARE", previous);
        else
            qunsetenv("LOGICTRACER_DENY_HARDWARE");
        QVERIFY(denied);
        QVERIFY(message.find("LOGICTRACER_DENY_HARDWARE") != std::string::npos);
        QVERIFY2(ptyAllowed, emulated.lastError().c_str());
    }

    // Firmware that reports FEATURES bit 0 gets the edge-sensitive trigger for user triggers only; the
    // immediate plan (which relies on a level firing at once) keeps the level-sensitive type.
    void edgeSensitiveTriggerIsUsedWhenTheFirmwareHasIt()
    {
        Device dev([](Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("0.3.0", true,
                                     "FREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nFEATURES:1\nCHANNELS:24\n"));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.recordRequest(r);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QVERIFY(conn.info().supportsEdgeTrigger());
        QCOMPARE(conn.info().channels, uint8_t{24});  // FEATURES before CHANNELS does not disturb the handshake

        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, false};
        const auto triggered = conn.capture(cfg);
        QVERIFY2(triggered.status == lt::CaptureStatus::Ok, triggered.error.c_str());
        QVERIFY(triggered.edgeSensitive);
        QCOMPARE(int(dev.requests().back().triggerType), 4);

        const auto immediate = conn.capture(config(2, 50));
        QVERIFY2(immediate.status == lt::CaptureStatus::Ok, immediate.error.c_str());
        QVERIFY(!immediate.edgeSensitive);
        QCOMPARE(int(dev.requests().back().triggerType), 0);

        // The "trigger already active" heuristic only applies to level-sensitive firmware.
        lt::CaptureRequestSpec spec;
        spec.channels = cfg.channels;
        spec.frequency = cfg.frequency;
        spec.samples = 110;
        spec.trigger = cfg.trigger;
        const auto doc = lt::captureDocument(conn.info(), spec, cfg, triggered, {}, {}).doc;
        QCOMPARE(doc["config"]["trigger"]["sensitivity"].get<std::string>(), std::string("edge"));
        QVERIFY(doc["warnings"].empty());

        // Session/MCP "invariant" parameter: evaluated into the document and failing the expectations.
        std::string err;
        const auto opts = lt::documentOptionsFromJson({{"invariant", "never(CH1 & !CH1); never(NOPE)"}}, false, 24,
                                                      cfg.channels, err);
        QVERIFY2(opts && opts->invariants, err.c_str());
        const auto checked = lt::captureDocument(conn.info(), spec, cfg, triggered, {}, *opts);
        QVERIFY(!checked.expectationsPassed);
        QVERIFY(checked.doc["invariants"]["rules"][0]["passed"].get<bool>());
        QVERIFY(checked.doc["invariants"]["rules"][1].contains("error"));
        QVERIFY(!lt::documentOptionsFromJson({{"invariant", "never(A &)"}}, false, 24, cfg.channels, err));
        QVERIFY(err.find("invariant") != std::string::npos);
    }

    // --segments: loopCount = windows - 1 in the request, the buffer holds pre + post x windows samples.
    void segmentedCaptureRequestsLoopCount()
    {
        Device dev([](Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("0.3.0", true,
                                     "FREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nFEATURES:1\nCHANNELS:24\n"));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.recordRequest(r);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, false};
        cfg.segments = 4;
        const auto r = conn.capture(cfg);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(dev.requests().back().loop, 3u);
        QCOMPARE(r.buffer->size(), std::size_t{410});
        QVERIFY(r.segments && r.segments->count == 4 && r.segments->postSamples == 100);

        QCOMPARE(int(dev.requests().back().measure), 0);  // FEATURES:1 has no edge burst timestamps
        QVERIFY(r.segments->triggerTimesSeconds.empty());

        cfg.segments = 2000;  // 10 + 100 x 2000 samples do not fit
        QVERIFY(conn.capture(cfg).error.find("exceed device buffer") != std::string::npos);
        cfg.segments = 2;
        cfg.trigger.reset();
        QVERIFY(conn.capture(cfg).error.find("needs a trigger") != std::string::npos);
    }

    // FEATURES bit 1: the host asks for burst timestamps and turns them into trigger times (rollover-safe).
    void segmentedCaptureReadsBurstTimestamps()
    {
        Device dev([](Device& d, const std::vector<uint8_t>& payload) {
            if (payload[0] == 0) {
                d.send(fake::idReply("0.3.0", true,
                                     "FREQ:100000000\nBLASTFREQ:200000000\nBUFFER:131072\nFEATURES:3\nCHANNELS:24\n"));
            } else if (payload[0] == 1) {
                const auto r = fake::parseRequest(payload);
                d.recordRequest(r);
                d.send("CAPTURE_STARTED\n");
                d.send(fake::captureData(r, 0x01));
            }
        });
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QVERIFY(conn.info().supportsEdgeBurstTimestamps());
        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, false};
        cfg.segments = 4;
        const auto r = conn.capture(cfg);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(int(dev.requests().back().measure), 1);
        const auto& t = r.segments->triggerTimesSeconds;
        QCOMPARE(t.size(), std::size_t{4});
        for (std::size_t k = 0; k < t.size(); ++k)
            QVERIFY(std::abs(t[k] - 1e-3 * static_cast<double>(k)) < 1e-12);
        QCOMPARE(r.segments->triggerTimeResolutionSeconds, 5e-9);
        QVERIFY(lt::triggerSpacing(*r.segments)->median > 0.999e-3);
    }

    void handshakeV5UsesDefaults()
    {
        Device dev(device("V5_2", false));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY2(conn.open(dev.port()), conn.lastError().c_str());
        QVERIFY(conn.info().generation == lt::ProtocolGeneration::V5);
        QCOMPARE(conn.info().maxFrequency, 100'000'000u);
        QCOMPARE(conn.info().bufferSize, 131072u);
        QCOMPARE(conn.info().channels, uint8_t{24});
    }

    void immediateCaptureStartsWithLowTrigger()
    {
        Device dev(device("V6_5", true, answerWith(0)));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.attempts, 1);
        QVERIFY(r.triggerUsed->inverted);
        QCOMPARE(r.buffer->size(), std::size_t{50});  // the 2 mandatory pre-trigger samples are dropped
        QCOMPARE(r.buffer->preSamples(), 0u);
    }

    void immediateCaptureFallsBackToHighTrigger()
    {
        // A HIGH line: the "low" trigger never fires, the "high" one does.
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            if (!r.inverted)
                d.send(fake::captureData(r, 0x03));
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        conn.setPollLatency(50ms);
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.attempts, 2);
        QVERIFY(!r.triggerUsed->inverted);
        QVERIFY(dev.cancelCount() >= 1);
        QCOMPARE(r.buffer->samples()[0], 0x3u);
    }

    void triggerTimeoutCancelsAndReports()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request&) { d.send("CAPTURE_STARTED\n"); }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        conn.setPollLatency(50ms);
        QVERIFY(conn.open(dev.port()));
        auto cfg = config(10, 100, 100ms);
        cfg.trigger = lt::EdgeTrigger{0, false};

        const auto t0 = std::chrono::steady_clock::now();
        const auto r = conn.capture(cfg);
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        QVERIFY(r.status == lt::CaptureStatus::Timeout);
        QVERIFY(r.error.find("trigger did not occur") != std::string::npos);
        QVERIFY(dev.cancelCount() >= 1);
        QVERIFY(elapsed < 3s);
    }

    void dataArrivingAfterCancelIsStillDelivered()
    {
        // The capture finishes in the window between the host timing out and the firmware noticing 0xFF.
        std::thread sender;
        Device dev(device("V6_5", true, [&](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            sender = std::thread([&d, r] {
                std::this_thread::sleep_for(220ms);
                d.send(fake::captureData(r, 0x01));
            });
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        conn.setPollLatency(50ms);
        QVERIFY(conn.open(dev.port()));
        auto cfg = config(10, 100, 100ms);
        cfg.trigger = lt::EdgeTrigger{0, false};
        const auto r = conn.capture(cfg);
        if (sender.joinable())
            sender.join();
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.buffer->size(), std::size_t{110});
    }

    void triggeredCaptureWaitsForPostSampleRecording()
    {
        // Regression for A-01: triggerWait must include captureTime = (pre + post) / freq.
        // At 4000 Hz, 1200 post-samples take 300 ms (> timeout 80 ms + pollLatency 50 ms).
        std::thread sender;
        Device dev(device("V6_5", true, [&](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            sender = std::thread([&d, r] {
                std::this_thread::sleep_for(260ms);
                d.send(fake::captureData(r, 0x01));
            });
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        conn.setPollLatency(50ms);
        QVERIFY(conn.open(dev.port()));
        auto cfg = config(10, 1200, 80ms);
        cfg.frequency = 4000;
        cfg.trigger = lt::EdgeTrigger{0, false};
        const auto r = conn.capture(cfg);
        if (sender.joinable())
            sender.join();
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.buffer->size(), std::size_t{1210});
    }

    void deviceRejectsRequest()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request&) { d.send("CAPTURE_ERROR\n"); }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY(r.status == lt::CaptureStatus::Error);
        QVERIFY(r.error.find("rejected") != std::string::npos);
    }

    void wrongSampleCountIsAnError()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            d.send(fake::captureData(r, 0, true, r.totalSamples() + 1));
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY(r.status == lt::CaptureStatus::Error);
        QVERIFY(r.error.find("unexpected sample count") != std::string::npos);
    }

    void missingTrailerIsAnError()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            d.send(fake::captureData(r, 0, false));
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY(r.status == lt::CaptureStatus::Error);
        QVERIFY(r.error.find("trailer") != std::string::npos);
        QVERIFY(!r.buffer);
    }

    void truncatedTimestampsAreAnError()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            auto data = fake::captureData(r, 0, false);
            data.insert(data.end(), {3, 0x01, 0x02});  // announces 3 timestamps (12 bytes), sends 2 bytes
            d.send(data);
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto r = conn.capture(config(2, 50));
        QVERIFY(r.status == lt::CaptureStatus::Error);
        QVERIFY(r.error.find("truncated") != std::string::npos);
    }

    void vanishedDeviceAbortsCaptureFast()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            auto data = fake::captureData(r, 0);
            data.resize(data.size() / 2);
            d.send(data);
            d.disappear();
        }));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = conn.capture(config(2, 1000));
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        QVERIFY(r.status == lt::CaptureStatus::Error);
        // The inactivity limit is 3 s: a vanished device must be noticed well before that.
        QVERIFY2(elapsed < 2s, "a vanished device must not be waited for until the inactivity timeout");
    }

    void validationUsesDeviceLimits()
    {
        Device dev(device("V6_5", true));
        LT_REQUIRE_PTY(dev);
        lt::PicoConnection conn;
        QVERIFY(conn.open(dev.port()));

        auto cfg = config(2, 65536);
        cfg.channels = {{0, {}}, {15, {}}};  // 16ch mode: 65536 samples total
        QVERIFY(conn.validate(cfg).has_value());  // 2 + 65536 > 65536
        cfg.postSamples = 65534;
        QVERIFY(!conn.validate(cfg).has_value());

        cfg.trigger = lt::EdgeTrigger{24, false};  // external trigger input
        QVERIFY(!conn.validate(cfg).has_value());
        cfg.trigger = lt::EdgeTrigger{25, false};
        QVERIFY(conn.validate(cfg).has_value());

        cfg.trigger.reset();
        cfg.channels = {{24, {}}};  // beyond the 24 device channels
        QVERIFY(conn.validate(cfg).has_value());
    }
};


// A 1 MHz recording: CH1 (STIM) pulses at samples 100, 300, 500 (20 samples wide); CH3 (REACT)
// follows each pulse 2 samples later. Saved as lossless JSON for the replay device.
QString writeRecording(const QTemporaryDir& dir)
{
    std::vector<uint32_t> s(700, 0u);
    for (std::size_t t0 : {100u, 300u, 500u})
        for (std::size_t w = 0; w < 20; ++w) {
            s[t0 + w] |= 1u;
            s[t0 + 2 + w] |= 2u;
        }
    const lt::CaptureBuffer buf({{0, "STIM"}, {2, "REACT"}}, 1'000'000, 0, s);
    lt::SignalAnalyzer::Options opts;
    opts.collectEdges = true;
    const QString path = dir.filePath("rec.json");
    if (!lt::writeJsonFile(path.toStdString(), lt::captureToJson(buf, lt::SignalAnalyzer(opts).analyze(buf), true)))
        return {};
    return path;
}

class AsyncTests : public QObject {
    Q_OBJECT

    static bool connect(lt::AsyncDevice& ad, Device& dev)
    {
        ad.setPollLatency(50ms);
        QSignalSpy opened(&ad, &lt::AsyncDevice::opened);
        if (!ad.open(QString::fromStdString(dev.port())))
            return false;
        return opened.wait(5000) && opened.first().at(0).toBool();
    }

private Q_SLOTS:
    void captureRunsOffTheCallerThread()
    {
        Device dev(device("V6_5", true, answerWith(0x01)));
        LT_REQUIRE_PTY(dev);
        lt::AsyncDevice ad;
        QVERIFY(connect(ad, dev));
        QVERIFY(ad.isConnected());
        QCOMPARE(ad.info().channels, uint8_t{24});

        qRegisterMetaType<lt::CaptureResult>();
        QSignalSpy finished(&ad, &lt::AsyncDevice::captureFinished);
        QSignalSpy progress(&ad, &lt::AsyncDevice::progress);
        auto cfg = config(10, 100);
        cfg.trigger = lt::EdgeTrigger{0, false};
        QVERIFY(ad.startCapture(cfg));
        QVERIFY(ad.isBusy());
        QVERIFY(!ad.startCapture(cfg));  // one operation at a time
        QVERIFY(finished.wait(5000));
        QVERIFY(!ad.isBusy());

        const auto r = finished.first().at(0).value<lt::CaptureResult>();
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.buffer->size(), std::size_t{110});

        QList<int> stages;
        for (const auto& args : progress)
            stages << args.at(0).toInt();
        const QList<int> expected{static_cast<int>(lt::CaptureStage::Starting),
                                  static_cast<int>(lt::CaptureStage::WaitingForTrigger),
                                  static_cast<int>(lt::CaptureStage::Downloading),
                                  static_cast<int>(lt::CaptureStage::Done)};
        QCOMPARE(stages, expected);
    }

    void cancelStopsAWaitingCapture()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request&) { d.send("CAPTURE_STARTED\n"); }));
        LT_REQUIRE_PTY(dev);
        lt::AsyncDevice ad;
        QVERIFY(connect(ad, dev));

        QSignalSpy progress(&ad, &lt::AsyncDevice::progress);
        QSignalSpy finished(&ad, &lt::AsyncDevice::captureFinished);
        auto cfg = config(10, 100, 60'000ms);  // would wait a minute for the trigger
        cfg.trigger = lt::EdgeTrigger{0, false};
        QVERIFY(ad.startCapture(cfg));
        QTRY_VERIFY_WITH_TIMEOUT(progress.count() >= 2, 5000);  // WaitingForTrigger reached

        QElapsedTimer t;
        t.start();
        ad.requestCancel();
        QVERIFY(finished.wait(5000));
        QVERIFY2(t.elapsed() < 2000, "cancel must not wait for the trigger timeout");
        const auto r = finished.first().at(0).value<lt::CaptureResult>();
        QVERIFY(r.status == lt::CaptureStatus::Cancelled);
        QVERIFY(dev.cancelCount() >= 1);  // the device was told to stop

        // The connection is still usable afterwards.
        QVERIFY(!ad.isBusy());
        QVERIFY(ad.isConnected());
    }

    void closeDuringHandshakeLeavesTheDeviceDisconnected()
    {
        Device dev(device("V6_5", true, answerWith(0x00)));
        LT_REQUIRE_PTY(dev);
        lt::AsyncDevice ad;
        QSignalSpy opened(&ad, &lt::AsyncDevice::opened);
        QSignalSpy closed(&ad, &lt::AsyncDevice::closed);
        QVERIFY(ad.open(QString::fromStdString(dev.port())));
        ad.close();  // while the handshake is still running on the worker thread
        QVERIFY(closed.wait(5000));
        QVERIFY(!ad.isConnected());
        QVERIFY(!ad.startProbe({{0, {}}}, 5));
        QVERIFY(opened.count() == 0 || !opened.first().at(0).toBool());
    }

    void probeWorksAndCancelWhenIdleIsHarmless()
    {
        Device dev(device("V6_5", true, answerWith(0x00)));
        LT_REQUIRE_PTY(dev);
        lt::AsyncDevice ad;
        QVERIFY(connect(ad, dev));
        ad.requestCancel();  // idle: must not poison the next operation
        QSignalSpy finished(&ad, &lt::AsyncDevice::probeFinished);
        QVERIFY(ad.startProbe({{0, {}}, {1, {}}}, 5));
        QVERIFY(finished.wait(5000));
        const auto r = finished.first().at(0).value<lt::ProbeResult>();
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
    }

    // Import caveats of a replayed recording (e.g. x/z levels in a VCD) travel with every capture.
    void replayCarriesImportWarnings()
    {
        QTemporaryDir dir;
        const QString vcd = dir.filePath("xz.vcd");
        QFile f(vcd);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("$timescale 1us $end\n$var wire 1 ! S $end\n$enddefinitions $end\n#0\n0!\n#1\nz!\n#2\n1!\n#3\n0!\n#4\n1!\n#5\n0!\n");
        f.close();
        auto dev = lt::createDevice("replay:" + vcd.toStdString());
        QVERIFY2(dev->open("replay:" + vcd.toStdString(), 100ms), dev->lastError().c_str());
        lt::CaptureConfig c;
        c.channels = {{0, {}}};
        c.frequency = 1'000'000;
        c.preSamples = 2;
        c.postSamples = 4;
        const auto r = dev->capture(c);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QVERIFY(!r.warnings.empty());
        QVERIFY(r.warnings.front().find("x/z") != std::string::npos);
    }

    // PYNQ in-design capture through the board agent with its simulated lt_capture core.
    void pynqConnectionAgainstSimulatedCore()
    {
#if !defined(LT_AGENT_SCRIPT)
        QSKIP("Python 3 was not found at configure time");
#else
        QProcess agentProc;
        agentProc.start(LT_PYTHON, {LT_AGENT_SCRIPT, "--sim", "--port", "0"});
        QVERIFY(agentProc.waitForStarted(5000));
        QString port;
        for (int i = 0; i < 100 && port.isEmpty(); ++i) {
            agentProc.waitForReadyRead(100);
            const auto line = QString::fromUtf8(agentProc.readAllStandardOutput()).trimmed();
            if (line.startsWith("LISTENING "))
                port = line.mid(10);
        }
        QVERIFY2(!port.isEmpty(), "the agent did not start");
        struct Stop {
            QProcess& p;
            ~Stop() { p.kill(); p.waitForFinished(2000); }
        } stop{agentProc};

        const std::string url = "pynq://127.0.0.1:" + port.toStdString();
        auto dev = lt::createDevice(url);
        QVERIFY2(dev->open(url, 3000ms), dev->lastError().c_str());
        QVERIFY(dev->info().dutClock);
        QCOMPARE(int(dev->info().channels), 2);
        QCOMPARE(dev->info().maxFrequency, 100'000'000u);

        lt::CaptureConfig c;
        c.channels = {{0, {}}, {1, {}}};
        c.frequency = 100'000'000;
        c.preSamples = 200;
        c.postSamples = 800;
        c.trigger = lt::EdgeTrigger{0, false};
        bool armed = false;
        dev->setProgressCallback([&](lt::CaptureStage s) { armed |= s == lt::CaptureStage::WaitingForTrigger; });
        auto r = dev->capture(c);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QVERIFY(armed);
        QCOMPARE(r.buffer->size(), std::size_t{1000});
        QCOMPARE(r.buffer->triggerIndex(), int64_t{200});
        QVERIFY(r.buffer->exactTiming());
        QCOMPARE(r.buffer->channels()[1].name, std::string("REACT"));  // probe names from the agent
        QVERIFY(!r.buffer->bit(199, 0) && r.buffer->bit(200, 0));
        QVERIFY(!r.buffer->bit(204, 1) && r.buffer->bit(205, 1));  // exactly 5 cycles
        const auto lat = lt::SignalAnalyzer::analyzeLatency(*r.buffer, {0, false, 1, false, std::nullopt});
        QVERIFY(lat.exact && lat.constantDelayConsistent());
        QCOMPARE(lat.upperBoundNs(lat.maxSamples), 50.0);

        c.frequency = 1'000'000;  // the core samples at the DUT clock only
        QVERIFY(dev->capture(c).error.find("--freq 100000000") != std::string::npos);
        c.frequency = 100'000'000;
        c.trigger = lt::EdgeTrigger{2, false};  // channel 3 = the core's external trigger input, never asserted
        c.timeout = 10s;
        std::thread canceller([&] {
            std::this_thread::sleep_for(300ms);
            dev->requestCancel();  // from another thread, like AsyncDevice
        });
        QElapsedTimer t;
        t.start();
        r = dev->capture(c);
        canceller.join();
        QVERIFY2(r.status == lt::CaptureStatus::Cancelled, r.error.c_str());
        QVERIFY(t.elapsed() < 5000);
        c.timeout = 100ms;
        QVERIFY(dev->capture(c).status == lt::CaptureStatus::Timeout);

        const auto probe = dev->probe(c.channels, 1ms, 0);
        QVERIFY2(probe.status == lt::CaptureStatus::Ok, probe.error.c_str());
        QCOMPARE(probe.buffer->size(), std::size_t{100'000});

        lt::PynqConnection bad;
        QVERIFY(!bad.open("pynq://127.0.0.1:1", 500ms));  // nothing listens there
        QVERIFY(!bad.open("pynq://", 500ms));
#endif
    }

    void replayDeviceRecordsSegments()
    {
        QTemporaryDir dir;
        const std::string port = "replay:" + writeRecording(dir).toStdString();
        auto dev = lt::createDevice(port);
        QVERIFY2(dev->open(port, 100ms), dev->lastError().c_str());
        lt::CaptureConfig c;
        c.channels = {{0, {}}, {2, {}}};
        c.frequency = 1'000'000;
        c.preSamples = 10;
        c.postSamples = 40;
        c.trigger = lt::EdgeTrigger{0, false};
        c.segments = 3;  // pulses at 100, 300, 500
        c.probeOnTimeout = false;
        const auto r = dev->capture(c);
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QCOMPARE(r.buffer->size(), std::size_t{130});
        const auto windows = lt::splitSegments(*r.buffer, *r.segments);
        QCOMPARE(windows.size(), std::size_t{3});
        QVERIFY(!windows[2].bit(0, 0) && windows[2].bit(1, 0));  // synthetic pre-trigger sample, then the edge
        QVERIFY(!windows[2].bit(2, 1) && windows[2].bit(3, 1));  // REACT 2 samples after the trigger
        QCOMPARE(r.segments->triggerTimesSeconds, (std::vector<double>{0.0, 200e-6, 400e-6}));  // exact from the file

        auto again = lt::createDevice(port);
        QVERIFY(again->open(port, 100ms));
        c.segments = 4;
        const auto partial = again->capture(c);
        QVERIFY(partial.status == lt::CaptureStatus::Timeout);
        QVERIFY(partial.error.find("3 of 4 windows") != std::string::npos);
    }

    void replayDeviceReplaysConsecutiveEvents()
    {
        QTemporaryDir dir;
        const QString rec = writeRecording(dir);
        QVERIFY(!rec.isEmpty());
        const std::string port = "replay:" + rec.toStdString();
        QVERIFY(lt::isReplayPort(port));

        auto dev = lt::createDevice(port);
        QVERIFY2(dev->open(port, 100ms), dev->lastError().c_str());
        QVERIFY(dev->info().version.family == lt::FirmwareFamily::Replay);
        QCOMPARE(int(dev->info().channels), 3);  // highest recorded channel is CH3
        QCOMPARE(dev->info().maxFrequency, 1'000'000u);

        lt::CaptureConfig c;
        c.channels = {{0, {}}, {2, {}}};
        c.frequency = 1'000'000;
        c.preSamples = 10;
        c.postSamples = 40;
        c.trigger = lt::EdgeTrigger{0, false};
        for (int event = 0; event < 3; ++event) {  // pulses at 100, 300, 500: one per capture, in order
            const auto r = dev->capture(c);
            QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
            QCOMPARE(r.buffer->size(), std::size_t{50});
            QCOMPARE(r.buffer->triggerIndex(), int64_t{10});
            QVERIFY(!r.buffer->bit(9, 0) && r.buffer->bit(10, 0));  // edge exactly at the trigger index
            QVERIFY(!r.buffer->bit(11, 1) && r.buffer->bit(12, 1));  // REACT 2 samples later
            QCOMPARE(r.buffer->channels()[1].name, std::string("REACT"));  // names come from the recording
        }
        const auto end = dev->capture(c);
        QVERIFY(end.status == lt::CaptureStatus::Timeout);
        QVERIFY(end.lineDump);

        // Integer decimation is supported, other rates and missing channels are rejected up front.
        c.trigger.reset();
        c.frequency = 500'000;
        QVERIFY(!dev->validate(c));
        QCOMPARE(dev->capture(c).buffer->frequency(), 500'000u);
        c.frequency = 300'000;
        QVERIFY(dev->validate(c));
        c.frequency = 1'000'000;
        c.channels = {{1, {}}};
        QVERIFY(dev->validate(c)->find("not in the recording") != std::string::npos);

        // Immediate capture mirrors the Pico: the 2 reserved pre-trigger samples are not returned.
        c.channels = {{0, {}}};
        c.preSamples = 2;
        c.postSamples = 50;
        const auto immediate = dev->capture(c);
        QVERIFY2(immediate.status == lt::CaptureStatus::Ok, immediate.error.c_str());
        QCOMPARE(immediate.buffer->size(), std::size_t{50});
        QVERIFY(!dev->enterBootloader());

        lt::ReplayDevice bad;
        QVERIFY(!bad.open("replay:/does/not/exist.json", 100ms));
    }

    void asyncDeviceDrivesAReplay()
    {
        QTemporaryDir dir;
        const QString rec = writeRecording(dir);
        lt::AsyncDevice ad;
        QSignalSpy opened(&ad, &lt::AsyncDevice::opened);
        QVERIFY(ad.open("replay:" + rec));
        QVERIFY(opened.wait(5000));
        QVERIFY(opened.first().at(0).toBool());
        QSignalSpy finished(&ad, &lt::AsyncDevice::captureFinished);
        lt::CaptureConfig c;
        c.channels = {{0, {}}, {2, {}}};
        c.preSamples = 5;
        c.postSamples = 30;
        c.trigger = lt::EdgeTrigger{2, true};  // REACT falling
        QVERIFY(ad.startCapture(c));
        QVERIFY(finished.wait(5000));
        const auto r = finished.first().at(0).value<lt::CaptureResult>();
        QVERIFY2(r.status == lt::CaptureStatus::Ok, r.error.c_str());
        QVERIFY(r.buffer->bit(4, 1) && !r.buffer->bit(5, 1));
    }

    void failedOpenIsReported()
    {
        lt::AsyncDevice ad;
        QSignalSpy opened(&ad, &lt::AsyncDevice::opened);
        QVERIFY(ad.open("/dev/does-not-exist"));
        QVERIFY(opened.wait(5000));
        QVERIFY(!opened.first().at(0).toBool());
        QVERIFY(!opened.first().at(1).toString().isEmpty());
        QVERIFY(!ad.isConnected());
        QVERIFY(!ad.startCapture(config(2, 10)));
    }

    void destroyingWhileWaitingDoesNotHang()
    {
        Device dev(device("V6_5", true, [](Device& d, const fake::Request&) { d.send("CAPTURE_STARTED\n"); }));
        LT_REQUIRE_PTY(dev);
        QElapsedTimer t;
        t.start();
        {
            lt::AsyncDevice ad;
            QVERIFY(connect(ad, dev));
            QSignalSpy progress(&ad, &lt::AsyncDevice::progress);
            auto cfg = config(10, 100, 60'000ms);
            cfg.trigger = lt::EdgeTrigger{0, false};
            QVERIFY(ad.startCapture(cfg));
            QTRY_VERIFY_WITH_TIMEOUT(progress.count() >= 2, 5000);
        }
        QVERIFY(t.elapsed() < 5000);
    }
};

class CliTests : public QObject {
    Q_OBJECT

    static int run(const QStringList& args, nlohmann::json* out = nullptr, QString* raw = nullptr)
    {
        QProcess p;
        p.start(LT_CLI_PATH, args);
        if (!p.waitForFinished(30'000))
            return -1000;
        const auto text = QString::fromUtf8(p.readAllStandardOutput());
        if (raw)
            *raw = text;
        if (out)
            *out = nlohmann::json::parse(text.toStdString(), nullptr, false);
        return p.exitCode();
    }

private Q_SLOTS:
    void diffComparesTwoFilesAndHonoursStrict()
    {
        QTemporaryDir dir;
        const auto base = writeRecording(dir);
        QVERIFY(!base.isEmpty());
        nlohmann::json j;
        QCOMPARE(run({"diff", base, base, "--measure-latency", "1..2", "--strict", "--json"}, &j), 0);
        QCOMPARE(j["verdict"].get<std::string>(), std::string("equivalent"));
        QCOMPARE(run({"diff", base, "--json"}, &j), 64);
        QCOMPARE(run({"diff", base, dir.filePath("missing.json"), "--json"}, &j), 1);
    }

    void analyzeInvariantsSetExitCode()
    {
        QTemporaryDir dir;
        const auto rec = writeRecording(dir);
        QVERIFY(!rec.isEmpty());
        nlohmann::json j;
        QCOMPARE(run({"analyze", rec, "--invariant", "within(STIM, REACT, 2us); never(STIM & !REACT, settle=2us)",
                      "--json"}, &j), 0);
        QVERIFY(j["invariants"]["passed"].get<bool>());
        QCOMPARE(run({"analyze", rec, "--invariant", "within(STIM, CH3, 1us)", "--json"}, &j), 3);
        QCOMPARE(j["invariants"]["rules"][0]["violations"].get<int>(), 3);
        QCOMPARE(run({"analyze", rec, "--invariant", "within(STIM, REACT)", "--json"}, &j), 64);
    }

    void captureSegmentsThroughReplay()
    {
        QTemporaryDir dir;
        const auto rec = writeRecording(dir);
        const QString port = "replay:" + rec;
        const QString out = dir.filePath("seg.json");
        nlohmann::json j;
        QCOMPARE(run({"capture", "--port", port, "--no-session", "--channels", "1:STIM,3:REACT", "--samples", "50",
                      "--pre-samples", "10", "--segments", "3", "--trigger", "1:rising", "--measure-latency", "1..3",
                      "--expect", "events==3,drops==0", "--out-json", out, "--json"}, &j), 0);
        QCOMPARE(j["segments"]["windows"].size(), std::size_t{3});
        QCOMPARE(j["latency"]["count"].get<int>(), 3);
        QCOMPARE(run({"analyze", out, "--measure-latency", "1..3", "--json"}, &j), 0);
        QCOMPARE(j["segments"]["layout"]["count"].get<int>(), 3);
        QCOMPARE(j["latency"]["count"].get<int>(), 3);
        QCOMPARE(run({"capture", "--port", port, "--no-session", "--segments", "3", "--json"}, &j), 64);
        QCOMPARE(run({"capture", "--port", port, "--no-session", "--segments", "3", "--trigger", "1:rising",
                      "--out-vcd", dir.filePath("x.vcd"), "--json"}, &j), 64);
        QCOMPARE(run({"analyze", out, "--out-vcd", dir.filePath("x.vcd"), "--json"}, &j), 64);
    }

    void unknownCommandYieldsJson()
    {
        nlohmann::json j;
        QCOMPARE(run({"bogus", "--json"}, &j), 64);
        QVERIFY(j.is_object());
        QCOMPARE(j["status"].get<std::string>(), std::string("ERROR"));
    }

    void bareJsonFlagYieldsJson()
    {
        nlohmann::json j;
        QCOMPARE(run({"--json"}, &j), 64);
        QVERIFY(j.is_object());
    }

    void invalidArgumentsYieldJsonUsageError()
    {
        nlohmann::json j;
        QCOMPARE(run({"capture", "--freq", "10ms", "--json"}, &j), 64);
        QVERIFY(j.is_object());
        QVERIFY(j["error"].get<std::string>().find("--freq") != std::string::npos);
        QCOMPARE(run({"capture", "--channels", "0", "--json"}, &j), 64);
        QVERIFY(j.is_object());
        QCOMPARE(run({"probe", "--window", "0", "--json"}, &j), 64);
        QVERIFY(j.is_object());
    }

    void probeAgainstEmulatedDevice()
    {
        Device dev(device("V6_5", true, answerWith(0x01)));  // channel 1 high, channel 2 low
        LT_REQUIRE_PTY(dev);
        nlohmann::json j;
        QString raw;
        const int rc = run({"probe", "--port", QString::fromStdString(dev.port()), "--channels", "1,2", "--json"}, &j, &raw);
        QVERIFY2(rc == 0, qPrintable(raw));
        QCOMPARE(j["status"].get<std::string>(), std::string("OK"));
        QCOMPARE(j["channels"].size(), std::size_t{2});
        QCOMPARE(j["channels"][0]["state"].get<std::string>(), std::string("HIGH"));
        QCOMPARE(j["channels"][1]["state"].get<std::string>(), std::string("LOW"));
        QVERIFY(j["warnings"].empty());
    }

    void captureRejectsOversizedImmediateRequest()
    {
        Device dev(device("V6_5", true, answerWith(0)));
        LT_REQUIRE_PTY(dev);
        nlohmann::json j;
        const int rc = run({"capture", "--port", QString::fromStdString(dev.port()), "--channels", "1,16",
                            "--samples", "65536", "--json", "--no-session"}, &j);
        QCOMPARE(rc, 64);
        QVERIFY(j["error"].get<std::string>().find("65534") != std::string::npos);
        QVERIFY(dev.requests().empty());
    }

    void captureFromReplayPort()
    {
        QTemporaryDir dir;
        const QString rec = writeRecording(dir);
        nlohmann::json j;
        QString raw;
        const int rc = run({"capture", "--port", "replay:" + rec, "--no-session", "--freq", "1M", "--samples", "60",
                            "--channels", "1:STIM,3:REACT", "--trigger", "1:rising", "--measure-latency", "1..3",
                            "--expect", "events==1,p50_ns==2000", "--json"},
                           &j, &raw);
        QVERIFY2(rc == 0, qPrintable(raw));
        QCOMPARE(j["device"]["firmware_family"].get<std::string>(), std::string("replay"));
        QVERIFY(j["assertions"]["passed"].get<bool>());
    }

    void armedHookLatencyAndExpectExitCode()
    {
        // Emulated burst: CH1 rises at sample 10, CH2 rises at sample 12 (delta = 2 us @ 1 MHz).
        Device dev(device("V6_5", true, [](Device& d, const fake::Request& r) {
            d.send("CAPTURE_STARTED\n");
            const uint32_t n = r.totalSamples();
            std::vector<uint8_t> out{static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
                                     static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24)};
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t b = 0;
                if (i >= 10 && i < 20) b |= 1u;
                if (i >= 12 && i < 22) b |= 2u;
                out.push_back(b);
                for (std::size_t k = 1; k < r.bytesPerSample(); ++k)
                    out.push_back(0);
            }
            out.push_back(0);
            std::this_thread::sleep_for(30ms);
            d.send(out);
        }));
        LT_REQUIRE_PTY(dev);

        QTemporaryDir dir;
        const QString ready = dir.filePath("armed.flag");
        const QString hookOut = dir.filePath("hook.txt");
        const QString evCsv = dir.filePath("events.csv");
        nlohmann::json j;
        QString raw;
        int rc = run({"capture", "--port", QString::fromStdString(dev.port()), "--no-session",
                      "--channels", "1:STIM,2:REACT", "--samples", "40", "--trigger", "1:rising",
                      "--ready-file", ready, "--on-armed", "echo FIRED > " + hookOut,
                      "--measure-latency", "1->2", "--out-events-csv", evCsv,
                      "--expect", "events==1,p99_ns<=2500,drops==0", "--json"},
                     &j, &raw);
        QVERIFY2(rc == 0, qPrintable(raw));
        QVERIFY(QFile::exists(ready));
        QVERIFY(QFile::exists(evCsv));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(hookOut), 2000);
        QCOMPARE(j["latency"]["count"].get<int>(), 1);
        QCOMPARE(j["latency"]["p99_ns"].get<double>(), 2000.0);
        QVERIFY(j["assertions"]["passed"].get<bool>());

        // Failing --expect must return exit code 3 (ExitExpectFailed).
        rc = run({"capture", "--port", QString::fromStdString(dev.port()), "--no-session",
                  "--channels", "1:STIM,2:REACT", "--samples", "40", "--trigger", "1:rising",
                  "--measure-latency", "1..2", "--expect", "p99_ns<=500", "--json"},
                 &j, &raw);
        QCOMPARE(rc, 3);
        QVERIFY(!j["assertions"]["passed"].get<bool>());

        // Offline analyze command converts/verifies the saved JSON without touching hardware.
        const QString savedJson = dir.filePath("cap.json");
        rc = run({"capture", "--port", QString::fromStdString(dev.port()), "--no-session",
                  "--channels", "1:STIM,2:REACT", "--samples", "40", "--trigger", "1:rising",
                  "--meta", "dut.board=PYNQ-Z1", "--meta", "dut.bitstream_sha256=ccf4b15b",
                  "--out-json", savedJson, "--json"},
                 &j, &raw);
        QCOMPARE(rc, 0);
        QCOMPARE(j["setup"]["dut.board"].get<std::string>(), std::string("PYNQ-Z1"));

        // The setup travels with the saved capture; analyze keeps it and lets --meta override a key.
        QString report;
        rc = run({"analyze", savedJson, "--meta", "dut.board=PYNQ-Z1 rev C", "--ai"}, nullptr, &report);
        QCOMPARE(rc, 0);
        QVERIFY2(report.contains("- Setup: dut.bitstream_sha256=ccf4b15b; dut.board=PYNQ-Z1 rev C;"), qPrintable(report));
        rc = run({"analyze", savedJson, "--measure-latency", "1..2", "--expect", "events==1,p99_ns==2000", "--json"},
                 &j, &raw);
        QVERIFY2(rc == 0, qPrintable(raw));
        QCOMPARE(j["command"].get<std::string>(), std::string("analyze"));
        QVERIFY(j["assertions"]["passed"].get<bool>());
        QCOMPARE(j["setup"]["dut.bitstream_sha256"].get<std::string>(), std::string("ccf4b15b"));

        QCOMPARE(run({"analyze", savedJson, "--meta", "bad key=x", "--json"}, &j), 64);
        QCOMPARE(run({"analyze", savedJson, "--meta", "novalue", "--json"}, &j), 64);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int rc = 0;
    {
        ConnectionTests t;
        rc |= QTest::qExec(&t, argc, argv);
    }
    {
        AsyncTests t;
        rc |= QTest::qExec(&t, argc, argv);
    }
    {
        CliTests t;
        rc |= QTest::qExec(&t, argc, argv);
    }
    return rc;
}

#include "test_connection.moc"

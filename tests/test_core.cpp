// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/AiReport.hpp"
#include "core/ArgParsing.hpp"
#include "services/Operations.hpp"
#include "core/CaptureBuffer.hpp"
#include "core/DeviceProtocol.hpp"
#include "core/Exporters.hpp"
#include "core/SignalAnalyzer.hpp"

#include <QByteArray>
#include <QTest>

#include <cstring>
#include <sstream>

namespace {

// Verbatim copies of the firmware structs (non-packed, natural alignment as on ARM EABI).
struct FwRequestV6 {  // firmware/rp2040/LogicAnalyzer_Structs.h
    uint8_t triggerType;
    uint8_t trigger;
    union {
        uint8_t inverted;
        uint8_t count;
    };
    uint16_t triggerValue;
    uint8_t channels[32];
    uint8_t channelCount;
    uint32_t frequency;
    uint32_t preSamples;
    uint32_t postSamples;
    uint16_t loopCount;
    uint8_t measure;
    uint8_t captureMode;
};

struct FwRequestV5 {  // upstream gusmanb/logicanalyzer: Firmware/LogicAnalyzer/LogicAnalyzer_Structs.h (V5)
    uint8_t triggerType;
    uint8_t trigger;
    union {
        uint8_t inverted;
        uint8_t count;
    };
    uint16_t triggerValue;
    uint8_t channels[24];
    uint8_t channelCount;
    uint32_t frequency;
    uint32_t preSamples;
    uint32_t postSamples;
    uint8_t loopCount;
    uint8_t measure;
    uint8_t captureMode;
};

lt::CaptureRequestPacket sampleRequest()
{
    lt::CaptureRequestPacket r;
    r.triggerType = lt::TriggerType::Edge;
    r.triggerChannel = 23;
    r.invertedOrCount = 1;
    r.triggerValue = 0xA55A;
    r.channels[0] = 0;
    r.channels[1] = 1;
    r.channels[2] = 23;
    r.channelCount = 3;
    r.frequency = 1'000'000;
    r.preSamples = 1000;
    r.postSamples = 0x00F0AA55;
    r.loopCount = 7;
    r.measure = 1;
    r.captureMode = lt::CaptureMode::Channels24;
    return r;
}

template <typename Fw>
Fw mirror(const lt::CaptureRequestPacket& r, std::size_t slotCount)
{
    Fw fw;
    std::memset(&fw, 0, sizeof fw);
    fw.triggerType = static_cast<uint8_t>(r.triggerType);
    fw.trigger = r.triggerChannel;
    fw.inverted = r.invertedOrCount;
    fw.triggerValue = r.triggerValue;
    std::memcpy(fw.channels, r.channels.data(), slotCount);
    fw.channelCount = r.channelCount;
    fw.frequency = r.frequency;
    fw.preSamples = r.preSamples;
    fw.postSamples = r.postSamples;
    fw.loopCount = static_cast<decltype(fw.loopCount)>(r.loopCount);
    fw.measure = r.measure;
    fw.captureMode = static_cast<uint8_t>(r.captureMode);
    return fw;
}

// Builds a 1-channel buffer from a string of '0'/'1'.
lt::CaptureBuffer bufferFrom(const std::string& bits, uint32_t freq = 1'000'000, uint32_t pre = 0)
{
    std::vector<uint32_t> s;
    for (char c : bits)
        s.push_back(c == '1' ? 1u : 0u);
    return lt::CaptureBuffer({{0, "SIG"}}, freq, pre, s);
}

}  // namespace

class CoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void requestLayoutMatchesFirmwareV6()
    {
        static_assert(sizeof(FwRequestV6) == 56);
        const auto req = sampleRequest();
        const auto bytes = lt::serialize(req, lt::ProtocolGeneration::V6);
        const auto fw = mirror<FwRequestV6>(req, 32);
        QCOMPARE(bytes.size(), sizeof fw);
        QVERIFY(std::memcmp(bytes.data(), &fw, sizeof fw) == 0);
    }

    void requestLayoutMatchesFirmwareV5()
    {
        static_assert(sizeof(FwRequestV5) == 48);
        const auto req = sampleRequest();
        const auto bytes = lt::serialize(req, lt::ProtocolGeneration::V5);
        const auto fw = mirror<FwRequestV5>(req, 24);
        QCOMPARE(bytes.size(), sizeof fw);
        QVERIFY(std::memcmp(bytes.data(), &fw, sizeof fw) == 0);
    }

    void frameEscaping()
    {
        QCOMPARE(lt::encodeFrame(lt::cmd::Id), (std::vector<uint8_t>{0x55, 0xAA, 0x00, 0xAA, 0x55}));
        const std::vector<uint8_t> payload{0xAA, 0x55, 0xF0, 0x12};
        const auto frame = lt::encodeFrame(lt::cmd::Capture, payload);
        QCOMPARE(frame, (std::vector<uint8_t>{0x55, 0xAA, 0x01, 0xF0, 0x5A, 0xF0, 0xA5, 0xF0, 0x00, 0x12, 0xAA, 0x55}));
        const auto decoded = lt::decodeFrame(frame);
        QVERIFY(decoded);
        QCOMPARE(*decoded, (std::vector<uint8_t>{0x01, 0xAA, 0x55, 0xF0, 0x12}));
    }

    void handshakeParsing()
    {
        const auto v = lt::parseVersionLine("LOGIC_ANALYZER_PICO_2_V6_5");
        QVERIFY(v);
        QCOMPARE(v->board, std::string("PICO_2"));
        QCOMPARE(v->major, 6);
        QCOMPARE(v->minor, 5);
        QVERIFY(!lt::parseVersionLine("CAPTURE_STARTED"));
        QVERIFY(lt::requestLayoutFor(*v) == lt::ProtocolGeneration::V6);
        QVERIFY(lt::requestLayoutFor(*lt::parseVersionLine("LOGIC_ANALYZER_PICO_V6_0")) == lt::ProtocolGeneration::V5);
        QCOMPARE(lt::parseKeyValue("FREQ:100000000", "FREQ").value_or(0), 100000000u);
        QVERIFY(!lt::parseKeyValue("BLASTFREQ:200000000", "FREQ"));
        QVERIFY(!lt::parseKeyValue("FREQ:12x", "FREQ"));
    }

    void captureModeAndLimits()
    {
        const std::vector<uint8_t> low{0, 7}, mid{3, 15}, high{0, 23};
        QVERIFY(lt::captureModeFor(low) == lt::CaptureMode::Channels8);
        QVERIFY(lt::captureModeFor(mid) == lt::CaptureMode::Channels16);
        QVERIFY(lt::captureModeFor(high) == lt::CaptureMode::Channels24);
        lt::DeviceInfo info;
        info.bufferSize = 131072;
        QCOMPARE(info.limits(lt::CaptureMode::Channels24).maxTotalSamples, 32768u);
        QCOMPARE(info.limits(lt::CaptureMode::Channels8).maxPreSamples, 13107u);
    }

    void rawDecoding24ch()
    {
        const std::vector<uint8_t> raw{0x01, 0x00, 0x80, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
        const auto buf = lt::CaptureBuffer::fromRaw({{0, {}}, {1, {}}, {23, {}}}, 1000, 0,
                                                    lt::CaptureMode::Channels24, raw);
        QCOMPARE(buf.size(), std::size_t{2});
        QCOMPARE(buf.samples()[0], 0x1u);  // masked to 3 channels
        QCOMPARE(buf.samples()[1], 0x7u);
    }

    void analyzerSquareWave()
    {
        // 100 kHz @ 1 MHz: 3 samples high, 7 low, 5 full periods.
        std::string bits = "00";
        for (int i = 0; i < 5; ++i)
            bits += "1110000000";
        bits += "111";
        const auto st = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom(bits), 0);
        QCOMPARE(st.idleLevel, 0);
        QCOMPARE(st.finalLevel, 1);
        QCOMPARE(st.risingEdges, 6u);
        QCOMPARE(st.fallingEdges, 5u);
        QCOMPARE(st.transitions, 11u);
        QCOMPARE(st.pulseCountHigh, 5u);
        QCOMPARE(st.pulseCountLow, 5u);
        QVERIFY(st.meanFrequencyHz);
        QCOMPARE(*st.meanFrequencyHz, 100000.0);
        QCOMPARE(*st.dutyCyclePercent, 30.0);
        QCOMPARE(st.highWidth->minSamples, 3u);
        QCOMPARE(st.lowWidth->maxSamples, 7u);
        QCOMPARE(st.glitchCount, 0u);
    }

    void analyzerGlitchAndStableLines()
    {
        const auto glitch = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom("1111011111"), 0);
        QCOMPARE(glitch.glitchCount, 1u);
        QCOMPARE(glitch.pulseCountLow, 1u);
        QCOMPARE(glitch.pulseCountHigh, 0u);  // outer high segments are incomplete

        const auto high = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom("1111"), 0);
        QVERIFY(lt::SignalAnalyzer::diagnose(high).state == lt::LineState::High);
        const auto low = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom("0000"), 0);
        QVERIFY(lt::SignalAnalyzer::diagnose(low).state == lt::LineState::Low);
        QVERIFY(!low.meanFrequencyHz);
    }

    void edgeTimesRelativeToTrigger()
    {
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        const auto st = lt::SignalAnalyzer(opts).analyzeChannel(bufferFrom("0011", 1'000'000, 1), 0);
        QCOMPARE(st.edges, std::vector<uint32_t>{2});
        const auto j = lt::toJson(st, true);
        QCOMPARE(j["edge_times_us"][0].get<double>(), 1.0);
        QCOMPARE(j["first_edge_us"].get<double>(), 1.0);
    }

    void vcdExport()
    {
        std::ostringstream os;
        lt::writeVcd(os, bufferFrom("0110", 1'000'000));
        const auto vcd = os.str();
        QVERIFY(vcd.find("$timescale 1 ns $end") != std::string::npos);
        QVERIFY(vcd.find("$var wire 1 ! SIG $end") != std::string::npos);
        QVERIFY(vcd.find("#1000\n1!\n#3000\n0!\n#4000\n") != std::string::npos);
    }

    void parsingRejectsOverflowAndGarbage()
    {
        QVERIFY(!lt::parseVersionLine("LOGIC_ANALYZER_PICO_V999999999999_0"));
        QVERIFY(!lt::parseKeyValue("FREQ:4294967296", "FREQ"));
        QVERIFY(!lt::parseKeyValue("FREQ:", "FREQ"));
    }

    void decodeFrameRejectsMalformedFrames()
    {
        QVERIFY(!lt::decodeFrame(std::vector<uint8_t>{0x55, 0xAA, 0xAA, 0x55}));        // too short
        QVERIFY(!lt::decodeFrame(std::vector<uint8_t>{0x00, 0xAA, 0x01, 0xAA, 0x55}));  // bad header
        QVERIFY(!lt::decodeFrame(std::vector<uint8_t>{0x55, 0xAA, 0x01, 0x55, 0x55}));  // bad trailer
        QVERIFY(!lt::decodeFrame(std::vector<uint8_t>{0x55, 0xAA, 0xF0, 0xAA, 0x55}));  // dangling escape
    }

    void rawDecoding8And16ch()
    {
        const auto b8 = lt::CaptureBuffer::fromRaw({{0, {}}, {1, {}}, {2, {}}}, 1000, 0,
                                                   lt::CaptureMode::Channels8, std::vector<uint8_t>{0x05, 0xFB});
        QCOMPARE(b8.samples(), (std::vector<uint32_t>{0x5, 0x3}));
        const auto b16 = lt::CaptureBuffer::fromRaw({{0, {}}, {15, {}}}, 1000, 0, lt::CaptureMode::Channels16,
                                                    std::vector<uint8_t>{0x01, 0x80, 0xFF, 0xFF, 0x42});
        QCOMPARE(b16.samples(), (std::vector<uint32_t>{0x1, 0x3}));  // masked; the dangling byte is ignored
    }

    void dropFrontClampsToBuffer()
    {
        const auto buf = bufferFrom("010101", 1000, 3);
        const auto d = buf.dropFront(2);
        QCOMPARE(d.size(), std::size_t{4});
        QCOMPARE(d.preSamples(), 1u);
        QCOMPARE(buf.dropFront(5).preSamples(), 0u);
        QCOMPARE(buf.dropFront(100).size(), std::size_t{0});
    }

    void analyzerEdgeCases()
    {
        const lt::SignalAnalyzer an;
        const auto empty = an.analyzeChannel(bufferFrom(""), 0);
        QCOMPARE(empty.transitions, 0u);
        QVERIFY(!empty.firstEdgeTimeS);

        const auto one = an.analyzeChannel(bufferFrom("1"), 0);
        QCOMPARE(one.idleLevel, 1);
        QCOMPARE(one.highTimePercent, 100.0);

        const auto step = an.analyzeChannel(bufferFrom("0011"), 0);
        QCOMPARE(step.transitions, 1u);
        QVERIFY(!step.meanFrequencyHz);  // a single edge defines no period
        QCOMPARE(step.pulseCountHigh + step.pulseCountLow, 0u);

        // Only one rising edge: frequency and duty come from the falling edges.
        const auto fall = an.analyzeChannel(bufferFrom("10100"), 0);
        QCOMPARE(fall.risingEdges, 1u);
        QCOMPARE(fall.fallingEdges, 2u);
        QCOMPARE(*fall.meanFrequencyHz, 500000.0);
        QCOMPARE(*fall.dutyCyclePercent, 50.0);

        // A channel the buffer does not have must not read outside the sample word.
        const auto bogus = an.analyzeChannel(bufferFrom("0101"), 5);
        QCOMPARE(bogus.transitions, 0u);
        QVERIFY(!bogus.firstEdgeTimeS);
        QCOMPARE(an.analyzeChannel(bufferFrom("0101"), 40).transitions, 0u);
    }

    void noisyLineIsClassified()
    {
        // Irregular single-sample glitches on an otherwise low line -> Noisy.
        const auto noisy = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom("0001000100100001000"), 0);
        QVERIFY(lt::SignalAnalyzer::diagnose(noisy).state == lt::LineState::Noisy);

        // Regular alternating samples (f_signal == f_sample / 2) -> Nyquist toggling, not floating noise.
        const auto nyquist = lt::SignalAnalyzer{}.analyzeChannel(bufferFrom("0101010101"), 0);
        const auto diag = lt::SignalAnalyzer::diagnose(nyquist);
        QVERIFY(diag.state == lt::LineState::Toggling);
        QCOMPARE(diag.code, std::string_view("nyquist_limit_toggling"));
    }

    void gpioMapAndFirmwarePollDefaults()
    {
        QCOMPARE(lt::gpioForChannel("PICO", 0).value_or(255), uint8_t{2});
        QCOMPARE(lt::gpioForChannel("PICO", 23).value_or(255), uint8_t{28});  // CH24 = GP28 (Pin 34)
        QCOMPARE(lt::gpioForChannel("PICO", 24).value_or(255), uint8_t{1});   // CH25 ext trigger = GP1
        QCOMPARE(lt::gpioForChannel("INTERCEPTOR", 27).value_or(255), uint8_t{5});
        QVERIFY(!lt::gpioForChannel("UNKNOWN_BOARD", 0));

        const auto v65 = lt::parseVersionLine("LOGIC_ANALYZER_PICO_V6_5");
        const auto v66 = lt::parseVersionLine("LOGIC_ANALYZER_PICO_V6_6");
        QVERIFY(v65 && v66);
        QCOMPARE(lt::defaultPollLatencyFor(*v65).count(), 2200);
        QCOMPARE(lt::defaultPollLatencyFor(*v66).count(), 150);
    }

    void crossChannelLatencyAndExpectations()
    {
        // Emulate AV-10 100-burst @ 100 MHz (10 ns step): 94 events with 20 ns latency, 6 with 10 ns.
        std::vector<uint32_t> samples(2500, 0u);
        for (int ev = 0; ev < 100; ++ev) {
            const std::size_t t0 = 20 + static_cast<std::size_t>(ev) * 20;
            const std::size_t delay = (ev < 6) ? 1 : 2;
            for (std::size_t w = 0; w < 5; ++w)
                samples[t0 + w] |= 1u;  // CH1 STIM pulse (50 ns)
            for (std::size_t w = 0; w < 5; ++w)
                samples[t0 + delay + w] |= 2u;  // CH2 REACT pulse
        }
        const lt::CaptureBuffer buf({{0, "STIM"}, {1, "REACT"}}, 100'000'000, 10, samples);
        const auto stats = lt::SignalAnalyzer{}.analyze(buf);
        const auto lat = lt::SignalAnalyzer::analyzeLatency(buf, {0, false, 1, false, std::nullopt}, true);

        QCOMPARE(lat.count, uint64_t{100});
        QCOMPARE(lat.unmatchedStimCount, uint64_t{0});
        QCOMPARE(lat.minNs, 10.0);
        QCOMPARE(lat.maxNs, 20.0);
        QCOMPARE(lat.p50Ns, 20.0);
        QCOMPARE(lat.p99Ns, 20.0);
        QVERIFY(std::abs(lat.meanNs - 19.4) < 1e-9);
        QCOMPARE(lat.events.size(), std::size_t{100});

        // Unmatched stimulus (fail-closed gate suppressed reaction #1) must increment drops without misaligning #2.
        const lt::CaptureBuffer withDrop({{0, "STIM"}, {1, "REACT"}}, 1'000'000, 0,
                                         {0, 1, 0, 0, 1, 2, 0});
        const auto dropLat = lt::SignalAnalyzer::analyzeLatency(withDrop, {0, false, 1, false, std::nullopt}, true);
        QCOMPARE(dropLat.count, uint64_t{1});
        QCOMPARE(dropLat.unmatchedStimCount, uint64_t{1});
        QCOMPARE(dropLat.minNs, 1000.0);

        std::string err;
        const auto passExpr =
            lt::cli::parseExpectations("p99_ns<=60,drops==0,STIM:high_pulses==100;REACT:glitches==0", &err);
        QVERIFY2(passExpr, err.c_str());
        for (const auto& c : lt::cli::evaluateExpectations(*passExpr, stats, lat))
            QVERIFY2(c.passed, c.detail.c_str());

        const auto failExpr = lt::cli::parseExpectations("p99_ns<=15", &err);
        QVERIFY(failExpr);
        const auto failRes = lt::cli::evaluateExpectations(*failExpr, stats, lat);
        QCOMPARE(failRes.size(), std::size_t{1});
        QVERIFY(!failRes.front().passed);
    }

    void losslessJsonCsvAndVcdRoundTrip()
    {
        const auto orig = twoChannelBuffer();
        lt::SignalAnalyzer::Options opts;
        opts.collectEdges = true;
        const auto stats = lt::SignalAnalyzer(opts).analyze(orig);
        auto doc = lt::captureToJson(orig, stats, true, "PICO");
        doc["trigger_description"] = "CH1 rising";
        doc["annotations"] = nlohmann::json::array({{{"id", 3}, {"time_us", 2.0}, {"label", "edge"}, {"color", "red"}, {"author", "ai"}}});
        QCOMPARE(doc["schema_version"].get<int>(), 1);
        QCOMPARE(doc["channels"][0]["gpio"].get<int>(), 2);

        lt::LoadedCapture fromJson;
        std::string err;
        QVERIFY2(lt::readCaptureJson(doc, fromJson, &err), err.c_str());
        QCOMPARE(fromJson.buffer.samples(), orig.samples());
        QCOMPARE(fromJson.buffer.triggerIndex(), orig.triggerIndex());
        QCOMPARE(fromJson.buffer.frequency(), orig.frequency());
        QCOMPARE(fromJson.triggerDescription, std::string("CH1 rising"));
        QCOMPARE(fromJson.annotations.size(), std::size_t{1});

        std::ostringstream vcdStream;
        lt::writeVcd(vcdStream, orig);
        std::istringstream vcdIn(vcdStream.str());
        lt::LoadedCapture fromVcd;
        QVERIFY2(lt::readVcd(vcdIn, fromVcd, &err), err.c_str());
        QCOMPARE(fromVcd.buffer.samples(), orig.samples());
        QCOMPARE(fromVcd.buffer.triggerIndex(), orig.triggerIndex());
        QCOMPARE(fromVcd.buffer.frequency(), orig.frequency());

        // External multi-line VCD (as written by pico_capture.py / Yosys).
        std::istringstream extVcd("$timescale\n 10ns\n$end\n$var wire 1 ! STIM $end\n$enddefinitions $end\n"
                                  "$dumpvars\n0!\n$end\n#0\n0!\n#2\n1!\n#4\n0!\n");
        lt::LoadedCapture extLoaded;
        QVERIFY2(lt::readVcd(extVcd, extLoaded, &err), err.c_str());
        QCOMPARE(extLoaded.buffer.size(), std::size_t{2});
        QCOMPARE(extLoaded.buffer.bit(0, 0), false);
        QCOMPARE(extLoaded.buffer.bit(1, 0), true);

        std::ostringstream csv;
        lt::writeCsv(csv, orig);
        QVERIFY(csv.str().find("SampleIndex,TimeRelativeNs,A,B\n0,-2000,0,1\n") != std::string::npos);

        // Output path sandbox checks (A-04).
        static constexpr std::string_view kVcd[]{".vcd"};
        QVERIFY(!lt::resolveSafeOutputPath("../escape.vcd", kVcd, "/tmp", &err));
        QVERIFY(!lt::resolveSafeOutputPath("/tmp/.hidden.vcd", kVcd, "/tmp", &err));
        QVERIFY(!lt::resolveSafeOutputPath("/tmp/script.sh", kVcd, "/tmp", &err));
        QVERIFY(!lt::resolveSafeOutputPath("/etc/hosts.vcd", kVcd, "/tmp", &err));
        QVERIFY(lt::resolveSafeOutputPath("/tmp/valid_out.vcd", kVcd, "/tmp", &err).has_value());

        // Shell-safe '..' separator in parseLatencyArg and legacy .lac import.
        const auto latSpec = lt::cli::parseLatencyArg("1:rising..2:falling", 24, &err);
        QVERIFY2(latSpec, err.c_str());
        QCOMPARE(latSpec->stimChannel, uint8_t{0});
        QVERIFY(!latSpec->stimFalling);
        QCOMPARE(latSpec->reactChannel, uint8_t{1});
        QVERIFY(latSpec->reactFalling);

        const nlohmann::json lacDoc = {
            {"Settings",
             {{"Frequency", 100'000'000},
              {"PreTriggerSamples", 2},
              {"TriggerType", 0},
              {"TriggerChannel", 0},
              {"TriggerInverted", true},
              {"CaptureChannels",
               nlohmann::json::array({{{"ChannelNumber", 0},
                                       {"ChannelName", "STIM"},
                                       {"Samples", std::string(QByteArray("\x00\x00\x01\x01\x00", 5).toBase64().constData())}}})}}}};
        lt::LoadedCapture fromLac;
        QVERIFY2(lt::readCaptureJson(lacDoc, fromLac, &err), err.c_str());
        QCOMPARE(fromLac.buffer.size(), std::size_t{5});
        QCOMPARE(fromLac.buffer.triggerIndex(), 2);
        QCOMPARE(fromLac.buffer.bit(2, 0), true);
        QCOMPARE(fromLac.triggerDescription, std::string("CH1 falling"));
    }

    void vcdSanitizesNamesAndUsesPicoseconds()
    {
        std::ostringstream os;
        lt::writeVcd(os, lt::CaptureBuffer({{0, "A B$\nC"}}, 3'000'000, 0, {0, 1}));
        const auto vcd = os.str();
        QVERIFY(vcd.find("$timescale 1 ps $end") != std::string::npos);  // 3 MHz is not a whole number of ns
        QVERIFY(vcd.find("$var wire 1 ! A_B__C $end") != std::string::npos);
        QVERIFY(vcd.find("#333333\n1!\n") != std::string::npos);
    }

    void limitsSurviveTinyBuffers()
    {
        lt::DeviceInfo info;
        info.bufferSize = 3;
        const auto dead = info.limits(lt::CaptureMode::Channels24);  // 0 samples fit
        QCOMPARE(dead.maxPostSamples, 0u);
        QCOMPARE(dead.maxTotalSamples, 0u);
        info.bufferSize = 8;
        const auto tiny = info.limits(lt::CaptureMode::Channels8);
        QCOMPARE(tiny.maxPreSamples, 2u);
        QCOMPARE(tiny.maxPostSamples, 6u);
        QCOMPARE(tiny.maxTotalSamples, 8u);
    }

    static lt::CaptureBuffer twoChannelBuffer()
    {
        // A: 0011110000, B: constant 1; 1 MHz, trigger at sample 2.
        std::vector<uint32_t> s;
        for (char c : std::string("0011110000"))
            s.push_back(2u | (c == '1' ? 1u : 0u));
        return lt::CaptureBuffer({{0, "A"}, {1, "B"}}, 1'000'000, 2, s);
    }

    void aiReportAsciiTimeline()
    {
        lt::ReportOptions o;
        o.timelineColumns = 10;
        o.cursorA = 2;
        o.cursorB = 6;
        const auto text = lt::asciiTimeline(twoChannelBuffer(), o);
        QVERIFY(text.find("     |  A   B   |\n") != std::string::npos);  // cursor markers over their columns
        QVERIFY(text.find("A    |__####____|\n") != std::string::npos);
        QVERIFY(text.find("B    |##########|\n") != std::string::npos);
    }

    void aiReportMarkdownMeasuresBetweenCursors()
    {
        lt::ReportOptions o;
        o.cursorA = 2;
        o.cursorB = 6;
        o.triggerDescription = "CH1 rising";
        const auto md = lt::markdownSummary(twoChannelBuffer(), o);
        QVERIFY(md.find("CH1 rising, t = 0 at sample 2") != std::string::npos);
        QVERIFY(md.find("Δt = 4 us (4 samples)") != std::string::npos);
        QVERIFY(md.find("1/Δt = 250 kHz") != std::string::npos);
        QVERIFY(md.find("| 1 | A | 0 | 0 | 2 (1/1) |") != std::string::npos);
        QVERIFY(md.find("B: no activity, constant HIGH") != std::string::npos);
        QVERIFY(md.find("| 1 | A | 1 | 0 | 1 |") != std::string::npos);  // between cursors: level at A, at B, edges

        const auto meta = lt::jsonMetadata(twoChannelBuffer(), o);
        QVERIFY(meta["cursors"].contains("between"));
        QCOMPARE(meta["cursors"]["between"][0]["edges_between"].get<int>(), 1);
    }

    void aiReportJsonUsesTriggerRelativeTimesForAnyRange()
    {
        lt::ReportOptions o;
        o.first = 1;  // the visible range starts after sample 0 and excludes the tail
        o.last = 8;
        const auto j = lt::jsonMetadata(twoChannelBuffer(), o);
        QCOMPARE(j["visible"]["first_sample"].get<int>(), 1);
        QCOMPARE(j["visible"]["start_us"].get<double>(), -1.0);
        const auto& a = j["channels"][0];
        QCOMPARE(a["initial_level"].get<int>(), 0);
        QCOMPARE(a["edge_times_us"].size(), std::size_t{2});
        QCOMPARE(a["edge_times_us"][0].get<double>(), 0.0);  // sample 2 is the trigger
        QCOMPARE(a["edge_times_us"][1].get<double>(), 4.0);
        QVERIFY(!a["edges_truncated"].get<bool>());

        o.maxEdgesPerChannel = 1;
        QVERIFY(lt::jsonMetadata(twoChannelBuffer(), o)["channels"][0]["edges_truncated"].get<bool>());
    }

    void aiReportSurvivesEmptyRange()
    {
        lt::ReportOptions o;
        o.first = 50;
        o.last = 60;
        QVERIFY(!lt::copyForAi(twoChannelBuffer(), o).empty());
        QCOMPARE(lt::asciiTimeline(twoChannelBuffer(), o), std::string("(empty range)\n"));
        QVERIFY(lt::copyForAi(lt::CaptureBuffer{}, {}).find("```json") != std::string::npos);
    }

    void aiReportIncludesAnnotations()
    {
        lt::ReportOptions o;
        o.timelineColumns = 10;
        lt::Annotation a;
        a.id = 1;
        a.timeSeconds = 4e-6;  // sample 6 on the 1 MHz buffer whose trigger is at sample 2
        a.label = "ECHO falls";
        a.author = "ai";
        lt::Annotation outside = a;
        outside.id = 2;
        outside.timeSeconds = 1.0;
        outside.label = "far away";
        o.annotations = {a, outside};

        const auto md = lt::markdownSummary(twoChannelBuffer(), o);
        QVERIFY(md.find("Annotations (2 total, 1 in the visible range)") != std::string::npos);
        QVERIFY(md.find("#1 t = 4 us [ai] \"ECHO falls\"\n") != std::string::npos);
        QVERIFY(md.find("#2 t = 1 s [ai] \"far away\" (outside the visible range)") != std::string::npos);

        const auto ascii = lt::asciiTimeline(twoChannelBuffer(), o);
        QVERIFY(ascii.find("note |      1   |\n") != std::string::npos);
        QVERIFY(ascii.find("[1] t = 4 us [ai] ECHO falls") != std::string::npos);

        const auto j = lt::jsonMetadata(twoChannelBuffer(), o);
        QCOMPARE(j["annotations"].size(), std::size_t{2});
        QVERIFY(j["annotations"][0]["visible"].get<bool>());
        QVERIFY(!j["annotations"][1]["visible"].get<bool>());
        QCOMPARE(j["annotations"][0]["time_us"].get<double>(), 4.0);
    }

    void timeParsingRequiresAUnit()
    {
        using lt::cli::parseTimeSeconds;
        QCOMPARE(*parseTimeSeconds("50.12ms"), 0.05012);
        QCOMPARE(*parseTimeSeconds("200us"), 200e-6);
        QCOMPARE(*parseTimeSeconds("-20us"), -20e-6);
        QCOMPARE(*parseTimeSeconds("1.5s"), 1.5);
        QCOMPARE(*parseTimeSeconds("250ns"), 250e-9);
        QCOMPARE(*parseTimeSeconds("0"), 0.0);
        QVERIFY(!parseTimeSeconds("5"));     // ambiguous without a unit
        QVERIFY(!parseTimeSeconds("10m"));
        QVERIFY(!parseTimeSeconds("ms"));
        QVERIFY(!parseTimeSeconds("1e999s"));
    }

    void requestParametersFromJson()
    {
        std::string err;
        auto spec = lt::captureSpecFromJson({{"channels", nlohmann::json::array({1, "2:B"})},
                                             {"freq", "500k"},
                                             {"samples", 1000},
                                             {"trigger", "25:falling"},
                                             {"timeout_ms", 250}},
                                            24, err);
        QVERIFY2(spec, err.c_str());
        QCOMPARE(spec->channels.size(), std::size_t{2});
        QCOMPARE(spec->channels[1].name, std::string("B"));
        QCOMPARE(spec->frequency, 500'000u);
        QCOMPARE(spec->samples, uint64_t{1000});
        QCOMPARE(spec->trigger->channel, uint8_t{24});  // 25 = the external trigger input
        QVERIFY(spec->trigger->inverted);
        QCOMPARE(spec->timeout.count(), 250);

        spec = lt::captureSpecFromJson({}, 24, err);  // defaults: CH1, immediate
        QVERIFY(spec && spec->channels.size() == 1 && !spec->trigger);

        for (const nlohmann::json& bad : {nlohmann::json{{"samples", 2}}, nlohmann::json{{"freq", 0}},
                                         nlohmann::json{{"channels", "25"}}, nlohmann::json{{"trigger", "26:rising"}},
                                         nlohmann::json{{"samples", 100}, {"pre_samples", 100}},
                                         nlohmann::json{{"channels", 3.5}}, nlohmann::json{{"timeout_ms", -1}}})
            QVERIFY2(!lt::captureSpecFromJson(bad, 24, err), bad.dump().c_str());

        const auto probe = lt::probeParamsFromJson({{"window_ms", 5}, {"rate", "2M"}}, 24, err);
        QVERIFY(probe && probe->channels.empty() && probe->window.count() == 5 && probe->rate == 2'000'000u);
        QVERIFY(!lt::probeParamsFromJson({{"window_ms", 0}}, 24, err));
    }

    void captureConfigAppliesDeviceLimits()
    {
        lt::DeviceInfo info;
        lt::CaptureRequestSpec spec;
        spec.channels = {{0, {}}, {15, {}}};  // 16-channel mode: 65536 samples
        std::vector<std::string> warnings;
        std::string err;

        spec.samples = 65536;
        QVERIFY(!lt::buildCaptureConfig(info, spec, warnings, err));  // immediate keeps 2 pre-trigger samples
        QVERIFY(err.find("65534") != std::string::npos);

        spec.samples = 1000;
        spec.preSamples = 5;
        auto cfg = lt::buildCaptureConfig(info, spec, warnings, err);
        QVERIFY(cfg && cfg->preSamples == 2 && cfg->postSamples == 1000);
        QCOMPARE(warnings.size(), std::size_t{1});  // pre-samples are ignored for immediate captures

        spec.trigger = lt::EdgeTrigger{0, false};
        spec.preSamples.reset();
        cfg = lt::buildCaptureConfig(info, spec, warnings, err);
        QVERIFY(cfg && cfg->preSamples == 100 && cfg->postSamples == 900);  // default: 10% before the trigger
        spec.preSamples = 4000;
        QVERIFY(!lt::buildCaptureConfig(info, spec, warnings, err));
    }

    void cliQuantities()
    {
        using lt::cli::parseQuantity;
        QCOMPARE(parseQuantity("1M").value_or(0), 1'000'000u);
        QCOMPARE(parseQuantity("2.5MHz").value_or(0), 2'500'000u);
        QCOMPARE(parseQuantity("500k").value_or(0), 500'000u);
        QCOMPARE(parseQuantity("100000").value_or(0), 100'000u);
        QVERIFY(!parseQuantity("1.5"));
        QVERIFY(!parseQuantity("abc"));
        QVERIFY(!parseQuantity("-1k"));
        QVERIFY(!parseQuantity("10ms"));  // time suffixes must not be read as "10 M"
        QVERIFY(!parseQuantity("5s"));
        QCOMPARE(parseQuantity("1Msps").value_or(0), 1'000'000u);
    }

    void cliChannelsAndTrigger()
    {
        std::string err;
        const auto ch = lt::cli::parseChannelList("1:STIM,2:REACT,24:JETSON_DO1", 24, &err);
        QVERIFY(ch);
        QCOMPARE(ch->size(), std::size_t{3});
        QCOMPARE((*ch)[2].number, uint8_t{23});
        QCOMPARE((*ch)[2].name, std::string("JETSON_DO1"));
        QCOMPARE(lt::cli::parseChannelList("1-8", 24, &err)->size(), std::size_t{8});
        QVERIFY(!lt::cli::parseChannelList("0", 24, &err));
        QVERIFY(!lt::cli::parseChannelList("1,1", 24, &err));

        const auto t = lt::cli::parseTrigger("24:falling", 24, &err);
        QVERIFY(t && !t->none);
        QCOMPARE(t->trigger.channel, uint8_t{23});
        QVERIFY(t->trigger.inverted);
        QVERIFY(lt::cli::parseTrigger("none", 24, &err)->none);
        QVERIFY(!lt::cli::parseTrigger("25:rising", 24, &err));
        QVERIFY(!lt::cli::parseTrigger("3:sideways", 24, &err));
    }

    void aiReportModesLatencyGpioAndCausalEdges()
    {
        // 100 MHz (10 ns step), CH1=STIM (GP2), CH24=DO1 (GP28).
        // Sample 2: CH1 rises; Sample 4 (+20 ns): CH24 falls; Sample 8: CH1 falls; Sample 10 (+20 ns): CH24 rises.
        std::vector<uint32_t> raw(12, 0x2u);  // CH24 idle HIGH (bit 1), CH1 idle LOW (bit 0)
        for (std::size_t i = 2; i < 8; ++i)
            raw[i] |= 0x1u;
        for (std::size_t i = 4; i < 10; ++i)
            raw[i] &= ~0x2u;
        lt::CaptureBuffer buf({{0, "STIM"}, {23, "DO1"}}, 100'000'000, 2, raw);

        lt::ReportOptions o;
        o.mode = lt::CopyMode::Brief;
        o.deviceVersion = "LOGIC_ANALYZER_PICO_V6_6";
        o.userNote = "J501 DO1 -> GP28 via 4.7k pull-up; verify 20 ns delay";
        o.triggerDescription = "CH1 rising";
        o.latency = lt::LatencyPairSpec{0, false, 1, true, std::nullopt};  // STIM rising -> DO1 falling
        o.cursorA = 2;
        o.cursorB = 5;

        const auto brief = lt::copyForAi(buf, o);
        QVERIFY(brief.find("> **Context / Question:** J501 DO1 -> GP28 via 4.7k pull-up") != std::string::npos);
        QVERIFY(brief.find("GPIO map: CH1=GP2, CH24=GP28") != std::string::npos);
        QVERIFY(brief.find("quantization ±1 sample (±10 ns)") != std::string::npos);
        QVERIFY(brief.find("| GP28 |") != std::string::npos);
        QVERIFY(brief.find("### Propagation latency: CH1 (STIM rising) -> CH24 (DO1 falling)") != std::string::npos);
        QVERIFY(brief.find("p50 2 (20.00 ns)") != std::string::npos);
        QVERIFY(brief.find("Δs histogram (samples×count): **2×1**") != std::string::npos);
        QVERIFY(brief.find("### Chronological transitions (4 events)") != std::string::npos);
        QVERIFY(brief.find("+2 (20 ns) | CH24 (GP28) DO1 | 1 -> 0 (falling)") != std::string::npos);
        QCOMPARE(brief.find("```json"), std::string::npos);  // Brief mode omits bulky JSON

        // CursorsOnly clamps to [cursorA..cursorB] (+1 pre-edge baseline sample because sample 2 is an edge).
        o.mode = lt::CopyMode::CursorsOnly;
        const auto ab = lt::copyForAi(buf, o);
        QVERIFY(ab.find("Cursor range (A..B): samples [1, 6)") != std::string::npos);
        QVERIFY(ab.find("```json") != std::string::npos);
        const auto meta = lt::jsonMetadata(buf, o);
        QVERIFY(meta["visible"]["clamped_to_cursors"].get<bool>());
        QCOMPARE(meta["visible"]["samples"].get<int>(), 5);
        QCOMPARE(meta["channels"][1]["gpio"].get<std::string>(), std::string("GP28"));
        QCOMPARE(meta["latency"]["p50_ns"].get<double>(), 20.0);
        QCOMPARE(meta["chronological_edges"]["total"].get<int>(), 2);  // sample 2 rising + sample 4 falling
    }

    // Two 1 MHz bursts of 50 periods separated by a 10 ms gap: frequency must describe the bursts, not
    // be averaged across the gap (the old span-based formula reported ~9.8 kHz).
    void burstyChannelUsesIntraBurstFrequency()
    {
        const std::size_t rate = 10'000'000, burstLen = 500, gap = 100'000;
        std::vector<uint32_t> s(burstLen * 2 + gap, 0u);
        for (std::size_t b : {std::size_t{0}, burstLen + gap})
            for (std::size_t i = 0; i < burstLen; ++i)
                s[b + i] = ((i % 10) < 3) ? 1u : 0u;  // 10-sample period, 30 % duty
        const auto st = lt::SignalAnalyzer{}.analyzeChannel(lt::CaptureBuffer({{0, "CLK"}}, rate, 0, s), 0);
        QVERIFY(st.period);
        QVERIFY(st.period->kind == lt::PeriodKind::Bursty);
        QCOMPARE(st.period->burstCount, 2u);
        QCOMPARE(st.period->medianSamples, 10u);
        QCOMPARE(*st.meanFrequencyHz, 1'000'000.0);
        QCOMPARE(*st.dutyCyclePercent, 30.0);
        QCOMPARE(st.period->irregularCount, uint64_t{0});

        std::string err;
        const auto ex = lt::cli::parseExpectations("CLK:bursts==2,CLK:freq_hz==1000000,CLK:irregular_periods==0", &err);
        QVERIFY2(ex, err.c_str());
        for (const auto& c : lt::cli::evaluateExpectations(*ex, {st}, std::nullopt))
            QVERIFY2(c.passed, c.detail.c_str());
    }

    // Border handling, spurious reactions, histogram, guard bands and the constant-delay test.
    void latencyCensoringSpuriousAndBounds()
    {
        // Pairs at delays 2,2,3 samples; a REACT at sample 1 answers a stimulus before the capture;
        // a doubled REACT after pair #2; a final STIM 2 samples before the end of the capture.
        std::vector<uint32_t> s(80, 0u);
        auto pulse = [&](std::size_t at, uint32_t bit) { s[at] |= bit; s[at + 1] |= bit; };
        pulse(1, 2);                      // censored REACT (before the first STIM, inside the window)
        pulse(10, 1); pulse(12, 2);       // Δ2
        pulse(30, 1); pulse(32, 2);       // Δ2
        pulse(40, 2);                     // spurious second REACT
        pulse(50, 1); pulse(53, 2);       // Δ3
        pulse(77, 1);                     // censored STIM: its reaction cannot fit in the window
        const lt::CaptureBuffer buf({{0, "STIM"}, {1, "REACT"}}, 100'000'000, 0, s);
        const auto l = lt::SignalAnalyzer::analyzeLatency(buf, {0, false, 1, false, std::nullopt});
        QCOMPARE(l.count, uint64_t{3});
        QCOMPARE(l.unmatchedStimCount, uint64_t{0});
        QCOMPARE(l.censoredStimCount, uint64_t{1});
        QCOMPARE(l.censoredReactCount, uint64_t{1});
        QCOMPARE(l.spuriousReactCount, uint64_t{1});
        QCOMPARE(l.spuriousSamples.front(), 40u);
        QCOMPARE(l.maxReactsPerStim, 2u);
        QCOMPARE(l.histogram.size(), std::size_t{2});
        QCOMPARE(l.histogram[0], (std::pair<uint32_t, uint64_t>{2, 2}));
        QCOMPARE(l.p50Samples, 2u);  // nearest rank: ceil(0.5 * 3) = 2nd value
        QCOMPARE(l.p99Samples, 3u);
        QCOMPARE(l.upperBoundNs(l.maxSamples), 40.0);
        QVERIFY(l.constantDelayConsistent());  // 2..3 samples fits one delay in (20, 30) ns
        QCOMPARE(l.constantDelayLowerNs(), 20.0);
        QCOMPARE(l.constantDelayUpperNs(), 30.0);

        std::string err;
        const auto ex = lt::cli::parseExpectations(
            "drops==0,spurious==1,censored==2,max_ns<=35,max_ub_ns<=35,p50_lb_ns>=10", &err);
        QVERIFY2(ex, err.c_str());
        const auto res = lt::cli::evaluateExpectations(*ex, {}, l);
        QVERIFY(res[0].passed && res[1].passed && res[2].passed && res[5].passed);
        QVERIFY(res[3].passed);  // point estimate 30 ns passes...
        QVERIFY(res[3].detail.find("INCONCLUSIVE") != std::string::npos);  // ...but cannot be guaranteed
        QVERIFY(!res[4].passed);  // the guaranteed bound (40 ns) fails
    }

    // 1000 STIM->REACT pairs with one 35-sample outlier at pair #742 and one dropped reaction at #911:
    // the Brief report must spotlight both even though they are far beyond the first 32 transitions.
    void anomalySpotlightFindsPulse742()
    {
        const std::size_t period = 500;
        std::vector<uint32_t> s(1000 * period + 100, 0u);
        for (std::size_t k = 0; k < 1000; ++k) {
            const std::size_t t0 = 50 + k * period;
            for (std::size_t w = 0; w < 200; ++w)
                s[t0 + w] |= 1u;
            if (k == 910)
                continue;  // pair #911: no reaction
            const std::size_t d = k == 741 ? 35 : 2;
            for (std::size_t w = 0; w < 200; ++w)
                s[t0 + d + w] |= 2u;
        }
        const lt::CaptureBuffer buf({{0, "STIM"}, {1, "REACT"}}, 100'000'000, 50, s);
        lt::ReportOptions o;
        o.mode = lt::CopyMode::Brief;
        o.latency = lt::LatencyPairSpec{0, false, 1, false, std::nullopt};
        const auto md = lt::copyForAi(buf, o);

        QVERIFY(md.find("repeating motif ×") != std::string::npos);
        QVERIFY(md.find("`STIM↑ +2 REACT↑") != std::string::npos);
        QVERIFY(md.find("Chronological transitions (first") == std::string::npos);  // no first-N dump
        const std::string stim742 = std::to_string(50 + 741 * period);
        const std::string stim911 = std::to_string(50 + 910 * period);
        QVERIFY(md.find("DroppedStimulus") != std::string::npos);
        QVERIFY(md.find("at sample " + stim911) != std::string::npos);
        QVERIFY(md.find("LatencyOutlier") != std::string::npos);
        QVERIFY(md.find("at sample " + stim742) != std::string::npos);
        QVERIFY(md.find("pair #742: delay 35 samples") != std::string::npos);
        QVERIFY(md.find("drops: **1**") != std::string::npos);
        QVERIFY(md.find("| +35 |") != std::string::npos);  // micro-window shows REACT 35 samples after STIM
        QVERIFY(md.size() < 4000);

        const auto meta = lt::jsonMetadata(buf, o);
        QVERIFY(meta["anomalies_total"].get<int>() >= 2);
        QCOMPARE(meta["motif"]["cycle_transitions"].get<int>(), 4);
    }

    void probeForAiFormatsPinAndState()
    {
        const nlohmann::json doc = {
            {"status", "OK"},
            {"port", "/dev/ttyACM0"},
            {"samples", 10000},
            {"sample_rate_hz", 1000000.0},
            {"elapsed_ms", 81},
            {"device", {{"version", "LOGIC_ANALYZER_PICO_V6_6"}, {"board", "PICO"}}},
            {"channels",
             nlohmann::json::array({{{"channel", 24},
                                     {"name", "DO1"},
                                     {"state", "HIGH"},
                                     {"high_pct", 100.0},
                                     {"edges", 0},
                                     {"diagnosis", "Stable HIGH (pull-up OK)"}}})},
        };
        const auto md = lt::probeForAi(doc, "Check 4.7k resistor on Pin 34");
        QVERIFY(md.find("Check 4.7k resistor on Pin 34") != std::string::npos);
        QVERIFY(md.find("| 24 | GP28 | DO1 | **HIGH** | 100.0% | 0 | - | Stable HIGH (pull-up OK) |") !=
                std::string::npos);
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"

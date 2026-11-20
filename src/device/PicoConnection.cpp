// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/PicoConnection.hpp"

#include "core/Segments.hpp"

#include <QSerialPort>
#include <QSerialPortInfo>

#include <algorithm>
#include <set>

namespace lt {

using Clock = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

namespace {

int remainingMs(Clock::time_point deadline)
{
    const auto left = duration_cast<milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(left, 0, 3'600'000));
}

bool isFatal(QSerialPort::SerialPortError e)
{
    return e == QSerialPort::ResourceError || e == QSerialPort::DeviceNotFoundError ||
           e == QSerialPort::PermissionError || e == QSerialPort::NotOpenError ||
           e == QSerialPort::ReadError || e == QSerialPort::WriteError ||
           e == QSerialPort::UnsupportedOperationError || e == QSerialPort::OpenError;
}

}  // namespace

PicoConnection::PicoConnection() : m_port(std::make_unique<QSerialPort>()) {}

PicoConnection::~PicoConnection() { close(); }

bool PicoConnection::isOpen() const noexcept { return m_port && m_port->isOpen(); }

void PicoConnection::close()
{
    if (m_port && m_port->isOpen())
        m_port->close();
    m_rx.clear();
}

bool PicoConnection::open(const std::string& port, milliseconds handshakeTimeout)
{
    close();
    m_error.clear();
    // Opt-in guard for audits and automated harnesses: never touch a serial device by accident.
    // Pseudo-terminals (Linux /dev/pts/*, macOS /dev/ttys*, used by the test emulators) are not hardware and stay
    // allowed.
#if defined(__APPLE__)
    const bool pseudoTerminal = port.starts_with("/dev/ttys");
#else
    const bool pseudoTerminal = port.starts_with("/dev/pts/");
#endif
    if (qEnvironmentVariableIsSet("LOGICTRACER_DENY_HARDWARE") && !pseudoTerminal) {
        m_error = "serial access is denied by LOGICTRACER_DENY_HARDWARE";
        return false;
    }
    m_portLost = false;
    m_cancelRequested = false;
    m_info = DeviceInfo{};
    m_info.port = port;

    bool hasForeignUsbId = false;
    const auto qPort = QString::fromStdString(port);
    for (const auto& pi : QSerialPortInfo::availablePorts()) {
        if (pi.systemLocation() == qPort || pi.portName() == qPort) {
            m_info.serialNumber = pi.serialNumber().toStdString();
            if (pi.hasVendorIdentifier() && pi.hasProductIdentifier() &&
                (pi.vendorIdentifier() != kVendorId || pi.productIdentifier() != kProductId)) {
                hasForeignUsbId = true;
            }
            break;
        }
    }

    m_port->setPortName(QString::fromStdString(port));
    m_port->setBaudRate(115200);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    m_port->setReadBufferSize(0);
    if (!m_port->open(QIODevice::ReadWrite)) {
        m_error = "cannot open " + port + ": " + m_port->errorString().toStdString();
        return false;
    }
    // TinyUSB only transmits while the host asserts DTR.
    m_port->setDataTerminalReady(true);
    m_port->setRequestToSend(true);

    if (handshake(handshakeTimeout))
        return true;

    // Do not send cancel bytes or stall for seconds on unrelated USB CDC devices (e.g. GPS/modem).
    if (hasForeignUsbId) {
        close();
        return false;
    }

    // The device may still be busy with an abandoned capture: abort it and retry once.
    const std::string firstError = m_error;
    const uint8_t cancel = kCancelByte;
    writeAll({&cancel, 1});
    drain(m_pollLatency, m_pollLatency + 2000ms);
    if (handshake(handshakeTimeout))
        return true;
    m_error = firstError;
    close();
    return false;
}

bool PicoConnection::handshake(milliseconds timeout)
{
    m_rx.clear();
    if (m_port->waitForReadyRead(50))
        m_port->readAll();
    m_port->clear(QSerialPort::Input);

    const auto frame = encodeFrame(cmd::Id);
    if (!writeAll(frame))
        return false;

    const auto deadline = Clock::now() + timeout;
    std::optional<FirmwareVersion> version;
    while (!version) {
        auto line = readLine(deadline);
        if (!line) {
            m_error = "handshake timeout: no LOGIC_ANALYZER_* version line from " + m_info.port;
            return false;
        }
        version = parseVersionLine(*line);
    }
    m_info.version = *version;
    m_info.generation = requestLayoutFor(*version);
    if (!m_pollLatencyOverridden)
        m_pollLatency = defaultPollLatencyFor(*version);

    if (version->family == FirmwareFamily::Upstream && version->major < 6) {
        // Upstream V5 firmware reports only the version line; use its compile-time limits.
        // LogicTracer firmware (SemVer 0.x included) always sends the capability lines.
        m_info.maxFrequency = 100'000'000;
        m_info.blastFrequency = 0;
        m_info.bufferSize = 128 * 1024;
        m_info.channels = 24;
        return true;
    }

    std::optional<uint32_t> freq, blast, buffer, channels;
    m_info.features = 0;
    while (!(freq && blast && buffer && channels)) {
        auto line = readLine(deadline);
        if (!line) {
            m_error = "handshake timeout: incomplete FREQ/BLASTFREQ/BUFFER/CHANNELS response";
            return false;
        }
        if (auto v = parseKeyValue(*line, "FREQ")) freq = v;
        else if (auto v2 = parseKeyValue(*line, "BLASTFREQ")) blast = v2;
        else if (auto v3 = parseKeyValue(*line, "BUFFER")) buffer = v3;
        else if (auto v4 = parseKeyValue(*line, "CHANNELS")) channels = v4;
        else if (auto v5 = parseKeyValue(*line, "FEATURES")) m_info.features = *v5;  // sent before CHANNELS
    }
    m_info.maxFrequency = *freq;
    m_info.blastFrequency = *blast;
    m_info.bufferSize = *buffer;
    m_info.channels = static_cast<uint8_t>(std::min<uint32_t>(*channels, 32));
    return true;
}

bool PicoConnection::enterBootloader()
{
    if (!isOpen()) {
        m_error = "device is not open";
        return false;
    }
    m_rx.clear();
    m_port->clear(QSerialPort::Input);
    if (!writeAll(encodeFrame(cmd::Bootloader)))
        return false;

    const auto deadline = Clock::now() + 3000ms;
    while (auto line = readLine(deadline)) {
        if (*line == "RESTARTING_BOOTLOADER") {
            close();
            return true;
        }
        if (line->starts_with("ERR_")) {
            m_error = "bootloader command rejected: " + *line;
            return false;
        }
    }
    m_error = "no RESTARTING_BOOTLOADER confirmation from device";
    return false;
}

std::optional<TestSignalInfo> PicoConnection::setTestSignal(const TestSignalRequest& request, std::string& error)
{
    if (!isOpen()) {
        error = "device is not open";
        return std::nullopt;
    }
    if (!m_info.supportsTestSignal()) {
        error = "this firmware has no test-signal generator (LogicTracer 0.6+ on a Pico or Pico 2)";
        return std::nullopt;
    }
    m_rx.clear();
    m_port->clear(QSerialPort::Input);
    if (!writeAll(encodeFrame(cmd::TestSignal, serialize(request)))) {
        error = m_error;
        return std::nullopt;
    }
    const auto deadline = Clock::now() + 2000ms;
    while (auto line = readLine(deadline)) {
        if (auto info = parseTestSignalReply(*line))
            return info;
        if (line->starts_with("ERR_")) {
            error = "the firmware rejected the test signal (" + *line +
                    "): every segment must last at least 3 system clock cycles and period > high + delay";
            return std::nullopt;
        }
    }
    error = "no reply to the test-signal command";
    return std::nullopt;
}

std::optional<std::string> PicoConnection::validate(const CaptureConfig& c) const
{
    if (!isOpen())
        return "device is not open";
    if (c.channels.empty())
        return "no channels selected";
    if (c.channels.size() > m_info.channels)
        return "too many channels (device supports " + std::to_string(m_info.channels) + ")";

    std::set<uint8_t> seen;
    std::vector<uint8_t> numbers;
    for (const auto& ch : c.channels) {
        if (ch.number >= m_info.channels)
            return "channel " + std::to_string(ch.displayNumber()) + " out of range 1.." +
                   std::to_string(m_info.channels);
        if (!seen.insert(ch.number).second)
            return "channel " + std::to_string(ch.displayNumber()) + " specified twice";
        numbers.push_back(ch.number);
    }
    // channel == device channel count selects the external trigger input.
    if (c.trigger) {
        if (c.trigger->isPattern()) {
            if (c.trigger->channel > 15 || c.trigger->bitCount < 1 || c.trigger->bitCount > 16 ||
                c.trigger->channel + c.trigger->bitCount > 16 ||
                c.trigger->channel + c.trigger->bitCount > m_info.channels)
                return "pattern trigger channels out of range (must be 1..16 contiguous channels within CH1..CH16)";
            if (c.segments > 1)
                return "segmented captures are not supported with a pattern trigger";
        } else if (c.trigger->channel > m_info.channels) {
            return "trigger channel out of range";
        }
    }

    if (c.frequency < m_info.minFrequency() || c.frequency > m_info.maxFrequency)
        return "frequency " + std::to_string(c.frequency) + " Hz out of range " +
               std::to_string(m_info.minFrequency()) + ".." + std::to_string(m_info.maxFrequency) + " Hz";

    const auto mode = captureModeFor(numbers);
    const auto lim = m_info.limits(mode);
    const std::string modeStr(toString(mode));
    if (c.preSamples < lim.minPreSamples || c.preSamples > lim.maxPreSamples)
        return "pre-trigger samples " + std::to_string(c.preSamples) + " out of range " +
               std::to_string(lim.minPreSamples) + ".." + std::to_string(lim.maxPreSamples) + " (" + modeStr +
               " mode)";
    if (c.postSamples < lim.minPostSamples || c.postSamples > lim.maxPostSamples)
        return "post-trigger samples " + std::to_string(c.postSamples) + " out of range " +
               std::to_string(lim.minPostSamples) + ".." + std::to_string(lim.maxPostSamples) + " (" + modeStr +
               " mode)";
    if (c.segments == 0 || c.segments > 65536)
        return "segments must be 1..65536";
    if (c.segments > 1 && !c.trigger)
        return "a segmented capture needs a trigger";
    if (c.segments > 256 && m_info.generation == ProtocolGeneration::V5)
        return "this firmware accepts at most 256 segments";
    const uint64_t total = static_cast<uint64_t>(c.preSamples) + static_cast<uint64_t>(c.postSamples) * c.segments;
    if (total > lim.maxTotalSamples)
        return "total samples (" + std::to_string(total) + ") exceed device buffer: max " +
               std::to_string(lim.maxTotalSamples) + " in " + modeStr + " mode";
    return std::nullopt;
}

CaptureRequestPacket PicoConnection::buildRequest(const CaptureConfig& c, const EdgeTrigger& trigger,
                                                  bool edgeSensitive) const
{
    CaptureRequestPacket req;
    if (trigger.isPattern()) {
        req.triggerType = trigger.patternTriggerType();
        req.triggerChannel = trigger.channel;
        req.invertedOrCount = trigger.bitCount;
        req.triggerValue = trigger.pattern;
    } else {
        req.triggerType = edgeSensitive ? TriggerType::EdgeSensitive : TriggerType::Edge;
        req.triggerChannel = trigger.channel;
        req.invertedOrCount = trigger.inverted ? 1 : 0;
    }
    std::vector<uint8_t> numbers;
    for (std::size_t i = 0; i < c.channels.size(); ++i) {
        req.channels[i] = c.channels[i].number;
        numbers.push_back(c.channels[i].number);
    }
    req.channelCount = static_cast<uint8_t>(c.channels.size());
    req.frequency = c.frequency;
    req.preSamples = c.preSamples;
    req.postSamples = c.postSamples;
    req.loopCount = static_cast<uint16_t>(c.segments - 1);
    // Burst timestamps give the time between windows; the firmware keeps at most 255 of them.
    if (edgeSensitive && m_info.supportsEdgeBurstTimestamps() && c.segments >= 2 && c.segments <= 254)
        req.measure = 1;
    if (m_info.supportsRecordedCount())
        req.measure |= 2;
    req.captureMode = captureModeFor(numbers);
    return req;
}

void PicoConnection::alignTriggerToEdge(CaptureBuffer& buffer, const EdgeTrigger& trigger)
{
    const auto& channels = buffer.channels();
    const auto it = std::find_if(channels.begin(), channels.end(),
                                 [&](const ChannelInfo& c) { return c.number == trigger.channel; });
    const auto ti = buffer.triggerIndex();
    if (it == channels.end() || ti < 2 || static_cast<std::size_t>(ti) >= buffer.size())
        return;  // the trigger channel was not captured: t = 0 may be one sample after the edge
    const auto bit = static_cast<std::size_t>(it - channels.begin());
    const bool after = !trigger.inverted;
    if (buffer.bit(ti, bit) == after && buffer.bit(ti - 1, bit) == after && buffer.bit(ti - 2, bit) != after)
        buffer.setTriggerIndex(ti - 1);
}

CaptureResult PicoConnection::capture(const CaptureConfig& config)
{
    const auto start = Clock::now();
    CaptureResult result;
    auto finish = [&]() -> CaptureResult {
        result.elapsed = duration_cast<milliseconds>(Clock::now() - start);
        result.portLost = m_portLost;
        m_cancelRequested = false;
        reportProgress(CaptureStage::Done);
        return result;
    };

    if (auto err = validate(config)) {
        result.error = *err;
        result.usageError = true;
        return finish();
    }

    std::vector<uint8_t> numbers;
    for (const auto& ch : config.channels)
        numbers.push_back(ch.number);
    result.mode = captureModeFor(numbers);

    std::vector<EdgeTrigger> plan;
    const auto captureTime = milliseconds(
        1 + (static_cast<uint64_t>(config.preSamples) + static_cast<uint64_t>(config.postSamples) * config.segments) *
                1000 / std::max(1u, config.frequency));
    milliseconds wait;
    if (config.trigger) {
        plan.push_back(*config.trigger);
        // The firmware reports completion only after all post-trigger samples have been recorded.
        wait = config.timeout + captureTime + m_pollLatency;
    } else {
        // Level-sensitive trigger: one of the two polarities fires immediately. RP2040 inputs
        // default to pull-down, so a floating/idle line is LOW: try the "low" trigger first.
        plan.push_back({config.channels.front().number, true});
        plan.push_back({config.channels.front().number, false});
        wait = m_pollLatency + 800ms + captureTime;
    }

    // A user trigger means "on this transition": use the edge-sensitive program when the firmware has it. The
    // immediate plan relies on the level-sensitive one (one of the two polarities fires at once).
    const bool edgeSensitive = config.trigger.has_value() && !config.trigger->isPattern() && m_info.supportsEdgeTrigger();
    result.edgeSensitive = edgeSensitive;
    if (config.trigger && config.trigger->isPattern()) {
        result.warnings.push_back(
            std::string("pattern trigger (") +
            (config.trigger->patternTriggerType() == TriggerType::Fast ? "Fast" : "Complex") +
            " PIO mode) requires a jumper between GP0 (trigger output) and GP1 (trigger input) on the Pico board");
    }
    std::vector<uint32_t> timestamps;
    std::optional<uint32_t> recorded;
    for (const auto& trig : plan) {
        ++result.attempts;
        result.triggerUsed = trig;
        auto attempt = runCapture(buildRequest(config, trig, edgeSensitive), config.channels, wait,
                                  config.trigger.has_value());
        result.status = attempt.status;
        result.error = attempt.error;
        result.buffer = std::move(attempt.buffer);
        timestamps = std::move(attempt.timestamps);
        recorded = attempt.recorded;
        if (attempt.status != CaptureStatus::Timeout)
            break;
        if (m_cancelRequested) {
            result.status = CaptureStatus::Cancelled;
            result.error = "capture cancelled";
            break;
        }
    }

    if (result.status == CaptureStatus::Cancelled && result.error.empty())
        result.error = "capture cancelled";

    // Immediate mode: the trigger fires on the first sample, so the pre-trigger part holds stale memory.
    if (!config.trigger && result.buffer)
        result.buffer = result.buffer->dropFront(config.preSamples);
    // A trigger that fires before the pre-trigger window has filled leaves its oldest samples unrecorded (the
    // firmware's zero fill): leave them out instead of showing a LOW that never happened.
    uint32_t preKept = config.preSamples;
    if (config.trigger && result.buffer && recorded && *recorded < result.buffer->size()) {
        const auto missing = std::min<std::size_t>(result.buffer->size() - *recorded, config.preSamples);
        result.buffer = result.buffer->dropFront(missing);
        preKept = config.preSamples - static_cast<uint32_t>(missing);
        result.warnings.push_back("the trigger fired before the pre-trigger window filled: " + std::to_string(preKept) +
                                  " of " + std::to_string(config.preSamples) +
                                  " pre-trigger samples were recorded, the older ones are left out");
    }
    // The edge programs store a sample ("in pins") and then test the trigger pin ("jmp pin"): an edge between the
    // two is already in the last pre-trigger sample. Put t = 0 on the first sample showing the new level (for a
    // segmented capture: of window 1; later windows start sampling on the detected edge).
    if (edgeSensitive && result.buffer && result.status == CaptureStatus::Ok)
        alignTriggerToEdge(*result.buffer, *config.trigger);
    if (config.segments > 1 && config.trigger)
    {
        result.segments = SegmentLayout{config.segments, preKept, config.postSamples, config.trigger->channel,
                                        config.trigger->inverted, {}, 0.0};
        if (!timestamps.empty()) {
            std::string why;
            if (auto times = triggerTimesFromTimestamps(timestamps, config.segments, m_info.blastFrequency, &why)) {
                result.segments->triggerTimesSeconds = std::move(*times);
                result.segments->triggerTimeResolutionSeconds = 1.0 / m_info.blastFrequency;
            } else {
                result.warnings.push_back("burst timestamps ignored: " + why);
            }
        }
    }

    if (result.status == CaptureStatus::Timeout) {
        if (result.error.empty())
            result.error = "trigger did not occur within " + std::to_string(config.timeout.count()) + " ms";
        if (config.probeOnTimeout) {
            auto p = probe(config.channels);
            if (p.status == CaptureStatus::Ok)
                result.lineDump = std::move(p.buffer);
        }
    }
    return finish();
}

ProbeResult PicoConnection::probe(const std::vector<ChannelInfo>& channels, milliseconds window,
                                  uint32_t frequency)
{
    ProbeResult pr;
    CaptureConfig c;
    c.channels = channels;
    c.frequency = std::clamp(frequency, m_info.minFrequency(), m_info.maxFrequency);
    c.probeOnTimeout = false;
    c.trigger.reset();

    std::vector<uint8_t> numbers;
    for (const auto& ch : channels)
        numbers.push_back(ch.number);
    const auto lim = m_info.limits(captureModeFor(numbers));
    if (lim.maxTotalSamples < lim.minPreSamples + lim.minPostSamples) {
        pr.error = "the device buffer is too small to probe";
        return pr;
    }
    c.preSamples = lim.minPreSamples;
    const uint64_t wanted = static_cast<uint64_t>(window.count()) * c.frequency / 1000;
    c.postSamples = static_cast<uint32_t>(std::clamp<uint64_t>(
        wanted, lim.minPostSamples, std::min(lim.maxPostSamples, lim.maxTotalSamples - c.preSamples)));

    auto r = capture(c);
    pr.status = r.status;
    pr.error = r.error;
    pr.attempts = r.attempts;
    pr.triggerUsed = r.triggerUsed;
    pr.elapsed = r.elapsed;
    pr.portLost = r.portLost;
    pr.buffer = std::move(r.buffer);
    return pr;
}

PicoConnection::Attempt PicoConnection::runCapture(const CaptureRequestPacket& req,
                                                   const std::vector<ChannelInfo>& channels,
                                                   milliseconds triggerWait,
                                                   bool waitPreFill)
{
    Attempt a;
    m_rx.clear();
    m_port->clear(QSerialPort::Input);

    reportProgress(CaptureStage::Starting);
    const auto frame = encodeFrame(cmd::Capture, serialize(req, m_info.generation));
    if (!writeAll(frame)) {
        a.error = m_error;
        return a;
    }

    const auto ackDeadline = Clock::now() + 5000ms;
    for (;;) {
        auto line = readLine(ackDeadline);
        if (!line) {
            a.error = m_portLost ? m_error : "no CAPTURE_STARTED acknowledgement from device";
            return a;
        }
        if (*line == "CAPTURE_STARTED")
            break;
        if (*line == "CAPTURE_ERROR" || line->starts_with("ERR_")) {
            a.error = "device rejected capture request: " + *line;
            return a;
        }
    }

    // Ensure the circular DMA pre-trigger window fills with real GPIO levels (overwriting the
    // firmware's initial memset(0)) before signaling WaitingForTrigger to external HIL stimuli.
    if (waitPreFill && req.preSamples > 0 && req.frequency > 0) {
        const auto preFillMs = std::clamp<uint64_t>(
            (static_cast<uint64_t>(req.preSamples) * 1000ull + req.frequency - 1) / req.frequency, 1, 2000);
        const auto preDeadline = Clock::now() + milliseconds(preFillMs);
        while (Clock::now() < preDeadline && !m_cancelRequested && m_rx.size() < 4 && m_port->bytesAvailable() < 4) {
            const int step = std::min(20, remainingMs(preDeadline));
            if (step <= 0)
                break;
            m_port->clearError();
            if (m_port->waitForReadyRead(step))
                m_rx += m_port->readAll();
            else if (isFatal(m_port->error()))
                break;
        }
    }

    reportProgress(CaptureStage::WaitingForTrigger);
    if (!waitForBytes(4, Clock::now() + triggerWait, true)) {
        if (portFailed()) {
            a.error = m_portLost ? m_error : "serial port error: " + m_port->errorString().toStdString();
            return a;
        }
        switch (cancelCapture()) {
        case CancelOutcome::DataArrived:
            if (m_cancelRequested) {  // finished while being cancelled: the user asked to stop, drop it
                drain(300ms);
                a.status = CaptureStatus::Cancelled;
                return a;
            }
            break;
        case CancelOutcome::Cancelled:
            a.status = m_cancelRequested ? CaptureStatus::Cancelled : CaptureStatus::Timeout;
            return a;
        case CancelOutcome::PortError: a.error = m_error; return a;
        }
    }

    reportProgress(CaptureStage::Downloading);
    const auto* p = reinterpret_cast<const uint8_t*>(m_rx.constData());
    const uint32_t length = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    m_rx.remove(0, 4);

    const uint64_t expected = static_cast<uint64_t>(req.preSamples) + req.postSamples * (req.loopCount + 1ull);
    if (length != expected) {
        a.error = "unexpected sample count " + std::to_string(length) + " (expected " + std::to_string(expected) + ")";
        drain(300ms);
        return a;
    }

    QByteArray raw;
    const auto bytes = static_cast<qsizetype>(length * bytesPerSample(req.captureMode));
    if (!readExact(bytes, raw, 3000ms)) {
        a.error = "sample stream interrupted (" + std::to_string(raw.size()) + "/" + std::to_string(bytes) +
                  " bytes)" + (m_portLost ? ": " + m_error : std::string());
        return a;
    }
    QByteArray stamps;
    if (!readExact(1, stamps, 1000ms)) {
        a.error = "capture trailer missing (burst timestamp count)";
        return a;
    }
    const auto stampCount = static_cast<uint8_t>(stamps[0]);
    if (stampCount > 1) {
        QByteArray stampBytes;
        if (!readExact(stampCount * 4, stampBytes, 1000ms)) {
            a.error = "capture trailer truncated (burst timestamps)";
            return a;
        }
        const auto* s = reinterpret_cast<const uint8_t*>(stampBytes.constData());
        for (int k = 0; k < stampCount; ++k, s += 4)
            a.timestamps.push_back(s[0] | (s[1] << 8) | (s[2] << 16) | (static_cast<uint32_t>(s[3]) << 24));
    }
    if (req.measure & 2) {
        QByteArray rec;
        if (!readExact(4, rec, 1000ms)) {
            a.error = "capture trailer truncated (recorded-sample count)";
            return a;
        }
        const auto* r = reinterpret_cast<const uint8_t*>(rec.constData());
        a.recorded = r[0] | (r[1] << 8) | (r[2] << 16) | (static_cast<uint32_t>(r[3]) << 24);
    }

    const std::span<const uint8_t> rawSpan(reinterpret_cast<const uint8_t*>(raw.constData()),
                                           static_cast<std::size_t>(raw.size()));
    a.buffer = CaptureBuffer::fromRaw(channels, req.frequency, req.preSamples, req.captureMode, rawSpan);
    a.status = CaptureStatus::Ok;
    return a;
}

PicoConnection::CancelOutcome PicoConnection::cancelCapture()
{
    const uint8_t cancel = kCancelByte;
    if (!writeAll({&cancel, 1}))
        return CancelOutcome::PortError;
    // The firmware checks for a cancel request once per poll cycle; the capture may also
    // have completed in the meantime, in which case its data is still delivered. Firmware with the extended
    // trailer (FEATURES bit 3) acknowledges a cancel with CAPTURE_CANCELLED; a sample count never starts with "CAPT".
    const auto deadline = Clock::now() + m_pollLatency + 300ms;
    if (waitForBytes(4, deadline)) {
        if (!m_info.supportsRecordedCount() || !m_rx.startsWith("CAPT"))
            return CancelOutcome::DataArrived;
        (void)readLine(deadline);
        m_rx.clear();
        return CancelOutcome::Cancelled;
    }
    if (portFailed()) {
        if (!m_portLost)
            m_error = "serial port error: " + m_port->errorString().toStdString();
        return CancelOutcome::PortError;
    }
    drain(200ms);
    m_rx.clear();
    return CancelOutcome::Cancelled;
}

bool PicoConnection::writeAll(std::span<const uint8_t> data)
{
    if (!isOpen()) {
        m_error = "port not open";
        return false;
    }
    const auto written = m_port->write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
    if (written != static_cast<qint64>(data.size()) || !m_port->waitForBytesWritten(2000)) {
        if (m_port->bytesToWrite() > 0 || written != static_cast<qint64>(data.size())) {
            m_error = "write failed: " + m_port->errorString().toStdString();
            return false;
        }
    }
    return true;
}

bool PicoConnection::portFailed() const
{
    return m_portLost || !isOpen() || isFatal(m_port->error());
}

bool PicoConnection::waitForBytes(qsizetype count, Clock::time_point deadline, bool honorCancel)
{
    int earlyReturns = 0;
    while (m_rx.size() < count) {
        if (m_port->bytesAvailable() > 0) {
            m_rx += m_port->readAll();
            continue;
        }
        if (honorCancel && m_cancelRequested)
            return false;
        const int left = remainingMs(deadline);
        if (left <= 0)
            return false;
        // Cancellable waits are sliced so requestCancel() is noticed promptly.
        const int slice = honorCancel ? std::min(left, 50) : left;
        m_port->clearError();  // a stale error must not be mistaken for a fresh failure
        const auto waitStart = Clock::now();
        if (!m_port->waitForReadyRead(slice)) {
            if (isFatal(m_port->error()))
                return false;
            // After a hang-up (e.g. the device vanished) Qt returns at once without raising an error,
            // which would otherwise spin the CPU until the deadline. A genuine timeout blocks for `left`.
            const auto waited = duration_cast<milliseconds>(Clock::now() - waitStart).count();
            if (waited + 20 < slice && ++earlyReturns >= 3) {
                m_portLost = true;
                m_error = "serial port closed by the device (disconnected?)";
                return false;
            }
            continue;
        }
        earlyReturns = 0;
        m_rx += m_port->readAll();
    }
    return true;
}

std::optional<std::string> PicoConnection::readLine(Clock::time_point deadline)
{
    constexpr qsizetype kMaxLineBytes = 8192;
    for (;;) {
        const auto nl = m_rx.indexOf('\n');
        if (nl >= 0) {
            if (nl > kMaxLineBytes) {
                m_error = "line overflow from device";
                m_rx.clear();
                return std::nullopt;
            }
            std::string line(m_rx.constData(), static_cast<std::size_t>(nl));
            m_rx.remove(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
                line.pop_back();
            return line;
        }
        if (m_rx.size() > kMaxLineBytes) {
            m_error = "line overflow from device";
            m_rx.clear();
            return std::nullopt;
        }
        if (!waitForBytes(m_rx.size() + 1, deadline))
            return std::nullopt;
    }
}

bool PicoConnection::readExact(qsizetype count, QByteArray& out, milliseconds inactivity)
{
    out.clear();
    out.reserve(count);
    while (out.size() < count) {
        if (m_rx.isEmpty() && !waitForBytes(1, Clock::now() + inactivity))
            return false;
        const auto take = std::min(count - out.size(), m_rx.size());
        out.append(m_rx.constData(), take);
        m_rx.remove(0, take);
    }
    return true;
}

void PicoConnection::drain(milliseconds quietPeriod, milliseconds maxTime)
{
    const auto hardDeadline = Clock::now() + maxTime;
    m_rx.clear();
    while (Clock::now() < hardDeadline) {
        const int wait = std::min(static_cast<int>(quietPeriod.count()), remainingMs(hardDeadline));
        if (!m_port->waitForReadyRead(wait) && m_port->bytesAvailable() == 0)
            break;
        m_port->readAll();
    }
    m_rx.clear();
}

std::vector<DetectedPort> PicoConnection::enumerate(bool includeAllAcm)
{
    std::vector<DetectedPort> out;
    for (const auto& info : QSerialPortInfo::availablePorts()) {
        const bool match = info.hasVendorIdentifier() && info.hasProductIdentifier() &&
                           info.vendorIdentifier() == kVendorId && info.productIdentifier() == kProductId;
#if defined(_WIN32)
        const bool acm = true;  // every COM port (USB CDC devices appear as COMn)
#else
        const bool acm = info.portName().startsWith("ttyACM");
#endif
        if (!match && !(includeAllAcm && acm))
            continue;
        DetectedPort d;
        d.port = info.systemLocation().toStdString();
        d.description = info.description().toStdString();
        d.manufacturer = info.manufacturer().toStdString();
        d.serialNumber = info.serialNumber().toStdString();
        d.vendorId = info.hasVendorIdentifier() ? info.vendorIdentifier() : 0;
        d.productId = info.hasProductIdentifier() ? info.productIdentifier() : 0;
        d.vidPidMatch = match;
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.port < b.port; });
    return out;
}

}  // namespace lt

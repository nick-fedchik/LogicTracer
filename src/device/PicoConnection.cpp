// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/PicoConnection.hpp"

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
    if (qEnvironmentVariableIsSet("LOGICTRACER_DENY_HARDWARE")) {
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
    if (c.trigger && c.trigger->channel > m_info.channels)
        return "trigger channel out of range";

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
    if (static_cast<uint64_t>(c.preSamples) + c.postSamples > lim.maxTotalSamples)
        return "total samples exceed device buffer: max " + std::to_string(lim.maxTotalSamples) + " in " +
               modeStr + " mode";
    return std::nullopt;
}

CaptureRequestPacket PicoConnection::buildRequest(const CaptureConfig& c, const EdgeTrigger& trigger) const
{
    CaptureRequestPacket req;
    req.triggerType = TriggerType::Edge;
    req.triggerChannel = trigger.channel;
    req.invertedOrCount = trigger.inverted ? 1 : 0;
    std::vector<uint8_t> numbers;
    for (std::size_t i = 0; i < c.channels.size(); ++i) {
        req.channels[i] = c.channels[i].number;
        numbers.push_back(c.channels[i].number);
    }
    req.channelCount = static_cast<uint8_t>(c.channels.size());
    req.frequency = c.frequency;
    req.preSamples = c.preSamples;
    req.postSamples = c.postSamples;
    req.captureMode = captureModeFor(numbers);
    return req;
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
        return finish();
    }

    std::vector<uint8_t> numbers;
    for (const auto& ch : config.channels)
        numbers.push_back(ch.number);
    result.mode = captureModeFor(numbers);

    std::vector<EdgeTrigger> plan;
    const auto captureTime = milliseconds(
        1 + (static_cast<uint64_t>(config.preSamples) + config.postSamples) * 1000 / std::max(1u, config.frequency));
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

    for (const auto& trig : plan) {
        ++result.attempts;
        result.triggerUsed = trig;
        auto attempt = runCapture(buildRequest(config, trig), config.channels, wait, config.trigger.has_value());
        result.status = attempt.status;
        result.error = attempt.error;
        result.buffer = std::move(attempt.buffer);
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
        QByteArray ignored;
        if (!readExact(stampCount * 4, ignored, 1000ms)) {
            a.error = "capture trailer truncated (burst timestamps)";
            return a;
        }
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
    // have completed in the meantime, in which case its data is still delivered.
    if (waitForBytes(4, Clock::now() + m_pollLatency + 300ms))
        return CancelOutcome::DataArrived;
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
        if (!match && !(includeAllAcm && info.portName().startsWith("ttyACM")))
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

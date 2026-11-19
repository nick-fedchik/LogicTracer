// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/PynqConnection.hpp"

#include <QTcpSocket>
#include <QUrl>
#include <QtGlobal>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace lt {

using json = nlohmann::json;
using namespace std::chrono;

namespace {

bool isLoopback(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Edge records [(cycles relative to the trigger, probe word)] -> samples [-pre, post) at the DUT clock.
CaptureBuffer expand(const json& records, const std::vector<ChannelInfo>& channels, uint32_t clockHz, uint32_t pre,
                     uint32_t post, int64_t endRel, std::vector<std::string>& warnings)
{
    std::vector<std::pair<int64_t, uint32_t>> recs;
    recs.reserve(records.size());
    for (const auto& r : records)
        recs.emplace_back(r.at(0).get<int64_t>(), r.at(1).get<uint32_t>());
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    uint64_t total = static_cast<uint64_t>(pre) + post;
    if (endRel >= 0 && endRel + 1 < static_cast<int64_t>(post)) {  // the ring filled before the window ended
        total = pre + static_cast<uint64_t>(endRel) + 1;
        warnings.push_back("the core's record buffer filled " + std::to_string(endRel) +
                           " cycles after the trigger: the capture ends there (fewer edges or a shorter window fit)");
    }
    if (!recs.empty() && recs.front().first > -static_cast<int64_t>(pre))
        warnings.push_back("recorded history starts " + std::to_string(-recs.front().first) +
                           " cycles before the trigger: the " + std::to_string(pre + recs.front().first) +
                           " earlier samples repeat the first recorded level");

    auto word = [&](uint32_t probe) {
        uint32_t w = 0;
        for (std::size_t c = 0; c < channels.size(); ++c)
            if ((probe >> channels[c].number) & 1u)
                w |= 1u << c;
        return w;
    };
    std::vector<uint32_t> samples(total);
    std::size_t k = 0;
    uint32_t current = recs.empty() ? 0u : word(recs.front().second);
    for (uint64_t i = 0; i < total; ++i) {
        const int64_t rel = static_cast<int64_t>(i) - static_cast<int64_t>(pre);
        while (k < recs.size() && recs[k].first <= rel)
            current = word(recs[k++].second);
        samples[i] = current;
    }
    return CaptureBuffer(channels, clockHz, pre, std::move(samples));
}

}  // namespace

PynqConnection::PynqConnection() = default;
PynqConnection::~PynqConnection()
{
    close();
}

bool PynqConnection::isOpen() const noexcept
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void PynqConnection::close()
{
    if (m_socket) {
        m_socket->abort();
        m_socket.reset();
    }
}

bool PynqConnection::enterBootloader()
{
    m_error = "the PYNQ capture core has no bootloader (load a bitstream on the board instead)";
    return false;
}

bool PynqConnection::open(const std::string& port, milliseconds handshakeTimeout)
{
    close();
    m_error.clear();
    if (!isPynqPort(port)) {
        m_error = "PYNQ port must look like pynq://host[:port]";
        return false;
    }
    m_cancelRequested = false;
    const QUrl url(QString::fromStdString(port), QUrl::StrictMode);  // also [IPv6]:port
    const int urlPort = url.port(kDefaultPort);
    if (!url.isValid() || url.host().isEmpty() || !url.path().isEmpty() || urlPort <= 0) {
        m_error = "PYNQ port must look like pynq://host[:port] (got '" + port + "')";
        return false;
    }
    m_host = url.host().toStdString();
    m_port = static_cast<uint16_t>(urlPort);
    // The hardware guard of audits and harnesses also covers boards; a local (simulated) agent stays allowed.
    if (qEnvironmentVariableIsSet("LOGICTRACER_DENY_HARDWARE") && !isLoopback(m_host)) {
        m_error = "board access is denied by LOGICTRACER_DENY_HARDWARE";
        return false;
    }

    m_socket = std::make_unique<QTcpSocket>();
    m_socket->connectToHost(QString::fromStdString(m_host), m_port);
    if (!m_socket->waitForConnected(static_cast<int>(handshakeTimeout.count()))) {
        m_error = "cannot reach the PYNQ agent at " + m_host + ":" + std::to_string(m_port) + " (" +
                  m_socket->errorString().toStdString() + "); start logictracer_agent.py on the board";
        close();
        return false;
    }
    const auto reply = call(R"({"cmd":"identify"})", handshakeTimeout, false);
    if (!reply) {
        close();
        return false;
    }
    const auto id = json::parse(*reply, nullptr, false);
    if (!id.is_object() || !id.value("ok", false) || id.value("protocol", 0) != 1 || id.value("core", "") != "lt_capture") {
        m_error = "unexpected identify reply from " + port;
        close();
        return false;
    }
    const auto width = id.value("probe_width", 0);
    const auto clock = id.value("clock_hz", 0u);
    if (width < 1 || width > 32 || clock == 0) {
        m_error = "the agent reports an invalid core configuration";
        close();
        return false;
    }
    m_probeNames.clear();
    if (id.contains("probes") && id["probes"].is_array())
        for (const auto& n : id["probes"])
            m_probeNames.push_back(n.is_string() ? n.get<std::string>() : std::string{});

    m_info = {};
    m_info.port = port;
    m_info.version.family = FirmwareFamily::Pynq;
    m_info.version.board = "PYNQ";
    m_info.version.raw = "LT_CAPTURE_" + id.value("version", std::string("?")) + " (" + id.value("backend", std::string("hardware")) +
                         ", " + m_host + ")";
    m_info.generation = ProtocolGeneration::V6;
    m_info.maxFrequency = clock;
    m_info.channels = static_cast<uint8_t>(width);
    m_info.bufferSize = kMaxSamples * 4u;  // limits() divides by the bytes per sample of the widest mode
    m_info.features = DeviceInfo::kFeatureEdgeTrigger;
    m_info.dutClock = true;
    m_info.syncStages = static_cast<uint8_t>(id.value("sync_stages", 0));
    return true;
}

std::optional<std::string> PynqConnection::call(const std::string& requestJson, milliseconds timeout, bool cancellable)
{
    if (!isOpen()) {
        m_error = "not connected to the PYNQ agent";
        return std::nullopt;
    }
    m_socket->write(QByteArray::fromStdString(requestJson) + '\n');
    m_socket->flush();
    auto deadline = steady_clock::now() + timeout;
    bool armed = false;
    bool cancelSent = false;
    while (true) {
        while (m_socket->canReadLine()) {
            const std::string line = m_socket->readLine().trimmed().toStdString();
            const auto msg = json::parse(line, nullptr, false);
            if (msg.is_object() && msg.contains("event")) {  // progress, e.g. {"event":"armed"}
                if (msg["event"] == "armed") {
                    armed = true;
                    report(CaptureStage::WaitingForTrigger);
                }
                continue;
            }
            return line;
        }
        // The agent clears its cancel flag when a capture starts: cancel only once it has armed. An agent
        // that does not answer the cancel promptly is given up on (the board may be gone).
        if (cancellable && m_cancelRequested && armed && !cancelSent) {
            sendCancel();
            cancelSent = true;
            deadline = std::min(deadline, steady_clock::now() + 3s);
        }
        if (steady_clock::now() > deadline) {
            m_error = "the PYNQ agent did not answer in time";
            close();
            return std::nullopt;
        }
        if (!m_socket->waitForReadyRead(50) && m_socket->state() != QAbstractSocket::ConnectedState) {
            m_error = "connection to the PYNQ agent lost: " + m_socket->errorString().toStdString();
            close();
            return std::nullopt;
        }
    }
}

// The agent serves one request per connection at a time: cancel through a second, short-lived one.
void PynqConnection::sendCancel()
{
    QTcpSocket s;
    s.connectToHost(QString::fromStdString(m_host), m_port);
    if (s.waitForConnected(1000)) {
        s.write(R"({"cmd":"cancel"})"
                "\n");
        s.waitForBytesWritten(1000);
        s.waitForReadyRead(1000);
    }
}

std::optional<std::string> PynqConnection::validate(const CaptureConfig& c) const
{
    if (!isOpen())
        return std::string("device is not open");
    if (c.channels.empty())
        return std::string("no channels selected");
    for (const auto& ch : c.channels)
        if (ch.number >= m_info.channels)
            return "channel " + std::to_string(ch.displayNumber()) + " out of range 1.." + std::to_string(m_info.channels) +
                   " (probe bits of the core)";
    if (c.trigger && c.trigger->channel > m_info.channels)
        return std::string("trigger channel out of range (") + std::to_string(m_info.channels + 1) +
               " = the core's external trigger input)";
    if (c.frequency != m_info.maxFrequency)
        return "the PL core samples at the DUT clock: use --freq " + std::to_string(m_info.maxFrequency) +
               " (requested " + std::to_string(c.frequency) + " Hz)";
    if (c.segments != 1)
        return std::string("segmented captures are not supported by the PL core (record a longer window instead)");
    if ((c.trigger ? static_cast<uint64_t>(c.preSamples) : 0ull) + c.postSamples > kMaxSamples)
        return "at most " + std::to_string(kMaxSamples) + " samples per capture";
    return std::nullopt;
}

CaptureResult PynqConnection::capture(const CaptureConfig& config)
{
    const auto start = steady_clock::now();
    CaptureResult r;
    r.attempts = 1;
    std::vector<uint8_t> numbers;
    for (const auto& ch : config.channels)
        numbers.push_back(ch.number);
    r.mode = captureModeFor(numbers);
    auto finish = [&](CaptureStatus status, std::string error = {}) {
        r.status = status;
        r.error = std::move(error);
        r.elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
        r.portLost = !isOpen();  // lets AsyncDevice and the GUI drop a dead connection
        if (r.portLost && status != CaptureStatus::Ok && m_cancelRequested) {
            r.status = CaptureStatus::Cancelled;
            r.error = "capture cancelled (the agent did not confirm)";
        }
        m_cancelRequested = false;
        report(CaptureStage::Done);
        return r;
    };
    report(CaptureStage::Starting);
    if (auto err = validate(config)) {
        r.usageError = true;
        return finish(CaptureStatus::Error, *err);
    }

    json req = {{"cmd", "capture"}, {"timeout_ms", config.timeout.count()}};
    uint32_t pre = 0;
    if (config.trigger) {
        r.triggerUsed = config.trigger;
        r.edgeSensitive = true;
        pre = config.preSamples;
        if (config.trigger->channel == m_info.channels) {
            req["external"] = true;
        } else {
            const uint32_t bit = 1u << config.trigger->channel;
            req[config.trigger->inverted ? "fall" : "rise"] = bit;
        }
    }
    req["post_cycles"] = config.postSamples;  // immediate: every mask is zero, the core triggers when armed
    const auto captureTime = milliseconds(1 + static_cast<int64_t>(config.postSamples) * 1000 / m_info.maxFrequency);
    const auto reply = call(req.dump(), config.timeout + captureTime + 5s, true);
    if (!reply)
        return finish(CaptureStatus::Error, m_error);
    const auto doc = json::parse(*reply, nullptr, false);
    if (!doc.is_object() || !doc.value("ok", false))
        return finish(CaptureStatus::Error, doc.is_object() ? doc.value("error", std::string("agent error"))
                                                            : std::string("malformed agent reply"));
    const auto status = doc.value("status", std::string{});
    if (status == "CANCELLED")
        return finish(CaptureStatus::Cancelled, "capture cancelled");
    if (status == "TIMEOUT") {
        if (config.probeOnTimeout) {  // the current line levels explain a missing trigger
            auto p = probe(config.channels, 10ms, 0);
            if (p.buffer)
                r.lineDump = std::move(p.buffer);
        }
        return finish(CaptureStatus::Timeout,
                      "trigger did not occur within " + std::to_string(config.timeout.count()) + " ms");
    }
    if (status != "OK" || !doc.contains("records") || !doc["records"].is_array() || doc["records"].empty())
        return finish(CaptureStatus::Error, "the agent returned no records");

    report(CaptureStage::Downloading);
    std::vector<ChannelInfo> channels = config.channels;
    for (auto& ch : channels)
        if (ch.name.empty() && ch.number < m_probeNames.size())
            ch.name = m_probeNames[ch.number];
    try {
        r.buffer = expand(doc["records"], channels, m_info.maxFrequency, pre, config.postSamples,
                          doc.value("overflow", false) ? doc.value("end_rel", int64_t{-1}) : int64_t{-1}, r.warnings);
        r.buffer->setExactTiming(m_info.syncStages == 0);
    } catch (const json::exception&) {
        return finish(CaptureStatus::Error, "malformed records from the agent");
    }
    return finish(CaptureStatus::Ok);
}

ProbeResult PynqConnection::probe(const std::vector<ChannelInfo>& channels, milliseconds window, uint32_t /*frequency*/)
{
    CaptureConfig c;
    c.channels = channels;
    c.frequency = m_info.maxFrequency;
    c.preSamples = 0;
    c.postSamples = static_cast<uint32_t>(std::clamp<uint64_t>(
        static_cast<uint64_t>(window.count()) * m_info.maxFrequency / 1000, 1, kMaxSamples));
    c.timeout = 2s;
    c.probeOnTimeout = false;
    auto r = capture(c);
    ProbeResult p;
    p.status = r.status;
    p.error = r.error;
    p.attempts = r.attempts;
    p.elapsed = r.elapsed;
    p.portLost = r.portLost;
    p.buffer = std::move(r.buffer);
    return p;
}

bool isPynqPort(std::string_view port) noexcept
{
    return port.starts_with(PynqConnection::kScheme);
}

}  // namespace lt

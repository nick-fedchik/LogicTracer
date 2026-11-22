// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/ReplayDevice.hpp"

#include "core/Exporters.hpp"
#include "core/Format.hpp"

#include <algorithm>
#include <limits>

namespace lt {

using namespace std::chrono;

namespace {

std::string channelList(const CaptureBuffer& b)
{
    std::string out;
    for (const auto& ch : b.channels())
        out += (out.empty() ? "" : ",") + std::to_string(ch.displayNumber());
    return out;
}

}  // namespace

CaptureBuffer ReplayDevice::demoRecording()
{
    std::vector<uint32_t> samples(4000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const bool clk = (i / 10) % 2;
        const bool echo = i >= 205 && ((i - 205) / 10) % 2;
        const bool burst = i >= 1000 && i < 1400 && ((i / 4) % 2);
        samples[i] = (clk ? 1u : 0u) | (echo ? 2u : 0u) | (burst ? 4u : 0u) | 8u;
    }
    return CaptureBuffer({{0, "CLK"}, {1, "ECHO"}, {2, "BURST"}, {3, "IDLE_HI"}}, 1'000'000, 200, std::move(samples));
}

bool ReplayDevice::open(const std::string& port, milliseconds /*handshakeTimeout*/)
{
    close();
    const bool demo = port == kDemoPort;
    if (!isReplayPort(port)) {
        m_error = "use replay:<capture file> or demo:pico";
        return false;
    }
    LoadedCapture loaded;
    if (demo)
        loaded.buffer = demoRecording();
    else if (!loadCaptureFile(port.substr(kScheme.size()), loaded, &m_error))
        return false;
    if (loaded.buffer.size() < 4 || loaded.buffer.channels().empty()) {
        m_error = "recording '" + port.substr(kScheme.size()) + "' holds no samples";
        return false;
    }
    m_recording = std::move(loaded.buffer);
    m_warnings = std::move(loaded.warnings);
    if (loaded.segments)
        m_warnings.push_back("the recording is a segmented capture of " + std::to_string(loaded.segments->count) +
                             " windows; replay plays them back to back as one continuous stream");

    uint8_t highest = 0;
    for (const auto& ch : m_recording.channels())
        highest = std::max(highest, ch.number);

    m_info = {};
    m_info.port = port;
    m_info.version.raw = demo ? "Demo Pico (simulated)" : "REPLAY " + port.substr(kScheme.size());
    if (demo)
        m_info.version.board = "PICO";
    m_info.version.family = FirmwareFamily::Replay;
    m_info.generation = ProtocolGeneration::V6;
    m_info.maxFrequency = m_recording.frequency();
    m_info.channels = static_cast<uint8_t>(highest + 1);
    // Any window of the recording fits, in every capture mode (4 bytes per sample is the widest).
    m_info.bufferSize = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(m_recording.size()) * 4u, std::numeric_limits<uint32_t>::max()));
    m_cursor = 0;
    m_open = true;
    m_error.clear();
    return true;
}

void ReplayDevice::close()
{
    m_open = false;
    m_recording = {};
    m_warnings.clear();
    m_info = {};
    m_cursor = 0;
}

bool ReplayDevice::enterBootloader()
{
    m_error = "a replay device has no bootloader";
    return false;
}

std::optional<std::size_t> ReplayDevice::sourceIndex(uint8_t channelNumber) const
{
    const auto& chs = m_recording.channels();
    for (std::size_t i = 0; i < chs.size(); ++i)
        if (chs[i].number == channelNumber)
            return i;
    return std::nullopt;
}

std::optional<std::string> ReplayDevice::validate(const CaptureConfig& config, CaptureField* field) const
{
    if (field) *field = CaptureField::Channels;
    if (!m_open)
        return "replay device is not open";
    if (config.channels.empty())
        return "no channels selected";
    for (const auto& ch : config.channels) {
        if (!sourceIndex(ch.number))
            return "channel " + std::to_string(ch.displayNumber()) + " is not in the recording (available: " +
                   channelList(m_recording) + ")";
    }
    if (field) *field = CaptureField::Trigger;
    if (config.trigger) {
        if (config.trigger->isPattern()) {
            if (config.segments > 1)
                return "segmented captures are not supported with a pattern trigger";
            for (uint8_t k = 0; k < config.trigger->bitCount; ++k) {
                if (!sourceIndex(config.trigger->channel + k))
                    return "trigger channel " + std::to_string(config.trigger->channel + k + 1) +
                           " is not in the recording";
            }
        } else if (!sourceIndex(config.trigger->channel)) {
            return "trigger channel " + std::to_string(config.trigger->channel + 1) + " is not in the recording";
        }
    }
    if (field) *field = CaptureField::Rate;
    if (config.frequency == 0 || m_recording.frequency() % config.frequency != 0)
        return "the recording was sampled at " + std::to_string(m_recording.frequency()) +
               " Hz; replay supports that rate or an integer divisor of it";
    const uint64_t step = m_recording.frequency() / config.frequency;
    // An immediate capture returns only the post samples (the device drops the 2 reserved pre-trigger ones).
    if (field) *field = CaptureField::Windows;
    if (config.segments == 0 || config.segments > 65536)
        return "segments must be 1..65536";
    if (config.segments > 1 && !config.trigger)
        return "a segmented capture needs a trigger";
    const uint64_t wanted =
        config.trigger ? static_cast<uint64_t>(config.preSamples) + static_cast<uint64_t>(config.postSamples) * config.segments
                       : static_cast<uint64_t>(config.postSamples);
    if (field) *field = CaptureField::Samples;
    const auto available = m_recording.size() / step;
    if (wanted > available)
        return "requested " + std::to_string(wanted) + " samples; maximum " + std::to_string(available) + " at " +
               formatHz(config.frequency) + " (" + formatTime(static_cast<double>(available) / config.frequency) + ")";
    return std::nullopt;
}

CaptureBuffer ReplayDevice::extract(const std::vector<ChannelInfo>& channels, std::size_t first, std::size_t count,
                                    int64_t triggerIndex) const
{
    // `first` and the result are in recording samples / output samples; step maps one to the other.
    const std::size_t step = m_info.maxFrequency / std::max<uint32_t>(1, m_captureFrequency);
    std::vector<std::size_t> src;
    std::vector<ChannelInfo> outChannels;
    for (const auto& ch : channels) {
        const auto idx = *sourceIndex(ch.number);
        src.push_back(idx);
        outChannels.push_back({ch.number, ch.name.empty() ? m_recording.channels()[idx].name : ch.name});
    }
    std::vector<uint32_t> samples(count, 0u);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t s = first + k * step;
        uint32_t word = 0;
        for (std::size_t c = 0; c < src.size(); ++c)
            if (m_recording.bit(s, src[c]))
                word |= 1u << c;
        samples[k] = word;
    }
    CaptureBuffer out(std::move(outChannels), m_captureFrequency, 0, std::move(samples));
    out.setTriggerIndex(triggerIndex);
    // Exact clock cycles survive only when every recorded sample is delivered (no decimation).
    out.setExactTiming(m_recording.exactTiming() && step == 1);
    return out;
}

CaptureResult ReplayDevice::capture(const CaptureConfig& config)
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
        r.warnings = m_warnings;
        r.elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
        m_cancelRequested = false;
        return r;
    };

    report(CaptureStage::Starting);
    if (auto err = validate(config)) {
        r.usageError = true;
        return finish(CaptureStatus::Error, *err);
    }
    m_captureFrequency = config.frequency;
    const std::size_t step = m_recording.frequency() / config.frequency;
    const std::size_t pre = config.preSamples, post = config.postSamples;

    if (!config.trigger) {
        r.buffer = extract(config.channels, 0, post, 0);
        report(CaptureStage::Done);
        return finish(CaptureStatus::Ok);
    }

    r.triggerUsed = config.trigger;
    r.edgeSensitive = !config.trigger->isPattern();  // edge replay searches for a real transition
    report(CaptureStage::WaitingForTrigger);
    const std::size_t lastStart = m_recording.size() - post * step;  // trigger sample + post must fit
    std::vector<std::size_t> starts;
    if (config.trigger->isPattern()) {
        std::vector<std::size_t> trigIndices;
        for (uint8_t k = 0; k < config.trigger->bitCount; ++k)
            trigIndices.push_back(*sourceIndex(config.trigger->channel + k));
        const std::size_t from = std::max<std::size_t>(m_cursor, pre * step);
        for (std::size_t i = from; i <= lastStart; ++i) {
            if ((i & 0xFFFF) == 0 && m_cancelRequested)
                break;
            bool match = true;
            for (uint8_t k = 0; k < config.trigger->bitCount; ++k) {
                const bool want = ((config.trigger->pattern >> k) & 1u) != 0;
                if (m_recording.bit(i, trigIndices[k]) != want) {
                    match = false;
                    break;
                }
            }
            if (match) {
                starts.push_back(i);
                break;
            }
        }
    } else {
        const std::size_t trigIdx = *sourceIndex(config.trigger->channel);
        const bool rising = !config.trigger->inverted;
        // Like the firmware: window k > 0 starts on the next trigger edge after window k - 1 (the line must first
        // return to the opposite level), and only complete series are delivered.
        auto nextEdge = [&](std::size_t from) -> std::optional<std::size_t> {
            for (std::size_t i = std::max<std::size_t>(from, 1); i <= lastStart; ++i) {
                if ((i & 0xFFFF) == 0 && m_cancelRequested)
                    return std::nullopt;
                const bool before = m_recording.bit(i - 1, trigIdx), now = m_recording.bit(i, trigIdx);
                if (before != now && now == rising)
                    return i;
            }
            return std::nullopt;
        };
        std::size_t from = std::max<std::size_t>({m_cursor, pre * step, 1});
        while (starts.size() < config.segments) {
            const auto edge = nextEdge(from);
            if (!edge)
                break;
            starts.push_back(*edge);
            from = *edge + post * step;
        }
    }
    if (m_cancelRequested)
        return finish(CaptureStatus::Cancelled, "capture cancelled");
    if (starts.size() < config.segments) {
        if (config.probeOnTimeout) {
            const std::size_t n = std::min<std::size_t>(m_recording.size() / step, 1000);
            r.lineDump = extract(config.channels, m_recording.size() - n * step, n, 0);
        }
        m_cursor = m_recording.size();
        std::string what = config.trigger->isPattern()
                               ? "pattern " + config.trigger->describe() + " not found in the recording"
                               : std::string("no further ") + (!config.trigger->inverted ? "rising" : "falling") +
                                     " edge on CH" + std::to_string(config.trigger->channel + 1) + " in the recording";
        if (config.segments > 1)
            what += " (" + std::to_string(starts.size()) + " of " + std::to_string(config.segments) + " windows found)";
        return finish(CaptureStatus::Timeout, what);
    }

    report(CaptureStage::Downloading);
    auto buffer = extract(config.channels, starts.front() - pre * step, pre + post, static_cast<int64_t>(pre));
    if (config.segments > 1) {
        std::vector<uint32_t> all = buffer.samples();
        all.reserve(pre + static_cast<std::size_t>(post) * config.segments);
        for (std::size_t k = 1; k < starts.size(); ++k) {
            const auto win = extract(config.channels, starts[k], post, 0);
            all.insert(all.end(), win.samples().begin(), win.samples().end());
        }
        const bool exact = buffer.exactTiming();
        buffer = CaptureBuffer(buffer.channels(), buffer.frequency(), static_cast<uint32_t>(pre), std::move(all));
        buffer.setExactTiming(exact);
        // The recording knows exactly when every window started.
        std::vector<double> times;
        for (const auto s : starts)
            times.push_back(static_cast<double>(s - starts.front()) / m_recording.frequency());
        r.segments = SegmentLayout{config.segments,          static_cast<uint32_t>(pre),
                                   static_cast<uint32_t>(post), config.trigger->channel,
                                   config.trigger->inverted, std::move(times),
                                   1.0 / m_recording.frequency()};
    }
    r.buffer = std::move(buffer);
    m_cursor = starts.back() + post * step;
    report(CaptureStage::Done);
    return finish(CaptureStatus::Ok);
}

ProbeResult ReplayDevice::probe(const std::vector<ChannelInfo>& channels, milliseconds window, uint32_t frequency)
{
    CaptureConfig c;
    c.channels = channels;
    c.frequency = frequency > 0 && frequency <= m_info.maxFrequency && m_info.maxFrequency % frequency == 0
                      ? frequency
                      : m_info.maxFrequency;
    const uint64_t available = m_info.maxFrequency ? m_recording.size() / (m_info.maxFrequency / c.frequency) : 0;
    const uint64_t wanted = static_cast<uint64_t>(window.count()) * c.frequency / 1000;
    c.preSamples = 0;
    c.postSamples = static_cast<uint32_t>(std::clamp<uint64_t>(wanted, 1, std::max<uint64_t>(available, 1)));
    c.probeOnTimeout = false;

    auto r = capture(c);
    ProbeResult pr;
    pr.status = r.status;
    pr.error = r.error;
    pr.attempts = r.attempts;
    pr.elapsed = r.elapsed;
    pr.buffer = std::move(r.buffer);
    return pr;
}

bool isReplayPort(std::string_view port) noexcept
{
    return port == ReplayDevice::kDemoPort || port.starts_with(ReplayDevice::kScheme);
}

}  // namespace lt

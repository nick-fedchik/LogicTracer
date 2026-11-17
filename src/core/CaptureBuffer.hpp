// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/DeviceProtocol.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lt {

struct ChannelInfo {
    uint8_t number = 0;  // 0-based hardware channel
    std::string name;    // user label (may be empty)

    // 1-based number as printed on the board / used by the CLI.
    [[nodiscard]] int displayNumber() const noexcept { return number + 1; }
    [[nodiscard]] std::string label() const;
};

// Samples as delivered by the firmware: bit i of every sample belongs to channels[i]
// (the firmware reorders GPIO bits into request order).
class CaptureBuffer {
public:
    CaptureBuffer() = default;
    CaptureBuffer(std::vector<ChannelInfo> channels, uint32_t frequency, uint32_t preSamples,
                  std::vector<uint32_t> samples);

    // Decodes the raw little-endian sample stream (1/2/4 bytes per sample).
    static CaptureBuffer fromRaw(std::vector<ChannelInfo> channels, uint32_t frequency, uint32_t preSamples,
                                 CaptureMode mode, std::span<const uint8_t> raw);

    [[nodiscard]] const std::vector<ChannelInfo>& channels() const noexcept { return m_channels; }
    [[nodiscard]] const std::vector<uint32_t>& samples() const noexcept { return m_samples; }
    [[nodiscard]] std::size_t size() const noexcept { return m_samples.size(); }
    [[nodiscard]] uint32_t frequency() const noexcept { return m_frequency; }
    // Pre-trigger sample count (0 when the trigger lies before the first sample).
    [[nodiscard]] uint32_t preSamples() const noexcept
    {
        return m_triggerIndex > 0 ? static_cast<uint32_t>(m_triggerIndex) : 0;
    }
    // Sample index of t = 0. Signed: a slice or a trimmed buffer may start after the trigger.
    [[nodiscard]] int64_t triggerIndex() const noexcept { return m_triggerIndex; }
    void setTriggerIndex(int64_t index) noexcept { m_triggerIndex = index; }
    [[nodiscard]] double samplePeriodSeconds() const noexcept { return m_frequency ? 1.0 / m_frequency : 0.0; }
    // True when the samples are taken in the clock domain of the signals (in-design capture without a
    // synchronizer): edge positions are exact clock cycles, not ±1 sample of an asynchronous sampler.
    [[nodiscard]] bool exactTiming() const noexcept { return m_exactTiming; }
    void setExactTiming(bool exact) noexcept { m_exactTiming = exact; }

    [[nodiscard]] bool bit(std::size_t sample, std::size_t channelIndex) const noexcept
    {
        return (m_samples[sample] >> channelIndex) & 1u;
    }
    // Time of a sample relative to the trigger (sample index == preSamples is t = 0).
    [[nodiscard]] double timeOfSample(std::size_t sample) const noexcept
    {
        return (static_cast<double>(sample) - static_cast<double>(m_triggerIndex)) * samplePeriodSeconds();
    }
    // Returns a copy without the first `count` samples (pre-trigger origin is adjusted).
    [[nodiscard]] CaptureBuffer dropFront(std::size_t count) const;
    // Samples [first, first + count) clamped to the buffer; t = 0 stays at the original trigger.
    [[nodiscard]] CaptureBuffer slice(std::size_t first, std::size_t count) const;

private:
    std::vector<ChannelInfo> m_channels;
    uint32_t m_frequency = 0;
    int64_t m_triggerIndex = 0;
    bool m_exactTiming = false;
    std::vector<uint32_t> m_samples;
};

}  // namespace lt

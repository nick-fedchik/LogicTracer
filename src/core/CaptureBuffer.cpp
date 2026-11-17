// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/CaptureBuffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace lt {

std::string ChannelInfo::label() const
{
    return name.empty() ? "CH" + std::to_string(displayNumber()) : name;
}

CaptureBuffer::CaptureBuffer(std::vector<ChannelInfo> channels, uint32_t frequency, uint32_t preSamples,
                             std::vector<uint32_t> samples)
    : m_channels(std::move(channels)), m_frequency(frequency), m_triggerIndex(preSamples),
      m_samples(std::move(samples))
{
    if (m_channels.size() > 32)
        throw std::invalid_argument("CaptureBuffer supports at most 32 channels");
    const uint32_t mask = m_channels.size() >= 32 ? 0xFFFFFFFFu : ((1u << m_channels.size()) - 1u);
    for (auto& s : m_samples)
        s &= mask;
}

CaptureBuffer CaptureBuffer::fromRaw(std::vector<ChannelInfo> channels, uint32_t frequency, uint32_t preSamples,
                                     CaptureMode mode, std::span<const uint8_t> raw)
{
    const std::size_t bps = bytesPerSample(mode);
    const std::size_t count = raw.size() / bps;
    std::vector<uint32_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        uint32_t v = 0;
        for (std::size_t b = 0; b < bps; ++b)
            v |= static_cast<uint32_t>(raw[i * bps + b]) << (8 * b);
        samples[i] = v;
    }
    return CaptureBuffer(std::move(channels), frequency, preSamples, std::move(samples));
}

CaptureBuffer CaptureBuffer::dropFront(std::size_t count) const
{
    return slice(count, m_samples.size());
}

CaptureBuffer CaptureBuffer::slice(std::size_t first, std::size_t count) const
{
    first = std::min(first, m_samples.size());
    count = std::min(count, m_samples.size() - first);
    const auto begin = m_samples.begin() + static_cast<std::ptrdiff_t>(first);
    CaptureBuffer out(m_channels, m_frequency, 0,
                      std::vector<uint32_t>(begin, begin + static_cast<std::ptrdiff_t>(count)));
    out.m_triggerIndex = m_triggerIndex - static_cast<int64_t>(first);
    out.m_exactTiming = m_exactTiming;
    return out;
}

}  // namespace lt

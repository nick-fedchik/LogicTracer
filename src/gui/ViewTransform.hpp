// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <algorithm>
#include <cstddef>

namespace lt::gui {

// Maps sample indices to horizontal pixels for the waveform view. Pure arithmetic (no Qt), so the
// zoom/pan rules are unit-testable. x = 0 is the left edge of the plot area.
//
// Invariants: the view never shows less than 1/kMaxPixelsPerSample of a sample per pixel and never
// shows more than the whole capture; it never scrolls past either end.
class ViewTransform {
public:
    static constexpr double kMaxPixelsPerSample = 64.0;

    void setTotalSamples(std::size_t total)
    {
        m_total = total;
        clamp();
    }
    void setWidth(double pixels)
    {
        m_width = std::max(1.0, pixels);
        clamp();
    }

    [[nodiscard]] double start() const noexcept { return m_start; }
    [[nodiscard]] double samplesPerPixel() const noexcept { return m_spp; }
    [[nodiscard]] double visibleSamples() const noexcept { return m_spp * m_width; }
    [[nodiscard]] std::size_t total() const noexcept { return m_total; }
    // True while the whole capture is shown (maximum zoom-out).
    [[nodiscard]] bool isFit() const noexcept { return m_spp >= fitSpp() * 0.999; }

    [[nodiscard]] double sampleAt(double x) const noexcept { return m_start + x * m_spp; }
    [[nodiscard]] double xAt(double sample) const noexcept { return (sample - m_start) / m_spp; }

    // Visible sample indices as [first, end) clamped to the capture.
    [[nodiscard]] std::size_t firstVisible() const noexcept
    {
        return static_cast<std::size_t>(std::clamp(m_start, 0.0, static_cast<double>(m_total)));
    }
    [[nodiscard]] std::size_t endVisible() const noexcept
    {
        const double end = std::min(static_cast<double>(m_total), m_start + visibleSamples());
        return static_cast<std::size_t>(std::max(0.0, end) + 0.999999);
    }

    void fitAll()
    {
        m_start = 0.0;
        m_spp = fitSpp();
        clamp();
    }

    // factor > 1 zooms in. The sample under anchorX stays under the pointer.
    void zoomAt(double factor, double anchorX)
    {
        if (factor <= 0.0)
            return;
        const double anchorSample = sampleAt(anchorX);
        m_spp /= factor;
        clampSpp();
        m_start = anchorSample - anchorX * m_spp;
        clamp();
    }

    // Dragging content by dx pixels to the right reveals earlier samples.
    void panByPixels(double dx)
    {
        m_start -= dx * m_spp;
        clamp();
    }

    void showRange(double firstSample, double endSample)
    {
        const double span = std::max(1.0, endSample - firstSample);
        m_spp = span / m_width;
        clampSpp();
        m_start = firstSample;
        clamp();
    }

private:
    [[nodiscard]] double fitSpp() const noexcept
    {
        return m_total == 0 ? 1.0 : std::max(1.0 / kMaxPixelsPerSample, static_cast<double>(m_total) / m_width);
    }
    void clampSpp() { m_spp = std::clamp(m_spp, 1.0 / kMaxPixelsPerSample, fitSpp()); }
    void clamp()
    {
        clampSpp();
        const double maxStart = std::max(0.0, static_cast<double>(m_total) - visibleSamples());
        m_start = std::clamp(m_start, 0.0, maxStart);
    }

    std::size_t m_total = 0;
    double m_width = 1.0;
    double m_start = 0.0;
    double m_spp = 1.0;
};

}  // namespace lt::gui

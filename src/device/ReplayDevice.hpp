// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// A recorded capture (lossless JSON, VCD or legacy .lac) presented as a capture device.
//
// Port syntax: "replay:<path>". The recording defines the channels, the sample rate and the buffer
// size. A triggered capture searches the recording for the next real *edge* on the trigger channel
// (rising: 0 -> 1, falling: 1 -> 0) after the previous capture, so consecutive captures replay
// consecutive events; when no further edge exists the capture ends with TIMEOUT. Immediate captures
// and probes start at the beginning of the recording. Useful for tests, demos, CI and offline
// review of a HIL run with the same tools that drive real hardware.

#include "device/IDevice.hpp"

#include <atomic>

namespace lt {

class ReplayDevice final : public IDevice {
public:
    static constexpr std::string_view kScheme = "replay:";
    static constexpr std::string_view kDemoPort = "demo:pico";
    [[nodiscard]] static CaptureBuffer demoRecording();

    ReplayDevice() = default;

    bool open(const std::string& port, std::chrono::milliseconds handshakeTimeout) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override { return m_open; }
    [[nodiscard]] const DeviceInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_error; }

    void requestCancel() noexcept override { m_cancelRequested = true; }
    void setProgressCallback(ProgressCallback callback) override { m_progress = std::move(callback); }
    bool enterBootloader() override;

    [[nodiscard]] std::optional<std::string> validate(const CaptureConfig& config, CaptureField* field = nullptr) const override;
    [[nodiscard]] CaptureResult capture(const CaptureConfig& config) override;
    [[nodiscard]] ProbeResult probe(const std::vector<ChannelInfo>& channels, std::chrono::milliseconds window,
                                    uint32_t frequency) override;

    // Restarts trigger searches from the beginning of the recording.
    void rewind() noexcept { m_cursor = 0; }

private:
    // Copies [first, first + count) of the recording, keeping only `channels` (in that order).
    [[nodiscard]] CaptureBuffer extract(const std::vector<ChannelInfo>& channels, std::size_t first,
                                        std::size_t count, int64_t triggerIndex) const;
    [[nodiscard]] std::optional<std::size_t> sourceIndex(uint8_t channelNumber) const;
    void report(CaptureStage stage) const
    {
        if (m_progress)
            m_progress(stage);
    }

    CaptureBuffer m_recording;
    std::vector<std::string> m_warnings;  // import caveats of the recording, attached to every capture
    DeviceInfo m_info;
    std::string m_error;
    bool m_open = false;
    std::size_t m_cursor = 0;  // next trigger search starts here
    uint32_t m_captureFrequency = 0;  // rate of the capture being extracted
    std::atomic<bool> m_cancelRequested{false};
    ProgressCallback m_progress;
};

}  // namespace lt

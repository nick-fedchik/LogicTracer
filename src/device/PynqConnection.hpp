// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Capture device backed by the in-design FPGA core lt_capture, reached through the board agent
// (scripts/rigs/pynq-z1/logictracer_agent.py) over TCP: port "pynq://host[:port]" (default port 7878).
//
// The core records in the clock domain of the design under test, so the sample rate is the DUT clock and
// edge times are exact clock cycles. Its edge records are expanded into an ordinary CaptureBuffer.

#include "device/IDevice.hpp"

#include <atomic>
#include <memory>

class QTcpSocket;

namespace lt {

class PynqConnection final : public IDevice {
public:
    static constexpr std::string_view kScheme = "pynq://";
    static constexpr uint16_t kDefaultPort = 7878;
    // Expanded captures are kept below this many samples (memory of the dense CaptureBuffer).
    static constexpr uint32_t kMaxSamples = 4u * 1024u * 1024u;

    PynqConnection();
    ~PynqConnection() override;

    bool open(const std::string& port, std::chrono::milliseconds handshakeTimeout) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] const DeviceInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_error; }

    void requestCancel() noexcept override { m_cancelRequested = true; }
    void setProgressCallback(ProgressCallback callback) override { m_progress = std::move(callback); }
    bool enterBootloader() override;

    [[nodiscard]] std::optional<std::string> validate(const CaptureConfig& config, CaptureField* field = nullptr) const override;
    [[nodiscard]] CaptureResult capture(const CaptureConfig& config) override;
    [[nodiscard]] ProbeResult probe(const std::vector<ChannelInfo>& channels, std::chrono::milliseconds window,
                                    uint32_t frequency) override;

private:
    // One request/response line; nullopt (and m_error) on a transport failure or timeout.
    [[nodiscard]] std::optional<std::string> call(const std::string& requestJson, std::chrono::milliseconds timeout,
                                                  bool cancellable);
    void sendCancel();
    void report(CaptureStage stage) const
    {
        if (m_progress)
            m_progress(stage);
    }

    std::unique_ptr<QTcpSocket> m_socket;
    std::string m_host;
    uint16_t m_port = kDefaultPort;
    DeviceInfo m_info;
    std::vector<std::string> m_probeNames;
    std::string m_error;
    std::atomic<bool> m_cancelRequested{false};
    ProgressCallback m_progress;
};

[[nodiscard]] bool isPynqPort(std::string_view port) noexcept;

}  // namespace lt

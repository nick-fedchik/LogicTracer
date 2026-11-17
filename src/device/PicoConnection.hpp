// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/CaptureTypes.hpp"
#include "core/DeviceProtocol.hpp"
#include "device/IDevice.hpp"

#include <QByteArray>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class QSerialPort;

namespace lt {

using namespace std::chrono_literals;

// RP2040/RP2350 running the LogicTracer or upstream LogicAnalyzer firmware, over USB CDC serial.
class PicoConnection final : public IDevice {
public:
    static constexpr uint16_t kVendorId = 0x1209;
    static constexpr uint16_t kProductId = 0x3020;
    // Firmware main loop sleeps 2 x 1 s between "capture finished?" checks.
    static constexpr std::chrono::milliseconds kFirmwarePollLatency = 2200ms;

    PicoConnection();
    ~PicoConnection() override;

    // Opens the port and performs the ID handshake (VERSION, FREQ, BLASTFREQ, BUFFER, CHANNELS).
    bool open(const std::string& port, std::chrono::milliseconds handshakeTimeout = 3000ms) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] const DeviceInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_error; }

    // Asks the running capture()/probe() to stop; safe to call from any thread. A capture that is
    // waiting for its trigger is aborted on the device and finishes with CaptureStatus::Cancelled.
    // The request stays pending until the operation ends, so call it only while one is running.
    void requestCancel() noexcept override { m_cancelRequested = true; }

    void setProgressCallback(ProgressCallback callback) override { m_progress = std::move(callback); }

    // Firmware completion-poll period used for all timeouts (default auto-detected from firmware version).
    // Adjustable for emulators / non-USB transports.
    void setPollLatency(std::chrono::milliseconds latency) noexcept override
    {
        m_pollLatency = latency;
        m_pollLatencyOverridden = true;
    }
    [[nodiscard]] std::chrono::milliseconds pollLatency() const noexcept { return m_pollLatency; }

    // Reboots the RP2040 into USB UF2 bootloader mode (RPI-RP2 mass storage) via protocol command 4.
    bool enterBootloader() override;
    // LogicTracer firmware with FEATURES bit 2: pulse pairs on GP0/GP1 (protocol command 7).
    [[nodiscard]] std::optional<TestSignalInfo> setTestSignal(const TestSignalRequest& request,
                                                              std::string& error) override;

    [[nodiscard]] std::optional<std::string> validate(const CaptureConfig& config) const override;
    [[nodiscard]] CaptureResult capture(const CaptureConfig& config) override;

    // Short untriggered snapshot (default 10 ms @ 1 MHz) of the current pin levels.
    [[nodiscard]] ProbeResult probe(const std::vector<ChannelInfo>& channels,
                                    std::chrono::milliseconds window = 10ms, uint32_t frequency = 1'000'000) override;

    // Lists serial ports with the capture-device VID/PID (and, optionally, every ttyACM*).
    [[nodiscard]] static std::vector<DetectedPort> enumerate(bool includeAllAcm = false);

private:
    struct Attempt {
        CaptureStatus status = CaptureStatus::Error;
        std::string error;
        std::optional<CaptureBuffer> buffer;
        std::vector<uint32_t> timestamps;  // raw burst timestamps (measure)
        std::optional<uint32_t> recorded;  // samples holding recorded levels (FEATURES bit 3)
    };

    bool handshake(std::chrono::milliseconds timeout);
    static void alignTriggerToEdge(CaptureBuffer& buffer, const EdgeTrigger& trigger);
    Attempt runCapture(const CaptureRequestPacket& req, const std::vector<ChannelInfo>& channels,
                       std::chrono::milliseconds triggerWait, bool waitPreFill);
    void reportProgress(CaptureStage stage) const
    {
        if (m_progress)
            m_progress(stage);
    }
    CaptureRequestPacket buildRequest(const CaptureConfig& config, const EdgeTrigger& trigger,
                                      bool edgeSensitive = false) const;

    [[nodiscard]] bool portFailed() const;
    bool writeAll(std::span<const uint8_t> data);
    std::optional<std::string> readLine(std::chrono::steady_clock::time_point deadline);
    // honorCancel: abort the wait (return false) as soon as requestCancel() is called.
    bool waitForBytes(qsizetype count, std::chrono::steady_clock::time_point deadline, bool honorCancel = false);
    bool readExact(qsizetype count, QByteArray& out, std::chrono::milliseconds inactivity);
    void drain(std::chrono::milliseconds quietPeriod, std::chrono::milliseconds maxTime = 3000ms);
    enum class CancelOutcome { DataArrived, Cancelled, PortError };
    CancelOutcome cancelCapture();

    std::unique_ptr<QSerialPort> m_port;
    QByteArray m_rx;
    DeviceInfo m_info;
    std::string m_error;
    std::chrono::milliseconds m_pollLatency = kFirmwarePollLatency;
    bool m_pollLatencyOverridden = false;
    std::atomic<bool> m_cancelRequested{false};
    ProgressCallback m_progress;
    bool m_portLost = false;  // the peer hung up / vanished (no QSerialPort error is raised for it)
};

}  // namespace lt

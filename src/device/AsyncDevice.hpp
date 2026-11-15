// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "device/IDevice.hpp"

#include <QObject>
#include <QString>

#include <chrono>
#include <vector>

class QThread;

namespace lt {

class AsyncWorker;  // defined in AsyncDevice.cpp

// Event-loop friendly front-end for any IDevice (serial RP2040 or replay), shared by the GUI, daemon
// and MCP server.
//
// The blocking serial work runs on a private worker thread (the QSerialPort is created there);
// results come back as queued signals on the thread that owns this object. One operation runs at
// a time: start*() returns false while busy. requestCancel() may be called from any thread.
class AsyncDevice : public QObject {
    Q_OBJECT

public:
    explicit AsyncDevice(QObject* parent = nullptr);
    ~AsyncDevice() override;

    [[nodiscard]] bool isBusy() const noexcept { return m_busy; }
    [[nodiscard]] bool isConnected() const noexcept { return m_connected; }
    // Handshake data of the connected device (valid while isConnected()).
    [[nodiscard]] const DeviceInfo& info() const noexcept { return m_info; }

    // Applies to connections opened afterwards (see IDevice::setPollLatency).
    void setPollLatency(std::chrono::milliseconds latency) noexcept { m_pollLatencyOverride = latency; }

public Q_SLOTS:
    // Connects and runs the handshake; emits opened(). Returns false if busy.
    bool open(const QString& port, int handshakeTimeoutMs = 3000);
    void close();
    bool startCapture(const lt::CaptureConfig& config);
    bool startProbe(const std::vector<lt::ChannelInfo>& channels, int windowMs = 10, uint32_t frequency = 1'000'000);
    // Aborts the running capture/probe (finishes with CaptureStatus::Cancelled). No-op when idle.
    void requestCancel();

Q_SIGNALS:
    void opened(bool ok, const QString& error);
    void closed();
    void progress(int stage);  // lt::CaptureStage
    void captureFinished(const lt::CaptureResult& result);
    void probeFinished(const lt::ProbeResult& result);
    void busyChanged(bool busy);

private Q_SLOTS:
    void onOpened(bool ok, const QString& error, const lt::DeviceInfo& info);
    void onCaptureFinished(const lt::CaptureResult& result);
    void onProbeFinished(const lt::ProbeResult& result);

private:
    void setBusy(bool busy);

    QThread* m_thread = nullptr;
    AsyncWorker* m_worker = nullptr;
    DeviceInfo m_info;
    std::optional<std::chrono::milliseconds> m_pollLatencyOverride;
    bool m_busy = false;
    bool m_connected = false;
};

}  // namespace lt

Q_DECLARE_METATYPE(lt::DeviceInfo)
Q_DECLARE_METATYPE(lt::CaptureConfig)
Q_DECLARE_METATYPE(lt::CaptureResult)
Q_DECLARE_METATYPE(lt::ProbeResult)

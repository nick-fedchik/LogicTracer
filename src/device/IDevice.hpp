// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Abstract capture device. Front-ends (CLI, GUI through AsyncDevice, MCP) only use this interface,
// so new capture hardware or a recorded trace can be plugged in next to the RP2040 serial driver.
//
// Threading: a device is used from one thread at a time, except requestCancel(), which may be
// called from any thread while capture()/probe() is running.

#include "core/CaptureTypes.hpp"
#include "core/DeviceProtocol.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

class IDevice {
public:
    using ProgressCallback = std::function<void(CaptureStage)>;

    virtual ~IDevice() = default;
    IDevice(const IDevice&) = delete;
    IDevice& operator=(const IDevice&) = delete;

    // Opens the device and reads its capabilities into info().
    virtual bool open(const std::string& port, std::chrono::milliseconds handshakeTimeout) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual const DeviceInfo& info() const noexcept = 0;
    [[nodiscard]] virtual const std::string& lastError() const noexcept = 0;

    // Asks the running capture()/probe() to stop (any thread).
    virtual void requestCancel() noexcept = 0;
    virtual void setProgressCallback(ProgressCallback callback) = 0;
    // Completion-poll latency added to trigger timeouts; only meaningful for polling transports.
    virtual void setPollLatency(std::chrono::milliseconds /*latency*/) noexcept {}
    // Reboots into the firmware update mode, if the device has one.
    virtual bool enterBootloader() = 0;

    [[nodiscard]] virtual std::optional<std::string> validate(const CaptureConfig& config) const = 0;
    [[nodiscard]] virtual CaptureResult capture(const CaptureConfig& config) = 0;
    // Short untriggered snapshot of the current pin levels.
    [[nodiscard]] virtual ProbeResult probe(const std::vector<ChannelInfo>& channels,
                                            std::chrono::milliseconds window = std::chrono::milliseconds(10),
                                            uint32_t frequency = 1'000'000) = 0;

protected:
    IDevice() = default;
};

// Port syntax selects the implementation:
//   "replay:<file>"   ReplayDevice: a saved capture (.json, .vcd, .lac) acting as a device
//   anything else     PicoConnection: RP2040/RP2350 serial device (e.g. /dev/ttyACM0)
[[nodiscard]] bool isReplayPort(std::string_view port) noexcept;
[[nodiscard]] std::unique_ptr<IDevice> createDevice(std::string_view port);

}  // namespace lt

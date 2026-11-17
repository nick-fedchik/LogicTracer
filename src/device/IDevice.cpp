// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/IDevice.hpp"

#include "device/PicoConnection.hpp"
#include "device/PynqConnection.hpp"
#include "device/ReplayDevice.hpp"

namespace lt {

std::unique_ptr<IDevice> createDevice(std::string_view port)
{
    if (isReplayPort(port))
        return std::make_unique<ReplayDevice>();
    if (isPynqPort(port))
        return std::make_unique<PynqConnection>();
    return std::make_unique<PicoConnection>();
}

}  // namespace lt

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/CaptureTypes.hpp"

namespace lt {

std::string_view toString(CaptureStatus status) noexcept
{
    switch (status) {
    case CaptureStatus::Ok: return "OK";
    case CaptureStatus::Timeout: return "TIMEOUT";
    case CaptureStatus::Error: return "ERROR";
    case CaptureStatus::Cancelled: return "CANCELLED";
    }
    return "ERROR";
}

}  // namespace lt

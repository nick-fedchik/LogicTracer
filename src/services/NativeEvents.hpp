// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "services/EventRecording.hpp"

#ifdef __linux__
namespace lt::events {
// Reads a bound, timestamp-enabled CAN socket or a requested GPIO v2 line. The caller owns the fd.
Result collectNativeEvents(int fd, const Request& request, const Cancel& cancel);
}  // namespace lt::events
#endif

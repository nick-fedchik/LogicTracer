// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "services/EventRecording.hpp"

class QWidget;

namespace lt::gui {
void showEventRecorder(QWidget* parent, nlohmann::json recording = nullptr);
}

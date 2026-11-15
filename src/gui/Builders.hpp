// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/CaptureTypes.hpp"

#include <QString>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QFrame;
class QLineEdit;
class QWidget;

namespace lt::gui {

// Visual builders for the text fields of the capture bar. Each one is a small non-modal popup that
// writes the canonical string into `target` on Apply; the field itself stays directly editable.
// The popups delete themselves when closed. Channel numbers are 1-based.

// "1:CLK,2,4-6" from (channel, optional name) pairs; unnamed runs of 3+ become ranges.
[[nodiscard]] QString composeChannelList(const std::vector<std::pair<int, QString>>& channels);
// "none" (channel 0) or "<ch>:rising|falling".
[[nodiscard]] QString composeTrigger(int channel, bool falling);
// "1..2" when both edges are rising, else "1:falling..2:rising".
[[nodiscard]] QString composeLatency(int stimChannel, bool stimFalling, int reactChannel, bool reactFalling);

// `board` (e.g. "PICO") adds the GPIO behind each channel; empty = no pin hints.
QFrame* showChannelsBuilder(QWidget* anchor, QLineEdit* target, const std::string& board, int maxChannels);
QFrame* showTriggerBuilder(QWidget* anchor, QLineEdit* target, const std::vector<int>& channels);
QFrame* showLatencyBuilder(QWidget* anchor, QLineEdit* target, const std::vector<int>& channels);

// Non-modal key/value editor for the test-setup metadata. Apply validates every row and calls `apply`
// with the cleaned list; invalid rows are reported inline and block Apply.
QFrame* showSetupEditor(QWidget* parent, const lt::SetupInfo& current,
                        const std::function<void(const lt::SetupInfo&)>& apply);

}  // namespace lt::gui

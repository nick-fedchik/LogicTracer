// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <QString>

namespace lt::gui {

struct CaptureFields {
    QString channels, rate, samples, trigger;
    int windows, timeout;

    static CaptureFields defaults(bool demo)
    {
        return {demo ? "1:CLK,2:ECHO,3:BURST,4:IDLE_HI" : "1-8", "1M", demo ? "1k" : "10k", "none", 1, 5000};
    }
};

}  // namespace lt::gui

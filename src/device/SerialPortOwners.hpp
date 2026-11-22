// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <QHash>
#include <QStringList>

namespace lt {

using SerialPortOwners = QHash<QString, QString>;

// Best-effort snapshot of other processes holding these ports. An absent entry does not mean free.
// Does not open the device. Call off the GUI thread.
[[nodiscard]] SerialPortOwners serialPortOwners(const QStringList& ports);

}  // namespace lt

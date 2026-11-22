// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "device/SerialPortOwners.hpp"

#include <QComboBox>
#include <QFutureWatcher>
#include <QTimer>

namespace lt::gui {

class SerialPortPicker : public QComboBox {
    Q_OBJECT

public:
    SerialPortPicker();
    QString port() const;
    QString owner() const { return m_owners.value(port()); }
    void refresh();
    void refreshOwners();

Q_SIGNALS:
    void availabilityChanged();

private:
    void applyOwners();
    SerialPortOwners m_owners;
    QFutureWatcher<SerialPortOwners> m_scan;
    QTimer m_delay;
    bool m_scanning = false;
    bool m_rescan = false;
};

}  // namespace lt::gui

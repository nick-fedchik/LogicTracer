// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/SerialPortPicker.hpp"
#include "device/PicoConnection.hpp"
#include "device/ReplayDevice.hpp"

#include <QLineEdit>
#include <QSignalBlocker>
#include <QtConcurrentRun>
#include <utility>

namespace lt::gui {

SerialPortPicker::SerialPortPicker()
{
    setObjectName("port");
    setEditable(true);
    setMinimumWidth(220);
    m_delay.setSingleShot(true);
    m_delay.setInterval(150);
    connect(this, &QComboBox::editTextChanged, this, [this] {
        Q_EMIT availabilityChanged();
        m_delay.start();
    });
    connect(&m_delay, &QTimer::timeout, this, &SerialPortPicker::refreshOwners);
    connect(&m_scan, &QFutureWatcher<SerialPortOwners>::finished, this, [this] {
        m_scanning = false;
        if (std::exchange(m_rescan, false)) {
            refreshOwners();
            return;
        }
        m_owners = m_scan.result();
        applyOwners();
        Q_EMIT availabilityChanged();
    });
}

QString SerialPortPicker::port() const
{
    const QString data = currentData().toString();
    return !data.isEmpty() && currentText() == itemText(currentIndex()) ? data : currentText().trimmed();
}

void SerialPortPicker::refresh()
{
    const QString previous = port();
    const bool typed = currentText() != itemText(currentIndex()) || currentData().toString().isEmpty();
    {
        const QSignalBlocker block(this);
        clear();
        int preferred = -1;
        for (const auto& p : PicoConnection::enumerate(true)) {
            addItem(QString("%1  (%2)").arg(QString::fromStdString(p.port), QString::fromStdString(p.description)),
                    QString::fromStdString(p.port));
            if (p.vidPidMatch && preferred < 0)
                preferred = count() - 1;
        }
        addItem(tr("Demo Pico (simulated)"), QString::fromLatin1(ReplayDevice::kDemoPort.data()));
        if (const int found = findData(previous); found >= 0)
            setCurrentIndex(found);
        else if (typed && !previous.isEmpty())
            setEditText(previous);
        else if (preferred >= 0)
            setCurrentIndex(preferred);
    }
    applyOwners();
    Q_EMIT availabilityChanged();
    refreshOwners();
}

void SerialPortPicker::refreshOwners()
{
#ifdef Q_OS_LINUX
    m_delay.stop();
    if (m_scanning) {
        m_rescan = true;
        return;
    }
    QStringList ports{port()};
    for (int i = 0; i < count(); ++i)
        ports << itemData(i).toString();
    ports.removeDuplicates();
    m_scanning = true;
    m_scan.setFuture(QtConcurrent::run(serialPortOwners, ports));
#endif
}

void SerialPortPicker::applyOwners()
{
    const QSignalBlocker block(this);
    const auto edit = currentText();
    const bool typed = edit != itemText(currentIndex());
    const int cursor = lineEdit()->cursorPosition();
    for (int i = 0; i < count(); ++i) {
        constexpr int baseLabel = Qt::UserRole + 1;
        if (!itemData(i, baseLabel).isValid())
            setItemData(i, itemText(i), baseLabel);
        const QString base = itemData(i, baseLabel).toString();
        const QString holder = m_owners.value(itemData(i).toString());
        const QString label = holder.isEmpty() ? base : tr("%1 [in use]").arg(base);
        if (itemText(i) != label)
            setItemText(i, label);
        const QString tip = holder.isEmpty() ? QString{} : QStringLiteral("<p>%1</p>").arg(holder.toHtmlEscaped());
        if (itemData(i, Qt::ToolTipRole).toString() != tip)
            setItemData(i, tip, Qt::ToolTipRole);
    }
    if (typed && currentText() != edit) {
        setEditText(edit);
        lineEdit()->setCursorPosition(cursor);
    }
}

}  // namespace lt::gui

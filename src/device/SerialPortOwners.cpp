// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/SerialPortOwners.hpp"

#ifdef Q_OS_LINUX
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QScopeGuard>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lt {

SerialPortOwners serialPortOwners(const QStringList& ports)
{
    SerialPortOwners owners;
#ifdef Q_OS_LINUX
    struct Target {
        struct stat device;
        QStringList aliases;
    };
    QHash<QByteArray, Target> targets;
    for (const auto& port : ports) {
        if (port.isEmpty() || (!port.startsWith('/') && port.contains(':')))
            continue;  // demo, replay and network transports have no serial descriptor
        const auto path = QFileInfo(port.startsWith('/') ? port : "/dev/" + port).canonicalFilePath();
        const auto name = QFile::encodeName(path);
        struct stat st{};
        if (!name.isEmpty() && ::stat(name.constData(), &st) == 0 && S_ISCHR(st.st_mode)) {
            auto& target = targets[name];
            target.device = st;
            target.aliases << port;
        }
    }
    if (targets.isEmpty())
        return owners;

    const QString self = QString::number(::getpid());
    const auto pids = QDir("/proc").entryList({"[0-9]*"}, QDir::Dirs, QDir::Unsorted);
    for (const auto& pid : pids) {
        if (pid == self)
            continue;
        const QString base = "/proc/" + pid;
        DIR* fds = ::opendir(QFile::encodeName(base + "/fd").constData());
        if (!fds)
            continue;  // another user's descriptors may be hidden
        const auto close = qScopeGuard([fds] { ::closedir(fds); });
        while (const auto* fd = ::readdir(fds)) {
            if (fd->d_name[0] == '.')
                continue;
            char link[PATH_MAX];
            const auto len = ::readlinkat(::dirfd(fds), fd->d_name, link, sizeof(link));
            if (len <= 0 || len == sizeof(link))
                continue;
            auto it = targets.find(QByteArray::fromRawData(link, len));
            if (it == targets.end())
                continue;
            // A matching pathname in another mount namespace need not be the same device node.
            struct stat st{};
            if (::fstatat(::dirfd(fds), fd->d_name, &st, 0) != 0 ||
                st.st_dev != it->device.st_dev || st.st_ino != it->device.st_ino)
                continue;
            QFile comm(base + "/comm");
            const auto name = comm.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(comm.read(64)).simplified() : QString{};
            const QString owner = name.isEmpty() ? QObject::tr("PID %1").arg(pid) : QObject::tr("%1 (PID %2)").arg(name, pid);
            for (const auto& alias : it->aliases)
                owners.insert(alias, owner);
            targets.erase(it);
            if (targets.isEmpty())
                return owners;
        }
    }
#else
    Q_UNUSED(ports);
#endif
    return owners;
}

}  // namespace lt

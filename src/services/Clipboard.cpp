// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/Clipboard.hpp"

#include <QByteArray>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <vector>

namespace lt {

namespace {

struct Candidate {
    QString program;
    QStringList args;
};

bool runClipboardTool(const Candidate& c, std::string_view text)
{
    const QString exe = QStandardPaths::findExecutable(c.program);
    if (exe.isEmpty())
        return false;

    QProcess proc;
    // Both wl-copy and xclip fork a background selection owner; keeping stdout/stderr pipes
    // attached would block waitForFinished() indefinitely.
    proc.setStandardOutputFile(QProcess::nullDevice());
    proc.setStandardErrorFile(QProcess::nullDevice());
    proc.start(exe, c.args, QIODevice::WriteOnly);
    if (!proc.waitForStarted(2000))
        return false;

    const char* ptr = text.data();
    qint64 remaining = static_cast<qint64>(text.size());
    while (remaining > 0) {
        const qint64 written = proc.write(ptr, remaining);
        if (written < 0) {
            proc.kill();
            proc.waitForFinished(500);
            return false;
        }
        ptr += written;
        remaining -= written;
        if (proc.bytesToWrite() > 0 && !proc.waitForBytesWritten(3000)) {
            proc.kill();
            proc.waitForFinished(500);
            return false;
        }
    }
    proc.closeWriteChannel();
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        proc.waitForFinished(500);
        return false;
    }
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

}  // namespace

bool copyTextToClipboard(std::string_view text, std::string* backendUsed, std::string* error)
{
    const QByteArray testFile = qgetenv("LOGICTRACER_CLIPBOARD_FILE");
    if (!testFile.isEmpty()) {
#if defined(_WIN32)
        const std::filesystem::path testPath(QString::fromLocal8Bit(testFile).toStdWString());
#else
        const std::filesystem::path testPath(testFile.toStdString());
#endif
        std::ofstream out(testPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error)
                *error = "cannot write LOGICTRACER_CLIPBOARD_FILE '" + testFile.toStdString() + "'";
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            if (error)
                *error = "failed writing LOGICTRACER_CLIPBOARD_FILE";
            return false;
        }
        if (backendUsed)
            *backendUsed = "file";
        return true;
    }

    const bool preferWayland = !qgetenv("WAYLAND_DISPLAY").isEmpty() ||
                               qgetenv("XDG_SESSION_TYPE").toLower() == "wayland";

    std::vector<Candidate> candidates;
    if (preferWayland) {
        candidates.push_back({"wl-copy", {}});
        candidates.push_back({"xclip", {"-selection", "clipboard"}});
        candidates.push_back({"xsel", {"--clipboard", "--input"}});
    } else {
        candidates.push_back({"xclip", {"-selection", "clipboard"}});
        candidates.push_back({"xsel", {"--clipboard", "--input"}});
        candidates.push_back({"wl-copy", {}});
    }
    candidates.push_back({"pbcopy", {}});
    candidates.push_back({"clip.exe", {}});

    for (const auto& c : candidates) {
        if (runClipboardTool(c, text)) {
            if (backendUsed)
                *backendUsed = c.program.toStdString();
            return true;
        }
    }

    if (error) {
        *error = "could not copy to clipboard (install 'wl-clipboard' on Wayland or 'xclip'/'xsel' on X11, and ensure "
                 "WAYLAND_DISPLAY or DISPLAY is set)";
    }
    return false;
}

}  // namespace lt

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/GuiDiagnostics.hpp"

#include <QRegularExpression>
#include <QtCore/qlogging.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace lt::gui {
namespace {

constexpr int byteLimit = GuiDiagnostics::MaxBytes;
constexpr int lineLimit = GuiDiagnostics::MaxLines;

QString sanitize(QByteArrayView bytes)
{
    QString text = QString::fromUtf8(bytes);
    static const QRegularExpression escapes(R"(\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\|$)))");
    static const QRegularExpression spaces(R"([\s\p{Z}])");
    static const QRegularExpression controls(R"([\p{Cc}\p{Cf}])");
    text.remove(escapes).replace(spaces, " ").remove(controls);
    static const QRegularExpression userInfo(R"((?i)([a-z][a-z0-9+.-]*://)[^/\s]+@)");
    static const QRegularExpression addresses(R"((?i)0x[0-9a-f]+|\b(?:[0-9a-fA-F]{16}|[0-9a-fA-F]{8})\b)");
    static const QRegularExpression tokens(
        R"((?i)\b(?:gh[pousr]_|github_pat_|sk-|xox[baprs]-|AKIA)[a-z0-9_+./=-]+|\beyJ[\w-]+\.[\w-]+\.[\w-]+)");
    // Unknown value syntax (quoted strings, cookies, multiple authorization schemes): keep the
    // diagnostic up to the sensitive field, not a guessed endpoint inside its credential.
    static const QRegularExpression field(
        R"((?i)((?:authorization|cookie|token|password|passwd|pwd|secret|(?:api|access|private)[-_]?key|credential)s?["']?\s*[:=]\s*|(?:\bbearer|\bbasic|--(?:token|password|api-key))\s+).*)");
    text.replace(userInfo, "\\1[redacted]@").replace(tokens, "[redacted]")
        .replace(field, "\\1[redacted]").replace(addresses, "[address]");
    return text;
}

// Track PEM boundaries during ingestion: the BEGIN line may already have left the tail at preview time.
bool privateKeyLine(QByteArrayView line, bool& inside)
{
    if (line.contains("-----BEGIN ") && line.contains("PRIVATE KEY-----"))
        inside = true;
    const bool sensitive = inside;
    if (inside && line.contains("-----END ") && line.contains("PRIVATE KEY-----"))
        inside = false;
    return sensitive;
}

const char* severity(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "Qt";
}

class Tail {
public:
    void direct(QByteArrayView input)
    {
        std::lock_guard lock(m_mutex);
        while (!input.empty()) {
            const auto nl = input.indexOf('\n');
            const auto piece = nl < 0 ? input : input.first(nl);
            if (!m_skipping) {
                if (m_fragment.size() + piece.size() >= byteLimit) {
                    // Never retain a credential suffix whose field name/prefix was already cut.
                    gap();
                    m_fragment.clear();
                    m_skipping = true;
                } else {
                    m_fragment.append(piece.data(), piece.size());
                }
            }
            if (nl < 0) {
                trim();
                break;
            }
            if (!m_skipping) {
                auto line = std::move(m_fragment);
                if (line.endsWith('\r'))
                    line.chop(1);
                const bool sensitive = privateKeyLine(line, m_directKey);
                const auto hash = qHash(line);
                const auto echo = std::find_if(m_echoes.begin(), m_echoes.end(),
                                               [hash](const Echo& e) { return e.hash == hash; });
                if (echo != m_echoes.end())
                    m_echoes.erase(echo);  // the previous Qt handler printed the already annotated record
                else
                    append(sensitive ? QByteArray("[redacted private key]") : std::move(line));
            }
            m_fragment.clear();
            m_skipping = false;
            input = input.sliced(nl + 1);
        }
    }

    uint64_t qt(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        QStringView rest(message);
        const char* category = context.category ? context.category : "default";
        const QByteArray prefix = QByteArray(severity(type)) + ' ' +
            QByteArray(category, static_cast<qsizetype>(qstrnlen(category, byteLimit))) + ": ";
        QByteArray rendered;
        if (rest.size() < byteLimit && prefix.size() < byteLimit) {
            const auto formatted = qFormatLogMessage(type, context, message);
            if (formatted.size() < byteLimit)
                rendered = formatted.toLocal8Bit();
        }
        std::lock_guard lock(m_mutex);
        const auto batch = ++m_batch;
        if (!rendered.isNull() && rendered.size() < byteLimit) {
            QByteArrayView view(rendered);
            for (;;) {
                const auto nl = view.indexOf('\n');
                m_echoes.push_back({batch, qHash(nl < 0 ? view : view.first(nl))});
                if (m_echoes.size() > lineLimit)
                    m_echoes.pop_front();
                if (nl < 0)
                    break;
                view = view.sliced(nl + 1);
            }
        }
        for (;;) {
            const auto nl = rest.indexOf('\n');
            const auto part = nl < 0 ? rest : rest.first(nl);
            if (part.size() >= byteLimit) {
                gap();
            } else {
                const QByteArray raw = part.toUtf8();
                append(prefix + (privateKeyLine(raw, m_qtKey) ? QByteArray("[redacted private key]") : raw));
            }
            if (nl < 0)
                break;
            rest = rest.sliced(nl + 1);
        }
        return batch;
    }

    void finishQt(uint64_t batch)
    {
        std::lock_guard lock(m_mutex);
        // A native/custom handler need not have printed anything on fd 2. Future direct writes
        // which happen to contain that same text are new diagnostics, not echoes.
        std::erase_if(m_echoes, [batch](const Echo& e) { return e.batch == batch; });
    }

    GuiDiagnostics::Preview preview() const
    {
        std::deque<QByteArray> lines;
        bool omitted;
        {
            std::lock_guard lock(m_mutex);
            lines = m_lines;
            if (!m_fragment.isEmpty()) {
                bool inside = m_directKey;
                lines.push_back(privateKeyLine(m_fragment, inside) ? QByteArray("[redacted private key]") : m_fragment);
            }
            omitted = m_omitted;
        }
        // Masking and replacement of invalid UTF-8 can expand the text. Bound the actual shared
        // representation too, counting its newlines and omission marker.
        const QByteArray marker("[older or oversized diagnostics omitted]");
        std::deque<QByteArray> safe;
        qsizetype bytes = 0;
        for (const auto& line : lines) {
            safe.push_back(sanitize(line).toUtf8());
            bytes += safe.back().size() + 1;
            while (!safe.empty() && (safe.size() + omitted > lineLimit ||
                   bytes + (omitted ? marker.size() + 1 : 0) > byteLimit)) {
                bytes -= safe.front().size() + 1;
                safe.pop_front();
                omitted = true;
            }
        }
        QByteArray out;
        if (omitted)
            out += marker + '\n';
        for (const auto& line : safe)
            out += line + '\n';
        if (!out.isEmpty())
            out.chop(1);
        return {QString::fromUtf8(out), omitted};
    }

private:
    struct Echo { uint64_t batch; size_t hash; };
    void gap()
    {
        m_omitted = true;
        m_lines.clear();
        m_bytes = 0;
    }
    void trim()
    {
        while (!m_lines.empty() && (m_lines.size() + !m_fragment.isEmpty() > lineLimit ||
               m_bytes + m_fragment.size() > byteLimit)) {
            m_bytes -= m_lines.front().size() + 1;
            m_lines.pop_front();
            m_omitted = true;
        }
    }
    void append(QByteArray text)
    {
        if (text.size() >= byteLimit) {
            gap();
            return;
        }
        m_bytes += text.size() + 1;
        m_lines.push_back(std::move(text));
        trim();
    }

    mutable std::mutex m_mutex;
    std::deque<QByteArray> m_lines;
    std::deque<Echo> m_echoes;
    uint64_t m_batch = 0;
    qsizetype m_bytes = 0;
    QByteArray m_fragment;
    bool m_skipping = false, m_omitted = false;
    bool m_directKey = false, m_qtKey = false;
};

#ifdef _WIN32
int closeFd(int fd) { return ::_close(fd); }
int duplicate(int fd)
{
    int low[3], n = 0, copy;
    while ((copy = ::_dup(fd)) >= 0 && copy <= 2)
        low[n++] = copy;
    while (n > 0)
        closeFd(low[--n]);
    return copy;
}
int replaceFd(int src, int dest) { return ::_dup2(src, dest); }
int openPipe(int (&fds)[2]) { return ::_pipe(fds, 4096, _O_BINARY | _O_NOINHERIT); }
int readFd(int fd, char* buf, int len) { return ::_read(fd, buf, static_cast<unsigned>(len)); }
int writeFd(int fd, const char* buf, int len) { return ::_write(fd, buf, static_cast<unsigned>(len)); }
HANDLE handle(int fd) { return reinterpret_cast<HANDLE>(::_get_osfhandle(fd)); }
int inheritFlags(int fd)
{
    DWORD flags = 0;
    return GetHandleInformation(handle(fd), &flags) ? static_cast<int>(flags & HANDLE_FLAG_INHERIT) : -1;
}
bool inheritance(int fd, int flags)
{
    return SetHandleInformation(handle(fd), HANDLE_FLAG_INHERIT, static_cast<DWORD>(flags)) != 0;
}
constexpr int privateFlags = 0;
int available(int fd)
{
    DWORD bytes = 0;
    return PeekNamedPipe(handle(fd), nullptr, 0, nullptr, &bytes, nullptr) ? static_cast<int>(bytes) : 0;
}
#else
int closeFd(int fd) { return ::close(fd); }
int duplicate(int fd) { return ::fcntl(fd, F_DUPFD_CLOEXEC, 3); }
int replaceFd(int src, int dest)
{
    int rc;
    do { rc = ::dup2(src, dest); } while (rc < 0 && errno == EINTR);
    return rc;
}
int openPipe(int (&fds)[2]) { return ::pipe(fds); }
int readFd(int fd, char* buf, int len) { return static_cast<int>(::read(fd, buf, static_cast<size_t>(len))); }
int writeFd(int fd, const char* buf, int len) { return static_cast<int>(::write(fd, buf, static_cast<size_t>(len))); }
int inheritFlags(int fd) { return ::fcntl(fd, F_GETFD); }
bool inheritance(int fd, int flags) { return ::fcntl(fd, F_SETFD, flags) == 0; }
constexpr int privateFlags = FD_CLOEXEC;
int available(int fd)
{
    int bytes = 0;
    return ::ioctl(fd, FIONREAD, &bytes) == 0 ? bytes : 0;
}
#endif

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) closeFd(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    explicit Fd(int value = -1) : fd(value) {}
};

}  // namespace

struct GuiDiagnostics::Impl {
    Tail tail;
    Fd saved, reader;
    int originalFlags = -1;
#ifdef _WIN32
    int originalMode = _O_BINARY;
    HANDLE originalNative = nullptr;
    bool nativeWasCrt = false;
#endif
    bool redirected = false;
    std::atomic_bool stopping{false};
    std::thread worker;
    std::mutex drainMutex;

    static inline std::mutex hookMutex;
    static inline Impl* owner = nullptr;
    static inline QtMessageHandler previous = nullptr;

    static void message(QtMsgType type, const QMessageLogContext& ctx, const QString& text)
    {
        QtMessageHandler next;
        uint64_t batch = 0;
        {
            std::lock_guard lock(hookMutex);
            if (owner) {
                owner->drain();
                batch = owner->tail.qt(type, ctx, text);
            }
            next = previous;
        }
        if (next)
            next(type, ctx, text);  // preserve the configured terminal pattern and native logging sinks
        std::lock_guard lock(hookMutex);
        if (owner) {
            owner->drain();  // Qt may abort immediately after returning from a fatal message
            owner->tail.finishQt(batch);
        }
    }

    Impl()
    {
        std::lock_guard lock(hookMutex);
        if (owner)
            return;
        installTee();
        owner = this;
        previous = qInstallMessageHandler(message);
    }

    void installTee()
    {
        constexpr int err = 2;
#ifdef _WIN32
        originalNative = GetStdHandle(STD_ERROR_HANDLE);
        if (::_fileno(stderr) == err) {
            originalFlags = inheritFlags(err);
            nativeWasCrt = originalNative == handle(err);
        }
#else
        originalFlags = inheritFlags(err);
        if (originalFlags < 0 && errno != EBADF)
            return;
#endif
        if (originalFlags >= 0 && ((saved.fd = duplicate(err)) < 0 || !inheritance(saved.fd, privateFlags)))
            return;
        int pipe[2];
        if (openPipe(pipe) < 0)
            return;
        reader.fd = pipe[0];
        Fd writer(pipe[1]);
        // Desktop launches need not have a terminal. Keep the pipe itself out of the standard
        // descriptor slots, even when the OS assigned its read or write end to the absent fd 2.
        for (auto* end : {&reader, &writer}) {
            if (end->fd <= 2) {
                const int moved = duplicate(end->fd);
                if (moved < 0)
                    return;
                closeFd(end->fd);
                end->fd = moved;
            }
        }
        if (!inheritance(reader.fd, privateFlags) || !inheritance(writer.fd, privateFlags))
            return;
        std::fflush(stderr);
#ifdef _WIN32
        // Text-mode writers should translate CRLF exactly once, not again in the tee.
        if (saved.fd >= 0)
            originalMode = ::_setmode(saved.fd, _O_BINARY);
        if (originalMode < 0 || ::_setmode(writer.fd, originalMode) < 0)
            return;
#endif
        if (replaceFd(writer.fd, err) < 0)
            return;
        redirected = true;
        if (!inheritance(err, privateFlags)) {
            restore();
            return;
        }
#ifdef _WIN32
        SetStdHandle(STD_ERROR_HANDLE, handle(err));
#endif
        try {
            worker = std::thread([this] {
                while (!stopping.load()) {
#ifdef _WIN32
                    if (!available(reader.fd))
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
#else
                    pollfd pfd{reader.fd, POLLIN, 0};
                    // Usually woken by data or EOF. Don't depend on EOF if a library dup'ed fd 2.
                    if (::poll(&pfd, 1, 250) <= 0)
                        continue;
#endif
                    drain();
                }
            });
        } catch (const std::system_error&) {
            restore();  // don't leave stderr writing into a pipe with nobody draining it
        }
    }

    void drain()
    {
        std::lock_guard lock(drainMutex);
        if (reader.fd < 0)
            return;
        char buffer[4096];
        // Drain a snapshot of bytes already written, so preview needn't wait on further output.
        for (int left = available(reader.fd); left > 0;) {
            const int n = readFd(reader.fd, buffer, std::min(left, static_cast<int>(sizeof buffer)));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            left -= n;
            for (int sent = 0; sent < n;) {
                int written = 0;
                if (saved.fd >= 0)
                    written = writeFd(saved.fd, buffer + sent, n - sent);
#ifdef _WIN32
                else if (originalNative && originalNative != INVALID_HANDLE_VALUE) {
                    DWORD count = 0;
                    if (WriteFile(originalNative, buffer + sent, static_cast<DWORD>(n - sent), &count, nullptr))
                        written = static_cast<int>(count);
                }
#endif
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    break;
                sent += written;
            }
            tail.direct({buffer, n});
        }
    }

    void restore()
    {
        if (!redirected)
            return;
        std::fflush(stderr);
        if (saved.fd >= 0) {
            if (replaceFd(saved.fd, 2) >= 0) {
#ifdef _WIN32
                ::_setmode(2, originalMode);
#endif
                inheritance(2, originalFlags);
            }
        } else {
            closeFd(2);
        }
#ifdef _WIN32
        SetStdHandle(STD_ERROR_HANDLE, nativeWasCrt ? handle(2) : originalNative);
#endif
        redirected = false;
    }

    ~Impl()
    {
        {
            std::lock_guard lock(hookMutex);
            if (owner != this)
                return;
            const auto current = qInstallMessageHandler(previous);
            if (current != message)
                qInstallMessageHandler(current);  // don't undo another component's later hook
            owner = nullptr;
            restore();
        }
        stopping = true;
        if (worker.joinable())
            worker.join();
        drain();
    }
};

GuiDiagnostics::GuiDiagnostics() : m(std::make_unique<Impl>()) {}
GuiDiagnostics::~GuiDiagnostics() = default;
GuiDiagnostics::Preview GuiDiagnostics::preview() const
{
    std::fflush(stderr);
    m->drain();
    return m->tail.preview();
}

}  // namespace lt::gui

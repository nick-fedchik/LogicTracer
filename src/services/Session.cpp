// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/Session.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memory>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lt::session {

using json = nlohmann::json;

#if defined(_WIN32)
namespace {

// The token user SID of a process (empty on failure).
std::unique_ptr<char[]> tokenUser(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::unique_ptr<char[]> buf(size ? new char[size] : nullptr);
    const bool ok = buf && GetTokenInformation(token, TokenUser, buf.get(), size, &size);
    CloseHandle(token);
    return ok ? std::move(buf) : nullptr;
}

// True when the process serving the pipe runs as the current user (another user could own a pipe of that name).
bool pipeServerIsCurrentUser(HANDLE pipe)
{
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid))
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    const auto theirs = tokenUser(process);
    CloseHandle(process);
    const auto mine = tokenUser(GetCurrentProcess());
    return theirs && mine &&
           EqualSid(reinterpret_cast<TOKEN_USER*>(theirs.get())->User.Sid,
                    reinterpret_cast<TOKEN_USER*>(mine.get())->User.Sid);
}

}  // namespace
#endif

QString defaultSocketPath()
{
    const QByteArray env = qgetenv("LOGICTRACER_SOCKET");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
#if defined(_WIN32)
    // A named pipe (\\.\pipe\logictracer-<user>); QLocalServer::UserAccessOption limits it to this user.
    QString user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    user.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    return "logictracer-" + (user.isEmpty() ? QString("user") : user);
#else
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty())
        return QDir(QString::fromLocal8Bit(runtime)).filePath("logictracer.sock");

    // Fallback when XDG_RUNTIME_DIR is unset (e.g. headless SSH): use a private 0700 directory.
    const QString privDir = QString("/tmp/logictracer-%1").arg(static_cast<unsigned>(::getuid()));
    const QByteArray privBytes = privDir.toLocal8Bit();
    ::mkdir(privBytes.constData(), 0700);
    struct stat st{};
    if (::lstat(privBytes.constData(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() &&
        (st.st_mode & 0077) == 0) {
        return QDir(privDir).filePath("session.sock");
    }
    return QString("/tmp/logictracer-%1.sock").arg(static_cast<unsigned>(::getuid()));
#endif
}

void Responder::ok(const json& result) const
{
    if (m_sink)
        m_sink({{"id", m_id}, {"result", result}});
}

void Responder::fail(int code, const std::string& message, const nlohmann::json& data) const
{
    if (!m_sink)
        return;
    json error = {{"code", code}, {"message", message}};
    if (!data.is_null())
        error["data"] = data;
    m_sink({{"id", m_id}, {"error", std::move(error)}});
}

Server::Server(QObject* parent) : QObject(parent) {}

Server::~Server()
{
    close();
}

bool Server::isListening() const
{
    return m_server && m_server->isListening();
}

bool Server::listen(const QString& path, QString* error)
{
    close();

    // A live session on this path must not be hijacked; a stale socket file is removed.
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(300)) {
        if (error)
            *error = QString("another session is already listening on %1").arg(path);
        return false;
    }
    QLocalServer::removeServer(path);

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(path)) {
        if (error)
            *error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_path = path;
    connect(m_server, &QLocalServer::newConnection, this, &Server::onNewConnection);
    return true;
}

void Server::close()
{
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    m_path.clear();
}

void Server::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        socket->setReadBufferSize(2 * 1024 * 1024);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void Server::onReadyRead(QLocalSocket* socket)
{
    QPointer<QLocalSocket> guard(socket);
    auto sink = [guard, alive = std::weak_ptr<int>(m_alive)](const json& message) {
        if (alive.expired() || !guard)
            return;
        // Invalid UTF-8 inside a string must never throw out of a Qt slot.
        guard->write(QByteArray::fromStdString(message.dump(-1, ' ', false, json::error_handler_t::replace)) + '\n');
    };

    if (guard && !guard->canReadLine() && guard->bytesAvailable() > 1024 * 1024) {
        Responder(sink, nullptr).fail(error::InvalidRequest, "request line exceeds 1 MiB limit");
        guard->disconnectFromServer();
        return;
    }

    while (guard && guard->canReadLine()) {
        const QByteArray rawLine = guard->readLine();
        if (rawLine.size() > 1024 * 1024) {  // measured before trimming, so padding cannot hide an oversized line
            Responder(sink, nullptr).fail(error::InvalidRequest, "request line exceeds 1 MiB limit");
            guard->disconnectFromServer();
            return;
        }
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        const json msg = json::parse(line.toStdString(), nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) {
            Responder(sink, nullptr).fail(error::ParseError, "invalid JSON");
            continue;
        }
        const json id = msg.value("id", json(nullptr));
        const Responder reply(sink, id);
        if (!msg.contains("method") || !msg["method"].is_string()) {
            reply.fail(error::InvalidRequest, "missing method");
            continue;
        }
        const json params = msg.contains("params") ? msg["params"] : json::object();
        if (!params.is_object()) {
            reply.fail(error::InvalidParams, "params must be an object");
            continue;
        }
        if (!m_handler) {
            reply.fail(error::MethodNotFound, "no handler");
            continue;
        }
        try {
            m_handler(msg["method"].get<std::string>(), params, reply);
        } catch (const std::exception& e) {  // wrongly typed parameters must not take the GUI down
            reply.fail(error::InternalError, std::string("request failed: ") + e.what());
        }
    }
}

CallResult call(const QString& socketPath, const std::string& method, const json& params, int timeoutMs)
{
    CallResult out;
#if !defined(_WIN32)
    // Never talk to (and trust reports from) a socket that another user could have created.
    struct stat st{};
    const QByteArray pathBytes = socketPath.toLocal8Bit();
    if (::lstat(pathBytes.constData(), &st) == 0 && (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())) {
        out.error = "refusing the session socket " + socketPath.toStdString() + ": not a socket owned by the current user";
        return out;
    }
#endif
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(std::min(timeoutMs, 1000))) {
        out.error = "no GUI session is listening on " + socketPath.toStdString();
        return out;
    }
#if !defined(_WIN32)
    // The path check above is racy: also verify that the process we reached runs as the current user.
    {
        const int fd = static_cast<int>(socket.socketDescriptor());
#if defined(__linux__)
        ucred cred{};
        socklen_t len = sizeof cred;
        const bool sameUser = ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == ::getuid();
#else  // macOS / BSD
        uid_t uid = 0;
        gid_t gid = 0;
        const bool sameUser = ::getpeereid(fd, &uid, &gid) == 0 && uid == ::getuid();
#endif
        if (!sameUser) {
            out.error = "refusing the session socket " + socketPath.toStdString() + ": peer is not the current user";
            socket.abort();
            return out;
        }
    }
#else
    if (!pipeServerIsCurrentUser(reinterpret_cast<HANDLE>(socket.socketDescriptor()))) {
        out.error = "refusing the session pipe " + socketPath.toStdString() + ": server is not the current user";
        socket.abort();
        return out;
    }
#endif
    out.connected = true;

    socket.write(QByteArray::fromStdString(json{{"id", 1}, {"method", method}, {"params", params}}.dump()) + '\n');
    if (!socket.waitForBytesWritten(timeoutMs)) {
        out.error = "could not send the request";
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine()) {
        const int left = timeoutMs - static_cast<int>(timer.elapsed());
        if (left <= 0 || !socket.waitForReadyRead(left)) {
            out.error = socket.state() == QLocalSocket::ConnectedState ? "timed out waiting for the session reply"
                                                                       : "session closed the connection";
            return out;
        }
    }
    const json reply = json::parse(socket.readLine().trimmed().toStdString(), nullptr, false);
    if (reply.is_discarded() || !reply.is_object()) {
        out.error = "invalid reply from the session";
        return out;
    }
    if (reply.contains("error")) {
        out.errorCode = reply["error"].value("code", 0);
        out.error = reply["error"].value("message", std::string("session error"));
        out.errorData = reply["error"].value("data", json(nullptr));
        return out;
    }
    out.ok = true;
    out.result = reply.value("result", json::object());
    return out;
}

}  // namespace lt::session

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "services/Session.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>

#include <sys/stat.h>
#include <unistd.h>

namespace lt::session {

using json = nlohmann::json;

QString defaultSocketPath()
{
    const QByteArray env = qgetenv("LOGICTRACER_SOCKET");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
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
        const QByteArray line = guard->readLine().trimmed();
        if (line.isEmpty())
            continue;
        if (line.size() > 1024 * 1024) {
            Responder(sink, nullptr).fail(error::InvalidRequest, "request line exceeds 1 MiB limit");
            guard->disconnectFromServer();
            return;
        }
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
    // Never talk to (and trust reports from) a socket that another user could have created.
    struct stat st{};
    const QByteArray pathBytes = socketPath.toLocal8Bit();
    if (::lstat(pathBytes.constData(), &st) == 0 && (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())) {
        out.error = "refusing the session socket " + socketPath.toStdString() + ": not a socket owned by the current user";
        return out;
    }
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(std::min(timeoutMs, 1000))) {
        out.error = "no GUI session is listening on " + socketPath.toStdString();
        return out;
    }
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

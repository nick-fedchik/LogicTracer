// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Local IPC between a running GUI (the "session") and headless tools (CLI, MCP server).
//
// Transport: a Unix domain socket (user-only access). Messages are single-line JSON objects:
//   request : {"id": 1, "method": "annotate", "params": {...}}
//   response: {"id": 1, "result": {...}}  or  {"id": 1, "error": {"code": -32602, "message": "..."}}
// One request may be answered later (e.g. "capture"), so the server hands a Responder to the handler.

#include <QObject>
#include <QString>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

class QLocalServer;
class QLocalSocket;

namespace lt::session {

namespace error {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
inline constexpr int NoCapture = -32000;
inline constexpr int Busy = -32001;
inline constexpr int NoDevice = -32002;
inline constexpr int Failed = -32003;
inline constexpr int StaleCapture = -32004;  // the request named a capture that is no longer on screen
}  // namespace error

// $LOGICTRACER_SOCKET, else $XDG_RUNTIME_DIR/logictracer.sock, else /tmp/logictracer-<uid>.sock;
// on Windows the named pipe logictracer-<user> (access limited to the current user).
[[nodiscard]] QString defaultSocketPath();

// Completes one request; may be copied and invoked later. Safe if the client has gone away.
class Responder {
public:
    using Sink = std::function<void(const nlohmann::json& message)>;
    Responder() = default;
    Responder(Sink sink, nlohmann::json id) : m_sink(std::move(sink)), m_id(std::move(id)) {}
    void ok(const nlohmann::json& result) const;
    // `data` (optional) is attached to the error object for machine-readable details.
    void fail(int code, const std::string& message, const nlohmann::json& data = nullptr) const;

private:
    Sink m_sink;
    nlohmann::json m_id;
};

class Server : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<void(const std::string& method, const nlohmann::json& params, Responder reply)>;

    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    void setHandler(Handler handler) { m_handler = std::move(handler); }
    // Fails (with a message) if another live session already owns the socket.
    bool listen(const QString& path, QString* error = nullptr);
    void close();
    [[nodiscard]] bool isListening() const;
    [[nodiscard]] QString path() const { return m_path; }

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);

    QLocalServer* m_server = nullptr;
    Handler m_handler;
    QString m_path;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);  // guards late Responders after destruction
};

struct CallResult {
    bool connected = false;  // false: no session is listening
    bool ok = false;
    nlohmann::json result;
    int errorCode = 0;
    std::string error;
    nlohmann::json errorData;  // null when the error has no structured details
};

// Blocking request/response; needs no event loop.
CallResult call(const QString& socketPath, const std::string& method, const nlohmann::json& params = nlohmann::json::object(),
                int timeoutMs = 5000);

}  // namespace lt::session

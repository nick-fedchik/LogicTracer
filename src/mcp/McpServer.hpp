// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Model Context Protocol server (JSON-RPC 2.0, newline-delimited messages over stdio) exposing the
// analyzer to AI agents. Tool calls are synchronous: the transport is a single stdio client.
//
// If a logictracer-gui session is running and connected to a device, capture/probe are forwarded to
// it so the engineer sees on screen what the agent asked for (and the port is not fought over);
// otherwise the device is opened directly.

#include <QString>

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <optional>
#include <string>

namespace lt::mcp {

struct Options {
    QString socketPath;       // GUI session socket (default: lt::session::defaultSocketPath())
    std::string defaultPort;  // serial port for direct access (default: first capture device found)
    std::string outputDir;    // sandbox directory for out_vcd/out_json/out_csv (empty = cwd + /tmp)
    bool useSession = true;
};

class McpServer {
public:
    explicit McpServer(Options options = {});

    // Handles one decoded JSON-RPC message (request, notification or batch). Returns the response to
    // send, or nullopt when none is due (notifications).
    [[nodiscard]] std::optional<nlohmann::json> handle(const nlohmann::json& message);
    // Reads one JSON message per line from `in` until EOF, writing responses to `out`.
    void run(std::istream& in, std::ostream& out);

    [[nodiscard]] static nlohmann::json toolList();
    // Executes a tool; the result follows the MCP tools/call shape (content + isError [+ structuredContent]).
    [[nodiscard]] nlohmann::json callTool(const std::string& name, const nlohmann::json& arguments);

private:
    [[nodiscard]] nlohmann::json handleRequest(const nlohmann::json& request);
    nlohmann::json dispatchTool(const std::string& name, const nlohmann::json& arguments);
    // JSON result of a device-facing tool, via the GUI session when it holds a device, else directly.
    nlohmann::json runDeviceTool(const std::string& tool, const nlohmann::json& args);
    nlohmann::json sessionOrError(const std::string& method, const nlohmann::json& params);
    [[nodiscard]] bool sessionHoldsDevice(const std::string& requestedPort = {}) const;

    Options m_options;
};

}  // namespace lt::mcp

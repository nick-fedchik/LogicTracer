// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

// logictracer-mcp: MCP server (JSON-RPC 2.0 over stdio) for AI agents. Logs go to stderr only;
// stdout carries nothing but protocol messages.

#include "mcp/McpServer.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>

#include <iostream>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("logictracer-mcp");
    QCoreApplication::setApplicationVersion(LT_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription("MCP server exposing LogicTracer to AI agents over stdio.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"socket", "GUI session socket (default: $LOGICTRACER_SOCKET or the per-user default).", "path"});
    parser.addOption({"port", "Serial port for direct access (default: first capture device found).", "path"});
    parser.addOption({"output-dir", "Confine out_vcd/out_json/out_csv writes strictly to this directory.", "dir"});
    parser.addOption({"no-session", "Never use a running GUI; always access the device directly."});
    parser.process(app);

    lt::mcp::Options options;
    options.socketPath = parser.value("socket");
    options.defaultPort = parser.value("port").toStdString();
    options.outputDir = parser.value("output-dir").toStdString();
    options.useSession = !parser.isSet("no-session");

    lt::mcp::McpServer server(options);
    std::cerr << "logictracer-mcp " LT_VERSION " ready (stdio)\n";
    server.run(std::cin, std::cout);
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/MainWindow.hpp"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("logictracer-gui");
    QApplication::setApplicationVersion(LT_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription("LogicTracer: waveform viewer with human/AI grounding (Copy for AI).");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"port", "Serial port to select and connect to (e.g. /dev/ttyACM0).", "path"});
    parser.addOption({"open", "Open a saved capture file (.json or .vcd) on startup.", "file"});
    parser.addOption({"demo", "Load a synthetic capture (no hardware needed)."});
    parser.addOption({"socket", "Session socket path (default: $LOGICTRACER_SOCKET or the per-user default).", "path"});
    parser.addOption({"no-session", "Do not open the local session endpoint (annotate/MCP integration)."});
    parser.process(app);

    lt::gui::MainWindow window;
    window.show();
    if (!parser.isSet("no-session"))
        window.startSession(parser.value("socket"));
    if (parser.isSet("demo"))
        window.loadDemo();
    if (parser.isSet("open"))
        window.openCaptureFile(parser.value("open"));
    if (parser.isSet("port")) {
        window.setPortText(parser.value("port"));
        window.connectOrDisconnect();
    }
    return app.exec();
}

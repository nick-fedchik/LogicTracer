// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/GuiDiagnostics.hpp"
#include "gui/MainWindow.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>

int main(int argc, char* argv[])
{
    lt::gui::GuiDiagnostics diagnostics;  // includes Qt platform initialization and shutdown
    // The platform plugin can contact the desktop portal while constructing QApplication.
    QApplication::setDesktopFileName("logictracer-gui");
    QApplication app(argc, argv);
    QApplication::setApplicationName("logictracer-gui");
    QApplication::setWindowIcon(QIcon(":/branding/logictracer.png"));
    QApplication::setApplicationVersion(LT_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription("LogicTracer: waveform viewer with human/AI grounding (Copy for AI).");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"port", "Serial port to select and connect to (e.g. /dev/ttyACM0).", "path"});
    parser.addOption({"open", "Open a saved capture or event recording (.json, .vcd, .lac, .ltevents) on startup.", "file"});
    parser.addOption({"demo", "Load a synthetic capture (no hardware needed)."});
    parser.addOption({"socket", "Session socket path (default: $LOGICTRACER_SOCKET or the per-user default).", "path"});
    parser.addOption({"no-session", "Do not open the local session endpoint (annotate/MCP integration)."});
    parser.addPositionalArgument("profile", "Next capture profile: a stored name or an explicit file path.", "[profile]");
    parser.process(app);

    std::optional<lt::gui::profiles::Profile> profile;
    const auto args = parser.positionalArguments();
    if (args.size() > 1) {
        qCritical("Expected at most one positional capture profile. Use --help for usage.");
        return 2;
    }
    if (!args.isEmpty()) {
        QString error;
        profile = lt::gui::profiles::load(args.front(), error);
        if (!profile) {
            qCritical().noquote() << error;
            return 2;
        }
    }
    lt::gui::MainWindow window(nullptr, &diagnostics);
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
    if (profile)
        window.applyStartupProfile(*profile);
    return app.exec();
}

# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

# Library layering (each layer may only depend on the ones above it):
#   core      pure C++20: wire protocol, capture model, signal analysis, file formats, AI reports
#   device    Qt Core + SerialPort: RP2040 serial driver and its worker-thread wrapper
#   services  Qt Core + Network: shared CLI/GUI/MCP operations, IPC session, clipboard
#   cli, gui, mcp   front-ends
add_subdirectory(core)
add_subdirectory(device)
add_subdirectory(services)
add_subdirectory(cli)
add_subdirectory(mcp)
if(LOGICTRACER_BUILD_GUI)
    add_subdirectory(gui)
endif()

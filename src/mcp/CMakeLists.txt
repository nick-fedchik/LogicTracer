# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

add_library(logictracer_mcp STATIC McpServer.cpp McpServer.hpp)
add_library(LogicTracer::Mcp ALIAS logictracer_mcp)
target_link_libraries(logictracer_mcp
    PUBLIC LogicTracer::Services
    PRIVATE LogicTracer::Options)

qt_add_executable(logictracer-mcp main.cpp)
target_link_libraries(logictracer-mcp PRIVATE LogicTracer::Mcp LogicTracer::Options)

install(TARGETS logictracer-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

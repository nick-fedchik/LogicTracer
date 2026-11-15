# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

qt_add_executable(logictracer-cli main.cpp)
target_link_libraries(logictracer-cli PRIVATE LogicTracer::Services LogicTracer::Options)

install(TARGETS logictracer-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

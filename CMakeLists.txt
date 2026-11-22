# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

cmake_minimum_required(VERSION 3.24)

# Single source of truth for the product version (SemVer 2.0), shared with the firmware.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LOGICTRACER_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(LogicTracer
    VERSION ${LOGICTRACER_VERSION}
    DESCRIPTION "Evidence-grade logic analysis for humans and AI agents (C++20 / Qt 6)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LOGICTRACER_BUILD_GUI "Build the Qt Widgets GUI (needs Qt6::Widgets)" ON)
option(LOGICTRACER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LOGICTRACER_HDL_TESTS "Simulate, lint and synthesize the HDL capture core when the tools are installed" ON)

include(CTest)  # provides BUILD_TESTING (ON by default)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network SerialPort)
find_package(nlohmann_json 3.10 REQUIRED)
if(LOGICTRACER_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Concurrent)
endif()
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

# AUTOMOC/AUTOUIC/AUTORCC and GNUInstallDirs.
qt_standard_project_setup()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(LogicTracerOptions)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
    if(LOGICTRACER_HDL_TESTS)
        add_subdirectory(hdl)
    endif()
endif()

install(FILES LICENSE NOTICE.md README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

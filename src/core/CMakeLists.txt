# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

# Pure C++20 (no Qt): usable from any front-end, a Python binding or embedded tooling.
add_library(logictracer_core STATIC
    AiReport.cpp AiReport.hpp
    ArgParsing.cpp ArgParsing.hpp
    CaptureBuffer.cpp CaptureBuffer.hpp
    CaptureTypes.cpp CaptureTypes.hpp
    DeviceProtocol.cpp DeviceProtocol.hpp
    Exporters.cpp Exporters.hpp
    Format.hpp
    SignalAnalyzer.cpp SignalAnalyzer.hpp
)
add_library(LogicTracer::Core ALIAS logictracer_core)
set_target_properties(logictracer_core PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(logictracer_core PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
target_link_libraries(logictracer_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE LogicTracer::Options)

# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

add_library(logictracer_device STATIC
    AsyncDevice.cpp AsyncDevice.hpp
    PicoConnection.cpp PicoConnection.hpp
)
add_library(LogicTracer::Device ALIAS logictracer_device)
set_target_properties(logictracer_device PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(logictracer_device
    PUBLIC LogicTracer::Core Qt6::Core Qt6::SerialPort
    PRIVATE LogicTracer::Options)

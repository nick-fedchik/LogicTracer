# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

add_library(logictracer_services STATIC
    Clipboard.cpp Clipboard.hpp
    Operations.cpp Operations.hpp
    Session.cpp Session.hpp
)
add_library(LogicTracer::Services ALIAS logictracer_services)
set_target_properties(logictracer_services PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(logictracer_services
    PUBLIC LogicTracer::Device Qt6::Core Qt6::Network
    PRIVATE LogicTracer::Options)

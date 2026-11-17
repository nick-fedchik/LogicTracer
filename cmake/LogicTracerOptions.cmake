# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
#
# Project-wide compile settings, attached to every LogicTracer target through the
# `LogicTracer::Options` interface target instead of global add_compile_options().

add_library(logictracer_options INTERFACE)
add_library(LogicTracer::Options ALIAS logictracer_options)

target_compile_features(logictracer_options INTERFACE cxx_std_20)

target_compile_definitions(logictracer_options INTERFACE
    LT_VERSION="${PROJECT_VERSION}"
    # Qt hygiene: no `signals`/`slots`/`emit` macros (use Q_SIGNALS/Q_SLOTS/Q_EMIT), no Q_FOREACH,
    # and no APIs deprecated before the minimum supported Qt.
    # nlohmann::json: explicit .get<T>() only (implicit conversions confuse overload resolution, e.g. QCOMPARE).
    JSON_USE_IMPLICIT_CONVERSIONS=0
    QT_NO_KEYWORDS
    QT_NO_FOREACH
    QT_DISABLE_DEPRECATED_BEFORE=0x060400)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logictracer_options INTERFACE -Wall -Wextra -Wpedantic)
    if(LOGICTRACER_WARNINGS_AS_ERRORS)
        target_compile_options(logictracer_options INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(logictracer_options INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(logictracer_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(LOGICTRACER_WARNINGS_AS_ERRORS)
        target_compile_options(logictracer_options INTERFACE /WX)
    endif()
endif()

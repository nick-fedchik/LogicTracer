# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

# Each suite is a QtTest executable registered with CTest.
function(logictracer_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;DEFINES;ENV" ${ARGN})
    qt_add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LIBS} Qt6::Test LogicTracer::Options)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(T_DEFINES)
        target_compile_definitions(${name} PRIVATE ${T_DEFINES})
    endif()
    add_test(NAME ${name} COMMAND ${name})
    if(T_ENV)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${T_ENV}")
    endif()
endfunction()

logictracer_add_test(logictracer-core-tests
    SOURCES test_core.cpp
    LIBS LogicTracer::Services)

# Protocol/timeout tests against a pty-based firmware emulator, plus end-to-end CLI tests.
logictracer_add_test(logictracer-connection-tests
    SOURCES test_connection.cpp FakeDevice.hpp
    LIBS LogicTracer::Services
    DEFINES LT_CLI_PATH="$<TARGET_FILE:logictracer-cli>")
add_dependencies(logictracer-connection-tests logictracer-cli)

# PYNQ board agent: its lt_capture register model (Python), and PynqConnection against it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(LT_AGENT_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/rigs/pynq-z1/logictracer_agent.py")
    add_test(NAME logictracer-pynq-agent-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pynq_agent.py" "${LT_AGENT_SCRIPT}")
    target_compile_definitions(logictracer-connection-tests PRIVATE
        LT_PYTHON="${Python3_EXECUTABLE}" LT_AGENT_SCRIPT="${LT_AGENT_SCRIPT}")
    # JSON contracts of the CLI (docs/schema), exit codes and determinism.
    add_test(NAME logictracer-contract-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_contracts.py"
                "$<TARGET_FILE:logictracer-cli>" "${PROJECT_SOURCE_DIR}/docs/schema")
endif()

if(TARGET LogicTracer::Gui)
    logictracer_add_test(logictracer-gui-tests
        SOURCES test_gui.cpp FakeDevice.hpp
        LIBS LogicTracer::Gui
        ENV "QT_QPA_PLATFORM=offscreen")

    # Session socket: annotations/capture from the CLI into a live GUI window.
    logictracer_add_test(logictracer-session-tests
        SOURCES test_session.cpp FakeDevice.hpp
        LIBS LogicTracer::Gui
        DEFINES LT_CLI_PATH="$<TARGET_FILE:logictracer-cli>" LT_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/docs/schema"
        ENV "QT_QPA_PLATFORM=offscreen")
    add_dependencies(logictracer-session-tests logictracer-cli)

    # MCP server over stdio, including the chain agent -> MCP -> GUI session -> device.
    logictracer_add_test(logictracer-mcp-tests
        SOURCES test_mcp.cpp FakeDevice.hpp
        LIBS LogicTracer::Gui LogicTracer::Mcp
        DEFINES LT_MCP_PATH="$<TARGET_FILE:logictracer-mcp>"
        ENV "QT_QPA_PLATFORM=offscreen")
    add_dependencies(logictracer-mcp-tests logictracer-mcp)
endif()

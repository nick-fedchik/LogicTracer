# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
#
# Compiles and runs an Icarus Verilog testbench; fails unless the simulation exits 0 and prints
# PASS_TEXT. Arguments: IVERILOG, VVP, OUT, PASS_TEXT, SOURCES (|-separated), DEFINES (|-separated -P/-D).

string(REPLACE "|" ";" SOURCES "${SOURCES}")
string(REPLACE "|" ";" DEFINES "${DEFINES}")

execute_process(
    COMMAND "${IVERILOG}" -g2012 -Wall ${DEFINES} -o "${OUT}" ${SOURCES}
    RESULT_VARIABLE compile_rc
    OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_out)
if(NOT compile_rc EQUAL 0)
    message(FATAL_ERROR "iverilog failed:\n${compile_out}")
endif()

execute_process(
    COMMAND "${VVP}" -n "${OUT}"
    RESULT_VARIABLE run_rc
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_out)
message("${run_out}")
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "simulation failed (exit ${run_rc})")
endif()
string(FIND "${run_out}" "${PASS_TEXT}" pass_pos)
if(pass_pos EQUAL -1)
    message(FATAL_ERROR "simulation did not report '${PASS_TEXT}'")
endif()

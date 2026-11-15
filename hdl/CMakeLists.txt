# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
#
# HDL checks for the lt_capture core. Each check is added only if its tool is installed:
#   iverilog + vvp   self-checking simulation, with and without the input synchronizer
#   verilator        lint (-Wall) of every synchronizer configuration
#   yosys            synthesis for Xilinx 7-series with the expected block-RAM count

find_program(IVERILOG_EXECUTABLE iverilog)
find_program(VVP_EXECUTABLE vvp)
find_program(VERILATOR_EXECUTABLE verilator)
find_program(YOSYS_EXECUTABLE yosys)

set(LT_CAPTURE_RTL ${CMAKE_CURRENT_SOURCE_DIR}/lt_capture/rtl/lt_capture.v)
set(LT_CAPTURE_TB ${CMAKE_CURRENT_SOURCE_DIR}/lt_capture/tb/lt_capture_tb.v)

if(IVERILOG_EXECUTABLE AND VVP_EXECUTABLE)
    foreach(sync 0 2)
        add_test(NAME lt-capture-sim-sync${sync}
            COMMAND ${CMAKE_COMMAND}
                -DIVERILOG=${IVERILOG_EXECUTABLE} -DVVP=${VVP_EXECUTABLE}
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lt_capture_tb_sync${sync}.vvp
                "-DSOURCES=${LT_CAPTURE_TB}|${LT_CAPTURE_RTL}"
                -DDEFINES=-Ptb.SYNC=${sync}
                "-DPASS_TEXT=LT_CAPTURE_TB PASS"
                -P ${PROJECT_SOURCE_DIR}/cmake/RunHdlSim.cmake)
    endforeach()
else()
    message(STATUS "iverilog/vvp not found: lt_capture simulation tests skipped")
endif()

if(VERILATOR_EXECUTABLE)
    foreach(sync 0 2)
        add_test(NAME lt-capture-lint-sync${sync}
            COMMAND ${VERILATOR_EXECUTABLE} --lint-only -Wall -GSYNC_STAGES=${sync} ${LT_CAPTURE_RTL})
    endforeach()
else()
    message(STATUS "verilator not found: lt_capture lint skipped")
endif()

if(YOSYS_EXECUTABLE)
    # Default parameters: 8192 records x (48-bit timestamp + 32 probe bits) = 18 x RAMB36E1.
    add_test(NAME lt-capture-synth-xc7
        COMMAND ${YOSYS_EXECUTABLE} -q -p
            "read_verilog ${LT_CAPTURE_RTL}; synth_xilinx -family xc7 -top lt_capture; tee -o /dev/stdout stat")
    set_tests_properties(lt-capture-synth-xc7 PROPERTIES PASS_REGULAR_EXPRESSION "RAMB36E1[ \t]+18")
else()
    message(STATUS "yosys not found: lt_capture synthesis check skipped")
endif()

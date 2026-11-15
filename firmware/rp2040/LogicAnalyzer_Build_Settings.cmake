# SPDX-License-Identifier: GPL-3.0-only
# Original work: LogicAnalyzer firmware, Copyright (C) Agustin Gimenez Bernad (gusmanb/logicanalyzer), GPL-3.0.
# Modified 2026 by Mykola Fedchyk for LogicTracer: BOARD_TYPE/TURBO_MODE overridable with -D, TURBO_MODE defaults to 0 (no overclock). See firmware/rp2040/README.md.

# This file controls the build settings, set your board version
# Current versions: "BOARD_PICO", "BOARD_PICO_W", "BOARD_PICO_W_WIFI", "BOARD_ZERO", "BOARD_PICO_2"
if(NOT DEFINED BOARD_TYPE)
    set(BOARD_TYPE "BOARD_PICO")
endif()

# Set to 1 to enable 200Mhz mode (warning! extreme overclock and overvoltage!)
# Set to 0 to disable turbo mode
# Not available for the Pico W
if(NOT DEFINED TURBO_MODE)
    set(TURBO_MODE 0)
endif()

# Uncomment to be able to debug the build
# set(DEBUG_BUILD 1)

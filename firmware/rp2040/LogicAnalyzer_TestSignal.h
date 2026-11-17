// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk
// LogicTracer test-signal generator: known pulse pairs on GP0/GP1 for the firmware self-test.

#ifndef __ANALYZER_TESTSIGNAL__
#define __ANALYZER_TESTSIGNAL__

#include <stdbool.h>
#include <stddef.h>
#include "LogicAnalyzer_Structs.h"

//Starts (or stops) the generator and writes the newline-terminated reply for the host into `reply`:
//"TESTSIGNAL:<sysclk Hz>,<high>,<delay>,<period>,<gap> cycles,<pulses>", "TESTSIGNAL:OFF", or an error.
bool ProcessTestSignalRequest(const TEST_SIGNAL_REQUEST* req, char* reply, size_t replySize);
//Releases GP0/GP1 (back to inputs); no effect when the generator is not running.
void StopTestSignal(void);
//True when this board has the generator.
bool TestSignalSupported(void);
//True while the generator drives GP0/GP1 (captures must leave those pins alone).
bool TestSignalRunning(void);

#endif

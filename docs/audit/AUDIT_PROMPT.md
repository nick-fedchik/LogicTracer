# LogicTracer independent code audit

You are an independent auditor. Review the repository at git tag `audit-base` (commit `149de16`).
Report findings only: **do not modify any source file, do not commit, do not run git push, do not flash
hardware (the live Pico on /dev/ttyACM0 must not be touched)**. Do not read other reports in `docs/audit/`;
independence is the point of this exercise.

## Project context
Native C++20 / Qt 6 host, RP2040 firmware and FPGA (`hdl/lt_capture`) capture core for AI-in-the-loop
hardware-in-the-loop logic analysis. Project rules that code must follow:
- `src/core` is pure C++20 with no Qt dependency.
- `QT_NO_KEYWORDS` is on; `JSON_USE_IMPLICIT_CONVERSIONS=0`.
- `QMetaObject::invokeMethod` only with lambdas.
- GUI dialogs/menus must not block the QTest event loop.
- Tests run offline via `FakeDevice` / `ReplayDevice` (`QT_QPA_PLATFORM=offscreen`).
- Code, UI strings, comments and docs are English.
- Must build on Ubuntu 24.04 / Qt 6.4.

## Scope (audit every area, in this order)
1. `src/core`: protocol parsing, CaptureBuffer, SignalAnalyzer, AiReport, ArgParsing (DSL parsers,
   integer overflow, off-by-one, unit handling, locale).
2. `src/device`, `src/services`: serial I/O, timeouts, cancel, replay device, output writers (path handling).
3. `src/services/Session*`, `src/mcp`, `src/cli`: IPC socket, authentication/permissions, JSON validation,
   `capture_id` / `StaleCapture` race conditions, `set_view` / `get_anomalies` input validation, exit codes.
4. `src/gui`: capture lifecycle (Live/Armed/Stale), history ring, `Repeat`, Setup editor, WaveformWidget
   geometry (vertical scroll, `rowAtY`, hit testing, zoom limits), object lifetime of popups, signal/slot misuse.
5. `firmware/rp2040` and `hdl/lt_capture`: buffer bounds, trigger logic, timing/CDC, protocol conformity
   with the host.
6. `CMakeLists.txt`, presets, `packaging`, `scripts`, CI: portability, warnings, install rules.
7. `tests`: gaps, flaky timing, tests that pass for the wrong reason.

## What to look for
Correctness bugs, undefined behaviour, integer overflow / signed-unsigned issues, resource and lifetime
bugs, races, error paths that lose data or report wrong results to an AI agent (the main risk: a wrong
report misleading an engineer), security issues, rule violations from the list above, missing validation,
misleading documentation. Ignore pure style.

## Evidence rules
- Every finding needs `file:line` and a concrete trigger (input, sequence or test sketch).
- Mark each finding `Confirmed` (you reproduced it or traced it fully) or `Hypothesis`.
- You may build and run tests in your own build directory: `cmake --preset default -B /tmp/audit-<model>`
  is allowed; do not write inside the source tree except your report.

## Output
Write exactly one file: `docs/audit/REPORT_<model-name>.md` (for example `REPORT_claude-opus-5.5.md`).
Structure:

```
# Audit report: <model> (audit-base 149de16)
## Summary (10 lines max, overall risk, top 5 issues)
## Findings
### F-001 [P0|P1|P2|P3] <title>
- Area: ...
- Location: file:line
- Status: Confirmed | Hypothesis
- Evidence: ...
- Impact: ...
- Suggested fix: ...
- Confidence: high | medium | low
...
## Areas reviewed with no findings
## Test gaps
## Limits of this audit (what you did not cover)
```
Severity: P0 data loss / wrong results silently / security hole; P1 likely bug in normal use;
P2 edge-case bug or robustness; P3 minor. Number findings F-001.. in order of severity.

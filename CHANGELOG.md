# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).

## [0.10.0] - 2026-11-22

### Added
- Record native Linux SocketCAN frames and GPIO v2 input edges from the CLI or GUI, without a Pico.
  Event recordings retain the kernel clock and individual timestamps, not synthetic waveform samples (#32, #34).

## [0.9.1] - 2026-11-21

### Fixed
- Installing the GUI on Linux refreshes the `hicolor` theme so an already running GNOME Dock can find
  the LogicTracer icon instead of showing a generic application icon.

## [0.9.0] - 2026-11-21

### Added
- On Linux the GUI marks serial ports held by another visible process and shows its name and PID.
  Refresh checks without opening the port; Connect checks again before sending the Pico handshake (#28).

## [0.8.0] - 2026-11-21

### Added
- Interactive Pico header view with pin numbers, GPIOs and signal names. Plan the next capture or rename
  the channels of a displayed recording without changing its samples. The read-only MCP tool
  `lt_get_pinout` gives agents the same wiring map and distinguishes a disconnected reference from a device.
- The Ask AI panel can be hidden with `_` and restored at its default size from the status bar.
- **Setup** opens the connected Pico's capture fields alongside its pin map. The built-in **Demo Pico**
  (`demo:pico`) replays the four demo signals without hardware; its settings do not replace the physical
  device's plan.
- Pin-level edge and pattern triggers, device-aware capture validation with copyable corrective
  guidance, and Reset for the active setup. Named experiment profiles can be selected in Setup
  or passed to the GUI at startup.
- Ask AI can preview recent GUI stderr without transmitting it. Operators may copy a sanitized
  snapshot with the report or explicitly share that snapshot through `lt_get_gui_diagnostics`.

### Changed
- The installed launcher and GUI use the same single-pulse strobe icon.
- New channel aliases in GUI, CLI and MCP are limited to 16 ASCII letters, digits, `_`, `.` and `-`.
  Invalid aliases are rejected rather than truncated. Existing capture files remain readable.

### Fixed
- The GUI sets its desktop ID before Qt initializes the platform plugin, so reading desktop settings
  at startup no longer prevents registration with the host portal on Wayland.

## [0.7.0] - 2026-11-21

### Added
- Session JSON contracts ([#23](https://github.com/nick-fedchik/LogicTracer/issues/23)): `logictracer-cli view --json`
  and `annotate --json` now include the standard `schema_version` and `warnings` envelope fields and are validated against
  `docs/schema/view.schema.json` and `docs/schema/annotate.schema.json`.
- Offline MCP tools (`#24`): `lt_analyze_file` (with `channels`, `measure_latency`, `expect`, `invariant`,
  `decode` and `exact_timing`) and `lt_diff_captures` (with `measure_latency` and `strict`) analyze and
  compare saved `.json`, `.vcd` and `.lac` captures without a connected device or open GUI.
- Protocol decoders (`#25`): `--decode` on `capture` and `analyze` (and `decode` in GUI/MCP) decodes
  `uart`, `spi`, `i2c` and `can` (CAN 2.0A 11-bit and 2.0B 29-bit with bit-destuffing, CRC-15 `0x4599`,
  ACK slot, and stuff/CRC/form/ACK error detection), window by window on segmented captures.
- Protocol expectations, anomaly incidents and GUI frame overlay (`#26`): `--expect` supports `frames`,
  `protocol_errors`, `nacks` and `<proto>:frames|errors|nacks` (`uart`, `spi`, `i2c`, `can`); protocol
  errors emit severity-3 anomaly incidents (`F3` navigation and `lt_get_anomalies`); the GUI adds a
  `Decode` field and draws decoded frame spans and labels over the waveform.
- Multi-channel pattern triggers (`#27`): `--trigger pattern:<base_ch>:<bits>` and `<ch>=0|1,...` across
  CLI, GUI and MCP, using RP2040 `TriggerType::Fast` (1–5 contiguous channels in `CH1..CH16` at full rate)
  and `TriggerType::Complex` (6–16 contiguous channels with a `GP0 -> GP1` jumper warning) plus
  `ReplayDevice` pattern matching.

## [0.6.2] - 2026-11-19

### Fixed
- `diff` analyzes segmented captures window by window, treats touching delay intervals as different fixed
  delays under `--strict`, and rejects a `--measure-latency` pair that one capture does not record.
  Present channels with no matched events yield null latency metrics, not evidence of jitter.
- Replay keeps `exact_timing` when it plays a recording at its own rate.
- GUI: cursors in different windows of a segmented capture report no stitched-buffer time; a cursor report
  describes the window that holds the cursors. Session reports can be bound to a capture attempt
  (`attempt_id`), also when it failed; requests rejected as invalid exit 64 through the GUI as they do
  directly and no longer mark the capture on screen stale; GUI-routed `capture` prints its results.
- `capture`/`analyze` exit 1 when a requested output cannot be written and reject two outputs sharing a
  file; `analyze --out-json` keeps annotations and device; VCD keeps hardware channel numbers; failed
  `probe` documents carry the envelope; `--json --copy` failures leave one document on stdout.

## [0.6.1] - 2026-11-17

### Added
- Self-test in the GUI (**Tools → Self-test**) and for AI agents (MCP tool `lt_self_test`); while the GUI
  holds the port, `logictracer-cli selftest` and MCP run it through the GUI session (method `selftest`).
- `logictracer-cli about` (and `--about`, `--json`) and a GUI `Help` menu with `About LogicTracer` and
  `About Qt`: version, author, licence and project links.

### Changed
- Documentation states what is not tested: the Pico 2, the 200 MHz Turbo image and Windows with a device;
  `selftest` is the way to check them.

## [0.6.0] - 2026-11-17

### Added
- JSON contracts: schemas for every `--json` document of `logictracer-cli` and for the lossless capture file
  (`docs/schema`, described in `docs/CONTRACTS.md`), an exit-code table, and conformance and determinism
  tests on every platform.
- Firmware self-test: a test-signal generator in the firmware (GP0/GP1, exact clock-cycle timing; Pico and
  Pico 2, `FEATURES` bit 2) and `logictracer-cli selftest`, which loops it back with two jumpers and checks
  trigger position, pre-trigger data, pulse widths, A -> B latency bounds (0..1000 ns), segmented trigger
  times, idle levels and the timeout against the known stimulus; JSON report (`selftest.schema.json`),
  `--keep` saves the captures of failed checks. See `docs/SELFTEST.md`.

### Fixed
- Found by the self-test: the edge-sensitive trigger put t = 0 one sample after the edge when the edge fell
  between the program's sample and its pin test; the host now aligns t = 0 to the edge when the trigger
  channel is captured (also window 1 of a segmented capture).
- Found by the self-test: when the trigger fired before the pre-trigger window filled, the oldest pre-trigger
  samples were the firmware's zero fill, shown as LOW levels (phantom edges). The firmware now reports the
  recorded-sample count (`FEATURES` bit 3); the host leaves the unrecorded samples out and warns.

### Changed
- Every JSON document carries the same envelope: `schema_version`, `command`, `status` and `warnings`
  (`diff`, `detect`, `bootloader` and error documents lacked some of them). `diff --json` lists the import
  caveats of its two files in `warnings` (the Markdown output keeps them under notes).
- A capture that ends in TIMEOUT returns about 0.6 s sooner: the firmware acknowledges the cancel
  (`CAPTURE_CANCELLED`, `FEATURES` bit 3) instead of the host waiting out its poll window.

## [0.5.0] - 2026-11-17

### Added
- PYNQ in-design capture backend: `logictracer_agent.py` serves the `lt_capture` core (UIO, `/dev/mem`, or a
  register-level model with `--sim`) over TCP, and `--port pynq://host[:port]` uses it from the CLI, GUI and
  MCP. Such captures have exact timing: latency is reported in exact clock cycles ("constant delay of N
  cycles"), and `diff` / `--expect` no longer allow ±1 sample for them.
- Release packages: every published release gets a Windows zip (Qt DLLs included), a macOS app bundle
  (with the CLI and MCP server inside) and a Linux tarball for Ubuntu 24.04, built from the tag in CI.
- GUI: a `Windows` field next to `Samples` records a segmented capture from the GUI; the pre-flight line
  shows the window count, whether trigger times are recorded and the samples stored.

### Fixed
- Windows: file paths with non-ASCII characters (UTF-8 is converted explicitly instead of through the
  ANSI code page).
- Windows packages ship the MSVC runtime next to the programs (no Visual C++ Redistributable needed); the
  packaging smoke test runs without Qt on `PATH`.
- Independent review (Gemini 4 Carbon) of the PYNQ backend before release: pre-trigger history after long
  waits, cancel/disconnect handling, lost-connection reporting, 16/24-channel limits, IPv6 addresses, and
  exact-vs-asynchronous delay comparison in `diff`.

## [0.4.0] - 2026-11-17

### Added
- Windows 10/11 (x64, MSVC 2022, Qt 6.8) builds with warnings as errors and passes the test suites in CI
  (emulator-based tests are skipped); macOS (Homebrew Qt) builds and passes every suite. The GUI session
  uses a user-only named pipe on Windows and verifies the server's user.
- Segmented capture: `logictracer-cli capture --segments N --trigger ...` records N trigger windows in one run
  (firmware `loopCount`; the edge-sensitive trigger re-arms between windows). Each window is analysed on its own
  and the summary, latency, `--expect` and `--invariant` results are combined; the JSON gains `segments`
  (layout and one row per window), latency events and invariant examples carry their window, and the events
  CSV a `segment` column. `analyze` re-splits a saved segmented capture; VCD/CSV exports are refused because
  the time between windows is not recorded. Replay devices support it. The GUI session and MCP accept
  `segments`; the GUI marks every window start with a dashed seam and analyses (report, anomaly markers)
  window by window.
- Trigger times of segmented windows: firmware with `FEATURES` bit 1 records burst timestamps with the
  edge-sensitive trigger (up to 254 windows); the host converts them to `trigger_times_us` (one system clock
  cycle resolution, rollover-safe) and reports the trigger-to-trigger spacing. Replay windows carry exact
  times. The firmware no longer misdates a stamp taken right at a SysTick rollover.
- `--invariant` protocol rules for `capture` and `analyze` (and the `invariant` session/MCP parameter):
  `never(EXPR)`, `always(EXPR)` with `! & | ( )` over channels, `onehot`/`onehot0`, `within(A[:edge], B[:edge], T)`,
  `stable(D..., CLK[:edge], setup=T, hold=T)` and `bus(D..., settle=T[, gap=G])`, with an optional `settle=`
  glitch allowance. One pass over the transitions; results go to `invariants` in the JSON (count, first 16
  violations with sample and time, `censored` edges too close to the end) and to the AI report; exit 3 on a
  violation, an unknown channel fails its rule.
- `logictracer-cli diff A B [--measure-latency S..R] [--strict] [--json]`: compares two captures of one test
  (latency shift beyond the ±1-sample quantization, drops, spurious reactions, anomaly kinds, channel activity)
  with a `regression` / `improved` / `equivalent` verdict; `--strict` exits 3 on a regression.
- Edge-sensitive trigger: LogicTracer firmware reports `FEATURES:1` and accepts trigger type 4, which fires only
  on a real transition (a rising trigger on a line that is already HIGH waits for the next edge). The host uses
  it for every user trigger when available; the capture JSON records `trigger.sensitivity` (`edge`/`level`).
- Compact AI reports: three or more constant channels with the same level share one table row, one observation
  and one timeline row; the timeline is omitted when no channel toggles.
- Latency direction check: when every reaction lags by about one stimulus period and the opposite pairing is
  short, the report, the latency JSON (`direction_warning`) and the CLI say the channels are probably swapped.
- `LOGICTRACER_DENY_HARDWARE=1` refuses all serial device access (for audits and harnesses).
- GUI capture lifecycle: Armed (dimmed old waveform + banner) and Stale-on-timeout states, with a
  line-state diagnosis instead of a copy of the old report; history of the last 8 captures
  (`Alt+Left` / `Alt+Right`); setup metadata persists between captures; pinnable AI note.
- `capture_id` in reports, state, CLI and MCP; `annotate --capture-id` / `lt_add_annotation`
  `capture_id` reject stale targets with `StaleCapture`.
- Pre-flight `Window | Step | Pre` line, capture warnings and inline field validation.
- `F3` / `Shift+F3` anomaly navigation; RP2040 input-physics notes in probe and timeout reports.
- Waveform: overview strip, `CHx (GPx) Name` gutter with activity dot, `H` hides inactive channels,
  draggable cursors, right-click context menu (Set Cursor A/B, annotations).
- Guided onboarding: empty-state card, `File -> Load Demo Capture` (`Ctrl+D`), and non-blocking visual
  builders for the Channels, Trigger and Latency fields (the fields stay editable by hand).
- `Setup Metadata...` editor (`Edit` menu and `Setup (N)` button); vertical channel scroll
  (`Shift+wheel`) with pinned ruler, row height (`Ctrl+Shift+wheel`, `View` actions); `Repeat` capture
  toggle that stops on Cancel, timeout, error or disconnect.
- Two-way AI steering: session `set_view` / `get_anomalies`, MCP `lt_set_view` / `lt_get_anomalies`,
  CLI `view --fit/--start/--end/--center/--span/--cursor-a/--cursor-b/--clear-cursors/--hide-inactive`
  and `view --anomalies` (all accept `capture_id` and refuse stale targets with `StaleCapture`).
- `View -> Show Anomaly Markers`: automatic markers on the ruler, separate from annotations.

### Changed
- Right click no longer places Cursor B; it opens the context menu. Shift+click sets Cursor B.

### Fixed
- Firmware: the Pico W WiFi variants compile again (an LED macro that expands to a block broke the fast
  completion poll); CI now builds the Pico 2, Pico W and WiFi variants as well.
- Handshake: LogicTracer 0.x firmware now reports its real frequency, blast rate, buffer and channels.
- AI reports: the motif reference comes from the longest periodic stretch (startup glitches no longer
  produce phantom faults); consecutive failures are separate incidents; report text is flattened to one line.
- Imports/exports: VCD `x`/`z` keep the previous level and warn, the last timestamp is a sample, time bases
  finer than 2 GHz are rejected; numeric JSON fields are range-checked; writes go through an exclusive random
  temp file and report flush errors.
- Session/CLI/MCP: handler exceptions no longer crash the GUI; `capture_id` is honoured on reads and
  `clear_annotations`; the CLI forwards absolute `--out-*` paths, reports a failed report fetch instead of
  hiding it, and clients refuse sockets of other users.
- GUI: reports name the capture's own device (also loaded from files); failures started from history are
  reported; one-off session `meta` no longer wipes or persists into the engineer's setup; dialogs are
  non-blocking; Repeat waits for a busy device.
- RP2040 firmware: bounds-checked receive buffer, exact request length, validated channels/trigger/mode,
  malformed escapes rejected; the Interceptor variant compiles again.
- Waveform: cursors, anomaly markers, annotations, ruler and trigger share one sample-to-x convention.
- Session/MCP output files are written through one directory descriptor and refuse symlinked directories.
- FPGA `lt_capture`: abort/re-arm no longer overwrite the oldest record; read index/auto-increment races fixed.
- Locale-dependent decimal commas in warnings (`1,31 ms`).

## [0.3.0] - 2026-11-15

### Added
- `IDevice` device interface with `createDevice(port)`; front-ends no longer depend on the serial
  driver.
- Replay device: `--port replay:<file>` plays a saved capture back as a device (edge-triggered,
  consecutive events, integer rate decimation) in the CLI, GUI and MCP.
- `hdl/lt_capture`: in-design FPGA capture core (CERN-OHL-W-2.0) — edge-encoded ring buffer,
  pattern/edge/external/force triggers, error-free AXI4-Lite registers; self-checking Icarus
  testbench, Verilator lint and Yosys xc7 synthesis run in CTest and CI.

## [0.2.0] - 2026-11-15

### Changed
- Firmware has its own identity and version: it reports `LOGICTRACER_<BOARD>_<SemVer>` (e.g.
  `LOGICTRACER_PICO_0.2.0`), program name and USB strings `LogicTracer`. One `VERSION` file drives
  host and firmware versions. UF2 images are named `LogicTracer-firmware-<version>-<board>-<variant>.uf2`.
- The host no longer treats upstream-format `LOGIC_ANALYZER_*_V6_6` as fast-polling firmware; only
  LogicTracer firmware is. Prototype boards still running "V6_6" must be reflashed.
- Device JSON: `firmware_family` and `firmware_version` replace `firmware_major` / `firmware_minor`.

### Added
- `--meta key=value` on `capture` and `analyze` (and `meta` in the session/MCP capture call):
  test-setup metadata stored in the capture JSON, preserved by `analyze`, shown in AI reports and
  in the GUI.
- `scripts/rigs/pynq-z1/`: one-command HIL run against a PYNQ-Z1 (bitstream programming with
  SHA-256 over SSH, idle check, armed notification, verdict and artefacts).

### Fixed
- Qt 6.4: worker calls in `AsyncDevice` are queued as type-safe functors (probe never started).
- Qt 6.4: implicit `nlohmann::json` conversions disabled (QCOMPARE failed to compile).

## [0.1.0] - 2026-11-15

First public release, migrated from a private prototype.

### Added
- Pure C++20 analysis core: capture model, `SignalAnalyzer` (pulses, burst-aware frequency/duty,
  stimulus→reaction latency with histogram, nearest-rank percentiles, ±1-sample bounds,
  constant-delay test, censored/spurious/dropped events, robust outliers), lossless
  `logictracer-capture-v1` JSON, VCD/CSV/event-CSV export, `.lac`/`.vcd`/JSON import.
- AI reports (`Copy for AI`, `--ai`): Brief / Cursors A..B / Full modes, GPIO map, motif compression
  and anomaly incidents with micro-windows.
- `logictracer-cli` with `--expect` assertions (guard-banded `*_ub_ns`), `--ready-file`/`--on-armed`
  HIL hooks, clipboard export (Wayland/X11).
- `logictracer-gui` (Qt Widgets): zoom/pan, snap-to-edge cursors, AI grounding bar, IPC session.
- `logictracer-mcp`: MCP server with device calls routed through the open GUI.
- Firmware with fast completion polling, derived from LogicAnalyzer V6_5 (identified then as
  `LOGIC_ANALYZER_<BOARD>_V6_6`; renamed in 0.2.0).

[0.9.1]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.9.1
[0.9.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.9.0
[0.8.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.8.0
[0.7.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.7.0
[0.6.2]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.6.2
[0.6.1]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.6.1
[0.6.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.6.0
[0.5.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.5.0
[0.4.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.4.0
[0.3.0]: https://github.com/nick-fedchik/LogicTracer/releases/tag/v0.3.0
[0.2.0]: https://github.com/nick-fedchik/LogicTracer/tree/v0.2.0
[0.1.0]: https://github.com/nick-fedchik/LogicTracer/tree/f423041

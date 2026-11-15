# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

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

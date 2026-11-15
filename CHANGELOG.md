# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).

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
- Firmware V6_6 (fast completion polling) derived from LogicAnalyzer V6_5.

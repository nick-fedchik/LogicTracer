# Backlog

Updated: 2026-11-15.

## Done recently
- Analyzer metrology (P0): censored edge events, spurious reactions, Δs histogram, ±1-sample bounds,
  guard-banded `*_ub_ns` assertions, burst-aware frequency, edge motif and anomaly incidents in AI
  reports.
- Live rerun on a PYNQ-Z1 100-transaction burst: 100/100 events, assertions PASS; rising-edge delay
  in (10, 20) ns, falling-edge delay in (0, 20) ns.
- Migration from the private prototype into this repository: layered modules, `lt` namespace,
  CMake presets, SPDX headers, NOTICE, CI; outputs match the prototype on recorded traces.

## B-01. Publication
License: host (`src/`, `tests/`, `scripts/`, `cmake/`) GPL-3.0-or-later; firmware `firmware/rp2040`
GPL-3.0 from upstream `gusmanb/logicanalyzer` with modification notices (see `NOTICE.md`).

- [x] Clean migration from the prototype.
- [x] Regression against the prototype on recorded traces (numbers, AI reports, VCD, CSV).
- [ ] First push to GitHub; check CI (`.github/workflows/build.yml`, Ubuntu 24.04, Qt 6.4).
- [ ] Release `v0.1.0`: tag + V6_6 UF2 images, linking the corresponding source (GPL requirement
      for binaries).

## B-02. Turbo 200 MS/s (5 ns step)
- [ ] `scripts/build_firmware.sh` → `logictracer-cli bootloader --yes` → copy the Turbo image from
      `build/firmware/` → `detect`.
- [ ] Repeat the burst measurement with `--freq 200M` for both edges, several runs (different phase).
- Expected: the constant-delay interval narrows from 10 ns to 5 ns; explain the rising (10..20 ns)
  vs falling (0..20 ns) asymmetry.
- Risk: Turbo overclocks the RP2040 (400 MHz @ 1.30 V); restore the 100 MHz image afterwards.

## B-03. Fail-closed path (third channel)
- [ ] Wire the reject output (PYNQ `JA3`, `Y16`) → RP2040 `GP4` (CH3).
- [ ] Run with the invariant true: `--expect "events>=100,drops==0,REJECT:edges==0"`.
- [ ] Run with the invariant false: `--measure-latency 1..3 --expect "events>=100,COMMIT:edges==0"`
      — no commit while the invariant is violated.

## B-04. Analyzer P1
- [ ] `--invariant` rules (never / within / stable / onehot / bus + settle), O(E) over transitions.
- [ ] `logictracer-cli diff A B` (histogram shift, Δp99, Δmax, new anomalies, sampling-phase verdict).
- [ ] Firmware V6_7: edge-sensitive trigger (+2–3 PIO instructions) and re-arm for `loopCount`
      (prevents re-triggering on the same pulse); expose `loopCount + measure` for N = 1000 series.
- [ ] GUI: automatic anomaly annotations, `F3` / `Shift+F3` navigation; MCP `lt_get_anomalies`.
- [ ] Compact report for idle captures (merge identical constant channels, skip the ASCII timeline).

## B-05. Device abstraction
- [ ] `IDevice` interface (`detect`, `probe`, `capture → CaptureBuffer`) so that other capture
      hardware (sigrok-compatible analyzers, FPGA-side capture) can plug in next to `PicoConnection`.

## B-06. NVIDIA Jetson `DO1` HIL test (blocked: 4.7–10 kΩ pull-up between Pico pin 36 and pin 34)
- [ ] `./scripts/hil_j501_do1.sh`.

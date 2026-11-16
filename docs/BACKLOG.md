# Backlog

Updated: 2026-11-16.

## Done recently
- Independent audit by four models plus a re-audit (`docs/audit/`): all confirmed P0/P1 findings fixed
  (tags `audit-fixes`, `audit-fixes-2`); consolidated triage in the session notes.
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
- [x] First push to GitHub; CI (`.github/workflows/build.yml`, Ubuntu 24.04, Qt 6.4) green.
- [x] Releases: tag `v0.2.0`; GitHub release `v0.3.0` with UF2 images (100 MHz, 200 MHz Turbo) and
      `SHA256SUMS`, built from the tagged sources.
- [~] **Deferred (decision 2026-11-15):** dedicated USB PID. The firmware keeps `1209:3020`
      (pid.codes allocation of the upstream LogicAnalyzer, see `NOTICE.md`). Not needed now:
      the device is a standard USB CDC class (no vendor driver), and the host identifies
      firmware by the `LOGICTRACER_…` ID line, not by PID. Revisit if any of these happens:
      - LogicTracer gets its own hardware (PCB), which makes a pid.codes request well-founded;
      - a vendor-specific (non-CDC) USB interface/driver is introduced;
      - the upstream owner or pid.codes objects to the shared PID.
      When adopted, keep `3020` as a discovery fallback for upstream-firmware boards.

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
- [x] GUI: automatic anomaly markers (`View -> Show Anomaly Markers`, separate from annotations), MCP
      `lt_get_anomalies` / `lt_set_view`, CLI `view --anomalies` and steering flags, `F3` / `Shift+F3`
      navigation (#5-#7).
- [x] Guided UI (#5-#6): empty-state card and `File -> Load Demo Capture` (`Ctrl+D`), visual builders for
      Channels / Trigger / Latency, `Setup Metadata...` editor, vertical channel scroll and row height
      (`Shift+wheel`, `Ctrl+Shift+wheel`), `Repeat` capture.
- [x] GUI UX (#1-#4): Armed/Stale capture states, 8-capture history (`Alt+Left`/`Alt+Right`), `capture_id`
      stale-capture guard, pre-flight window and inline validation, locale-independent numbers, standard
      mouse model (cursor drag, right-click menu), overview strip, `CHx (GPx) Name` gutter, `H` hides
      inactive channels.
- [ ] Compact report for idle captures (merge identical constant channels, skip the ASCII timeline).

## B-05. Device abstraction and PYNQ-Z1 capture backend
- [x] B1: `IDevice` interface + `createDevice(port)`; `ReplayDevice` (`replay:<file>`) for tests,
      demos and offline review; CLI, GUI (`AsyncDevice`) and MCP use the interface.
- [x] B2: `lt_capture` RTL core (CERN-OHL-W-2.0) with self-checking simulation, Verilator lint and
      Yosys synthesis (18 × RAMB36E1) in CTest/CI; see [hdl/README.md](../hdl/README.md).
- [ ] B3: board agent + `PynqConnection : IDevice`; integrate `lt_capture` into one DUT top level.
- [ ] B4: live cross-check of the PL core against the RP2040 on the same pins.
      Design: [design/pynq-capture-backend.md](design/pynq-capture-backend.md).

## B-08. PYNQ-Z1 rig automation
- [x] `--meta` setup metadata and `scripts/rigs/pynq-z1/pynq-burst-hil.sh` (verified live: PASS,
      bitstream SHA-256 recorded).
- [ ] Software start of the burst instead of BTN1. The AV-11 RTL decodes bit 1 written to
      `REG_CONTROL` (0x08), but the gate answers that write with SLVERR, which can raise an
      imprecise abort in Linux. Needs a dedicated, error-free start register in the DUT RTL.

## B-06. NVIDIA Jetson `DO1` HIL test (blocked: 4.7–10 kΩ pull-up between Pico pin 36 and pin 34)
- [ ] `./scripts/hil_j501_do1.sh`.

## B-07. Portability
- [ ] Windows: replace the POSIX parts of `Session.cpp` (`getuid`, `lstat`, `/tmp` fallback) with
      Qt equivalents / named pipes, and give the pty-based test emulator a Windows alternative or
      skip those suites; add a `windows-latest` CI job (MSVC 2022, Qt via `install-qt-action`).
- [ ] macOS: add a `macos-latest` CI job (Homebrew Qt) to turn "expected to build" into "tested".

## B-09. Audit leftovers (accepted, not fixed)
- [ ] P3: annotations are drawn at the continuous-time position of a sample (`xAt(sample)`, consistent
      with the ruler) while cursors sit at the cell centre (`xAt(sample + 0.5)`); at very high zoom an
      annotation placed "at cursor A" appears half a sample to the left. Pick one convention.
- [ ] P2 (hypothesis): `resolveSafeOutputPath` validates a path before the writer creates its temp file in
      the same directory; a different user who can swap a parent directory in a shared writable location
      could still redirect the write. Open the parent directory by descriptor (`O_DIRECTORY|O_NOFOLLOW`)
      and use `openat`/`renameat`.
- [ ] Replay: a failure report for a session request ignores `mode`/`latency` (only `note` is honoured).
- [ ] Never auto-detect a physical device when a test or audit passes no `--port`: add an opt-in
      `LOGICTRACER_DENY_HARDWARE=1` guard so harnesses cannot touch a live Pico by accident.

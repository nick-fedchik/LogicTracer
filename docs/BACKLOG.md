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
- [x] `--invariant` rules (never / always / within / stable / onehot / onehot0 / bus + settle), O(E) over
      transitions; `capture`, `analyze`, session and MCP (`invariant`), exit 3 on a violation.
- [ ] Use `--invariant "never(COMMIT & REJECT)"` in B-03 once CH3 is wired.
- [x] `logictracer-cli diff A B` (latency p50/p99/max against the ±quantization of both captures, drops,
      spurious reactions, anomaly kinds, channel activity, sampling-phase verdict; `--strict` exits 3 on a
      regression).
- [x] Firmware: edge-sensitive trigger (type 4, `FEATURES:1`) with re-arm for `loopCount` (the same pulse
      cannot re-trigger). Verified on the live Pico: a falling trigger on an idle-LOW line now waits
      (TIMEOUT) instead of firing at once; immediate captures and probes unchanged.
- [x] `capture --segments N` (firmware `loopCount`, edge re-arm): window-by-window analysis combined for the
      summary, latency, `--expect` and `--invariant`; `segments` in the JSON (layout + per-window rows),
      `analyze` re-splits saved files, replay supports it. Verified on the Pico with the PYNQ burst: one BTN1
      press, 100 windows of 2 us (20 020 samples instead of ~50 000), 100/100 pairs, 0 drops, 50..60 ns.
- [x] Segmented captures through the GUI session and MCP (`segments` parameter): the GUI draws a dashed
      seam (`W2`, `W3`, ...) at every window start, reports and anomaly markers are computed window by window,
      saved JSON keeps the layout; VCD/CSV are refused for segmented captures.
- [x] Burst timestamps for the edge programs (`FEATURES:3`, `trigger_times_us`, spacing in reports).
      Verified on the Pico (2026-11-17, two BTN1 bursts, 100 windows each): trigger spacing 4.995..5.005 us
      (±1 cycle of 5 ns around the 5 us PYNQ period), span 495 us; latency 100/100 pairs.
      Observation: one run had a single 70 ns pair (window 1); the repeat and all earlier rising-edge
      captures stay within 50..60 ns, so it is not systematic. Watch for it in longer series.
- [ ] A `Windows` control in the GUI capture setup.
- [x] Edge-fire on hardware with the PYNQ burst (BTN1, 100 MHz, 1000 pre-samples): `1:rising` fires on the
      first STIM rise (sample 1000, REACT at 994); `1:falling` on an idle-LOW line waits and fires on the
      first STIM fall with the rise 201 samples earlier in the pre-trigger buffer; `2:rising` with
      `--invariant` / `--expect` passes 100/100 pairs at 50..60 ns, and deliberately wrong rules fail.
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
- [x] Compact report for idle captures: constant channels with the same level (3 or more) share one table row,
      one observation and one timeline row; the timeline is omitted when nothing toggles (a real 24-channel
      idle capture: about 1.1 kB of Markdown). The JSON stays per channel.

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
- [x] Windows: `windows-latest` CI job (MSVC 2022 with `/WX`, Qt 6.8, vcpkg nlohmann-json), all suites
      green. Win32 writer (exclusive temp file, `MoveFileExW`, no symlinked/junctioned directories in
      confined mode), session named pipe `logictracer-<user>` with a token-SID check of the server,
      `cmd.exe` for `--on-armed`, every COM port for `detect --all`, portable bit operations and `gmtime_s`.
      The pty emulator is a stub there: 37 emulator tests and 3 POSIX file-mode/symlink tests are skipped.
- [ ] Windows on hardware: `detect`, `probe`, a triggered capture and the GUI against the Pico; a
      packaged build (`windeployqt` zip in the release); UTF-8 paths (`std::filesystem::path` from UTF-8).
- [x] macOS: `macos-latest` CI job (Homebrew Qt, warnings as errors, all tests green): `getpeereid` for
      the session peer check, umask without `/proc`, `/dev/ttys*` pseudo-terminals under the hardware guard.

## B-09. Audit leftovers (accepted, not fixed)
- [x] P3: one sample-to-x convention: cursors, anomaly markers, annotations, ruler, trigger and edges all
      use the start of the sample (`xAt(sample)`).
- [x] P2 (hypothesis): `resolveSafeOutputPath` validates a path before the writer creates its temp file in
      the same directory; a different user who can swap a parent directory in a shared writable location
      could still redirect the write. Fixed: writers work through one directory descriptor
      (`openat`/`renameat`); session/MCP writes open every path component with `O_NOFOLLOW`.
- [x] A stale-state `get_report` validates `mode`, honours `note`, and lists `mode`/`latency` under
      `ignored_params` (a failed attempt has no samples to apply them to).
- [x] `LOGICTRACER_DENY_HARDWARE=1` makes every serial open fail, so test/audit harnesses cannot touch a
      live Pico by accident (replay devices are unaffected).
- [x] P2 (found on hardware, 2026-11-16; resolved by the edge-sensitive trigger): `trigger_already_active` is not raised for `--trigger 1:rising`
      while CH1 is already HIGH. The firmware zero-fills the pre-trigger ring, so the capture shows a
      fabricated rising edge at `trigger_sample - 1` and the host heuristic (`sawOppositeIdle`) is fooled
      by the zeros. Needs a validity count for the pre-trigger samples from the firmware (protocol
      extension) or a short level probe of the trigger channel before arming.
- [x] HIL on the PYNQ-Z1 burst bitstream after the audit fixes (2026-11-16, Pico firmware rebuilt from this
      tree): 100 STIM/REACT pulse pairs captured at 100 MHz, JSON/VCD/events CSV round-trip, setup and device
      identity recorded. With the lines as wired CH2 leads CH1: `2..1` gives 100 pairs, 0 drops, rising
      delay 50-60 ns, falling 70 ns (constant); the default `1..2` pairs each STIM with the *next* REACT
      (about one period, 4.94 us) and reports one false drop. Earlier BTN1 runs timed out because nobody
      pressed the button.
- [x] Hint for swapped latency direction (`direction_warning` in latency JSON, "Direction check" in the
      report, a CLI warning): when every pair delay is close to the stimulus period (here 494-495
      of 500 samples) and the reverse direction gives a tiny constant delay, say so in the report.

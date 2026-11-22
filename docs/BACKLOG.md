# Backlog

Updated: 2026-11-21.

## Scope (decision 2026-11-17)
The value of this project is the **RP2040 firmware** and the **analyzer application** (CLI, GUI, MCP), and
both must behave **predictably for a human and for an AI agent**: stable, documented outputs; claims
(timing bounds, triggers, timeouts, exit codes) that are tested. Work is judged against that.
Hardware designs under test, their bitstreams and their RTL are out of scope; the project uses
open-source tools only (no Vivado). Markers: `[x]` done, `[ ]` open, `[-]` not planned (with the reason).

## B-10. Contracts for humans and AI (priority)
- [x] JSON Schemas for the documents of `capture`, `analyze`, `diff`, `probe`, `detect`, `bootloader`,
      errors and the lossless capture file ([CONTRACTS.md](CONTRACTS.md), `docs/schema`); one envelope
      (`schema_version`, `command`, `status`, `warnings`) for every document; exit-code table; conformance
      tests on every platform through replay devices, including documents without data (no edges, no pairs).
- [x] Determinism: identical runs give identical JSON and AI reports apart from `x-volatile` fields.
- [x] Schemas for the GUI session responses (`annotate`, `view`).

## B-11. Firmware self-test without external hardware — done
- [x] Test-signal generator in the firmware (GP0/GP1, exact clock cycles, PIO + DMA), advertised in
      `FEATURES` bit 2; jumpers GP0 → CH3 (GP4), GP1 → CH4 (GP5).
- [x] `logictracer-cli selftest`: triggers, pre-trigger data, latency bounds against the known delay,
      segments and trigger times, idle, timeouts; a pass/fail JSON report (`docs/SELFTEST.md`).
- Verified on a Pico at 1, 3, 10, 24, 50 and 100 MHz; 50 consecutive runs at 100 MHz passed. It found and
  fixed the t = 0 off-by-one, the phantom pre-trigger samples, the generator being cut off by captures and
  the slow TIMEOUT.
- [x] In the GUI (Tools → Self-test) and MCP (`lt_self_test`), routed through the GUI session when it
      holds the port; verified on the Pico through the GUI, CLI and MCP.
- Pico 2: the generator builds for it but has never run on one (no board; not planned). The Turbo image
  has not been run through the self-test either (see B-02).

## B-12. Hide and restore the Ask AI panel
- [x] `_` hides only **Ask AI**. **Show Ask AI** in the status bar restores the default panel size, not the
      last dragged size, and keeps the note and report.

## B-13. Device-specific capture setup
- [x] **Setup** opens **Pico pinout and wiring → Next capture setup** for a Pico or a demo. **Channels**,
      **Rate**, **Samples**, **Windows**, **Trigger** and **Timeout** configure the next capture without
      modifying the displayed recording. Disconnecting disables Setup; viewing a saved Pico capture does
      not enable it. File replay without a known header retains generic capture fields, not a guessed pin map.
      **Metadata** in the Ask AI panel remains a separate report editor.

## B-14. Choose the trigger directly on the Pico pinout
- [x] In **Next capture setup**, replace the Trigger text field and `…` popup with controls beside the pins.
      Keep the trigger independent of the selected capture pins: changing what fires the trigger must not change
      what gets recorded. Clearing the trigger returns to immediate capture.
      For an edge, select `↑` or `↓` on one input; selecting another replaces it. Include `EXT TRIG` on a
      physical Pico. For a pattern, select `0` or `1` on consecutive channels within CH1–CH16: all levels
      must match at once. Flag gaps and `Windows > 1`; show the GP0–GP1 jumper requirement on physical Pico.
      Acceptance: with the demo recording CH1 and CH2, set `CH3 ↑`, switch to `CH3=0 AND CH4=1`, then clear
      the trigger. Each capture must use the chosen condition and return only CH1 and CH2, without typing
      or opening a popup. No change to the firmware or CLI/MCP syntax.

## B-15. Validate capture settings before enabling Capture
- [x] In **Next capture setup**, check the plan against the active device's limits whenever the device,
      Channels, Rate, Samples, Windows or Trigger changes. Use the same limits as capture, including Pico
      buffer capacity and replay's available samples at the selected rate. Show the requested value and
      the allowed limit beside the offending field, not only in a tooltip or after a failed capture.
      Disable **Capture** while the plan is invalid; re-enable it when corrected and the device is idle.
      Invalid capture-only settings must not disable **Probe**.
      Acceptance: on a connected **Demo Pico**, with no trigger and one window, `Rate=500k, Samples=3000`
      must show “requested 3000; maximum 2000 at 500k (4 ms)” before any request is sent. Changing Samples
      to `2000` must clear the error and allow capture. On an emulated Pico, also check an out-of-range rate
      and a single triggered window too large for the selected channels.

## B-16. Reset capture setup to device defaults
- [x] Add **Reset** to **Next capture setup**. One click restores **Channels, Rate, Samples, Windows,
      Trigger and Timeout** to the active device's defaults, even when the current input is invalid.
      Update the selected pins, channel names and validation immediately, without a separate **Apply**.
      Reset affects only the next capture plan: it does not start a capture, change the displayed
      recording or history, or overwrite the other device's plan.
      Acceptance: change all six fields and leave one invalid. After **Reset**, all six must match a
      fresh setup for the same device and the error must clear. Check both **Demo Pico** (four named
      channels, `Samples=1k`) and an emulated standard Pico (`Channels=1-8`, `Samples=10k`).

## B-17. Save and switch named experiment profiles
- [x] In **Next capture setup**, let the operator save the current plan under a name and select a saved
      profile from a **Profile** list. Keep profiles across application restarts and independent of
      port or board identity, so two Picos used on the same USB port can have different setups.
      Save and restore every setup field: channel numbers in capture order with their user-assigned
      names, Rate, Samples, Windows, Timeout and the complete Trigger (none, edge with source and
      polarity, or pattern with channels and levels). Selecting a profile updates the pin map and checks
      the plan against the connected device without starting a capture or changing the displayed recording.
      Editing fields or pressing **Reset** changes only the working plan; overwriting a profile requires
      an explicit save.
- **Storage:** save one `<name>.ltprofile` file per profile in `~/.logictracer/profiles/` under the
  current user's home; create the directory on first save. No separate registry or application limit
  on the number of files stored. Profiles outside the dropdown remain loadable by name or path.
- **Dropdown:** list at most the 100 most recently modified `.ltprofile` files, newest first (filesystem
  modification time; break ties by full filename). Show at most five rows at once, with a vertical
  scrollbar only when there are more than five. Display the filename without its extension, capped at
  32 user-visible characters including `…` when shortened. Keep the full filename for loading and deletion.
  Build the list from directory metadata without opening profile files. An unordered directory may need
  a full metadata scan to find the newest 100; do that off the GUI thread and retain at most 100 candidates
  rather than collecting and sorting every filename.
- **Deletion:** provide **Delete profile** beside the selector. Show the selected profile's name and
  file path for confirmation. On confirmation remove only that file and refresh the list; on failure
  show the error. Cancelling makes no changes.
- **Startup:** accept an optional positional argument. `logictracer-gui I2C` or `logictracer-gui I2C.ltprofile`
  looks in the profiles directory. An explicit relative or absolute path, e.g.
  `logictracer-gui ./bench/UART.ltprofile`, loads that file even outside the directory. This selects the
  next capture setup without starting a capture; `--open` still opens a recording. Report a missing or
  invalid profile instead of silently falling back to defaults.
- **Acceptance:** save `I2C` (`4:SCL,3:SDA`, CH4 rising, three windows) and `UART` (`1:TX,2:RX`,
  `1=0,2=1` pattern, one window) with different rates, sample counts and timeouts. Restart the GUI,
  reconnect an emulated Pico on the same port and switch profiles: the channel order, names and all
  other settings must match each saved setup. Copy, rename or remove a profile in the filesystem: reopening the list
  reflects the change and ignores unrelated files. Launch by name and by explicit path and verify the
  same settings as when the profile is selected in Setup. Cancel deleting `UART`: both profiles remain.
  Confirm deletion: `UART.ltprofile` is gone from disk and the list after restart; `I2C` still loads unchanged.
- **Dropdown check:** with five profiles there is no scrollbar; adding a sixth enables scrolling.
  With 101 matching files whose modification times differ from creation order, scrolling must reach the
  100th newest but not the oldest; the oldest must still load by name. Two long names sharing a 32-character
  prefix must have labels of at most 32 characters and still open and delete the correct files.

## B-18. Give LogicTracer a digital strobe icon
- [x] Redesign the [application icon](../packaging/logictracer.svg) around one short digital
      pulse: a low baseline, a rising edge, a brief high level and a falling edge back to low. Use the
      same artwork for the GUI and the installed launcher; keep the Linux desktop icon name `logictracer`.
      Acceptance: both edges remain distinct at 16×16 and 32×32 px. After installation, the launcher
      and the running application in the desktop app switcher show the same strobe icon.

## B-19. Make GUI setup errors copyable and show how to fix them
- [x] With **Demo Pico** connected, set `Windows=3` and `Trigger=none`, then close Setup. The main window
      previously showed only “Error: a segmented capture needs a trigger (every window starts on a trigger
      edge)”, with no way to copy it or find the conflicting settings.
- **Expected:** name the conflicting fields and their values, then explain the valid corrections in the
  message itself, not only in a tooltip. Let the operator copy the full diagnosis and guidance as plain
  text from the main window or Setup. Provide an **Open Setup** action beside the main-window error
  that opens **Next capture setup**, focuses the relevant field and identifies any related field.
  This complements [B-15](#b-15-validate-capture-settings-before-enabling-capture); the validation rules do not change.
- **Acceptance:** in the example above, copy the message to the clipboard and verify it gives both options:
  set **Windows** to `1` for an immediate capture, or keep `3` and select a rising or falling edge on a
  channel under **Trigger**. **Open Setup** must lead directly to those controls without changing the plan
  or starting a capture. Trying each correction separately must clear the error in both views.

## B-20. Share recent GUI stderr with Copilot on request
- [x] Keep a bounded in-memory tail of `logictracer-gui`'s own stderr from startup, including Qt
      platform messages and direct stderr writes. Preserve normal terminal output. Keep the Qt
      category and severity when available; a diagnostic is not a failed capture just because it was
      written to stderr. Limit both lines and bytes and indicate when older text was omitted.
- **Access:** let the operator preview the diagnostics and opt in to sharing them from **Ask AI** or
  through a read-only GUI-session/MCP tool. Share only the current GUI session's diagnostics, not the
  terminal history or the stderr of Copilot or the MCP server. Mask credentials and process addresses
  in the shared text; do not append raw logs to every AI report or write them to MCP protocol stdout.
- **Acceptance:** an offscreen test emits a synthetic `qt.qpa.wayland.textinput` focus warning and a
  direct stderr marker. Both still reach the terminal. With sharing off, neither is exposed to Copilot;
  with sharing on, both appear in the preview and tool response, with sample addresses and tokens masked.
  Exceeding the buffer limit returns only the marked tail and does not change capture status. No Pico or
  real Wayland focus fault is needed for the test.

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
- [-] Not planned (2026-11-17): no use case needs the 5 ns step, and it runs the RP2040 outside its
      specification. The Turbo image stays in the releases marked untested; `selftest --freq 200M` is how
      anyone who needs it can check it.

## B-03. Fail-closed path (third channel)
- [-] Not planned (2026-11-17): it verifies a property of the design under test, not of the analyzer.
      `--invariant "never(COMMIT & REJECT)"` already covers the check when such a DUT is wired.

## B-04. Analyzer P1
- [x] `--invariant` rules (never / always / within / stable / onehot / onehot0 / bus + settle), O(E) over
      transitions; `capture`, `analyze`, session and MCP (`invariant`), exit 3 on a violation.
- [-] Use `--invariant "never(COMMIT & REJECT)"` in B-03 (B-03 not planned).
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
- [x] A `Windows` control in the GUI capture setup (pre-flight shows the window count, whether the time
      between windows is recorded, and the stored samples).
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
- [x] B3 (software): board agent `scripts/rigs/pynq-z1/logictracer_agent.py` (UIO or `/dev/mem`, JSON over
      TCP, loopback by default, an event-driven register model of `lt_capture` for development) and
      `PynqConnection : IDevice` (`pynq://host[:port]`, rise/fall/external triggers, cancel, probe). Captures
      carry exact timing: latency bounds collapse to exact cycles, `diff` and `--expect` drop the ±1-sample
      allowance, the flag survives the JSON. Tested against the model (Python unit tests, C++ connection test).
- [-] B3 (hardware) and B4 (live cross-check against the RP2040): not planned (2026-11-17). They need the
      core inside a DUT bitstream, which is outside the project scope. The `pynq://` backend stays
      **experimental**: verified only against the register model of the core.
      Design: [design/pynq-capture-backend.md](design/pynq-capture-backend.md).

## B-08. PYNQ-Z1 rig automation
- [x] `--meta` setup metadata and `scripts/rigs/pynq-z1/pynq-burst-hil.sh` (verified live: PASS,
      bitstream SHA-256 recorded).
- [-] Software start of the burst instead of BTN1: not planned here (2026-11-17), it is a change of the DUT
      RTL. The B-11 self-test removes the need for the button in firmware regression.

## B-06. NVIDIA Jetson `DO1` HIL test (deferred; blocked: 4.7–10 kΩ pull-up between Pico pin 36 and pin 34)
- [ ] `./scripts/hil_j501_do1.sh`.

## B-07. Portability
- [x] Windows: `windows-latest` CI job (MSVC 2022 with `/WX`, Qt 6.8, vcpkg nlohmann-json), all suites
      green. Win32 writer (exclusive temp file, `MoveFileExW`, no symlinked/junctioned directories in
      confined mode), session named pipe `logictracer-<user>` with a token-SID check of the server,
      `cmd.exe` for `--on-armed`, every COM port for `detect --all`, portable bit operations and `gmtime_s`.
      The pty emulator is a stub there: 37 emulator tests and 3 POSIX file-mode/symlink tests are skipped.
- [-] Windows on hardware: not planned (2026-11-17). The packages are built and tested in CI only; the
      release notes and README say so. A Windows user can verify with `logictracer-cli selftest`.
- [x] Packaged builds for Windows/macOS/Linux attached to releases (`release.yml`); UTF-8 paths converted
      explicitly (`core/Paths.hpp`, non-ASCII round-trip test).
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

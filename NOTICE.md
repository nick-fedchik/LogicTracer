# Notices and attribution

## LogicAnalyzer (upstream)

LogicTracer is based on, and wire-compatible with, **LogicAnalyzer** by **Agustín Gimenez Bernad**
(<https://github.com/gusmanb/logicanalyzer>), distributed under the GNU General Public License v3.0.

Material taken from that project:

| LogicTracer path | Upstream path | Status |
|---|---|---|
| `firmware/rp2040/` | `Firmware/LogicAnalyzer_V2/` | Modified (see below) |
| `LICENSE` | `LICENSE` | Unchanged GPL-3.0 text |

The host software in `src/` was written from scratch for LogicTracer. It implements the
LogicAnalyzer serial protocol (documented from the upstream firmware sources) and reads the upstream
`.lac` session format; it contains no code from the upstream C# application.

"LogicAnalyzer" is used here only to identify the compatible upstream firmware and file formats.
LogicTracer is not affiliated with or endorsed by the upstream project.

### Changes to the upstream firmware (GPL-3.0 §5a)

Modified files carry a notice at the top. Changes, 2026, by Mykola Fedchyk:

1. `LogicAnalyzer.c`: the capture loop checks for completion/cancel every 10 ms (previously
   2 × 1 s, LED blink rate preserved) and the pre-transfer USB delays were shortened from 200 ms to
   7 ms. Turnaround drops from ~2.2 s to ~15 ms.
2. `CMakeLists.txt`: firmware version `V6_6`.
3. `LogicAnalyzer_Build_Settings.cmake`: `BOARD_TYPE` and `TURBO_MODE` can be set with `-D`;
   `TURBO_MODE` defaults to `0` (no 400 MHz overclock unless requested).
4. Removed upstream IDE/publishing files (`.vscode/`, `publish.ps1`).

The version string `LOGIC_ANALYZER_PICO_V6_6` keeps the upstream format so that both LogicTracer and
the upstream host software recognise the device.

## Third-party dependencies (not bundled)

| Component | License |
|---|---|
| Qt 6 (Core, Network, SerialPort, Widgets, Test) | LGPL-3.0 / GPL-3.0 |
| nlohmann/json | MIT |
| Raspberry Pi Pico SDK (firmware build only) | BSD-3-Clause |

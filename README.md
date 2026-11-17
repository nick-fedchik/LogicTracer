# LogicTracer

**Evidence-grade logic analysis for humans and AI agents.**

LogicTracer is a native C++20 / Qt 6 host for low-cost RP2040/RP2350 logic analyzers (Raspberry Pi
Pico and compatible boards running the LogicAnalyzer firmware). It is built for
hardware-in-the-loop (HIL) verification: the engineer and an AI agent look at *the same* data, and
every timing claim comes with its measurement uncertainty and a pass/fail verdict.

> LogicTracer is an independent project based on, and compatible with, the
> [LogicAnalyzer](https://github.com/gusmanb/logicanalyzer) firmware by Agustín Gimenez Bernad
> (GPL-3.0). It is not affiliated with or endorsed by that project. See [NOTICE.md](NOTICE.md).

## Why

| | |
|---|---|
| **Human–AI grounding** | `Copy for AI`, an MCP server and an IPC session: an agent sees the engineer's view (range, cursors, pins) and can place annotations on the live waveform. |
| **Honest metrology** | Stimulus→reaction latency with a Δs histogram, nearest-rank percentiles, ±1-sample bounds, a constant-delay consistency test, burst-aware frequency, censored/spurious/dropped event accounting. |
| **Automated verdicts** | `--expect "p99_ub_ns<=60,drops==0"` with guaranteed bounds; exit codes for CI and HIL scripts. |
| **Anomaly spotlight** | Repeating edge motifs are compressed to one line; deviations are reported as incidents with a ±4-transition window. |
| **Fast turnaround** | LogicTracer firmware: ~40–80 ms probe/capture round trip instead of ~2–8 s. |
| **Open formats** | Lossless compact JSON (`logictracer-capture-v1`), VCD, CSV, event CSV; imports legacy `.lac`. |

## Components

| Path | What |
|---|---|
| `src/core` | Pure C++20 (no Qt): wire protocol, capture model, `SignalAnalyzer`, AI reports, file formats |
| `src/device` | Qt SerialPort driver (`PicoConnection`) and its worker-thread wrapper (`AsyncDevice`) |
| `src/services` | Operations shared by all front-ends, IPC session, clipboard |
| `src/cli` | `logictracer-cli`: `detect`, `probe`, `capture`, `analyze`, `diff`, `annotate`, `view`, `bootloader` |
| `src/gui` | `logictracer-gui`: waveform viewer with cursors, snap-to-edge and the AI grounding bar |
| `src/mcp` | `logictracer-mcp`: Model Context Protocol server (JSON-RPC over stdio) |
| `firmware/rp2040` | LogicTracer firmware for RP2040, derived from LogicAnalyzer (GPL-3.0) |
| `hdl/` | `lt_capture`: in-design FPGA capture core, exact clock-cycle timing (CERN-OHL-W-2.0) |
| `docs/` | Specification and backlog |

## Platform support

| OS | Status | Notes |
|---|---|---|
| **Ubuntu 24.04 LTS** (x86_64) | ✅ Supported, tested in CI | Qt 6.4, GCC 13 |
| **Ubuntu 26.04 LTS** (x86_64) | ✅ Supported, tested on hardware | Qt 6.10, GCC 15 |
| Other Linux (Debian 12+, Fedora 39+, aarch64 / NVIDIA Jetson) | ⚠️ Expected to work, not tested | needs Qt ≥ 6.4 and a C++20 compiler |
| **macOS** (Apple silicon, `macos-latest`) | ✅ Built and tested in CI (no hardware run yet) | Homebrew Qt 6; device appears as `/dev/cu.usbmodem*` |
| **Windows** 10/11 (x64) | ✅ Built and tested in CI (no hardware run yet) | MSVC 2022, Qt 6.8; device appears as `COMn`; emulator-based tests are skipped (no pseudo-terminals) |

Requirements: CMake ≥ 3.24, Ninja, a C++20 compiler (GCC ≥ 12, Clang ≥ 15, Apple Clang ≥ 15),
Qt ≥ 6.4 (Core, Network, SerialPort, Widgets, Test) and nlohmann_json ≥ 3.10.

## Install dependencies

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-serialport-dev nlohmann-json3-dev

# Optional: clipboard support for `--copy` (Wayland or X11)
sudo apt install wl-clipboard xclip

# Serial port access (log out and back in afterwards)
sudo usermod -aG dialout $USER
```

### Fedora

```bash
sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtserialport-devel json-devel
sudo usermod -aG dialout $USER
```

### macOS (Homebrew)

```bash
xcode-select --install
brew install cmake ninja qt nlohmann-json
export CMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

No extra driver is needed: the RP2040 is a USB CDC device. The clipboard uses the built-in
`pbcopy`.

### Windows 10/11

Visual Studio 2022 (MSVC, C++20, "Desktop development with C++"), Qt 6.4+ for MSVC 2022 64-bit
with the Qt Serial Port module (Qt Online Installer), and nlohmann-json from vcpkg. In a
"x64 Native Tools Command Prompt":

```bat
vcpkg install nlohmann-json:x64-windows
cmake --preset default -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64;%VCPKG_ROOT%/installed/x64-windows"
cmake --build --preset default
ctest --preset default
```

The device is a standard USB CDC port (`COM3`, ... ; `logictracer-cli detect` finds it by VID:PID,
`--all` lists every COM port). The GUI session uses the named pipe `logictracer-<user>`, limited to the
current user. Run `windeployqt` on the executables to copy the Qt DLLs next to them. Not yet tested on
hardware; paths with non-ASCII characters may not work.

### Firmware toolchain (optional)

Only needed to rebuild the RP2040 firmware: the Arm GNU toolchain (`arm-none-eabi-gcc`; on Ubuntu
`sudo apt install gcc-arm-none-eabi`) and the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x
(`PICO_SDK_PATH`).

## Build

```bash
cmake --preset default          # or: debug (warnings as errors), release, headless (no GUI)
cmake --build --preset default
ctest --preset default
sudo cmake --install build      # optional: installs logictracer-cli, -gui, -mcp
```

## Quick start

```bash
build/src/cli/logictracer-cli detect
build/src/cli/logictracer-cli probe --port /dev/ttyACM0 --channels 1:STIM,2:REACT --ai

# Wait for a burst, measure STIM->REACT, assert, export, and print an AI-ready report
build/src/cli/logictracer-cli capture --port /dev/ttyACM0 --freq 100M --samples 131k \
    --channels 1:STIM,2:REACT --trigger 1:rising --timeout 60000 \
    --measure-latency 1..2 --expect "events>=100,drops==0,p99_ub_ns<=60" \
    --invariant "within(STIM, REACT, 100ns); never(STIM & !REACT, settle=200ns)" \
    --out-json run.json --out-vcd run.vcd --ai --copy

build/src/cli/logictracer-cli analyze run.json --measure-latency 1:falling..2:falling --ai
# One trigger window per pulse: 100 windows of 2 us in one run (time between windows not recorded)
build/src/cli/logictracer-cli capture --port /dev/ttyACM0 --freq 100M --samples 220 --pre-samples 20 \
    --segments 100 --channels 1:STIM,2:REACT --trigger 2:rising --measure-latency 2..1 --expect "drops==0"
build/src/cli/logictracer-cli diff baseline.json run.json --measure-latency 1..2 --strict   # exit 3 on a regression
build/src/gui/logictracer-gui --port /dev/ttyACM0
```

Exit codes: `0` OK, `1` error, `2` trigger timeout, `3` `--expect` / `--invariant` failed (or `diff --strict`
found a regression), `64` usage error.

`--port replay:<file>` plays a saved capture (`.json`, `.vcd`, `.lac`) back as if it were a device:
triggered captures find the next real edge in the recording, so the CLI, GUI, MCP and HIL scripts
can be developed and demonstrated without hardware.

`--meta key=value` (repeatable) records the test setup with the result — device under test,
bitstream hash, wiring, switch positions. It is stored in the capture JSON, kept by `analyze` and
shown in AI reports, so every verdict can be traced back to exactly what was connected.

### HIL rigs

`scripts/rigs/` holds ready-made runs for concrete benches. `scripts/rigs/pynq-z1/pynq-burst-hil.sh`
programs a bitstream into a Digilent PYNQ-Z1 over SSH (recording its SHA-256), checks that the lines
are idle, arms the capture, tells the operator when to start the stimulus, and saves JSON, VCD,
events CSV and an AI report with the verdict:

```bash
scripts/rigs/pynq-z1/pynq-burst-hil.sh --bitstream /home/xilinx/my_design.bit \
    --switches "SW0=1,SW1=1" --start "press BTN1"
```

### MCP

```json
{ "mcpServers": { "logictracer": { "command": "/path/to/logictracer-mcp" } } }
```

Tools: `lt_detect_devices`, `lt_probe_inputs`, `lt_configure_and_capture`, `lt_get_view`,
`lt_add_annotation`, `lt_clear_annotations`. While `logictracer-gui` is open, device calls are routed
through it so the engineer sees what the agent requested.

## Firmware

`firmware/rp2040` holds the LogicTracer firmware, derived from the LogicAnalyzer V2 firmware (fast
completion polling). It identifies itself as `LOGICTRACER_<BOARD>_<version>`. Build and flash with
`scripts/build_firmware.sh` (needs the Pico SDK 2.x and `arm-none-eabi-gcc`). The host also works
with unmodified upstream firmware (V5_x, V6_0–V6_5); it then waits ~2 s longer per capture.

## Versioning

[Semantic Versioning 2.0](https://semver.org/). Host and firmware share one version, defined in the
[`VERSION`](VERSION) file; see [CHANGELOG.md](CHANGELOG.md).

## License

- Host software (`src/`, `tests/`, `scripts/`, `cmake/`): **GPL-3.0-or-later**.
- Firmware (`firmware/rp2040/`): **GPL-3.0**, derived from LogicAnalyzer by Agustín Gimenez Bernad.
- FPGA capture core (`hdl/`): **CERN-OHL-W-2.0** ([hdl/LICENSE](hdl/LICENSE)), so it can be embedded
  in proprietary designs; changes to the core itself stay open.

The full license text is in [LICENSE](LICENSE); attribution and the list of changes are in
[NOTICE.md](NOTICE.md).

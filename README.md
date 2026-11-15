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
| **Fast turnaround** | Fast-poll firmware V6_6: ~80 ms probe/capture round trip instead of ~2–8 s. |
| **Open formats** | Lossless compact JSON (`logictracer-capture-v1`), VCD, CSV, event CSV; imports legacy `.lac`. |

## Components

| Path | What |
|---|---|
| `src/core` | Pure C++20 (no Qt): wire protocol, capture model, `SignalAnalyzer`, AI reports, file formats |
| `src/device` | Qt SerialPort driver (`PicoConnection`) and its worker-thread wrapper (`AsyncDevice`) |
| `src/services` | Operations shared by all front-ends, IPC session, clipboard |
| `src/cli` | `logictracer-cli`: `detect`, `probe`, `capture`, `analyze`, `annotate`, `view`, `bootloader` |
| `src/gui` | `logictracer-gui`: waveform viewer with cursors, snap-to-edge and the AI grounding bar |
| `src/mcp` | `logictracer-mcp`: Model Context Protocol server (JSON-RPC over stdio) |
| `firmware/rp2040` | Modified LogicAnalyzer firmware V6_6 (GPL-3.0) |
| `docs/` | Specification and backlog |

## Platform support

| OS | Status | Notes |
|---|---|---|
| **Ubuntu 24.04 LTS** (x86_64) | ✅ Supported, tested in CI | Qt 6.4, GCC 13 |
| **Ubuntu 26.04 LTS** (x86_64) | ✅ Supported, tested on hardware | Qt 6.10, GCC 15 |
| Other Linux (Debian 12+, Fedora 39+, aarch64 / NVIDIA Jetson) | ⚠️ Expected to work, not tested | needs Qt ≥ 6.4 and a C++20 compiler |
| **macOS** 13+ | ⚠️ Expected to build, not tested | device appears as `/dev/cu.usbmodem*` |
| **Windows** 10/11 | ❌ Not supported yet | the IPC session and the test emulator use POSIX APIs; the core library and the analysis CLI commands are portable |

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

### macOS (Homebrew, untested)

```bash
xcode-select --install
brew install cmake ninja qt nlohmann-json
export CMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

No extra driver is needed: the RP2040 is a USB CDC device. The clipboard uses the built-in
`pbcopy`.

### Windows (not supported yet)

A port is planned. The intended toolchain is Visual Studio 2022 (MSVC, C++20), Qt 6.4+
for MSVC 2022 64-bit with the Qt Serial Port module (Qt Online Installer), and
`vcpkg install nlohmann-json`.

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
    --out-json run.json --out-vcd run.vcd --ai --copy

build/src/cli/logictracer-cli analyze run.json --measure-latency 1:falling..2:falling --ai
build/src/gui/logictracer-gui --port /dev/ttyACM0
```

Exit codes: `0` OK, `1` error, `2` trigger timeout, `3` `--expect` failed, `64` usage error.

### MCP

```json
{ "mcpServers": { "logictracer": { "command": "/path/to/logictracer-mcp" } } }
```

Tools: `lt_detect_devices`, `lt_probe_inputs`, `lt_configure_and_capture`, `lt_get_view`,
`lt_add_annotation`, `lt_clear_annotations`. While `logictracer-gui` is open, device calls are routed
through it so the engineer sees what the agent requested.

## Firmware

`firmware/rp2040` holds the LogicAnalyzer V2 firmware with LogicTracer changes (V6_6 fast polling).
Build and flash with `scripts/build_firmware.sh` (needs the Pico SDK 2.x and `arm-none-eabi-gcc`).
The host also works with unmodified upstream firmware (V5_x, V6_0–V6_5); it then waits ~2 s longer
per capture.

## License

- Host software (`src/`, `tests/`, `scripts/`, `cmake/`): **GPL-3.0-or-later**.
- Firmware (`firmware/rp2040/`): **GPL-3.0**, derived from LogicAnalyzer by Agustín Gimenez Bernad.

The full license text is in [LICENSE](LICENSE); attribution and the list of changes are in
[NOTICE.md](NOTICE.md).

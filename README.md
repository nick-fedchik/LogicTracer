# LogicTracer

LogicTracer is a native C++20 / Qt 6 host and RP2040 firmware for low-cost logic analyzers
(Raspberry Pi Pico and compatible boards). Where the upstream
[LogicAnalyzer](https://github.com/gusmanb/logicanalyzer) project is designed as an interactive
desktop GUI, LogicTracer focuses on automated hardware-in-the-loop (HIL) verification, deterministic
CLI/CI workflows, and shared human–AI debugging: the engineer and an AI agent inspect the same
capture, and timing measurements are reported with their sampling uncertainty and pass/fail bounds.

> LogicTracer is an independent project based on, and compatible with, the
> [LogicAnalyzer](https://github.com/gusmanb/logicanalyzer) firmware by Agustín Gimenez Bernad
> (GPL-3.0). It is not affiliated with or endorsed by that project. See [NOTICE.md](NOTICE.md).

## Why

| Area | What LogicTracer provides |
|---|---|
| **Human–AI shared session** | `logictracer-gui`, `logictracer-cli` and `logictracer-mcp` share one IPC session: an AI agent reads the active capture (`capture_id` prevents stale-read races), steers the view or cursors, and places annotations directly on the engineer's waveform. |
| **Sampling uncertainty & bounds** | Cross-channel stimulus→reaction latency with a Δs histogram, nearest-rank percentiles, asynchronous `((d−1)T, (d+1)T)` bounds, constant-delay consistency check, burst-aware frequency, and separation of censored window-edge events from real drops. |
| **Automated verdicts & contracts** | `--expect`, temporal rules (`--invariant`), segmented captures (`--segments` with hardware burst timestamps), and `diff --strict` for regression checks. All `--json` outputs follow versioned JSON schemas ([docs/CONTRACTS.md](docs/CONTRACTS.md)) with deterministic exit codes. |
| **Capture integrity (firmware)** | Real edge-sensitive trigger (waiting for the opposite level before arming), exact `t = 0` edge alignment, DMA wrap tracking so unwritten pre-trigger buffer zero-fill is omitted instead of shown as `LOW`, and ~15 ms polling/cancel turnaround (vs ~2.2 s on upstream firmware). |
| **Closed-loop self-test** | Onboard PIO1 + DMA test-signal generator (`GP0`/`GP1`) looped back with two jumpers: `selftest` (CLI, GUI, MCP) verifies triggers, pre-trigger trimming, pulse widths, latency bounds, segments and timeouts against known clock-cycle ground truth. |
| **Open formats** | Lossless capture JSON (`logictracer-capture-v1`), VCD, CSV, paired latency-events CSV, and `.lac` import; any saved file can be replayed as a virtual device (`--port replay:<file>`). |

### Pico (RP2040) positioning and hardware comparison

A Raspberry Pi Pico (`RP2040`) is a **store-and-forward** capture device:
- **What it does well:** PIO state machines sample up to 24 pins at up to **100 MS/s** (10 ns step) directly into a **128 KiB on-chip SRAM** DMA ring, completely isolated from USB and host OS scheduling jitter. Triggers are evaluated in hardware by PIO, and segmented captures (`--segments`) record only the active windows around each trigger edge with 5 ns (`SysTick`) inter-window timestamps.
- **Its physical limits:** USB 1.1 Full-Speed (12 Mbit/s, ~1 MB/s) cannot stream high-speed samples continuously, so one capture is bounded by the 128 KiB SRAM (131,072 samples at 8 ch, 65,536 at 16 ch, 32,768 at 24 ch). Inputs are direct **3.3 V CMOS** GPIOs without overvoltage protection, level shifters, or adjustable voltage thresholds.

In the entry-level class, the main architectural alternative is the **Infineon (formerly Cypress) EZ-USB FX2LP (`CY7C68013A`)**, which takes the opposite **stream-first** approach: its GPIF engine feeds a 4 KiB FIFO directly into a **USB 2.0 High-Speed (480 Mbit/s)** bulk endpoint (~35–42 MB/s) with **5 V-tolerant** inputs, streaming gigabytes into host RAM, but relying on software triggers and uninterrupted USB host polling.

| Class & device | Architecture, rate & memory | Inputs | Strengths & when to choose |
|---|---|---|---|
| **RP2040 + LogicTracer** (~$4 Pico) | **Store-and-forward** (PIO → 128 KiB SRAM, USB 1.1 FS readout); up to **100 MS/s** (`--segments` for bursts) | 24 ch, fixed 3.3 V CMOS | Deterministic hardware edge trigger, zero USB drop risk during capture, 10 ns resolution, 5 ns burst timestamps, onboard `selftest` generator, CLI/CI assertions (`--expect`, `--invariant`) and MCP/AI session. Best for fast pulse/latency metrology and HIL automation. |
| **RP2040 + upstream LogicAnalyzer / sigrok-pico** (~$4) | Same RP2040 store-and-forward hardware | 24 ch, 3.3 V CMOS | Upstream `gusmanb` offers an interactive desktop GUI with pattern/blast triggers; `sigrok-pico` integrates with PulseView's protocol decoders. |
| **Infineon / Cypress FX2LP (`CY7C68013A`, `fx2lafw`)** (~$8–$15) | **Stream-first** (GPIF → 4 KiB FIFO → USB 2.0 HS 480 Mbit/s to host RAM); up to **24 MS/s** (8 ch) / 12 MS/s (16 ch) | 8–16 ch, **5 V-tolerant** | Continuous multi-second/minute captures of UART/I²C/SPI and 100+ protocol decoders in sigrok/PulseView; 5 V tolerance. Limited to 24 MS/s, software triggers only, and aborts if the host OS delays USB reads beyond the 4 KiB FIFO. |
| **USB 3.0 SuperSpeed streamers** (Infineon **EZ-USB FX3 `CYUSB3014`**, ~$35–$60 boards) | **Stream-first** (GPIF II 32-bit @ 100 MHz → 512 KiB SRAM → USB 3.0 5 Gbit/s); 100+ MS/s continuous | 16–32 ch, 1.8–3.3 V I/O | Combines 100 MHz synchronous GPIF II sampling with ~350–400 MB/s USB 3.0 streaming when neither 128 KiB SRAM nor 24 MS/s USB 2.0 is enough. |
| **Mid-range FPGA analyzers** (DreamSourceLab **DSLogic Plus / U3Pro**, Digilent **Digital Discovery**, ~$100–$300) | **FPGA + SDRAM buffer + USB 2.0/3.0**; 400 MS/s – 1 GS/s, 256 Mbit–2 Gbit RAM | 16–32 ch, programmable thresholds (1.2–5 V), shielded probes | Deep hardware memory, adjustable logic thresholds (1.8 V / 2.5 V / 3.3 V / 5 V), input protection and impedance control. |
| **Professional USB 3.0 analyzers** (**Saleae Logic Pro 8 / 16**, ~$500–$1,500) | Up to 500 MS/s digital + 50 MS/s analog, USB 3.0 continuous streaming | 8–16 ch, programmable thresholds + analog | Simultaneous analog/digital capture, multi-gigabyte streaming, mature protocol analyzers and SDK. |

## Components

| Path | What |
|---|---|
| `src/core` | Pure C++20 (no Qt): wire protocol, capture model, `SignalAnalyzer`, AI reports, file formats |
| `src/device` | Qt SerialPort driver (`PicoConnection`) and its worker-thread wrapper (`AsyncDevice`) |
| `src/services` | Operations shared by all front-ends, IPC session, clipboard |
| `src/cli` | `logictracer-cli`: `detect`, `probe`, `capture`, `analyze`, `diff`, `annotate`, `view`, `selftest`, `bootloader`, `about` |
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
| **Windows** 10/11 (x64) | ✅ Built and tested in CI (not tested with a device; `selftest` checks one) | MSVC 2022, Qt 6.8; device appears as `COMn`; emulator-based tests are skipped (no pseudo-terminals) |

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
```

No extra driver is needed: the RP2040 is a USB CDC device. The clipboard uses the built-in
`pbcopy`.

### Windows 10/11

1. Visual Studio 2022 or newer with the "Desktop development with C++" workload (MSVC, CMake and
   Ninja are included).
2. Qt 6.4+ for **MSVC 2022 64-bit** with the **Qt Serial Port** module (Qt Online Installer), e.g.
   `C:\Qt\6.8.3\msvc2022_64`.
3. nlohmann-json through a standalone vcpkg (the copy bundled with Visual Studio only supports
   manifest mode):

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install nlohmann-json:x64-windows
```

No driver is needed: the RP2040 is a standard USB CDC device and appears as `COM3`, `COM4`, ...

### Firmware toolchain (optional)

Only needed to rebuild the RP2040 firmware (Linux or macOS): the Arm GNU toolchain
(`arm-none-eabi-gcc`; on Ubuntu `sudo apt install gcc-arm-none-eabi`) and the
[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x (`PICO_SDK_PATH`). The Pico W
WiFi variants also need the SDK submodules `lib/cyw43-driver` and `lib/lwip`
(`git -C "$PICO_SDK_PATH" submodule update --init lib/cyw43-driver lib/lwip`). See
[firmware/rp2040/README.md](firmware/rp2040/README.md) and `scripts/build_firmware.sh`.

## Build

The CMake presets use Ninja: `default` (RelWithDebInfo), `debug` (warnings as errors), `release`,
`headless` (no GUI). Binaries land in `build/src/cli`, `build/src/gui` and `build/src/mcp`.

### Linux

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
sudo cmake --install build      # optional: installs logictracer-cli, -gui, -mcp to /usr/local
```

### macOS

```bash
cmake --preset default -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset default
ctest --preset default
cmake --install build --prefix "$HOME/.local"   # optional
```

### Windows

In an "x64 Native Tools Command Prompt for VS" (it puts MSVC, CMake and Ninja on `PATH`):

```bat
cmake --preset default -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64;C:/vcpkg/installed/x64-windows"
cmake --build --preset default
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%
ctest --preset default
```

The executables need the Qt DLLs at run time: either keep Qt's `bin` on `PATH` as above, or copy them
next to the programs with `C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe build\src\gui\logictracer-gui.exe`
(repeat for `logictracer-cli.exe` and `logictracer-mcp.exe`). Tests that need the pseudo-terminal device
emulator are skipped on Windows. Not yet tested against hardware on Windows.

## Quick start

Examples use the Linux port name; on macOS it is `/dev/cu.usbmodem…`, on Windows `COM3` (and
`build\src\cli\logictracer-cli.exe`). `logictracer-cli detect` lists the actual name.

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

JSON output follows the schemas in [docs/schema](docs/schema) ([contracts](docs/CONTRACTS.md)).
Exit codes: `0` OK, `1` error, `2` trigger timeout, `3` `--expect` / `--invariant` failed (or `diff --strict`
found a regression, or a `selftest` check failed), `64` usage error.

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

### In-design capture on the PYNQ-Z1 (`pynq://`, experimental)

> Experimental: verified against the register model of the core only; running it needs `lt_capture` inside
> your FPGA design, which this project does not provide.

With the `lt_capture` core in the FPGA design ([hdl/README.md](hdl/README.md)), the board agent serves it to
every LogicTracer front-end. Samples are the design's own clock cycles, so delays are exact:

```bash
# on the board (Python 3 standard library only; --devmem 0x43C00000 instead of --uio works as root)
python3 logictracer_agent.py --uio /dev/uio0 --probes STIM,REACT
# on the host: tunnel the agent's loopback port, then use it like any device
ssh -N -L 7878:localhost:7878 xilinx@pynq &
build/src/cli/logictracer-cli capture --port pynq://localhost:7878 --freq 100M --samples 10k \
    --channels 1:STIM,2:REACT --trigger 1:rising --measure-latency 1..2
```

`--sim` runs the agent with a register-level model of the core (no board needed). Channel numbers are probe
bits + 1; the next number is the core's external trigger input.

### MCP

```json
{ "mcpServers": { "logictracer": { "command": "/path/to/logictracer-mcp" } } }
```

Tools: `lt_detect_devices`, `lt_probe_inputs`, `lt_configure_and_capture`, `lt_self_test`, `lt_get_view`,
`lt_set_view`, `lt_get_anomalies`, `lt_add_annotation`, `lt_clear_annotations`. While `logictracer-gui` is
open, device calls are routed through it so the engineer sees what the agent requested. `lt_self_test` lets
an agent check the analyzer itself (two jumpers, see [docs/SELFTEST.md](docs/SELFTEST.md)) before it
trusts a measurement.

## Firmware

`firmware/rp2040` holds the LogicTracer firmware (`LOGICTRACER_<BOARD>_<version>`), derived from the
upstream LogicAnalyzer V6_5 firmware. Build and flash with `scripts/build_firmware.sh` (needs the
Pico SDK 2.x and `arm-none-eabi-gcc`). Relative to upstream V6_5, it adds opt-in extensions
advertised via `FEATURES`:
- **Edge-sensitive trigger (type 4)** and **hardware burst timestamps** across segmented captures;
- **Recorded-sample count** in the capture trailer so the host omits unwritten pre-trigger RAM when a
  trigger fires before the pre-buffer fills;
- **10 ms completion/cancel polling** and `CAPTURE_CANCELLED` acknowledgement (~15 ms turnaround);
- **PIO1 + DMA test-signal generator** on `GP0`/`GP1` for closed-loop self-testing.

The host remains compatible with unmodified upstream firmware (V5_x, V6_0–V6_5), falling back to
level-sensitive triggers and the ~2.2 s poll interval.

After flashing, check the analyzer against its own test-signal generator (two jumpers: `GP0` → `GP4`,
`GP1` → `GP5`; Pico, Pico 2 untested): `logictracer-cli selftest --port /dev/ttyACM0` (exit `0` = every check
passed). See [docs/SELFTEST.md](docs/SELFTEST.md).

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

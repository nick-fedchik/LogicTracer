# LogicTracer Specification

Native C++20 / Qt 6 tooling for hardware-in-the-loop (HIL) logic analysis with an AI agent in the loop:
core library, headless CLI, Qt 6 GUI, IPC session and MCP server.

---

## 1. Goals

### 1.1. Context
LogicTracer is compatible with the hardware and firmware of
[gusmanb/logicanalyzer](https://github.com/gusmanb/logicanalyzer): a Raspberry Pi Pico / RP2040
(or Pico 2 / RP2350) samples up to 24 digital channels at up to 100–200+ MHz using PIO and
double-buffered DMA. LogicTracer replaces the upstream .NET host with a native stack.

### 1.2. Architecture decisions
* **Language:** C++20 (RAII, `std::span`, `std::optional`, no GC pauses).
* **Framework:** Qt 6 (Core, Network, SerialPort, Widgets; custom `QPainter` waveform rendering).
* **Platforms:** Linux first (Ubuntu x86_64, aarch64 for NVIDIA Jetson); the code avoids
  Linux-only APIs except the clipboard helpers and the per-user socket directory.
* **Purpose:** one toolchain for an engineer and an AI agent working together on
  **AI-in-the-loop hardware testing and verification**.

### 1.3. Core principle: human–AI grounding
Everything the engineer sees on screen (visible range, cursors, timings, edges, detected anomalies)
has an **equivalent machine-readable representation** available to an AI agent through:
1. The clipboard (**Copy for AI**).
2. The headless **CLI** (`--json`, `--ai`).
3. The **Model Context Protocol (MCP)** server for agents.
4. A local **IPC session** through which an agent can place markers that appear immediately on the
   engineer's screen.

All times are relative to the trigger (t = 0), channels carry both their logical number (`CHx`) and
physical pin (`GPx`), and every timing figure states its quantization bounds.

---

## 2. Device interface and RP2040 firmware protocol

The firmware is a USB CDC virtual serial port (`/dev/ttyACM*`, VID:PID `1209:3020`, 115200 8N1) or
TCP (Pico W). The protocol below was verified against the firmware sources (`firmware/rp2040`, V6_5
upstream; upstream `Firmware/LogicAnalyzer`, V5_x) and the upstream C# driver. Implementation:
`src/core/DeviceProtocol.*`, `src/device/PicoConnection.*`.

> TinyUSB only transmits while the host asserts **DTR**, so the port is opened with DTR = RTS = 1.

### 2.1. Command framing (host → device)
Each command is `0x55 0xAA` + payload + `0xAA 0x55`. Bytes `0xAA`, `0x55` and `0xF0` inside the
payload are escaped as `0xF0, b ^ 0xF0`. `payload[0]` is the command: `0` = identify, `1` = capture,
`4` = reboot to bootloader, `5`/`6` = blink on/off. The firmware frame buffer is 128 bytes.

### 2.2. Capture request (`CAPTURE_REQUEST`)
The struct is **not packed**: it is sent as laid out by the ARM EABI, so padding is part of the
wire format.

| Field | V6_5+ (56 bytes) | V5_x and V6_0–V6_4 (48 bytes) |
|---|---|---|
| `triggerType` u8 (0 Edge, 1 Complex, 2 Fast, 3 Blast) | 0 | 0 |
| `trigger` u8 (0-based; 24 = external) | 1 | 1 |
| `inverted` / `count` u8 | 2 | 2 |
| *pad* | 3 | 3 |
| `triggerValue` u16 | 4 | 4 |
| `channels[]` u8 | 6 (×32) | 6 (×24) |
| `channelCount` u8 | 38 | 30 |
| *pad* | 39 | 31 |
| `frequency` u32 | 40 | 32 |
| `preSamples` u32 | 44 | 36 |
| `postSamples` u32 | 48 | 40 |
| `loopCount` | 52 (u16) | 44 (u8) |
| `measure` u8 | 54 | 45 |
| `captureMode` u8 (0 = 8 ch, 1 = 16 ch, 2 = 24 ch) | 55 | 46 (+1 pad) |

`captureMode` follows the highest channel number: < 8 → 8 ch, < 16 → 16 ch, otherwise 24 ch.
Limits (as in the upstream driver): `total = BUFFER / bytesPerSample`, `pre ∈ [2, total/10]`,
`post ∈ [2, total − 2]`, `pre + post ≤ total`, `minFreq = FREQ · 2 / 65535`.

### 2.3. Handshake
The host sends command `0`. The device answers with ASCII lines (`\n`):
1. Identity line, one of:
   * LogicTracer firmware: `LOGICTRACER_<BOARD>_<SemVer 2.0>` (e.g. `LOGICTRACER_PICO_0.2.0`);
   * upstream firmware: `LOGIC_ANALYZER_<BOARD>_V<major>_<minor>` (e.g. `LOGIC_ANALYZER_PICO_V6_5`).
2. `FREQ:<uint32>` (maximum sample rate)
3. `BLASTFREQ:<uint32>`
4. `BUFFER:<uint32>` (capture memory in bytes)
5. `CHANNELS:<uint8>`

LogicTracer firmware always uses the 56-byte request and fast polling. For upstream firmware the
request layout is chosen by version (§2.2), not by the presence of `FREQ:` lines: V6_0 already
sends them but still expects the 48-byte request. V5 firmware sends only the version line; defaults
are then used (100 MHz, 131072 bytes, 24 channels).

### 2.4. Capture and raw data
1. The device answers `CAPTURE_STARTED\n` or `CAPTURE_ERROR\n`.
2. When done: `u32 LE` sample count (= pre + post · (loop + 1)), then little-endian samples:
   8 channels → 1 byte, 16 → 2 bytes, **24 → 4 bytes** (`uint32`). Bit *i* belongs to the *i*-th
   entry of `channels[]`.
3. `u8` number of burst timestamps; if > 1, followed by `count × 4` bytes.

**Notes:**
* The simple edge trigger in PIO is **level-sensitive** (`jmp pin`): a rising trigger fires while
  the line is high. Immediate captures and `probe()` (which tries both polarities) rely on this.
  If the trigger line is already at the target level when arming, the host adds a
  `trigger_already_active` warning and, before signalling readiness (`WaitingForTrigger`,
  `--ready-file`, `--on-armed`), waits for the circular pre-trigger buffer to fill.
* Upstream firmware (≤ V6_5) checks for completion every ~2 s (`sleep 1 s` × 2), so the host adds
  ~2200 ms to the timeout. LogicTracer firmware polls every 10 ms (~15 ms turnaround); the host
  recognises it by its identity line (`defaultPollLatencyFor` = 150 ms).
* Any byte received during a capture cancels it without a reply (the host sends `0xFF` and drains
  the input). Command `4` (`logictracer-cli bootloader --yes`) reboots the RP2040 into the `RPI-RP2`
  UF2 bootloader.
* The firmware cannot configure pull resistors: RP2040 inputs default to pull-down, so a floating
  line reads LOW. `--pull-up` therefore only produces a warning.

### 2.5. CLI conventions and native capture format
CLI channels are 1-based (1..`CHANNELS`; on a Pico: `CH1..21 = GP2..22`, `CH22..24 = GP26..28`;
Interceptor board: 28 channels); the core library is 0-based. The physical `gpio` is reported in JSON
(`gpio_map` in `detect`, `gpio` per channel). Channel `CHANNELS + 1` (on a Pico **25** = `GP1`) in
`--trigger` is the external trigger input.

Exit codes: `0` OK, `1` error / not found, `2` TIMEOUT, `3` `--expect` failed, `64` usage error.

The native lossless format is **`logictracer-capture-v1`** (`.json`, `schema_version: 1`): device
metadata, `trigger_sample`, annotations and per-channel transitions (`idle_level` + `edge_samples[]`).
It reconstructs the waveform exactly while being 18–500× smaller than the upstream Base64 `.lac`,
and opens directly in `logictracer-gui` (`File → Open Capture…` or `--open`). VCD, CSV and
event CSV exports are also available; `.lac`, `.vcd` and the legacy `logicanalyzer-capture-v1` JSON
can be imported.

```bash
logictracer-cli detect --json
logictracer-cli probe --port /dev/ttyACM0 --channels 1,2,24 --json
logictracer-cli capture --port /dev/ttyACM0 --freq 100M --samples 30k \
    --channels 1:STIM,2:REACT,24:DO1 --trigger 1:rising --timeout 2000 \
    --ready-file /tmp/lt.armed --measure-latency 1..2 \
    --expect "events>=100,p99_ub_ns<=60,drops==0" \
    --out-vcd cap.vcd --out-json cap.json --out-events-csv events.csv --summary --json
logictracer-cli bootloader --port /dev/ttyACM0 --yes
```

`--meta key=value` (repeatable; key `[A-Za-z0-9_.-]{1,64}`, single-line value ≤ 512 chars) records
the test setup under `"setup"` in the result and in the lossless file; `analyze` keeps it and lets
`--meta` override keys, and AI reports print it as a `Setup:` line.

`--measure-latency` accepts `1..2` or `'1->2'` (quote the arrow form: unquoted `>` is a shell
redirection), with optional edges: `24:falling..2:rising`.

### 2.6. Measurement semantics
* A pair delay measured as `d` samples lies in the open interval `((d − 1)·T, (d + 1)·T)`, because
  stimulus and reaction are both sampled asynchronously. Percentiles use the nearest-rank method.
  `--expect` offers guaranteed bounds `min|max|mean|p50|p90|p95|p99` + `_ub_ns` / `_lb_ns`; when a
  point estimate such as `p99_ns<=X` passes but its bound may not, the CLI prints
  `WARN … INCONCLUSIVE`.
* Event accounting: `drops` (stimulus without reaction), `spurious` (reaction without a pending
  stimulus; `reacts_per_stim`), `censored` (edge at a window border whose partner could not have
  been captured — not a drop), `coincident` (Δs = 0), `outliers` (|Δs − median| > max(2, 5·MAD)).
* Δs histogram and the constant-delay test: if the spread is ≤ 1 sample, all pairs are explained by
  one fixed delay in `((max − 1)·T, (min + 1)·T)`; a histogram shift between runs is then sampling
  phase, not proven jitter.
* Frequency and duty cycle use intra-burst periods only (`periodicity.kind`: `periodic` /
  `jittery` / `bursty`, `bursts`, `irregular_periods`; channel metrics `CH:bursts`,
  `CH:period_cv`, `CH:irregular_periods`).
* `Copy for AI` / `--ai` compresses the repeating cross-channel edge motif into one line
  (`STIM↑ +2[1..2] REACT↑ +199 …`) and lists **anomaly incidents** (drop, spurious reaction, latency
  outlier, motif deviation, irregular period, unresolved pulse) with a ±4-transition window, instead
  of the first N edges. In Brief mode a saturated ASCII timeline is omitted.

---

## 3. Repository layout

```text
logictracer/
├── CMakeLists.txt, CMakePresets.json, cmake/LogicTracerOptions.cmake
├── docs/            # SPECIFICATION.md (this document), BACKLOG.md
├── firmware/rp2040/ # LogicTracer firmware, derived from LogicAnalyzer V2 (GPL-3.0)
├── packaging/       # desktop entry
├── scripts/         # firmware build, HIL scenarios
├── src/
│   ├── core/        # LogicTracer::Core — pure C++20, no Qt: DeviceProtocol, CaptureBuffer,
│   │                #   CaptureTypes, SignalAnalyzer, AiReport, Exporters, ArgParsing
│   ├── device/      # LogicTracer::Device — Qt SerialPort: PicoConnection, AsyncDevice
│   ├── services/    # LogicTracer::Services — Qt Network: Operations, Session (IPC), Clipboard
│   ├── cli/         # logictracer-cli
│   ├── gui/         # logictracer-gui (Qt Widgets: MainWindow, WaveformWidget)
│   └── mcp/         # logictracer-mcp (MCP server, JSON-RPC over stdio)
└── tests/           # QtTest suites + pty-based firmware emulator
```

Each library layer may depend only on the layers above it: `core` ← `device` ← `services` ←
front-ends.

---

## 4. Subsystems

### 4.1. Core and device libraries
1. **Device access** (`IDevice`, `PicoConnection`, `ReplayDevice`, `AsyncDevice`):
   * `IDevice` is the interface every front-end uses; `createDevice(port)` picks the
     implementation: `replay:<file>` → `ReplayDevice`, anything else → `PicoConnection`.
   * `ReplayDevice` presents a recorded capture as a device: channels, rate and depth come from the
     recording; integer decimation of the rate is supported; a triggered capture returns the window
     around the next real edge after the previous capture (TIMEOUT when none is left).
   * Discovery of serial ports by VID/PID (optionally every `ttyACM*`) and handshake.
   * Blocking driver on `QSerialPort`, run on a worker thread by `AsyncDevice` for the GUI.
   * **`probe()`**: a short (~10 ms) untriggered observation of the current pin levels, classified
     as HIGH (pulled up / driven), LOW (driven low, shorted or floating), TOGGLING or NOISY.
   * **Timeouts**: a capture never hangs. If the trigger does not occur in time, the result is
     `TIMEOUT` with a dump of the current line state.

2. **`SignalAnalyzer`** — single O(N) pass per channel:
   * `idle_level`, `final_level`, `transitions`, rising/falling edges;
   * complete high/low pulse counts and widths (min/mean/max);
   * intra-burst frequency, duty cycle and periodicity;
   * unresolved pulses (shorter than 2 sample periods);
   * cross-channel stimulus → reaction latency (§2.6).

### 4.2. CLI (`logictracer-cli`)
For scripts, CI and HIL rigs, and for AI agents calling it directly. Commands: `detect`, `probe`,
`capture`, `analyze`, `annotate`, `view`, `bootloader`.

```bash
logictracer-cli capture \
  --port /dev/ttyACM0 --freq 1M --samples 500k \
  --channels 1:STIM,2:REACT,24:JETSON_DO1 \
  --trigger 24:falling --timeout 5000 \
  --out-json capture.json --out-vcd waveform.vcd --summary
```

`--ai` prints an AI-ready Markdown report, `--note` adds the engineer's question, `--copy` puts it on
the Wayland/X11 clipboard (`wl-copy`, `xclip`, `xsel`). While `logictracer-gui` holds the port,
`detect`, `probe` and `capture` are transparently proxied through it.

### 4.3. GUI (`logictracer-gui`)
Qt 6 Widgets (`WaveformWidget`, `MainWindow`):
* Ctrl + wheel zooms around the mouse pointer, dragging pans, double-click or `Home` fits all,
  `+` / `-` zoom.
* Click sets cursor A; Shift + click or right click sets cursor B (Δt and 1/Δt shown). Cursors snap
  to the nearest edge (hold `Alt` to disable); `Z` zooms to A..B.
* `--demo` opens a synthetic capture; `--port /dev/ttyACM0` connects on start; `--open <file>` loads
  a saved capture. Build without the GUI with `-DLOGICTRACER_BUILD_GUI=OFF`.
* **AI grounding bar** above the report pane: an *Ask AI* field (context / question placed at the
  top of the report), a latency pair (`1..2`), a character/token counter and three detail levels:
  1. **Brief** (`Ctrl+Shift+B`, `--ai-mode brief`): chat-sized digest without raw JSON — device and
     `CHx=GPx` map, quantization, channel table, latency with histogram and bounds, edge motif and
     anomaly incidents.
  2. **Cursors A..B** (`Ctrl+Shift+R`, `--ai-mode cursors`): the same, restricted to `[A..B]`.
  3. **Full** (`Ctrl+Shift+C`, `--ai-mode full`): the visible window including JSON edge arrays.
  The report pane always shows exactly the text that will be copied.
* **AI markers:** through the IPC session an agent can call
  `logictracer-cli annotate --time 50.12ms --label "Jetson pulse start" --color red`, and the marker
  appears on the engineer's waveform immediately.

### 4.4. MCP server (`logictracer-mcp`)
JSON-RPC 2.0, one message per line over stdio (stdout carries only protocol traffic, logs go to
stderr); protocol versions 2024-11-05, 2025-03-26 and 2025-06-18.

| Tool | Purpose |
|---|---|
| `lt_detect_devices` | List connected capture devices and their limits |
| `lt_probe_inputs` | Current pin levels with a diagnosis |
| `lt_configure_and_capture` | Capture with trigger and timeout; compact analytics, latency, assertions, optional files |
| `lt_get_view` | What the engineer sees in the GUI (range, cursors, annotations), optionally the AI report |
| `lt_add_annotation` / `lt_clear_annotations` | Markers on the engineer's timeline |

Results carry `content[0].text` and `structuredContent` with the same JSON; `isError = true` only for
real failures (a `TIMEOUT` is a valid result with `line_dump`). If `logictracer-gui` is running and
connected, `capture` / `probe` are **routed through the GUI**, so the engineer sees what the agent
requested and the port is not contended. Otherwise the device is opened directly (`--port`). Files
requested by an agent are confined to `--output-dir`.

Client configuration (e.g. Claude Desktop / Claude Code):
`{"mcpServers": {"logictracer": {"command": "/path/to/logictracer-mcp"}}}`; options `--socket`,
`--port`, `--no-session`, `--output-dir`.

**IPC session socket** (`$LOGICTRACER_SOCKET`, else `$XDG_RUNTIME_DIR/logictracer.sock`, else a
private `/tmp/logictracer-<uid>/` directory; owner-only access): JSON lines
`{"id","method","params"}` → `{"id","result"|"error"}`; methods `ping`, `get_state`, `get_report`,
`annotate`, `list_annotations`, `clear_annotations`, `capture`, `probe`. Annotation times are
relative to the trigger (`time_us`, `time_s` or `time: "50.12ms"`) with text, colour and an optional
duration; they are cleared by a new capture.

---

## 5. Versioning

LogicTracer follows [Semantic Versioning 2.0](https://semver.org/). The single source of truth is the
`VERSION` file at the repository root; it sets the CMake project version (`LT_VERSION`, shown by
`--version` and in MCP `serverInfo`) and the firmware version (`FIRMWARE_VERSION`, identity line
`LOGICTRACER_<BOARD>_<version>`, `picotool info`). Host and firmware are released together under one
version; firmware images are named `LogicTracer-firmware-<version>-<board>-<variant>.uf2`.

## 6. Implementation status

* **Core and CLI:** protocol V5/V6, `gpio_map`, `--ready-file` / `--on-armed` HIL hooks, latency
  analysis with bounds and histogram, `--expect` with exit code `3`, `logictracer-capture-v1`,
  VCD/CSV/event CSV export, `analyze` for saved captures, `bootloader`.
* **GUI:** waveform viewer, cursors with snap-to-edge, AI grounding bar, `Open Capture…`,
  `AsyncDevice` worker thread.
* **Session and MCP:** IPC session with transparent proxying of `detect` / `probe` / `capture`
  through an open GUI, AI annotations (`annotate`, `view`), MCP server with output sandboxing.
* **LogicTracer firmware** (`firmware/rp2040`, SemVer from `VERSION`): completion polling reduced from ~2.2 s to 10 ms (~15 ms
  turnaround); 100 MHz and 200 MHz Turbo UF2 images via `scripts/build_firmware.sh`.
* **Hardware validation:** verified live with an RP2040 against a Digilent PYNQ-Z1 (100-transaction
  burst, stimulus → reaction latency, `--expect` PASS). Pending: NVIDIA Jetson AGX Orin `DO1` →
  RP2040 `GP28` (needs an external pull-up), see [BACKLOG.md](BACKLOG.md).

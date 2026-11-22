# Design: PYNQ-Z1 as a capture backend

Status: B1 (`IDevice`, replay device) and B2 (`lt_capture` RTL with simulation, lint and synthesis
checks, see [hdl/README.md](../../hdl/README.md)) are done. B3: the board agent
(`scripts/rigs/pynq-z1/logictracer_agent.py`, with a register-level model of the core) and `PynqConnection`
are done and tested against the model; integrating the core into a DUT bitstream and B4 are pending.

## Why

The RP2040 samples the device under test (DUT) **asynchronously**: a delay measured as `d` samples
is only known to lie in `((d − 1)·T, (d + 1)·T)`. On the PYNQ-Z1 runs this gives an interval of
10 ns for a pipeline that is really an integer number of 10 ns clock cycles, and only the pins
routed to Pmod headers are visible.

A capture core inside the Zynq programmable logic (PL), clocked by **the same clock as the DUT**,
removes both limits:

| | RP2040 (today) | PL capture core |
|---|---|---|
| Time base | independent crystal, ±1 sample | DUT clock: exact integer cycles |
| Visible signals | Pmod pins only | any internal net (FSM state, valid/ready, counters) |
| Depth | 128 KiB | BRAM (up to ~560 KiB on XC7Z020) or DDR via DMA |
| Toolchain | — | works with the open OpenXC7 flow, where no vendor ILA exists |

The RP2040 remains the non-intrusive, design-independent reference; the PL core is an in-design
probe. Both feed the same `CaptureBuffer`, analysis and AI reports.

## Architecture

```text
 DUT clock domain (PL)                          PS (Linux)                    Host
┌──────────────────────────────┐   AXI-Lite   ┌────────────────────┐  TCP/SSH  ┌──────────────────────┐
│ lt_capture core              │◄────────────►│ logictracer-agent  │◄─────────►│ PynqConnection       │
│  probe[31:0] ─► trigger unit │   (UIO/mmap) │  arm, poll, read,  │   JSON    │   : IDevice          │
│              ─► edge encoder │              │  edge list         │           │ → CaptureBuffer      │
│              ─► BRAM ring    │              └────────────────────┘           └──────────────────────┘
└──────────────────────────────┘
```

### PL core `lt_capture`
* Parameters: `PROBE_WIDTH` (≤ 32), `ADDR_WIDTH` (log2 of the record count, 13 = 8192), `TS_WIDTH` (33..63,
  default 48), `SYNC_STAGES` (0 or 2), `CLOCK_HZ`.
* **Edge encoding:** a record `{timestamp, probe value}` is written only when `probe` changes, plus
  one record at arm time. This matches the edge-based `logictracer-capture-v1` format and turns
  BRAM depth into *events*, not samples: a 100-transaction burst needs ~400 records instead of
  131 k samples.
* **Trigger unit:** `(probe & mask) == value` combined with an edge mask (rising/falling per bit),
  plus an external trigger input. Pre-trigger history comes from the ring buffer; a post-trigger
  record or cycle count ends the capture.
* **[AXI-Lite register map](../../hdl/README.md#register-map):** `ID` ("LTRC"), `VERSION`, `CTRL` (arm, abort,
  force, external-trigger mode), `STATUS` (armed, triggered, done, overflow, wrapped, aborted),
  `TRIG_MASK`, `TRIG_VALUE`, `TRIG_RISE`, `TRIG_FALL`, `POST_LO`/`POST_HI`, `WRITE_PTR`, `TRIG_PTR`,
  `TRIG_TS`/`END_TS`, `CLOCK_HZ`, `CONFIG`, `RECORDS`, `OLDEST`, and the readout window
  `READ_INDEX`/`READ_TS`/`READ_PROBE` (reading `READ_PROBE` advances the index). Every write is accepted with OKAY;
  unknown addresses return zero (no SLVERR).
* **Resources (estimate):** 32-bit probe + 48-bit timestamp, 8 k records ≈ 640 Kib ≈ 18 BRAM36 of
  140; logic below 1 % of the device; closes timing at 100 MHz.

### Board agent `logictracer-agent`
A small service on the PYNQ Linux (C++ or Python, UIO or `/dev/mem`): `detect` (ID, version, probe
names from a JSON sidecar shipped with the bitstream), `arm`, `wait`, `read` → edge list in
`logictracer-capture-v1` form. It also programs bitstreams (the `pynq_program_pl.py` logic) and
reports the SHA-256 for `--meta`.

### Host backend
* `IDevice` interface extracted from `PicoConnection`: `info()`, `probe()`, `capture()`,
  `requestCancel()`.
* `PynqConnection : IDevice` talks to the agent; `AsyncDevice`, CLI, GUI and MCP select the backend
  by port syntax (`/dev/ttyACM0` vs `pynq://192.168.68.69`).
* `CaptureBuffer` gets a `clockDomain` field: `async` (RP2040) or `dut` (PL core). Reports state
  delays in exact cycles for `dut` and with ±1-sample bounds for `async`.

## Metrology notes
* Probes in the DUT clock domain give exact cycle counts. Probes on external pins must pass a
  2-FF synchronizer: that adds a fixed 2-cycle offset and a ±1-cycle uncertainty, which the core
  reports per probe (`sync_stages`).
* A cross-check run measures the same Pmod pins with both backends; the PL result must fall inside
  the RP2040 interval.

## Phases
1. **B1:** `IDevice` refactor plus a file-replay device (useful for tests and demos).
2. **B2:** `lt_capture` RTL with a Verilator/Icarus testbench (trigger, ring wrap, overflow).
3. **B3:** board agent and `PynqConnection`; integrate the core into one DUT top-level as a
   reference.
4. **B4:** live cross-check against the RP2040 on the AV-10 burst; document in the specification.

## Open questions
* Integration into DUT designs lives in the DUT repository (e.g. a probe port on the top level);
  agree on a minimal interface there.
* Transport: plain SSH command per call (simple) vs a persistent TCP agent (lower latency).

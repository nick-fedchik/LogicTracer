# lt_capture: in-design capture core

`lt_capture` records signals **inside** an FPGA design, in the clock domain of the design under test
(DUT). Delays between probe bits are therefore exact integer clock cycles, not the ±1-sample
intervals of an external, asynchronously sampling analyzer, and internal nets (FSM state,
valid/ready, counters) are visible, not only pins. It is the PL-side half of the LogicTracer
capture backend described in [docs/design/pynq-capture-backend.md](../docs/design/pynq-capture-backend.md).

License: **CERN-OHL-W-2.0** (see [LICENSE](LICENSE)). You may embed the core in a proprietary
design; modifications of the core itself must be shared under the same licence.

## Status

| Phase | State |
|---|---|
| RTL core, self-checking simulation, lint, synthesis check (B2) | ✅ done, in CTest and CI |
| Board agent and host backend `PynqConnection` (B3) | ✅ agent, register model and host backend, tested against the model; integration into a DUT bitstream pending |
| Hardware cross-check against the RP2040 (B4) | planned |

## How it works

* **Edge-encoded storage.** A record `{timestamp, probe}` is written only when the probe vector
  changes, plus one record at arm time, one at the trigger and one at the end. Buffer depth counts
  events: a 100-transaction burst on two signals needs ~400 records, not 131 k samples.
* **Ring before the trigger, linear after.** While waiting for the trigger the buffer is a ring
  holding the most recent history. After the trigger the capture runs for `POST_CYCLES` clock
  cycles, or stops early when the ring would overwrite the trigger record (`overflow`).
* **Trigger unit.** `(probe ^ TRIG_VALUE) & TRIG_MASK == 0` combined with per-bit rising/falling edge
  masks; with all masks zero it triggers immediately. Alternatively an external trigger input, or
  a software force. `trig_out` pulses for one cycle (cross-triggering of other instruments).
* **Error-free bus.** AXI4-Lite in the probe clock domain; every access is answered `OKAY` and
  unknown addresses read 0, so the processor never sees a bus error.

## Parameters

| Parameter | Default | Meaning |
|---|---|---|
| `PROBE_WIDTH` | 32 | probe bits, 1..32 |
| `ADDR_WIDTH` | 13 | log2 of the record count (13 → 8192 records) |
| `TS_WIDTH` | 48 | timestamp bits, 33..63 (48 bits = 32 days at 100 MHz) |
| `SYNC_STAGES` | 0 | 0 for probes already in `clk`; 2 for asynchronous inputs such as pins (adds a fixed latency and ±1 cycle uncertainty to every bit) |
| `CLOCK_HZ` | 100 000 000 | frequency of `clk`, reported to software |

## Ports

| Port | Dir | Description |
|---|---|---|
| `clk`, `rst_n` | in | probe and bus clock; synchronous active-low reset |
| `probe[PROBE_WIDTH-1:0]` | in | signals to record |
| `ext_trig` | in | external trigger (level, sampled like the probes) |
| `trig_out` | out | one-cycle pulse when the capture triggers |
| `s_axi_*` | | AXI4-Lite slave, 12-bit address, 32-bit data (`wstrb` ignored) |

## Register map

| Offset | Name | Access | Description |
|---|---|---|---|
| 0x000 | `ID` | R | `0x4C545243` ("LTRC") |
| 0x004 | `VERSION` | R | `0x0001_0000` (1.0) |
| 0x008 | `CTRL` | W | bit 0 arm, bit 1 abort, bit 2 force trigger (pulses); bit 3 external-trigger mode (stored, readable) |
| 0x00C | `STATUS` | R | bit 0 armed, 1 triggered, 2 done, 3 overflow, 4 wrapped, 5 aborted |
| 0x010 | `TRIG_MASK` | RW | bits compared with `TRIG_VALUE` |
| 0x014 | `TRIG_VALUE` | RW | required levels |
| 0x018 | `TRIG_RISE` | RW | bits whose rising edge triggers |
| 0x01C | `TRIG_FALL` | RW | bits whose falling edge triggers |
| 0x020 / 0x024 | `POST_CYCLES` lo/hi | RW | clock cycles to record after the trigger |
| 0x028 | `WRITE_PTR` | R | next write index; bit `ADDR_WIDTH` = wrapped |
| 0x02C | `TRIGGER_PTR` | R | index of the trigger record |
| 0x030 / 0x034 | `TRIG_TS` lo/hi | R | timestamp of the trigger (cycles since arm) |
| 0x038 / 0x03C | `END_TS` lo/hi | R | timestamp of the last record |
| 0x040 | `CLOCK_HZ` | R | `CLOCK_HZ` parameter |
| 0x044 | `CONFIG` | R | `TS_WIDTH << 24 \| SYNC_STAGES << 16 \| ADDR_WIDTH << 8 \| PROBE_WIDTH` |
| 0x048 | `READ_INDEX` | RW | record selected for reading |
| 0x04C / 0x050 | `READ_TS` lo/hi | R | timestamp of the selected record |
| 0x054 | `READ_PROBE` | R | probe value of the selected record; **reading advances `READ_INDEX`** |
| 0x058 | `RECORDS` | R | valid records (`2^ADDR_WIDTH` once wrapped) |
| 0x05C | `OLDEST` | R | index of the oldest valid record |

### Software sequence

1. Write `TRIG_MASK`, `TRIG_VALUE`, `TRIG_RISE`, `TRIG_FALL`, `POST_CYCLES`.
2. Write `CTRL = 1` (arm), or `CTRL = 9` for external-trigger mode.
3. Poll `STATUS` until `done` (bit 2); `CTRL = 2` aborts.
4. Read `RECORDS`, `OLDEST`, `TRIGGER_PTR`, `TRIG_TS`; write `READ_INDEX = OLDEST`; then per record
   read `READ_TS` lo/hi and `READ_PROBE` (indices wrap modulo the depth).
5. A record's timestamp minus `TRIG_TS` is its time relative to the trigger, in cycles of `CLOCK_HZ`. The
   subtraction must be done **modulo 2^`TS_WIDTH`**: the timestamp counter wraps (at 100 MHz, 2^33 cycles
   is 86 s, the 48-bit default is 32 days), so a record from before a wrap can look larger than `TRIG_TS`.

`CTRL` bits are applied with a fixed precedence per clock: **arm > abort > force**. Writing `arm` and
`force` together arms the core and drops the force (issue them in two writes); writing `abort` together with
`arm` re-arms instead of aborting. `CTRL` pulses are one clock wide; `ext_mode` (bit 3) is stored with every
`CTRL` write.

## Resources

Yosys `synth_xilinx` (xc7), default parameters (8192 records × 80 bits): **18 × RAMB36E1**, ~560 LUTs,
~500 flip-flops, 40 CARRY4 — about 1 % of the logic and 13 % of the block RAM of an XC7Z020.

## Verification

`hdl/CMakeLists.txt` adds these CTest checks when the tools are installed:

| Test | Tool | What |
|---|---|---|
| `lt-capture-sim-sync0`, `-sync2` | Icarus Verilog | self-checking testbench: registers, exact 2-cycle latency on a 10-pulse burst, pattern + falling-edge trigger, ring wrap with pre-trigger history, overflow, abort, external trigger |
| `lt-capture-lint-sync0`, `-sync2` | Verilator `-Wall` | lint of both synchronizer configurations |
| `lt-capture-synth-xc7` | Yosys | synthesis for Xilinx 7-series, expects 18 × RAMB36E1 |

```bash
sudo apt install iverilog verilator yosys
ctest --preset default -R lt-capture
```

The testbench was checked against mutations (wrong stimulus delay, broken edge detection, missing
overflow flag): each makes it fail.

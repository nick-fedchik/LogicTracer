# Firmware self-test

`logictracer-cli selftest` checks a capture device against its own test-signal generator. Use it after every
firmware change, on a new board, or whenever a measurement looks doubtful: a pass means the analyzer itself
(firmware and host) reproduces a stimulus whose timing is known exactly.

It needs LogicTracer firmware with the generator (`FEATURES` bit 2; Pico and Pico 2, not the W boards, whose
radio uses the PIO block the generator needs) and two jumpers. Nothing else may drive those pins.

## Wiring

The generator drives `GP0` (A) and `GP1` (B); loop them back into two capture channels:

| Jumper | Pico pins | Default channel |
|---|---|---|
| `GP0` → `GP4` | pin 1 → pin 6 | A = `CH3` |
| `GP1` → `GP5` | pin 2 → pin 7 | B = `CH4` |

The pinout labels these pins UART TX/RX; the firmware does not use the UART, so this is one output into one
input. Other channels: `--a N --b M` (the jumpers then go to the GPIOs of those channels).

```bash
logictracer-cli selftest --port /dev/ttyACM0            # table; exit 0 = all passed, 3 = a check failed
logictracer-cli selftest --port /dev/ttyACM0 --json     # docs/schema/selftest.schema.json
logictracer-cli selftest --freq 10M --keep /tmp/st      # another sample rate; save failed checks' captures
```

The same self-test is available as **Tools → Self-test** in `logictracer-gui` (default channels; the JSON
report under *Show Details*) and as the MCP tool `lt_self_test` (`a`, `b`, `freq`), so an AI agent can check
the analyzer before it trusts a measurement. While the GUI holds the port, the CLI and MCP run it through
the GUI.

`--freq` takes 1 MHz up to the device maximum (default). Pulse widths, periods and gaps scale with the sample
period (about 200 samples per pulse at every rate); the A → B delays stay absolute. A run takes about 2 s.
The generator is always stopped at the end.

## Checks

The generator reports its timing in system clock cycles; every check compares a capture with that ground
truth. A measured width or delay of `d` samples admits a true value in `((d − 1)·T, (d + 1)·T)`.

| Check | What must hold |
|---|---|
| `generator` | the generator starts and reports its clock (else the self-test is an error, exit 1) |
| `wiring` | A and B toggle in a probe; otherwise the message names the missing jumper and the rest is SKIP |
| `trigger_rising`, `trigger_falling` | the trigger edge is exactly at t = 0: the sample before it has the old level |
| `pretrigger` | with a pre-trigger window longer than the time to the trigger, only recorded samples are shown (no pulse shorter than the generated one) and the shortened pre-trigger part comes with a warning |
| `waveform` | high width and period of A and B admit the generated ones, no glitches, a burst has the generated pulse count |
| `latency_<n>ns` | for delays of 0, 20, 50, 100 and 1000 ns, rising and falling: no drops, no spurious edges, every pair admits the true delay |
| `segments` | 20 trigger windows: the window 1 edge is at t = 0, every trigger-to-trigger spacing is one period or period + gap, within one sample plus the timestamp resolution |
| `idle` | after the generator stops, both lines stay LOW (pull-downs) |
| `timeout` | a trigger that never comes ends as TIMEOUT within `timeout` .. `timeout + 400 ms`, with the line state |

Each check carries its measurements in `data` (sample counts, true and bound values, `capture_file` with
`--keep`), so a failure can be analysed without rerunning.

## What it found

The first runs on a Pico found four analyzer defects, all fixed in the same change:

- **t = 0 one sample late** (all rates, intermittent). The edge program stores a sample and then tests the
  trigger pin one instruction later; an edge between the two is already in the last pre-trigger sample. The
  host now puts t = 0 on the first sample with the new level when the trigger channel is captured.
- **Phantom pre-trigger pulses.** When the trigger came before the pre-trigger window filled, the oldest
  pre-trigger samples were the firmware's zero fill, shown as LOW levels. The firmware now reports how many
  samples it recorded; the host leaves the rest out and warns.
- **Generator cut off by a capture.** Every capture released GP0/GP1; it now leaves them alone while the
  generator runs.
- **Slow TIMEOUT.** A cancelled capture had no reply, so the host waited out its poll window: `--timeout 300`
  returned after ~1.15 s. The firmware now acknowledges (`CAPTURE_CANCELLED`): ~0.5 s.

## Limits

- Verified only on a Raspberry Pi Pico with the 100 MHz image. Pico 2 and the 200 MHz Turbo image have not
  been run; `--freq 200M` on a Turbo image is the way to check one.
- It tests the analyzer, not a device under test: the stimulus comes from the same chip.
- Both lines are sampled through the same input synchronizers; skew between different pins and external
  signal integrity are outside its reach.
- The t = 0 correction needs the trigger channel in the capture (in a segmented capture it applies to window 1;
  later windows start sampling on the detected edge).

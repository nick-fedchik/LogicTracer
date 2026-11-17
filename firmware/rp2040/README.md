# LogicTracer firmware (RP2040 / RP2350)

LogicTracer firmware, derived from the **LogicAnalyzer V2 firmware** by Agustín Gimenez Bernad
(<https://github.com/gusmanb/logicanalyzer>, `Firmware/LogicAnalyzer_V2`, V6_5), **GPL-3.0**. The
changes are listed in [`../../NOTICE.md`](../../NOTICE.md).

The firmware is versioned with the whole product ([SemVer 2.0](https://semver.org/), repository
`VERSION` file) and answers the identify command with `LOGICTRACER_<BOARD>_<version>`, e.g.
`LOGICTRACER_PICO_0.2.0`.

| Version | Change |
|---|---|
| Unreleased | Edge-sensitive simple trigger (type 4, re-armed per `loopCount` loop) advertised as `FEATURES:1` before `CHANNELS`; request validation (exact frame length, channels, trigger type, mode, frequency, post length, escapes); bounded receive buffer; WiFi `tcp_recved`; burst timestamps with the edge trigger (`FEATURES:3`), SysTick rollover race in the timestamp NMI fixed, `measure` without loops rejected (it would hang); Pico W WiFi build fixed |
| 0.3.0 | Version bump only (no firmware changes) |
| 0.2.0 | LogicTracer identity line, program name and USB strings; SemVer from `VERSION` |
| 0.1.0 | Completion/cancel polling every 10 ms (was 2 × 1 s), shorter USB pre-transfer delays: ~15 ms turnaround (identified then as `LOGIC_ANALYZER_<BOARD>_V6_6`) |

## Build

```bash
# From the repository root; needs Pico SDK 2.x ($PICO_SDK_PATH) and arm-none-eabi-gcc.
scripts/build_firmware.sh            # -> build/firmware/LogicTracer-firmware-<version>-pico-{100mhz,200mhz-turbo}.uf2
scripts/build_firmware.sh --flash    # also reboots the device into BOOTSEL and flashes the 100 MHz image
```

Manual build: `cmake -S firmware/rp2040 -B build/firmware/pico -DBOARD_TYPE=BOARD_PICO -DTURBO_MODE=0`.

`TURBO_MODE=1` overclocks the RP2040 to 400 MHz at 1.30 V (200 MS/s capture). Use it only when
needed; it is outside the RP2040 specification.

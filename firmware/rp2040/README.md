# LogicTracer firmware (RP2040 / RP2350)

This directory is the **LogicAnalyzer V2 firmware** by Agustín Gimenez Bernad
(<https://github.com/gusmanb/logicanalyzer>, `Firmware/LogicAnalyzer_V2`), **GPL-3.0**, with the
LogicTracer changes listed in [`../../NOTICE.md`](../../NOTICE.md).

| Version | Change |
|---|---|
| V6_6 | Completion/cancel polling every 10 ms (was 2 × 1 s), shorter USB pre-transfer delays: ~15 ms turnaround |

## Build

```bash
# From the repository root; needs Pico SDK 2.x ($PICO_SDK_PATH) and arm-none-eabi-gcc.
scripts/build_firmware.sh            # -> build/firmware/*.uf2 (100 MHz and 200 MHz Turbo images)
scripts/build_firmware.sh --flash    # also reboots the device into BOOTSEL and flashes the 100 MHz image
```

Manual build: `cmake -S firmware/rp2040 -B build/firmware/pico -DBOARD_TYPE=BOARD_PICO -DTURBO_MODE=0`.

`TURBO_MODE=1` overclocks the RP2040 to 400 MHz at 1.30 V (200 MS/s capture). Use it only when
needed; it is outside the RP2040 specification.

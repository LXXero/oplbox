# firmware/bsp: felucca-bsp, vendored

OPLBOX's chip and board layer: registers, boot entry, interrupts, flash, USB (MIDI and audio in), the M-UPGRADE
updater and the update loader. It is a copy of part of [felucca-bsp](https://github.com/hugelton/felucca-bsp)
(Felucca's chip and board layer, split out), taken as files rather than as a git submodule. OPLBOX's own code
(`firmware/src/`, `firmware/hal/`) is built on it.

| | |
| --- | --- |
| Upstream | https://github.com/hugelton/felucca-bsp, branch `main` |
| Commit | `d6b8f771795be04d77e15d72a79f474a723796d7` (2026-10-24, Charles Vestal), the same commit docs/BASE.md read |
| Taken | 2026-10-25 |
| Licence | MPL-2.0, each file's SPDX line; full text in `LICENSE`, credits in `NOTICE` (both upstream's) |

## What is here

The upstream layout, so a file's path here is its path upstream:

- `chip/`: `crt0.S`, `fm1_vec.S`, `fm1_isr.S`, `fm1_cc.h`, `fm1_cpu1_stop.h`, `fm1_flash.h`, `fm1_gpio.h`,
  `fm1_guard.h`, `fm1_irq.h`, `fm1_sys.h`, `fm1_time.h`, `fm1_timer.h`, `fm1_uart.h`, `fm1_usb.h`, `fm1_xip.h`,
  `libc.c`, `ota.c`, `usb.c`
- `boards/fm1/`: all of it (`fm1_adc.h`, `fm1_audio.h`, `fm1_input.h`, `fm1_lcd_hw.h`, `fm1_led_anim.h`)
- `loader/`: all of it (`crt0_ldr.S`, `loader.c`, `ldr_core.c`, `ldr_screen.c`, `loader.ld`)

`fm1_uart.h` is not used yet; it is here for TRS MIDI IN (TODO 2b). `fm1_cpu1_stop.h` is here because `fm1_sys.h`
includes it. Not taken: the experimental second core and expanded flash (`chip/fm1_cpu1.h`, `fm1_cpu1.S`,
`fm1_flash_ext.h`), `fm1_clock.h`, `fm1_debug.h`, `fm1_perf.h`, and upstream's `examples/`, `tools/`, `tests/`,
`docs/`, README.md, LICENSING.md and LISTING.md.

## Changes from upstream

One, marked in the file with a comment "OPLBOX change". Changes to these files stay MPL-2.0.

- `chip/ota.c`, `ota_reply_identity()`: with `OTA_IDENTITY` defined, the handshake answers that string, chosen at
  run time, instead of `FELUCCA_ID`. OPLBOX's platform.c sets it so the USB rescue answers "FM-1_000". OPLBOX's
  copy of `ota.c` before the move had the same hook.

## How OPLBOX uses it

- `tools/build.py` compiles with `-Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1` (then `firmware/hal`,
  `firmware/src`), assembles `chip/crt0.S`, `fm1_vec.S` and `fm1_isr.S`, and builds the update loader from
  `loader/`. `firmware/src/platform.c` includes `usb.c`, `ota.c` and `libc.c` by name.
- OPLBOX's `uac_fir.h` (`firmware/src/`) is the 48 kHz resampler table `usb.c` includes: OPLBOX's 12-tap Q15
  filter, stored as `int32_t` because upstream's `usb.c` reads that type.
- `tests/loader_test.c` runs `chip/ota.c` and `loader/ldr_core.c` on the host.

## Updating it

Clone upstream, copy the files listed above over these, re-apply the change above, update the commit in this file,
then run `./build.sh` and `tests/run_host.sh` and test an update and the rescue on an FM-1. Upstream calls itself
beta and says to "expect changes to file names and layout".

# Which base OPLBOX syncs with

OPLBOX is a copy of Dinghy 0.3 plus dinghy PR #4 (TODO.md item 1). Its platform files (`firmware/hal/`,
`firmware/loader/`, `usb.c`, `ota.c`, `lcd.c`, `gfx.c`, `libc.c`) are Felucca's, through SLOOP and Dinghy. Most of
`firmware/hal/` and `lcd.c` are byte-identical to Felucca v0.9-beta (e5a908d, 2026-10-03); `usb.c` is closest to
Felucca 1.0 with SLOOP's 1.1.5 backports (`usb.c:17`). Since then there are four places OPLBOX could take its
platform from. This file compares them so xero can pick one.

Read on 2026-10-24 from clones in /tmp (nothing built or run from them):

| Repo | Commit read | Date |
| --- | --- | --- |
| OPLBOX, this tree | cf22896 | 2026-10-24 |
| Dinghy, https://github.com/zvenson/dinghy | 22acb91 (v0.4 = 7c31bb2) | 2026-10-23 |
| fm1-base, https://github.com/charlesvestal/fm1-base | aa2b3bc (submodule `bsp` at d6b8f77) | 2026-10-24 |
| felucca-bsp, https://github.com/hugelton/felucca-bsp | d6b8f77 | 2026-10-24 |
| Felucca, https://github.com/hugelton/Felucca | 63f88ec (last tag v1.6.1 = a53ecf4) | 2026-10-22 |
| M-VAVE FM1 Open, https://github.com/m-vave/fm1_open | 4c7496d | 2026-10-21 |

Paths below are relative to the repo named in each section; `O:` means this tree.

## What OPLBOX needs from a base

- **Song slots:** 9 × 32 KiB from 0x98000 to 0xE0000 (`O:firmware/src/oplbox.c:199-202`), written through the
  `FL_RANGE_OK` override (`O:firmware/src/platform.c:71-73`). Settings in one sector at 0xFC000 (`platform.c:70`).
- **Audio:** the ALNK0 I2S interrupt calls `app_audio()` for 256 frames at 44.1 kHz (`platform.c:228-252`), and
  dbopl2 renders inside it. `ISR_BUDGET_US` 5805 (`oplbox.c:59`) stops the sound after 43 late blocks in a row
  (`oplbox.c:192-193`).
- **USB:** class-compliant MIDI, USB audio input (`FELUCCA_UAC 1`, `platform.c:65`), and for TODO 2a a way to
  send and receive OPLBOX's own SysEx.
- **MIDI in:** MIDI clock for the arpeggiator (TODO 3) and TRS MIDI IN as a second input for recording (TODO 2b).
  OPLBOX builds with `FELUCCA_UART 0` (`platform.c:66`), so TRS is off today.
- **Update and rescue:** M-UPGRADE over USB-MIDI (`ota.c`), OCT− held at power-on opens the USB rescue
  (`platform.c:5-6`, `recovery.c`), and PR #4's `fm1_lcd_hw_init()` before that check.

## Summary

| | Dinghy 0.4 | fm1-base | felucca-bsp | Felucca | M-VAVE FM1 Open |
| --- | --- | --- | --- | --- | --- |
| Licence | GPL-3.0-only | MPL-2.0 | MPL-2.0 | GPL-3.0-only | APP: GPL-3.0-only + linking exception; engine: closed binary |
| OPLBOX (GPL-3.0-only) can take it | yes | yes, files stay MPL | yes, files stay MPL | yes | APP code yes; linking the engine no |
| Activity | 10 commits, 1 author, 2026-10-09..23 | 174 commits, 4 authors, since 2026-10-16, "beta" | 52 commits, 3 authors, since 2026-10-09, "beta" | 46 commits; maintainer stepping back | 8 commits, 2026-10-16..21 |
| Slots at 0x98000, 9 × 32 KiB | collide with its slots at 0x97000 | fit in `PLAT_DATA`; its panel object at 0xDC000 collides with slot 9 | fit (data window 0x97000..0xE0000) | collide with its projects, samples, presets | inside its USER partition, as offsets, if OPLBOX owns it all |
| Audio fits unchanged | yes, same ISR | yes, `app_render` 256 frames | yes, the app owns the ISR | yes (128-frame halves there) | needs a rewrite: 128-frame blocks, SRAM-only audio code |
| App SysEx in / out | no app hook | yes (`app_sysex`, `app_usb_cmd`) | yes (`ota_frame_get`, `ota_wire_send`) | updater only | yes, in (`fm1_app_sysex`) |
| MIDI clock in / TRS IN | yes / yes | packets / yes | packets / UART driver only | yes / yes | yes / yes |
| Host simulator | host scripts + tests | yes, plus browser emulator | no | yes, plus browser emulator | WASM simulator in the Workbench |
| Rescue | OCT− (PR #4 not merged) | REC at power-on, boot guard | none (tools only) | OCT− + OCT+ 5 s, boot guard | Workbench back to V15; no key |
| Change to platform.c / usb.c / ota.c | ~230 / 16 / 22 lines | all three replaced, ~50 call sites in oplbox.c | small / replaced / replaced | as felucca-bsp, plus the GPL parts | all replaced, plus a new build |

## Dinghy 0.4

- **Licence.** 55 source files carry `SPDX-License-Identifier: GPL-3.0-only`; the font is SIL OFL 1.1
  (`LICENSING.md:3,22`). The logo is all rights reserved (`LICENSING.md:27-28`), as in 0.3. OPLBOX can take any of it.
- **Activity.** 10 commits, all by zvenson, 2026-10-09 to 2026-10-23. 0.4 landed as one commit (7c31bb2) of
  41 files, +4045 −256 (`git diff --stat v0.3 v0.4`). Its platform now also carries parts of zvenson's OberVave
  firmware and the SMK-37 board layer (`LICENSING.md:11-16`, `platform.c:2-3`). It still sits on Felucca, not on
  felucca-bsp. `README.md:27-28`: "Dinghy 0.4 itself is tested on the host only so far."
- **Flash.** `firmware/src/store.h:39-44`: settings 0xFC000, blobs 0xF4000..0xFC000 (8 × 4 KiB), slots
  0x97000..0xB7000 (8 × 16 KiB, at most 16372 bytes each, `store.h:48`). The slots overlap OPLBOX's 0x98000..0xB7000,
  and the whole data area (0x97000..0xE0000, `firmware/hal/fm1_flash_map.h:81-95`) cannot hold both. `NSLOT`,
  `SLOT_SECT` and `FL_RANGE_OK` are plain `#define`s (`store.h:24-25,42`), so OPLBOX would edit `store.h` to keep its
  own slots. Dinghy's static `slot_save()` (`platform.c:252`) clashes with OPLBOX's `slot_save()`
  (`O:oplbox.c:257`). Its 12-byte slot header cannot read OPLBOX's 16-byte "OPLS" slots.
- **Audio.** The same `fm1_alnk0_irq` (`platform.c:366-389`), 256 frames (`:106`) at 44.1 kHz (`:105`). OPLBOX's
  render fits unchanged. TIMER5 now also polls the TRS UART at 2 kHz and skips while `in_audio` is set
  (`:316-358`): a 5.8 ms render leaves about 18 bytes in its 128-byte ring.
- **SysEx.** `usb.c:677-727` knows the UBOOT and M-UPGRADE keys; any other frame goes to `ota_take` (`ota.c:161-176`),
  which drops anything that is not M-UPGRADE. `docs/API.md:174-175`: "SysEx is the platform's (the updater) and does
  not reach the app." Same as 0.3: no app hook.
- **Clock and TRS.** New `firmware/src/midi_uart.c`: PH8, UART1 RX at 31250 baud, DMA ring, cable 1, SysEx dropped
  (`midi_uart.c:3-7,41-46`). `midi_get()` returns 1 for USB and 2 for TRS (`platform.c:294-304`). USB clock and
  transport were already queued in 0.3's `usb.c:748`, which OPLBOX has; `app_main()` throws them away.
- **Host.** 0.4 adds `tests/run_tests.sh` with `trs` and `clock BPM N` script events and tests for the updater, the
  loader and the SMK-37. OPLBOX's `tests/host.c` differs from 0.3's by 43 lines and from 0.4's by 270.
- **Update and rescue.** `ota.c` +22 lines, for the SMK-37 only; the FM-1 path is the same. The update loader now
  draws a screen (`firmware/loader/ldr_screen.c`, name and colour from `tools/build.py`). The rescue key is OCT−
  alone. **PR #4 is not in 0.4 or HEAD:** `fm1_cstart` calls `recovery_key()` (`platform.c:466`) with no
  `fm1_lcd_hw_init()` before it.
- **Change for OPLBOX.** `platform.c` +45 −191 against OPLBOX's, `usb.c` 16 lines, `ota.c` 22, plus the new
  `store.h`, `midi_uart.c`, `fm1_flash_map.h` and `ldr_screen.c`. Most of the 4000 lines are the SMK-37
  (`firmware/hal/smk_input.h` alone is 713 lines) and the CI release, which do nothing on an FM-1.

## fm1-base (on felucca-bsp)

- **Licence.** MPL-2.0 (`LICENSE`, `LICENSING.md:3-6`), with no "Incompatible With Secondary Licenses" notice, so
  under MPL §3.3 it can be part of a GPL work. `README.md:134-142`: "A Felucca-based firmware (GPL-3.0) can move onto
  fm1-base and felucca-bsp and keep its licence: their MPL-2.0 files may be part of a GPL work, and stay MPL-2.0
  file by file." OPLBOX stays GPL-3.0-only; changes to fm1-base's files are published under MPL-2.0. Bluetooth and
  WiFi link JieLi's closed libraries, which a GPL firmware cannot ship (`skill/felucca-to-fm1-base/RULES.md:196-200`),
  so neither is open to OPLBOX.
- **Activity.** 174 commits from 2e11ef9 (2026-10-16) to aa2b3bc: Charles Vestal 168, luchak 3, curist 2, Kerem
  Kilic 1. `README.md:9`: "Status: beta. … Expect the platform interface (`include/plat.h`) and `fm1.toml`'s settings
  to change before 1.0." The forks by durutti320, jvitkauskas and luchak exist; their `main` is 71128a3, an ancestor
  of HEAD (`git ls-remote`).
- **Flash.** App at XIP 0x8DFBC as OPLBOX's (`device/app.ld:14`). Data area 0x97000..0xE0000 through `plat_data`,
  `plat_data_erase`, `plat_data_write` (`include/plat.h:146-170`), plus windows 0x93000..0x97000 and
  0xE5000..0xFC000 where bytes 0xF00..0xF07 of a sector must never look like an update record (`plat.h:149-154`).
  OPLBOX's slots fit the data area as they are. The conflict: fm1-base needs an `OBJ_PANEL` store pair, which the
  template puts at 0xDC000 (`template/app.h:19-20`), inside slot 9. It would move to 0xE5000 or one slot would move.
  Its store format ("X0X0", `device/storage.c:16`) differs from OPLBOX's settings, so settings reset once.
- **Audio.** `fm1_alnk0_irq` → `render_block` → `app_render(lr, n, master_q12)` (`device/main_fm1.c:33-110`),
  `APP_BLOCK` 256 frames at 44.1 kHz (`device/fm1b_config.h:30-31`), an `APP_OVERLOAD` hook above 90 %
  (`main_fm1.c:82-84`). OPLBOX's render and its self-mute fit; the MASTER, clip and `<<7` stage moves into the
  `app_render` wrapper.
- **SysEx.** `app_sysex(p, n)` gets the bytes between F0 and F7 in the main loop (`plat.h:255`,
  `main_fm1.c:893-903`); `plat_sysex_send` sends one (`plat_fm1.c:169`). `app_usb_cmd` uses the updater's framing
  with `plat_host_read` pulling 512 bytes at a time (`plat.h:173-179`, `main_fm1.c:591-632`), which is what a `.CMF`
  push needs.
- **Clock and TRS.** Clock and transport bytes from USB and TRS arrive as packets through `plat_midi_in(pkt, &port)`,
  TRS as `PORT_TRS` (`plat.h:54-66`); there is no tempo API.
- **Host.** `host/plat_host.c` implements `plat.h` on a computer from a script (keys, encoders, midi, clock, wav,
  shot, expect, `data FILE OFFSET`, `plat_host.c:1-70`); `web/emu/emu_web.c` runs it in a browser (no WebMIDI).
- **Update and rescue.** `tools/fm1_install.py` and `web/installer.html` with "Back to the stock firmware" (6c0b47e).
  **REC held at power-on** enters the chip's update mode (`main_fm1.c:849-850`); OCT− + OCT+ at power-on opens the
  panel setup (`:847-848`). Boot guard: 2 failed boots → safe mode, 4 → the chip's update mode (`main_fm1.c:955-966`).
  OPLBOX's OCT− rescue and `recovery.c` go away.
- **Change for OPLBOX.** `platform.c`, `usb.c`, `ota.c`, `recovery.c`, `hal/`, `loader/`, `crt0.S`, `app.ld`, `libc.c`
  and `tools/build.py` are all replaced by fm1-base and `fm1.toml`. `lcd.c`/`gfx.c` calls keep their signatures
  (`device/gfx.c:63-194`). About 50 call sites in `oplbox.c` change: `app_main()` splits into `app_init`, `app_frame`
  and friends; `fm1_ms` → `plat_ms` (19), `fm1_enc_take` → `plat_enc` (7), `fm1_led_key` → `plat_leds` (6), keys to
  `plat_buttons`/`plat_keys`, flash to `plat_data*`. `plat_battery` gives 0..4 bars only, so the battery work in
  TODO 6 would lose its raw ADC reading. There is a porting guide, `skill/felucca-to-fm1-base/PORTING.md` (file map at
  `:33-48`).

## felucca-bsp

- **What it is.** Felucca's chip and board layer, split out: "It has no sound code and no user interface"
  (`README.md:3-5`). fm1-base builds on it, but a firmware can sit on it directly: `examples/hwtest` builds with
  `-Ichip -Iboards/fm1` (`README.md:58-59`).
- **Licence.** MPL-2.0 for `chip/`, `boards/`, `loader/`, `examples/` and the tools (`LICENSING.md:6-8,36-40`), relicensed
  from Felucca with the authors' consent (bsp#3, bsp#4, 6eee4bf; `LICENSING.md:3-4,17-22`). Copies already taken from
  Felucca, like OPLBOX's, stay GPL (`LICENSING.md:14-15`). OPLBOX can take it as for fm1-base.
- **Activity.** 52 commits from d4b2d47 (2026-10-09) to d6b8f77: Charles Vestal 33, kurogedelic 18, luchak 1; no
  tags. Since 2026-10-16 all commits are Vestal's. `README.md:26`: beta, "Expect changes to file names".
- **Flash.** The same store map as Felucca (`chip/fm1_flash.h:39-62`), plus `FL_XDATA_OK` windows for a firmware's
  own data at 0x93000..0x96FFF and 0xE7000..0xFBFFF (7d8f849). OPLBOX keeps its `FL_RANGE_OK` override.
- **Audio.** The app owns the buffer, the half size and the ALNK0 ISR; `boards/fm1/fm1_audio.h` equals OPLBOX's
  `firmware/hal/fm1_audio.h` apart from the SPDX line. OPLBOX's ISR and budget guard stay as they are.
- **SysEx.** `ota_service()` passes M-UPGRADE-framed frames it does not own back to the app for its own commands
  (`chip/ota.c:159-166`, 7d8f849); any F0..F7 goes out with `ota_wire_send` (`chip/usb.c:1028`). That is the hook
  TODO 2a needs, in the framing `ota.c` already uses.
- **Clock and TRS.** USB F8/FA/FB/FC go into the input ring with timestamps (`chip/usb.c:973-976`). UART1 at
  31250 baud with a DMA ring (`chip/fm1_uart.h:3-10`); the MIDI parser is not in the bsp, so OPLBOX would take Dinghy
  0.4's GPL `midi_uart.c` or Felucca's.
- **Host.** Only `tests/flash_ext_test.c`, `mem_test.c`, `install_test.py`; the simulator is fm1-base's.
- **Update and rescue.** `chip/ota.c` is the M-UPGRADE session with hooks for the app; the loader shows an
  "UPDATING" screen (0f08fca); `tools/fm1_rescue.py` restores stock V15 through the chip's UBOOT mode (macOS only).
  No rescue key or boot guard in the bsp: OPLBOX keeps its own `recovery.c`, `bootguard.h` and PR #4's line.
- **Fixes OPLBOX's copy lacks:**
  - **afd83ed** (2026-10-23): `ota.c` stages any well-formed writer. OPLBOX's `ota.c:289-300` accepts only V15's
    writer (by CRC) and only a boot area equal to the device's, so installing M-VAVE's V16 (a new bootloader) or
    m-bootloader from OPLBOX is refused. The commit says it was tested on an FM-1.
  - **The loader's record cleanup.** OPLBOX's `firmware/loader/ldr_core.c:92-103` scans every sector from 0x93000 to
    0xFC000 (`:33-34`) and erases any whose last 256 bytes look like an update record ("AT" and a matching CRC16).
    The bsp skips the data area (`loader/ldr_core.c:174-175`), as Felucca has since 1.1.5 (276f72a). A song slot
    matching by chance is unlikely, but the erase would lose that slot.
  - **USB audio ring:** OPLBOX renders 256-frame halves but keeps the ring band set for 128-frame halves
    (`UA_LO` 80, `UA_HI` 128, `O:usb.c:66-76`); the bsp sets 96..160 for 256 (ae0139e) and added `FELUCCA_UAC_DEEP` for renders that take most of a half (55f6a91).
  - Supply-rail raises for units that misread flash at boot (067b0bb, a449b48, opt-in) and a crash register dump
    (528d6c2).
- **Change for OPLBOX.** Swap `firmware/hal/`, `usb.c` (1305 lines, 766 differ from the bsp's 1651), `ota.c`
  (121 differ), `libc.c` and `loader/` for the bsp's files. Re-apply OPLBOX's `OTA_IDENTITY` hook (`O:ota.c:130-142`)
  as a change to an MPL file. In `platform.c`: `uac_tap` takes 24-bit samples instead of Q15 (`platform.c:247`, a
  shift), `ota_service` returns a value, set `FELUCCA_UAC_DEEP 1`, new include paths. `oplbox.c`, `lcd.c`, `gfx.c`,
  `recovery.c`, `bootguard.h` stay. About a day of edits, then the update and rescue path tested on an FM-1.

## Felucca itself

- **Licence.** GPL-3.0-only (`LICENSING.md:3`), as OPLBOX.
- **Activity.** 46 commits, 2026-10-02 to 63f88ec, mostly squashed releases ("Felucca 1.x"), tags v0.8-beta to
  v1.6.1. `README.md:5-13` (3a91058): Kuroshita is stepping back; "new features, issue responses, and pull request
  reviews are not guaranteed".
- **Flash.** Its store fills the data area: projects 0x97000..0x9EFFF, 3 sample slots 0xA0000..0xDBFFF, presets
  0xDC000..0xDFFFF, autosave 0xE5000/0xE6000, FM6 voice bank 0xE7000, settings A/B 0xFC000/0xFD000
  (`firmware/src/storage.c:20-24,64-75`, `eng_sample.c:45-47`). OPLBOX's slots and settings sit on top of these. Neither
  firmware misreads the other's data (both check magic and CRC), but saving in one erases the other's sectors. This is
  true on every base above, since all of them keep Felucca's map.
- **The rest.** Its platform part is what felucca-bsp now holds, with the fixes landing there first; the rest is
  Felucca's instrument. As a base it is felucca-bsp plus code OPLBOX does not use, from a maintainer who says pull
  request reviews are not guaranteed.

## M-VAVE FM1 Open (SDK and Workbench)

The Synthtopia article's repo is https://github.com/m-vave/fm1_open; the Workbench is
https://open.m-vave.com/fm1-workbench. It is not a platform in the sense of the others.

- **Licence.** The APP sources are GPL-3.0-only with a section 7 permission to link the binary SDK
  (`LICENSE.md:5,9-13`). The engine, `sdk/lib/libfm1_engine.a`, is one closed prelinked object: "No proprietary SDK
  source is supplied" (`THIRD_PARTY.md:29-35`), redistributable only "on M-VAVE FM1 devices". It holds the
  sequencer, USB MIDI and audio, Bluetooth, FreeRTOS and the updater, based on JieLi's AC79 SDK (`THIRD_PARTY.md:17,59`).
  OPLBOX can read and take APP code, but cannot link the engine: OPLBOX contains GPL code from the DOSBox Team and
  from Felucca, SLOOP and Dinghy (`O:LICENSING.md:11-20`) whose authors never granted that exception.
- **Activity.** 8 commits, 2026-10-16 ("Initial FM1 Open V12 release", 3be7fbd) to 4c7496d, by M-VAVE staff and one
  outside PR (24beff7). Open versions V12..V14 (`app/version.h:5`) are a separate line from stock V15.
- **Build.** JieLi's Windows toolchain 2.5.3, PowerShell and C#, with `clang.exe` and every SDK file hash-checked
  (`README.md:40-42`, `sdk/Build.ps1:346-347`); OPLBOX builds on Linux (`O:build.sh`).
- **Flash, audio, MIDI.** USER partition 0x7C000..0xFF000 through `fm1_flash_*` offsets (`sdk/include/fm1.h:141-167`).
  Audio is 128-frame blocks (`sdk/include/fm1_block.h:5-6`) rendered in the APP's ALNK0 interrupt
  (`app/board/audio.c:27-64`), with all audio code in internal SRAM (`fm1.h:15-19`) that it shares with the RTOS.
  `fm1_app_sysex` delivers SysEx up to 1024 bytes (`fm1.h:94-100`); clock, transport and TRS are handled
  (`app/midi_router.c:313`, `app/panel.c:11`).
- **Update and rescue.** The Workbench flashes over USB-MIDI and can return to V15 (`sdk/WebUpdate.js:140,290-316`).
  No power-on rescue key.
- **Change for OPLBOX.** `crt0.S`, `platform.c`, `usb.c`, `ota.c` and the loader all go, since the engine owns boot,
  USB, the updater and the RTOS; the UI, storage, MIDI and audio are rewritten against `fm1_*`, and the build moves to
  Windows. The licence stops it before the work does.

## Recommendation

Move OPLBOX's platform layer onto felucca-bsp and keep the rest of Dinghy 0.3's code as it is. That is where
Felucca's platform fixes now land (the V16 update writer, the loader no longer erasing data, the USB audio ring for
256-frame halves, app SysEx in the updater's framing for TODO 2a), and it changes neither OPLBOX's audio interrupt,
its flash map, its OCT− rescue nor its screen code: about a day of edits plus a hardware test of update and rescue.
From Dinghy 0.4 take only `midi_uart.c` and the `midi_get()` port number for TRS MIDI IN, and its host-test ideas;
the rest of 0.4 is the SMK-37 and CI, tested on the host only, with slots that collide with OPLBOX's and without
PR #4. fm1-base is the step after that if the host simulator and browser emulator are worth a rewrite of about 50
call sites, a REC rescue key and a beta API; it can wait until its 1.0. M-VAVE's SDK is out, because OPLBOX cannot
link its closed engine.

## Questions for xero

1. felucca-bsp now (platform files only, the app untouched), or straight to fm1-base (bigger port, gains the
   simulator and browser emulator, REC replaces OCT− as the rescue key)?
2. Do you switch one FM-1 between Felucca (or a Felucca-based firmware) and OPLBOX? On every base, OPLBOX's slots
   at 0x98000..0xE0000 overwrite Felucca's projects, samples and presets, and the other way round. If you do, the
   slots have to move into the free window 0xE7000..0xFBFFF, which holds only 2 of the 9.

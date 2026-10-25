# Licensing

OPLBOX is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

One part is under another licence: `firmware/bsp/`, felucca-bsp's chip and board layer, is MPL-2.0 (each file's
SPDX line, full text in `firmware/bsp/LICENSE`). Those files sit inside the GPL-3.0-only work as MPL-2.0 files, as
MPL-2.0 section 3.3 allows (they carry no "Incompatible With Secondary Licenses" notice). A firmware built from
OPLBOX is GPL-3.0-only as a whole; the source of the MPL files, changes included, stays available under MPL-2.0.

## Who made what

| | |
| --- | --- |
| The chip and board layer, `firmware/bsp/` (registers, boot entry, USB, the updater `ota.c`, the update loader), **MPL-2.0**: felucca-bsp d6b8f77, vendored, one change marked in `chip/ota.c` (`firmware/bsp/README.md`) | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, Charles Vestal, Kerem Kilic (keremimo), and the others named in `firmware/bsp/NOTICE` and the files' copyright lines ([felucca-bsp](https://github.com/hugelton/felucca-bsp)) |
| The rest of the platform: `app.ld`, `firmware/src/` (`recovery.c`, `lcd.c`, `gfx.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| Dinghy (based on Dinghy by zvenson, https://github.com/zvenson/dinghy): its app (`dinghy.c` there, now `firmware/src/oplbox.c` here), the wiring `platform.c`, the host mode `tests/`, the docs | zvenson |
| OPLBOX: the app `firmware/src/oplbox.c`, the player `macsop.c`/`.h`, `opl2fast.c`, `songs.h`, `host/`, `tools/gen_songs.py`, the WPA2 key code `firmware/wifi/wpa_crypto.c`/`.h` | 2026, GPL-3.0-only like the rest; the player follows AdPlug's `cmfmcsop.cpp` (Sebastian Kienzl, LGPL-2.1) |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| DOSBox OPL emulator `dbopl` (DOSBox r3635 via Chocolate Doom 2.2.1, Copyright (C) 2002-2010 The DOSBox Team), reduced to an OPL2 as `firmware/src/dbopl2.c`; its float-derived tables generated for 49716 Hz into `dbopl2_tables.h` by `host/dbopl_gen.c` | GPL-2.0-or-later (combined under GPL-3) | <https://www.dosbox.com> |
| Nuked-OPL3 1.8 (Alexey Khokholov), `firmware/src/opl3.c`/`.h`, the cycle-exact core (`-DOPL_CORE=OPL_NUKED`) | LGPL-2.1 | `firmware/src/LICENSE.nuked-opl3` |
| The 18 songs in `songs.h`: Thomas Detert (Stone Age, Eclipse 1992) and Michael Tschögl (LINEL 1990-92), as shipped with the games and with SoundFX Macs Opera | the composers' / publishers' copyright, included as game data for playback | |
| Linux rt2x00 `rt2800.h` (v6.6; Copyright (C) 2004-2010 Ivo van Doorn, (C) 2010 Willow Garage, (C) 2009 Alban Browaeys, Felix Fietkau, Luis Correia, Mattias Nissler, Mark Asselstine, Xose Vazquez Perez, Bart Zolnierkiewicz): the WiFi MAC register names, addresses and masks in `firmware/wifi/fm1wifi_regs.h`; the SDK's `WL82.h`/`hwi.h` (Apache-2.0) for its CPU-side blocks | GPL-2.0-or-later (combined under GPL-3) | <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/ralink/rt2x00/rt2800.h?h=v6.6> |
| BearSSL (Thomas Pornin, commit 7bea48e5): SHA-1, HMAC and AES decryption in `firmware/wifi/bearssl/` (files listed, with their changes, in its `README.md`) | MIT (combined under GPL-3) | `firmware/wifi/bearssl/LICENSE.txt`, <https://www.bearssl.org/> |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It is Dinghy's logo and OPLBOX still carries it; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. OPLBOX and Dinghy are not made or endorsed by them.

# Licensing

OPLBOX is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| Dinghy (based on Dinghy by zvenson, https://github.com/zvenson/dinghy): its app (`dinghy.c` there, now `firmware/src/oplbox.c` here), the wiring `platform.c`, the host mode `tests/`, the docs | zvenson |
| OPLBOX: the app `firmware/src/oplbox.c`, the player `macsop.c`/`.h`, `opl2fast.c`, `songs.h`, `host/`, `tools/gen_songs.py` | 2026, GPL-3.0-only like the rest; the player follows AdPlug's `cmfmcsop.cpp` (Sebastian Kienzl, LGPL-2.1) |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| DOSBox OPL emulator `dbopl` (DOSBox r3635 via Chocolate Doom 2.2.1, Copyright (C) 2002-2010 The DOSBox Team), reduced to an OPL2 as `firmware/src/dbopl2.c`; its float-derived tables generated for 49716 Hz into `dbopl2_tables.h` by `host/dbopl_gen.c` | GPL-2.0-or-later (combined under GPL-3) | <https://www.dosbox.com> |
| Nuked-OPL3 1.8 (Alexey Khokholov), `firmware/src/opl3.c`/`.h`, the cycle-exact core (`-DOPL_CORE=OPL_NUKED`) | LGPL-2.1 | `firmware/src/LICENSE.nuked-opl3` |
| The 18 songs in `songs.h`: Thomas Detert (Stone Age, Eclipse 1992) and Michael Tschögl (LINEL 1990-92), as shipped with the games and with SoundFX Macs Opera | the composers' / publishers' copyright, included as game data for playback | |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It is Dinghy's logo and OPLBOX still carries it; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. OPLBOX and Dinghy are not made or endorsed by them.

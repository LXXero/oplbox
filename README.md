# OPLBOX — the M-VAVE FM-1 as an AdLib

Custom firmware for the M-VAVE FM-1 that plays the **SoundFX Macs Opera** songs — the DOS soundtracks of
LINEL's games (Michael Tschögl, 1990–92) and of Eclipse's **Stone Age** (Thomas Detert, 1992) — through an
OPL2 emulation, exactly the way FXDRIVER V1.06 drove a Sound Blaster.

**Status: 0.2.** 0.1 ran on hardware and could not keep up: its OPL core (Nuked-OPL3, cycle-exact) needed more
than the whole core, the audio interrupt ran over its 5.8 ms budget every time, the sound stuttered at half
speed and the main loop starved — which also starved USB, so the installer could not talk to it. 0.2 fixes the
cause and the consequence:

- the OPL core is now **DOSBox's `dbopl`** (the sound of DOSBox, where most people heard these songs), cut down
  to one OPL2 and to integers, its sample loop and wave tables in RAM instead of flash. The FM-1 emulator
  measures the audio interrupt at **13 %** of the core (0.1: 62 %, and worse on the chip). The screen shows the
  real figure (`CPU nn%`, three digits now, amber past 85 %, red over budget).
- an **overload guard**: if the interrupt runs over budget for a quarter of a second, playback stops itself and
  the screen says `AUDIO OVERLOAD - PAUSED`; the main loop, USB and the updater keep running, so the device
  always stays reachable. PLAY starts it again.
- unity gain (0.1 clipped the loud LINEL songs), and the few songs that touch the OPL's full scale are
  clipped at the DAC instead of wrapping.
- the MIDI input ring is drained (0.1 never read it; after ~40 notes / clocks from the host the USB endpoint
  NAKs and SysEx — the installer's handshake — is locked out until a power-cycle; found by Smalley).

Nuked-OPL3 stays in the tree as the cycle-exact option (`-DOPL_CORE=OPL_NUKED`), for when it can run on the
second core. The register stream is the same for both: the player is verified against DOSBox captures of the
original driver, and `dbopl2.c` is checked sample for sample against the unmodified DOSBox code
(`host/dbcmp.c`, 18 songs, 30 s each, 0 mismatches).

## Controls

| | |
| --- | --- |
| **PLAY** | play / pause |
| **HOME** | back to the start of the song |
| **OCT−  OCT+** or the **SELECT** knob | previous / next song |
| **REC** | loop one song ↔ play them all in turn (the LED shows which) |
| **MASTER** | volume |

The song and the loop mode are kept across a power-off.

## Install

Exactly like any Felucca-family firmware. Over USB from a computer, with the FM-1 switched on and running
any firmware that has the update path (stock V14/V15, Felucca, SLOOP, Dinghy, …):

```sh
pip3 install mido python-rtmidi
python3 tools/fm1_install.py oplbox-0.2.fwsc        # asks before it writes, restarts into OPLBOX
python3 tools/fm1_install.py --info                 # what the connected FM-1 runs
```

Or through the FM-1 Firmware Hub (https://fm1.designburgapps.com) in Chrome/Edge, which can install a
local `.fwsc`. Installing firmware is at your own risk.

**Back to stock or to any other firmware:** the hub, or M-VAVE's own updater with their `FM-1.fwsc` —
OPLBOX answers the updater like every firmware in the family.

**If it will not start, or will not answer the installer:** hold **OCT−** (only OCT−) *before* switching on,
switch on, keep holding until the screen says `OPLBOX USB RESCUE` → no sound, only the updater; install
anything from there. The key is read in the first 50 ms after power-up, so it must already be down. A
firmware that crashes twice at boot goes to the rescue by itself. If even that is gone, the chip's own boot
ROM still updates over USB (FM-1-transporter).

If 0.1 is what is running (the one that starves itself): button presses are latched by a timer interrupt even
while the audio interrupt hogs the core, so press **PLAY** once and wait for the song to end (2–5 min) — in that
quiet moment the main loop sees the press, pauses, and the installer gets its answer.

## What is in it

- `firmware/src/oplbox.c` — the app: audio interrupt, song control, overload guard, screen.
- `firmware/src/dbopl2.c` — DOSBox's `dbopl` (GPL-2+) as one OPL2: 9 channels, 18 operators, no OPL3 modes,
  no floating point — every table the original computes with floats is generated for 49716 Hz by
  `host/dbopl_gen.c` into `dbopl2_tables.h`. The sample loop is linked into RAM (`.ram_code`, app.ld) and the
  wave / volume tables into `.data`, so nothing hot runs out of the SPI flash. One fix over the r3635 code the
  tables come from: the noise LFSR counter is masked as later DOSBox does (r3635's mask let it run hundreds of
  steps a sample, which is why rhythm-mode songs cost ten times the others). `host/dbcmp.c` runs it in lockstep
  with the unmodified original: 0 mismatches over 30 s of every song.
- `firmware/src/opl2fast.c` + `opl3.c` / `.h` — the cycle-exact option: Nuked-OPL3 1.8 (LGPL-2.1) with an OPL2
  fast path (18 slots and 9 channels instead of 36/18, dormant melodic slots on a phase-only path, cached
  register-derived constants, the silent bank's 18 noise clocks as one LFSR jump), bit-exact with full Nuked
  (`host/skiptest.c`). Too slow for the chip on one core; `-DOPL_CORE=OPL_NUKED` builds it.
- `firmware/src/macsop.c` / `.h` — the Macs Opera player, freestanding C99, after AdPlug's `cmfmcsop.cpp`
  (LGPL-2.1), with FXDRIVER's own arithmetic: rounded /128 volume scaling, the asymmetric 14-bit pitch bend
  fed as `(pitch << 8) | 0xFF`, the driver's F-number table, op-1 KSL for single-op drums. Verified
  register-for-register against DOSBox captures of Stone Age tracks 1, 4 and 5 (9,141 of 9,141 key-on events).
- `firmware/src/songs.h` — the 18 songs as they are on disk (252 KB), from `tools/gen_songs.py`.
- Everything else is Dinghy (zvenson) / Felucca (Leo Kuroshita, Hügelton Instruments) / SLOOP (isod89):
  boot, USB, the updater, the rescue, the screen, the panel. GPL-3.0; see `LICENSING.md` and `CREDITS.md`.

## Build

As Dinghy: Linux x86-64, Python 3 with Pillow, JieLi's Linux toolchain (`tools/get_toolchain.sh`) and the
three boot files of the AC79 SDK at tag `AC79NN_SDK_V1.2.1_2023-12-13` (hash-checked by the build).

```sh
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/fw-AC79_AIoT_SDK ./build.sh --release 0.2
tests/run_host.sh                 # the same app on this computer: WAV + screen PNGs, no FM-1 needed
```

The OPL cores on their own (`host/`, from the `oplbox` reference tree): `dbopl_gen.c` makes the tables,
`dbcmp.c` is the lockstep check against DOSBox, `render.c` renders a song through Nuked, `skiptest.c` checks
the Nuked fast path.

Package identity `FM-1_96N` (N = minor version).

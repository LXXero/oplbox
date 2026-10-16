# OPLBOX — the M-VAVE FM-1 as an AdLib

Custom firmware for the M-VAVE FM-1 that plays the **SoundFX Macs Opera** songs — the DOS soundtracks of
LINEL's games (Michael Tschögl, 1990–92) and of Eclipse's **Stone Age** (Thomas Detert, 1992) — through an
OPL2 emulation (Nuked-OPL3), exactly the way FXDRIVER V1.06 drove a Sound Blaster.

**Status: 0.1, untested on hardware.** Built, run in the FM-1 emulator (boots, plays, draws, the controls work,
the USB rescue works), and the player's register stream is verified against DOSBox captures of the original
driver. The one open question is CPU headroom on the real chip: the emulator predicts the audio interrupt at
60–75 % of the core. The screen shows the measured figure (`CPU nn%`); if it sits above ~85 % on hardware the
sound will crackle and the next step is the second core.

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
python3 tools/fm1_install.py oplbox-0.1.fwsc        # asks before it writes, restarts into OPLBOX
python3 tools/fm1_install.py --info                 # what the connected FM-1 runs
```

Or through the FM-1 Firmware Hub (https://fm1.designburgapps.com) in Chrome/Edge, which can install a
local `.fwsc`. Installing firmware is at your own risk.

**Back to stock or to any other firmware:** the hub, or M-VAVE's own updater with their `FM-1.fwsc` —
OPLBOX answers the updater like every firmware in the family.

**If it will not start:** hold **OCT−** while switching on → the USB rescue (no sound, only the updater),
then install anything from there. A firmware that crashes twice at boot goes to the rescue by itself.
If even that is gone, the chip's own boot ROM still updates over USB (FM-1-transporter).

## What is in it

- `firmware/src/oplbox.c` — the app: audio interrupt, song control, screen.
- `firmware/src/opl2fast.c` — the OPL2 fast path over Nuked-OPL3: 18 slots and 9 channels instead of 36/18,
  dormant melodic slots on a phase-only path, register-derived constants cached at write time, the silent
  bank's 18 noise clocks as one LFSR jump. **Bit-exact with full Nuked-OPL3** (allowing its one-sample output
  latency), checked over 30 s of every song (`host/skiptest.c`).
- `firmware/src/opl3.c` / `.h` — Nuked-OPL3 1.8 (LGPL-2.1), with the stereo-extension blocks removed and
  three small per-slot caches added.
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
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/fw-AC79_AIoT_SDK ./build.sh --release 0.1
tests/run_host.sh                 # the same app on this computer: WAV + screen PNGs, no FM-1 needed
```

Package identity `FM-1_96N` (N = minor version).

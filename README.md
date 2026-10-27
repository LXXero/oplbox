# OPLBOX — the M-VAVE FM-1 as an AdLib

Custom firmware for the M-VAVE FM-1 that plays the **SoundFX Macs Opera** songs — the DOS soundtracks of
LINEL's games (Michael Tschögl, 1990–92) and of Eclipse's **Stone Age** (Thomas Detert, 1992) — through an
OPL2 emulation, exactly the way FXDRIVER V1.06 drove a Sound Blaster.

**Status: 0.4.1 — it is a tracker now.** EDIT turns the tracker screen into an editor: cursor on the knobs,
notes from the keys, a song screen for the order list and an instrument screen for the patches, SAVE to one of
nine flash slots, and the slots are plain `.CMF` files, the format SoundFX Macs Opera / FXDRIVER read. Proof:
a song edited on the (host-mode) FM-1, saved, and played by Stone Age's own driver in DOSBox gives a register
stream identical to the FM-1's (`host/evcmp` / `rowcmp3`, 434 of 434 key-ons), and every original song
round-trips through the editor's model byte for byte (`host/roundtrip.c`, 18 of 18). Hardware: 0.2–0.3 ran at
~20 % CPU on a real FM-1 (Smalley); 0.4 adds nothing to the audio path.

## Controls

**Playing** (jukebox / tracker / scope screens)

| | |
| --- | --- |
| **PLAY** | play / pause |
| **HOME** | back to the start of the song |
| **SELECT** knob | previous / next song: the 18 built in, then slots 1–9 |
| **OCT−  OCT+** | the keys' octave, `OCT0`–`OCT6` (`OCT3` = F3 is the lowest key, as in EDIT); the status line shows it for two seconds (on the arp screen, that screen's last line shows it instead). A playing arp follows it from its next step |
| **REC** | loop one song ↔ play them all in turn (the LED shows which) |
| **SEQ** | jukebox screen ↔ **tracker screen**: the pattern scrolling by, one column per channel, the playing row in the middle. Notes as `C4` / `c4` (lowercase = sharp), `==` key off, `EN` end of pattern. Rhythm songs show 11 columns (6 + BD SD TM TC HH), melodic songs 9. |
| **FX** | jukebox screen ↔ **scope screen**: nine oscilloscopes, one per channel, labelled `06 produc` (instrument number, then name; `--` = the driver's default patch). In a rhythm song boxes 7-9 are the drums: `BD`, `SD`, `HH TM CY`. The first nine white keys (F3–G4) mute / unmute channels 1–9: red label, dim trace, key LED lit. Mutes survive a restart, clear on a song change. |
| **GLO** | the battery's raw ADC count in the status line for two seconds, e.g. `BATTERY 0571 (2/3)` (count, bars), to tune the battery icon by |
| **SAVE** | the **WiFi test page** (below). Nothing touches the radio until you press PLAY there |
| **ARP** | the arpeggiator on (its settings screen opens) / off; the LED shows it. Not in EDIT, where ARP keeps its editor meaning |
| **LFO** | the arpeggiator's settings screen: open it, or turn its page |
| **PRESETS** | which of the song's instruments the keys (the arp) play: the status line says `I03 t.detert`. It is the editor's instrument too (`I03` in EDIT) |
| **ALGO** | that instrument's connection and feedback, one list: FM with feedback 0–7, then additive with feedback 0–7 (`I03 FM FB 5`). On a copy only the keys / the arp play; the song keeps its own (on its channels and in SAVE). Loading a song or picking another instrument drops the copy |
| **MASTER** | volume |

**Arpeggiator.** With ARP on, held keys play an arpeggio in the keys' instrument (PRESETS / ALGO above; `I03` in the editor); with one key held it repeats that note, so hold a chord (or turn **LATCH** on, below) to hear it step. The settings screen's last two lines show the channel, the notes held, latch, that instrument and the keys' octave; there PRESETS, ALGO and OCT−/+ change them without the 2 s status message, and the status line below shows the song's title (the new one as soon as you change songs) instead of LOOP SONG / ALL IN TURN.
Not from the scope screen, whose keys stay the mutes; EDIT gives the keys back to the editor. While a song plays
it takes a channel the playing pattern leaves silent, else a channel muted on the scope (heard again, for the
arp), else the last melodic one (6 in rhythm songs, 9 otherwise; never the drums); the song's notes in that column
are left out while it plays there. The settings screen shows the channel. Page 1: **K1** mode (up, down,
inclusive, exclusive, random, order, repeat), **K2** octaves 1–4, **K3** rate (1/4 1/8 1/8T 1/16 1/32), **K4**
tempo 30–300 BPM, or **SYNC** (turn K4 left past 30; right leaves it at 30). **OCT−/+** move the notes it plays (held or latched) an octave from the next step, as the stock firmware does; letting go of a key still takes its own note out. Page 2 (LFO): **K1** gate 20–100 %, **K2** swing 50–75 %, **K3** latch (right on, left off),
**K4** how much ARP writes into the tracker, 1–8 patterns (below). The settings and the keys' octave survive a restart, except K4,
which starts at 1; the arp itself starts off.

**SYNC: the arp in the song's tempo.** With SYNC the arp has no BPM of its own: it steps with the song's rows, and
**one row is a 1/16**, so 1/4 is 4 rows, 1/8 2, 1/8T 4/3 (three steps every 4 rows), 1/16 one row, 1/32 half a row.
While the song plays, a step that falls on a row starts on that row, exactly (the pattern's rows 0, 4, 8 … for 1/4),
and a new chord's first step waits for the next step of that grid. Swing and gate work as with a BPM. When the
song is stopped the arp keeps the same step length from the song's speed; a new speed (song screen, K2) or a new
song changes it at once. To play along and keep it: ARP on, K4 to SYNC, play the song and the chord until it sounds
right; then EDIT, cursor to the row it should start on, and **ARP** writes what you heard (below).

**WiFi test page** (stages 2-6 of docs/WIFI.md; for xero to read back off a real FM-1). Outside EDIT press
**SAVE**: the page opens and says `PLAY: POWER UP, TRIM, READ`. Nothing touches the radio yet. Press **PLAY**: the
sound stops, the WiFi core is powered up and read (stage 2), then the radio is brought up and calibrated (stage 3:
the PLL, a fresh trim, channel 6), each of channels 1-13 is surveyed for 200 ms as JieLi's own channel select does,
and the receive state is read on channel 6. Then stages 4-6 in the same run (docs/WIFI_STAGE4.md): a passive scan, an
active scan, and joining an open network named `OPLBOX` (make one: a phone's hotspot, open, 2.4 GHz), 2 s joined,
leaving and joining again. It takes about 10-15 seconds (`WORKING`). Page 1:

```
RESULT: OK                    OK, or what went wrong and where: TIMEOUT AT TRIM, NO ANSWER, ...
MAC_CSR0     00300100         the chip's ASIC version (stage 2 read 00300100 on xero's FM-1)
MAC_SYS_CTRL 0000000C         after the MAC and BBP resets are released: 0000000C (TX and RX on) is right
BBP 0        xx               baseband register 0 once out of reset: anything but 00 or FF is right
PLL BANK  nn  MID  mm         the PLL's bank for the reference divider (0-127) and the scan's middle bank
FILTER  aaa bbb ccc           the receive filter tune's codes for 6, 10 and 20 MHz
RAM CHECK: n CHANGED          bytes of free RAM the hardware wrote (0 is the good answer)
FIRST AT xxxxxxxx             the first changed byte's address (only when n is not 0)
```

Press **PLAY** again for page 2, and so on to page 11, then back to page 1. Page 2:

```
TRIM  CH  6  PWR 6            the channel and TX power the radio was left on
TX LO  ii  qq  DC   ii  qq    TX trim: LO leakage I and Q (0-127), DC I and Q (-128-127)
TX GAIN   +g  PHASE   +p Q    TX trim: gain imbalance, and the phase on the side it went (I or Q)
RX GAIN   +g  PHASE   +p I    RX trim: the same
RX DC I   +a   +b   +c   +d   RX DC trim: the analog DC per gain range, I then Q
RX DC Q   +a   +b   +c   +d
CCA  1- 5   nn  nn  nn  nn  nn    the false CCA count on channels 1-5, 6-10, 11-13 over 200 ms (1000 and
CCA  6-10 ...                     over in thousands: 6K; page 4 has the exact counts)
CCA 11-13 ...
```

Page 3, the receiver's ADC peak outside the trim (main c4aa202's run: it reads only inside the trim's loopback, so the
page no longer scans with it; docs/WIFI_STAGE3.md "ADC peak 0 on every channel"):

```
RX CHECK  CH  6       3/10
VGA GAIN   nn  nn  nn         the VGA gain each trim search settled on (TX LO, TX DC, RX IQ); 36 = it ran to its limit
VGA PEAK  nnn nnn nnn         the ADC peak each search read last; above 47 ends a search early
PEAK AT VGA  0  nnn           the ADC peak on channel 6 at VGA gain 0 (where the bring-up leaves it)
PEAK AT VGA 41  nnn           the same at the highest VGA gain (then back to 0)
RXFE 80 xx  23 xx             RX front end 0x80 before the peak starts; 23 read back once started (82 expected)
RXFE 0  xx  19 xx  20 xx      the RX front end set-up the trim changes (3A 00 00 after the bring-up; trim: 3E 1E 34)
BBP 25  xx                    00 receive, 18 the PHY's loopback (the trim)
```

Reading page 3: VGA GAIN below 36 with VGA PEAK above 47 means the peak reads fine inside the trim, so an all-zero
scan is the receive state; 36 36 36 with peaks of 0 means the peak reads 0 everywhere. A nonzero PEAK AT VGA 41 means
the receive state reads too, only below 1 at VGA gain 0. A `--` means the step was never reached.

Page 4, the channel survey (docs/WIFI_STAGE3.md "Receive level per channel"): per channel, what JieLi's AP channel
select reads after 200 ms on it, channels 1-7 on the left, 8-13 on the right:

```
SURVEY  200 MS EACH   4/10
    CCA   BUSY     CCA   BUSY
1  nnnn  nnnnn  8  nnnn  nnnnn    channel, false CCA count, busy time (rt2800: microseconds, at most 200000)
...
7  nnnn  nnnnn
```

Reading page 4: both are counts, not dBm. An empty, quiet channel counts little of either; channels with an access
point on them or next to them count more (JieLi's channel select picks the lowest false CCA count), so access points on
1, 6 and 11 should show as higher counts on and around those channels than in the gaps. `BUSY` 0 everywhere while
`CCA` is not: the busy-time accounting is off on this chip. Both 0 everywhere with access points near: these counters
do not count either, and a level waits for received frames (stage 4).

Pages 5-11 are stages 4-8 (how to read them, and what a failure would mean: docs/WIFI_STAGE4.md "Reading pages
5-10" and docs/WIFI_STAGE8.md): page 5 each step's result (`RADIO`, `SURVEY`, `LINK`, `PASSIVE`, `ACTIVE`, `JOIN`,
`4-WAY`, `GROUP KEY`, `STAY 2S`, `REJOIN`: OK with what it found, FAIL with the reason, or NOT REACHED), page 6 the
networks heard (SSID, channel, RSSI, security, `P` when it answered a probe), pages 7 and 8 the first four received
frames' bytes in hex, page 9 the core's FIFO and counter registers and the FM-1's WiFi address, page 10 the join in
detail, page 11 the WPA2 handshake and key tables. The run joins the WPA2 network set from a computer
(`python3 tools/fm1_wifi_report.py --ssid NAME --psk-from-nm CONNECTION --run`; the FM-1 keeps it in RAM only), or
without one the open network OPLBOX.

Read back everything on all eleven pages: from a computer the FM-1 is plugged into, `python3 tools/fm1_wifi_report.py`
prints the last run's report (all eleven pages as text, every network, the counters) and `--run` starts a run first, with
no button pressed (refused in EDIT; `--out DIR` also writes each received frame's bytes; docs/WIFI_STAGE4.md "Reading
the results over USB"). Without a computer, a photo of each page is enough. What would look wrong: `RESULT` other than OK (the word after `AT` says which
step stopped: PHY, PLL, TRIM, RF, TABLE), `BBP 0` still `00`, a PLL bank of 0 or 127, a middle bank of 15 (the scan
found no edge), trim values sitting at the end of their range (TX LO 0 or 127, TX DC ±127, an RX DC near ±510) or a
filter code of 0 or 255: each means a search ran to its limit (a VGA GAIN of 36 on page 3 too, though JieLi's
trim goes on after one). The trim runs from RAM with interrupts off for about half a second, so USB
goes quiet meanwhile. **SAVE** leaves: the core is powered down (every register back to how it was) and the sound
comes back. If the FM-1 hangs on the page, power it off and on: nothing was written to flash, and it boots as before.
Before the core is powered up, PLAY also raises the chip's supply rails to the levels M-VAVE's own firmware uses
(VDC14 1.60 V, SYSVDD 1.38 V, the 1.3 V supply on its DC-DC, the WiFi 1.2 V regulator on automatic); they stay until
the FM-1 is switched off.

**If it crashes** (a red `OPLBOX CRASH` screen, for 8 seconds, then a reboot), photograph the screen or copy every
line:

```
OPLBOX CRASH
VEC 00000001  EMU xxxxxxxx    the vector (00000001: a CPU exception); EMU_MSG: stack limit, illegal instruction, ...
PC  xxxxxxxx  DBG xxxxxxxx    where the CPU was; DBG_MSG: a bus error, a write into a guarded area, a PC limit
RET xxxxxxxx  SP  xxxxxxxx    the return address and the stack pointer
ETM xxxxxxxx  xxxxxxxx        the last four branches the trace unit kept
    xxxxxxxx  xxxxxxxx
RAM CODE SAME AS FLASH        or CHANGED AT xxxxxxxx: the code that runs from RAM was overwritten there
WIFI AT TX LO I               only for a crash inside the WiFi library: the step it was in (RX READ, TX SEND, ...)
WR 00011930 = xxxxxxxx        and its last register write (address = value)
```

The crash on 2026-10-26 (vector 1, PC 01C02674 in `rmw()`) is in docs/WIFI_STAGE3.md "First run on an FM-1".

**Battery.** Every screen shows a battery at the top right: 3 bars, red outline when empty. It reads ADC ch3
(the battery divider) once a second, smoothed over a few seconds. The bars switch at raw counts 531 / 561 / 591,
the thresholds Felucca and SLOOP use; they are not converted to volts and have not been checked against a meter
on OPLBOX. They live in one table, `BATT_LEVEL` in `firmware/src/oplbox.c`.

**Editing** — **EDIT** (LED lit). The tracker screen follows the cursor instead of the playback; SEQ now cycles
tracker → song → instrument screens. Everything edits live: a change is heard on the next row.

| tracker screen | |
| --- | --- |
| **K1 / K2** | cursor column / row |
| **SELECT** | order position (which pattern you are in) |
| **keys** | enter that note at the cursor (and play it, when paused), then step down; **OCT−/+** shift the keyboard (`OCT3` in the status line = F3 is the lowest key) |
| **K3** | the note under the cursor ± a semitone |
| **K4** | instrument: the cell's, and the one new notes get (`I03`) |
| **ALGO** | volume: the cell's, and the one new notes get |
| **PRESETS** | step after a note, 0–16 (`STEP1`) |
| **FX** | key off (`==`) here |
| **SCL** | clear the cell |
| **GLO** | end the pattern at this row (`EN`) / undo that |
| **PLAY** | play from the cursor / pause |
| **HOME** | cursor to row 0 |
| **ENV** | undo the last edit, in any editing screen (32 steps, no redo; a song change clears them). A knob turned on and on is one step |
| **REC** | arm recording (`REC` in the status line, LED lit); press again to disarm |
| **SAVE** | save the song to its flash slot (a built-in song goes to the first empty slot; the status line says which) |
| **ARP** | write the arpeggio into the cursor's column, from the cursor down (below) |

**Recording while the song plays.** Arm REC, press PLAY, and play the keys: each note goes into the cursor's
column at the row the song is on (the row that just played, or the next one when the key comes more than half a
row after it begins), and sounds. Letting go of a key writes nothing, so the note rings until the next note in
that column. Each recorded note is one ENV step. With REC armed and the song paused, keys write at the cursor as usual.

**Writing the arpeggio into the tracker.** Play a chord into the arp (held or latched), go into EDIT, put the
cursor where it should start and press **ARP**: the chord, with the arp's settings, becomes ordinary notes in the
cursor's column, one pattern of rows from the cursor down (page 2, K4: up to 8). Each step is a note in the editor's
instrument and volume (`I03`, ALGO), a key off where the gate ends, and empty rows between; what was in those rows
is replaced. Past row 64, or at an end of pattern, it goes on in new empty patterns put into the song right after
this position. A step must be a whole number of rows, so the song's speed sets what tempos fit: at 9.1 rows a second,
1/8 at 91 BPM is exactly 3 rows. Otherwise the nearest number of rows is used and the status line says so, with the
tempo you get (`ROUNDED: 136.5 BPM`); swing rounds to rows too (`ROUNDED: SWING`). With SYNC there is nothing to
round: each step goes on the row the synced arp starts it on (1/8T: rows 0 1 2, 4 5 6 …), the gate's key off on the
row it ends in when that is a later row than the note's and before the next step (`ARP: SYNC`); only 1/32, two
steps a row, does not fit cells and is written a row a step (`ROUNDED: 1/32 AS 1/16`). One **ENV** takes all of it
back. Not in a drum column; the song is still a plain .CMF.

| song screen (SEQ once) | |
| --- | --- |
| **SELECT** | position in the order list |
| **K1** | which pattern plays at this position |
| **GLO** | insert a position after this one (a copy of it) |
| **SCL** | delete this position |
| **ARP** | a new, empty pattern at this position |
| **K2 / K3** | speed 1–3 (18.2 / 9.1 / 4.55 rows per second) / rhythm mode on–off |

| instrument screen (SEQ twice) | |
| --- | --- |
| **PRESETS** | which instrument |
| **SELECT** | the row: name, then operator 1 and 2 (AR DR SL RR TL KSL MULT AM VIB EG KSR WAVE), feedback, connection |
| **K1** | the value (on the name row: the character position; **K2** the character) |
| **ARP** | a new instrument, a copy of this one |
| **keys** | play the instrument on channel 1 |

The header line while editing: `03/14 P07  c4 I05 V127 B32` = order position / positions, pattern, and the
cursor cell's note, instrument, volume and bend.

**Slots.** Nine 32 KiB slots in the flash area the Felucca family keeps for data (`0x98000`–`0xE0000`), each
holding one `.CMF` behind a small header with a CRC. Picking an empty slot in the jukebox starts a new, blank
song (one pattern, one instrument). Songs in slots play like the built-in ones. Getting a slot's `.CMF` to a
computer over USB is next (0.5); until then the host build writes `build/host/slotN.cmf`.

## Install

Exactly like any Felucca-family firmware. Over USB from a computer, with the FM-1 switched on and running
any firmware that has the update path (stock V14/V15, Felucca, SLOOP, Dinghy, …):

```sh
pip3 install mido python-rtmidi
python3 tools/fm1_install.py oplbox-0.4.1.fwsc        # asks before it writes, restarts into OPLBOX
python3 tools/fm1_install.py --info                 # what the connected FM-1 runs
```

Or through the FM-1 Firmware Hub (https://fm1.designburgapps.com) in Chrome/Edge, which can install a
local `.fwsc`. Installing firmware is at your own risk.

**Back to stock or to any other firmware:** the hub, or M-VAVE's own updater with their `FM-1.fwsc` —
OPLBOX answers the updater like every firmware in the family.

**If it will not start, or will not answer the installer:** hold **OCT−** (only OCT−) *before* switching on,
switch on, keep holding until the screen says `OPLBOX USB RESCUE` → no sound, only the updater; install
anything from there. The key is read in the first 50 ms after power-up, so it must already be down. (0.4.1: on some boards the keys cannot be read at all until the screen's pins are set up, so the rescue never opened; the screen is now set up before the check, after dinghy PR #4.) A
firmware that crashes twice at boot goes to the rescue by itself. If even that is gone, the chip's own boot
ROM still updates over USB (FM-1-transporter).

If 0.1 is what is running (the one that starves itself): button presses are latched by a timer interrupt even
while the audio interrupt hogs the core, so press **PLAY** once and wait for the song to end (2–5 min) — in that
quiet moment the main loop sees the press, pauses, and the installer gets its answer.

## What is in it

- `firmware/src/oplbox.c` — the app: audio interrupt, song control, overload guard, the screens, the editors, the slots.
- `firmware/src/macsop.c` / `.h` — the song model (patterns as 65 × 11 cells, instruments, order list), the lossless
  `.CMF` reader / writer, and the player (FXDRIVER's arithmetic). `host/roundtrip.c` checks the model against
  the 18 files byte for byte and hashes the register streams.
- `firmware/src/dbopl2.c` — DOSBox's `dbopl` (GPL-2+) as one OPL2: 9 channels, 18 operators, no OPL3 modes,
  no floating point — every table the original computes with floats is generated for 49716 Hz by
  `host/dbopl_gen.c` into `dbopl2_tables.h`. Its own object at -O2 (the app and the platform build at -Os, see
  0.4.2 in CHANGES.md). The sample loop is linked into RAM (`.ram_code`, app.ld) and the
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
- `firmware/bsp/` — the chip and board layer: boot, USB, the updater and the update loader, the panel's key scan.
  felucca-bsp (Felucca's, split out; Leo Kuroshita, Charles Vestal and others), vendored, MPL-2.0.
- Everything else is Dinghy (zvenson) / Felucca (Leo Kuroshita, Hügelton Instruments) / SLOOP (isod89):
  the wiring, the rescue, the screen. GPL-3.0; see `LICENSING.md` and `CREDITS.md`.

## Build

As Dinghy: Linux x86-64, Python 3 with Pillow, JieLi's Linux toolchain (`tools/get_toolchain.sh`) and the
three boot files of the AC79 SDK at tag `AC79NN_SDK_V1.2.1_2023-12-13` (hash-checked by the build).

```sh
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/fw-AC79_AIoT_SDK ./build.sh --release 0.4.1
tests/run_host.sh                 # the same app on this computer: WAV + screen PNGs, no FM-1 needed
```

The OPL cores on their own (`host/`, from the `oplbox` reference tree): `dbopl_gen.c` makes the tables,
`dbcmp.c` is the lockstep check against DOSBox, `render.c` renders a song through Nuked, `skiptest.c` checks
the Nuked fast path.

Package identity `FM-1_96N` (N = minor version).

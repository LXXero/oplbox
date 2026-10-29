# OPLBOX — TODO

In order. Each item says what exists today (as of 0.4.2, commit a4b62d9), where the change goes, and what is
still open. Line numbers are `firmware/src/oplbox.c` unless another file is named.

## 1. Upstream: Dinghy (zvenson)

https://github.com/zvenson/dinghy, default branch `main`.

**Today.** OPLBOX is a copy of Dinghy 0.3. This checkout has `origin` (git@github.com:LXXero/oplbox.git,
branch `main`) and a fetch-only `upstream` remote for Dinghy (added later; nothing merged from it); its history starts at OPLBOX 0.1 (6353ff6), so git does not know
the relationship and every upstream change has to be carried over by hand. Compared file by file with Dinghy's
tags (a clone in /tmp, no remote added):

- `firmware/src/usb.c`, `ota.c`, `gfx.c`, `lcd.c`, `recovery.c`, `docs/API.md`: identical to Dinghy v0.3.
- `firmware/src/platform.c`: v0.3 plus two changes. `FL_RANGE_OK` lets the app write the song slots
  (0x98000–0xE0000), and `fm1_cstart()` calls `fm1_lcd_hw_init()` before the OCT− rescue check. The second
  one is dinghy PR #4 (sgarrettnelson), cited in README.md ("after dinghy PR #4") and CHANGES.md 0.4.1.
  PR #4 is still open upstream and is not in Dinghy 0.4.
- `dinghy.c` became `firmware/src/oplbox.c` (the whole app; `tools/build.py` compiles it under that name).
- `tools/build.py`: OPLBOX's own build. `PRODUCT = "FM-1_960"` (OPLBOX's 96N identity, not Dinghy's 99N),
  output names `oplbox.*`, and `dbopl2.c` as a separate object at -O2 with the app and platform at -Os
  (0.4.2).
- Everything OPLBOX adds (`macsop.c/.h`, `dbopl2*`, `opl3.c`, `opl2fast.c`, `songs.h`, `host/`, `tests/`
  scripts) has no counterpart upstream.

**Dinghy 0.4** (7c31bb2, 2026-10-23; HEAD is 22acb91, a CI change) was a large release: 41 files, about
4000 lines. It adds TRS MIDI IN (`midi_uart.c`), MIDI clock and transport in, a MIDI channel setting,
`store.h` with blobs and slots, `fm1_flash_map.h` (FL_GUARD), LCD_IDLE, the update loader's screen, the SMK-37
boards, a test suite and a CI release with attestation. `platform.c` changes by about 230 lines. If OPLBOX
took 0.4:

- Dinghy 0.4's `platform.c` defines `static slot_load()` / `slot_save()`. `oplbox.c:254` defines its own
  `slot_save()`, so the build fails until one of them is renamed.
- Dinghy 0.4's FM-1 slots are at 0x97000, 8 × 16 KiB (to 0xB7000). That overlaps OPLBOX's slots at 0x98000,
  9 × 32 KiB (`SLOT_BASE`, `oplbox.c:200`). A Dinghy slot holds 16 KiB − 12 bytes; four of the 18 built-in
  songs are bigger than that (TRADINTR 28510 B, ECHOS 26260, DR_TALE 21014, GOLDVAL 20953). So OPLBOX would
  keep its own slots and change `FL_RANGE_OK`, which moved to `store.h` in 0.4.
- The MIDI clock in would give the arpeggiator (item 3) an external clock, and TRS MIDI IN would give live
  recording (item 2) a second input.

**Which base.** xero said to keep syncing unless OPLBOX plans its own base, and asked about fm1-base.
[docs/BASE.md](docs/BASE.md) compares Dinghy 0.4, fm1-base, felucca-bsp, Felucca and M-VAVE's FM1 Open SDK
(licence, activity, flash map, audio, SysEx, MIDI clock and TRS, simulator, update and rescue, porting size). It
recommends moving the platform files onto felucca-bsp, keeping the app and the OCT− rescue, and taking only
`midi_uart.c` from Dinghy 0.4. It also found two problems in OPLBOX's current copy: `ota.c` refuses M-VAVE's V16
writer, and the update loader can erase a song slot that happens to look like an update record. Its last section
has the two questions for xero. The questions below are from before it.

**xero's answers to BASE.md (2026-10-25).**
1. felucca-bsp now, not fm1-base ("i guess we can start with felucca-bsp").
2. He does not switch one FM-1 between Felucca (or a Felucca-based firmware) and OPLBOX for now ("i dont at the
   moment, not to say i never could"). So the song slots stay at 0x98000..0xE0000. If he ever does, they have to
   move: the free window 0xE7000..0xFBFFF holds only 2 of the 9.

**The platform on felucca-bsp (2026-10-25, branch `xyphia/fanout/c8e7be1c55c37b5b/0`, waiting for the FM-1 test
below).** The Felucca-derived files listed under "Today" were swapped for felucca-bsp's:

- felucca-bsp `d6b8f771795be04d77e15d72a79f474a723796d7` (2026-10-24), its latest `main` on 2026-10-25 and the
  same commit BASE.md read: nothing changed upstream since. Vendored in `firmware/bsp/` (`chip/`, `boards/fm1/`,
  `loader/`), MPL-2.0 with its SPDX lines, `LICENSE` and `NOTICE`; the file list and the one change in
  `firmware/bsp/README.md`. A vendored copy rather than a git submodule was chosen by the colony; xero can change it.
- Gone: `firmware/hal/` (all Felucca's), `firmware/loader/`, `firmware/crt0.S`, `firmware/src/usb.c`, `ota.c`,
  `libc.c`. `firmware/hal/` stays as the place for OPLBOX's own hardware files (README.md there; the WiFi branches
  add theirs to it), on the include path after the bsp.
- `chip/ota.c` has OPLBOX's `OTA_IDENTITY` hook again, marked "OPLBOX change" (it stays MPL-2.0): the USB rescue
  answers "FM-1_000".
- `platform.c`: `uac_tap` gets 24-bit samples (Q15 × 256, then `>> 1` for the I2S, so the USB recording keeps its
  old level and the speaker its -6 dBFS ceiling); `ota_service()`'s return value is read and dropped (OPLBOX has no
  commands of its own yet, TODO 2a); `FELUCCA_UAC_DEEP 1`. `tools/build.py`: the bsp's include paths and sources;
  the register check now allows `bsp/` and `hal/` and checks `src/`.
- `uac_fir.h` (the 48 kHz resampler) stays OPLBOX's 12-tap Q15 table, stored as `int32_t`, the type the bsp's
  `usb.c` reads. Felucca's 24-tap Q30 table would do twice the work per frame in the audio interrupt. Chosen by
  the colony; xero can change it.
- Unchanged: `oplbox.c`, `lcd.c`, `gfx.c`, `recovery.c`, `bootguard.h`, the OCT− rescue with PR #4's
  `fm1_lcd_hw_init()` before it, `FL_RANGE_OK` with the song slots at 0x98000..0xE0000, the settings sector at
  0xFC000, `app.ld`. No slot moved. Nothing from Dinghy 0.4 (`midi_uart.c`, SysEx and recording are TODO 2a/2b).
- The fixes BASE.md lists, all in d6b8f77:
  - afd83ed: `ota.c` stages any well-formed update writer and checks that a flash image's boot area is one the
    FM-1 boots (`ota_boot_valid`), so M-VAVE's V16 can be installed from OPLBOX.
  - The update loader skips Felucca's store 0x97000..0xE0000 when it clears update records (`ldr_core.c`), and
    its flash guard cannot write there or at 0xFC000 and up (`loader.c`): no song slot or settings sector is
    erased by mistake. `tests/loader_test.c` checks it.
  - The USB audio ring band 96..160 for 256-frame halves, a half higher with `FELUCCA_UAC_DEEP` (352..416 in a
    1024-frame ring; +5.8 ms of latency on the recording).
  - The supply-rail raises (`fm1_vdc14_raise`, `fm1_sysvdd_raise`, `fm1_power_stock` in `fm1_sys.h`): there, and
    since 2026-10-26 called by the WiFi page before the core comes up (`platform_supply_full`; docs/WIFI_STAGE3.md
    "First run on an FM-1"), not at boot.
  - The crash register dump: `fm1_crash_ctx` keeps r0..r15, usp, ssp and pc of the last four crashes in
    `.noinit`. OPLBOX's crash screen shows vec, pc, rets, EMU_MSG, DBG_MSG, sp and the ETM PCs (2026-10-26); nothing
    reads the r0..r15 dump yet.
- Also new from the bsp: the update loader's screen ("UPDATING", "DO NOT UNPLUG", a bar). The key scan debounce is
  unchanged; the bsp's analog key rows, LED glow levels and power-on sweep stay off unless the app calls them.
- Size: app image 346,708 B (main 9ef2a93: 342,792 B, +3,916), RAM .data+.bss 38,832 B (+2,056: the deeper USB
  audio ring), loader `ota.bin` 8,340 B (6,954 B, +1,386: its screen). `./build.sh` and `tests/run_host.sh` pass.
- `tests/loader_test.c` (in `run_host.sh`) builds a package in the test and runs the bsp's `ota.c` and
  `ldr_core.c` against a fake host. It checks the identity (FM-1_960, FM-1_000 in the rescue) and that
  `ota_service()` hands back a frame of the firmware's own. It also checks an install that keeps the song slots and the
  settings even where their bytes look like an update record, and the refusal of a damaged package and of one for
  another chip key before anything is erased. Each of six planted bugs (the old loader's record scan among them)
  fails it.

**Test on an FM-1 before this ships** (nothing of it has run on hardware):
1. Update over M-UPGRADE: install this branch's `build/oplbox.fwsc` from OPLBOX 0.4.x with the web installer or
   `tools/fm1_install.py`. The loader's new screen shows "UPDATING", "DO NOT UNPLUG" and a filling bar; OPLBOX
   starts after it.
2. The OCT− rescue: hold OCT− at power-on. The USB rescue opens, the installer sees "FM-1_000", and an install
   through it works.
3. Installing V16 from OPLBOX: from a running OPLBOX, install M-VAVE's V16 package (its new bootloader). The old
   `ota.c` refused it. Then put OPLBOX back on.
4. Song slots and settings survive an update: save songs in slot 1 and slot 9, change a setting (MASTER isn't one;
   e.g. the arp's), update OPLBOX (step 1), and check that both songs and the setting are still there.
5. USB audio in: record the FM-1 on a computer at 44.1 kHz and at 48 kHz while a song plays. No crackle or
   dropout, and the same level as with OPLBOX 0.4.x.
6. The screen at boot: OPLBOX's screen comes up as before. Check the keys, the knobs and the LEDs too (the key
   scan is the bsp's now).

**Open questions.**
- Keep tracking upstream? If yes: add Dinghy as a remote (done: `upstream`, fetch-only), merge or cherry-pick 0.4's
  platform and HAL files, and decide whether to offer OPLBOX's platform changes (`FL_RANGE_OK` and the PR #4
  line) back upstream. If no: say in README.md that OPLBOX is based on Dinghy 0.3 plus PR #4 and is no
  longer synced.
- Which parts of 0.4 are wanted on their own if the whole release is not taken: the MIDI clock in, TRS MIDI
  IN, the test suite.
- If tracking: record in CHANGES.md which upstream commit each port came from.
- Either way: update the Dinghy-era docs to name `oplbox.c`, and write or drop the `CREDITS.md`
  reference. **Done:** the docs name `oplbox.c`, and `CREDITS.md` is written.

## 2. The 0.5 items — xero said yes to all three

CHANGES.md line 34 (0.4): "Not yet: pulling/pushing .CMF files over USB (0.5), live recording while playing,
undo."

### 2a. Pull and push `.CMF` slot files over USB — built, host-tested, not yet tried on hardware

**Built.** `tools/fm1_slot.py pull SLOT FILE` / `push SLOT FILE` (SLOT 1-9 as the jukebox shows them; it finds the
FM-1 with `fm1_install.py`'s handshake and uses its framing). On the FM-1, `firmware/src/slot_sysex.c`: two
commands in the updater's framing (`F0 pack7(00 59 cmd len24 body ~sum) F7`), `0x60` pull and `0x61` push, with the
tables and status codes in docs/API.md "Song slots over SysEx". `platform.c` hands every frame in that framing that
is not the updater's handshake to the app's `app_usb_cmd()` (before, `ota_service()` dropped it).
- Pull: the computer asks for 512 bytes at an offset; the FM-1 answers from `slot_ptr()`/`slot_len()`.
- Push: the computer announces the length and CRC-32; the FM-1 reads the file with the updater's own read requests
  (`ota_read`, cmd `0x30`, as fm1-base's `plat_host_read` does), into `slot_buf`, checks the CRC and the file with
  `macsop_check()`, writes the slot header and calls `slot_program()` with the sound stopped as SAVE does. It
  refuses a slot number out of range, a file bigger than 32 KiB − 16, one `macsop_check()` refuses, and the slot
  whose song is in the player. The screen says "USB: SLOT n SAVED / IS PLAYING / NOT SAVED".
- `macsop_check()` (new, in `macsop.c`) is `macsop_parse()`'s walk without a song to write into: a second 190 KB
  `macsop_song` does not fit beside the one in the player. `macsop_parse()` runs it first, so both refuse the same
  files; the host test also compares them on every built-in song, cuts and 2000 random one-byte changes.
- Tests in `tests/run_host.sh`: `tests/slot_sysex_test.c` feeds the SysEx byte stream through a usb.c-like
  assembler into `ota.c` and `slot_sysex.c` against a fake computer (round trips of all 18 songs through the nine
  slots, every refusal leaving the slot as it was, a computer that stops answering, a byte changed on the way, every
  message the FM-1 sends is SysEx in the framing and at most 605 bytes); `tests/slot_sysex_tool.py` runs
  `fm1_slot.py` against the same C code over a pipe. Three planted bugs (the in-player check off, the size limit one
  slot header too big, `macsop_check()` taking note 0) each fail it.
- Size: image 343,836 B (+1,044 B), RAM +32 B; the replies are built in `slot_buf`, so no new buffer.

**Decided** (chosen by the colony; xero can change it):
- The updater's framing, and command bytes `0x60`/`0x61`: none of the stock updater's (`0x11`, `0x30`) or
  fm1-base's (`0x41`-`0x43` backup, `0x50`-`0x54` mirror); Felucca's editor uses its own header (`F0 7D 46 4C`).
  felucca-bsp's `ota_service()` hands such frames to the app the same way (`chip/ota.c:159-166`).
- A push goes straight to flash (the erase stalls the sound, like SAVE), not into RAM as the current song.
- "The slot that is playing" is the slot whose song is in the player, playing or paused: its edits live in RAM.
- The FM-1 pulls a pushed file with read requests, so the computer only answers and never floods the 640-byte
  frame buffer; a push holds the main loop (the screen) until it is done or 30 s pass (plus up to 3 × 5 s for the
  request in flight).

**Still open.**
- USB-MIDI throughput on hardware: a 32 KiB push is 64 request/answer round trips and a pull 64 more. Test on the
  FM-1: push and pull a 28 KB song (TRADERS INTRO) and a small one, time them, and check the sound while a push
  reads (it should only stop for the write).
- The FM-1 Firmware Hub (web MIDI) could do the same with the same messages; only the Python tool exists.
- Merging with the felucca-bsp branch (`xyphia/fanout/c8e7be1c55c37b5b/0`): both change the `ota_service()` line in
  `platform.c`. Keep this branch's dispatch, or with the bsp's `ota_service()` (it returns the frame's length)
  write `d = ota_service(); if (d) app_usb_cmd(ota_dec, d);` (`slot_sysex()` ignores the handshake's `0x11`).

### 2b. Live recording while playing — built, host-tested, not yet tried on hardware

REC in EDIT arms it (LED, `REC` in the status line; REC outside EDIT is still the loop toggle). While the song
plays and recording is armed, a key writes its note into the cursor's column at the player's position
(`pos_order`/`pos_row` after `tick_row()`; the row that played last until half a row has passed, then the next
one), and is heard through the write queue (`macsop_play_note()` → `opl_write()`, with the ISR held off for the
note). Key-up writes nothing; the note rings until the next note in the column. Each recorded note is one undo step.
A recorded note in the next row sounds twice (now, and again when that row plays), which is a retrigger.
Decided by xero ("i think thats fine yeah"): cursor's column, key-up writes nothing, REC arms in EDIT.
Checked by `tests/script_rec.txt`.

**Still open.** Record MIDI in too (`app_main()` still discards `midi_get()`); quantise to something coarser than a row.

### 2c. Undo — built, host-tested, not yet tried on hardware

ENV in EDIT (any editing screen) undoes one step; no redo. A log of (offset into `song`, size, old value), 3 scalars
a step, 32 steps in a ring, the oldest dropped; an order-list insert or delete keeps the whole 198-byte list. A
knob turned on and on (same control, same value, within 500 ms) is one step. A song change (`load_song()`) clears it;
SAVE does not; it is not saved. Covers cells, the order list and its length, new patterns and instruments, speed,
rhythm, instrument fields and names. Checked by `tests/script_undo.txt`.

## 3. A working arpeggiator — built, host-tested, not yet tried on hardware

Decided by xero ("um okay sure whatever you want for the arp"): the colony's proposed defaults, below.

**Controls.** ARP outside EDIT turns the arp on and opens its settings screen, or turns it off (and leaves the
screen). LFO outside EDIT opens the settings, or turns their page (SELECT keeps picking the song). SEQ / FX leave
the screen as usual; the arp plays on. In EDIT, ARP keeps its old meanings (new pattern on the song screen, new
instrument on the instrument screen), and entering EDIT gives the keys back to the editor (held or latched arp
notes stop). The ARP LED shows whether the arp is on.
- Page 1: K1 mode (Up, Down, Inclusive, Exclusive, Random, Order, Repeat), K2 octaves 1–4, K3 rate
  (1/4, 1/8, 1/8T, 1/16, 1/32), K4 tempo 30–300 BPM. Page 2: K1 gate 20–100 % (steps of 5), K2 swing 50–75 %,
  K3 latch (right on, left off). The screen also shows the channel the arp is on and how many notes it holds.
- Keys: with the arp on, held keys feed it (jukebox, tracker and settings screens). On the scope screen the
  first nine white keys stay the mutes and feed nothing; a key let go there still leaves the arp.
- Latch: notes stay after the keys are up; a key pressed when every key was up starts a new chord. Latch off
  keeps only the keys still down.
- Inclusive plays the ends twice (C E G G E C), Exclusive once (C E G E), Repeat each note twice, Order the notes
  as pressed. Octaves go up octave by octave; notes above B7 fold down an octave.

**Sound and timing.** The arp clock runs in the audio ISR beside the row clock (`arp_acc`, native samples in Q8,
in `refill()` / `next_native()`), so steps keep the audio stream's time. Each step and each gate's end is one
`macsop_play_note()` (the `audition()` path): a few queued register writes, less than one row of the song. Swing
only lengthens every other step and shortens the next, so it adds no ISR work; it is in. Instrument: the
editor's `ed_instr`; volume `ed_vol`.

**Channel.** While a song plays: the highest melodic channel whose column has no note in the playing pattern and
whose envelope has ended (the arp's own note aside); else the highest channel muted on the scope (let back into
the mix for the arp); else the last melodic one (6 in rhythm mode, 9 otherwise). Never the rhythm-mode drums
7–9. With no song playing, the highest channel not muted. While the arp holds a channel the player skips that
column (`macsop.skip`, the one change in `macsop.c`), so the song's notes there are not heard. Picked again when
the pattern changes and when playback starts, given back when the notes stop.

**Settings.** Kept in the settings sector after the song and loop mode (10 bytes instead of 2). A sector written
by an older OPLBOX still loads (its 2 bytes, the arp at defaults); an older OPLBOX reading the new sector sees
the length differ and starts at song 1 with loop off. Whether the arp is on is not kept.

**Checked** by `tests/script_arp.txt` in `tests/run_host.sh` (new `is` / `within` checks in `tests/host.c`, arp
state in `host_cell()` rows 96–97): first note and channel, gate off at 50 %, step order Up and Down, 6214–6215
native samples a step at 120 BPM 1/16 and 8 steps in a second, 12429 at 60 BPM, legato at gate 100 %, release
on key up, latch and a new latched chord, the free channel 6 in pattern 0 of STARDUST FALLING with the player
skipping it, and the muted channel 3 taken (and unmuted) in pattern 2, which uses all six columns.

**Left out / not checked.**
- Nothing tried on hardware: the ISR time of an arp step is reasoned, not measured (`fm1_ticks()` is 0 on the host).
- The stock rate values are still not checked against stock firmware.
- No MIDI clock sync (needs Dinghy 0.4's clock in, item 1); MIDI in does not feed the arp.
- The borrowed or muted channel keeps the arp's instrument when the song gets it back, until the song's next
  note there names one.
- Swing, Random, Inclusive / Exclusive / Order / Repeat and octaves above 1 have no host check of their own.

## 4. Save an arpeggio into the tracker as a track — built, host-tested, not yet tried on hardware

A bassline generator and similar: the arp's output is written as ordinary note rows (note, instrument, volume,
pitch 32) in one column of a pattern, so the saved song is still a plain `.CMF` that FXDRIVER / SoundFX Macs
Opera plays. No new data in the file.

**Built.** ARP in the pattern editor (EDIT, tracker screen) runs `arp_write()` (`oplbox.c`, "the arp into the
tracker"). Offline, from the chord the arp last played and its settings now, it writes into the cursor's column
from the cursor down, `arp_len` patterns of rows:
- Each step is a note in `ed_instr` and `ed_vol`, pitch 32. The gate ends in a key off (`==`) at
  round(step × gate) rows, at least 1. When that is the whole step (gate 100 %, or a 1-row step) there is no key
  off and the next note cuts it, which is the live arp's legato. Every other row in the range is emptied.
- The notes come from the live arp's own step function (`arp_note_of()`, now shared by the ISR and the writer), so
  every mode and octave setting writes what it plays. RANDOM has its own random state for writing.
- Rows per step are the nearest whole number to the arp's rate at the song's speed: 1092 / 546 / 273 rows a
  minute at speed 1 / 2 / 3, divided by BPM × steps a beat. When that isn't exact, the status line says
  `ROUNDED: 136.5 BPM` (the tempo the rows give). Swing makes pairs of 2 × N rows, the first round(2N × swing)
  of them, at most 2N − 1; when that rounds, it says `ROUNDED: SWING`. When it is exact: `ARP: 3 ROWS A STEP`.
- Past row 63, or at the cursor's pattern's end of pattern (or a row that emptying this column would turn into one,
  macsop's first-cell rule), it goes on in new empty patterns put into the order list right after the cursor's
  position. It refuses, with a status message and no change, when the song has no room for them (64 patterns,
  99 positions), on a drum column of a rhythm song, and when no chord has been played since power-on.
- One ENV takes back all of it: an undo slot now holds either the order list or the 64 cells of the cursor's
  column (`undo_list` is a union, 256 B a slot; the pool grows 1856 B). Undo removes the inserted order positions
  and restores the counts and the column.
- Page 2 of the arp's settings, K4: length 1–8 patterns.
- Firmware image +2,040 B.

Checked by `tests/script_arp_write.txt`:
- A live arp (1/8 at 91 BPM: 3 rows a step at speed 2) is keyed and then written over row 40 of pattern 0.
- The cells are checked, then played from the cursor. `host_cell` rows 93–95 report each channel's key-ons from the
  register writes. The player keys the same pitches as the live arp did, at the same 16,390 native samples a step,
  lets go 2 rows in, and goes on into the new pattern.
- A second write checks 75 % swing (5 + 1 rows), legato and two patterns over the song's own notes.
- Undo of both, and the drum-column refusal.
- Each of five planted bugs fails it: gate or swing truncated instead of rounded, the new pattern's rows mapped to
  the cursor's pattern, undo not restoring the column, the range not emptied.

**Decided** (chosen by the colony; xero can change it):
- Offline from the chord the arp last played, not live recording. "Last played" means the notes as the last key
  pressed for the arp left them, held or latched; letting go doesn't clear it, so it can be written after the keys
  are up and EDIT is on (EDIT clears the arp's live chord).
- Into the cursor's column of the current pattern, overwriting the range, rather than always into a new pattern.
- Length one pattern of rows by default, 1–8 on page 2 K4. The length isn't kept in the settings sector, because
  adding a byte would change the sector's size and an older sector would no longer load the arp settings.
- The overflow goes into new patterns inserted after the cursor's position, so the rest of the song moves one
  position later per new pattern, rather than overwriting the next position's pattern.
- Volume is `ed_vol`, not key velocity (the panel keys have none).
- SAVE gives no warning. The tempo message comes when ARP writes.
- The last step's key off is dropped when it falls past the range, so the last note rings into what follows.

**Still open.**
- Recording the arp live while the song plays, through 2b's path.
- Velocity from USB MIDI (2b MIDI in) into the volume byte.
- Whether a stock tempo should snap to one the song's speed can give, or the speed be offered.
- Hardware: the write's time on the FM-1 (up to 8 × 64 cells and a pattern clear each, in the main loop).

## 5. Change the instrument while playing (with an arp running too) — built, host-tested, not yet tried on hardware

**Stock firmware.** PRESETS scrolls through 128 presets and loads each one at once. ALGORITHM picks one of 32
6-operator algorithms; holding OCT− / OCT+ and turning ALGORITHM transposes −12…+12 semitones.

**OPL2.** Two operators a channel, no 4-op algorithms. The per-channel equivalents are the connection bit
(FM: operator 1 modulates operator 2; or additive) and feedback 0–7, both in register 0xC0 + channel, both
already fields in `macsop_instr` (`feedback`, `connection`) and rows 25–26 of the instrument screen
(`ins_field()` :597).

**Built.** Outside EDIT, in every view (jukebox, tracker, scope, the arp's settings), `oplbox.c` "the instrument
the keys and the arp play":
- **PRESETS** steps through the song's instruments, 1…`n_instr`, wrapping. The number is `ed_instr`, the editor's
  instrument, so the editor's `I03` and what ARP writes into the tracker (4) are the instrument just played. The
  status line says `I03 t.detert` (`say()`).
- **ALGO** steps connection and feedback through one 16-step list: FM with feedback 0–7 (steps 0–7), then additive
  with feedback 0–7 (8–15), stopping at both ends. The first click starts from the instrument's own step. It
  changes a copy (`live_patch`) that only the keys and the arp play, not the song: its channels and SAVE keep the
  song's instrument. The status line says `I03 FM FB 5` / `I03 ADDITIVE FB 5`.
- The copy is dropped when a song loads (`load_song()`: another song, or power-on) and when PRESETS picks another
  instrument, and when EDIT ends with K4 having picked another one. When EDIT ends on the same instrument the copy
  is made again from it, so edits on the instrument screen reach it. A restart of the same song (HOME, the end of a
  looped song: `reload_song()`, edits kept) keeps it.
- The arp plays through `live_note()`, which a later MIDI-in path (2b) can share. For the copy it calls
  `macsop_play_patch()` (new in `macsop.c`), which plays an instrument that isn't in the song and sends its
  registers on every note, since the copy can change between notes. A change is heard from the arp's next step.
- In EDIT both knobs keep their editor meanings (tracker: ALGO volume, PRESETS step; instrument screen: PRESETS
  the instrument). No hold-OCT transpose: outside EDIT, OCT−/OCT+ stay previous / next song.
- Firmware image +1,084 B, RAM +64 B.
- Found on the way, fixed in its own commit: the instrument screen's CONNECT showed FM for the file's 0 and ADD
  for 1. The player writes C0 bit 0 as 1 − connection (FXDRIVER's own writes), so the file's 1 is FM; 164 of the
  223 instruments in the built-in songs are 1. Only the label and the comment in `macsop.h` changed.

Checked by `tests/script_live_instr.txt` against the chip's C0 writes (`host_cell` rows 90–92):
- PRESETS changes the instrument the arp's channel is keyed with, and wraps from 3 to 11 of 11.
- ALGO's copy reaches the chip (the exact C0 at steps 11, 0, 15 and 5) while the song's instrument keeps its own
  connection and feedback. With the song playing, the song's channels on that same instrument keep the song's C0
  while the arp's channel plays the copy.
- The status line messages, by hash; the longest, `I09 ADDITIVE FB 7`, fits beside the CPU figure (screenshot).
- In EDIT the knobs change neither the instrument nor the copy. Leaving EDIT keeps the copy, or drops it after K4.
  A song load drops it.
- Each of six planted bugs fails it: the copy not played, the song's instrument changed instead of a copy, the copy
  kept over a song load, the FM/additive halves swapped, the knobs acting in EDIT, the copy kept after K4.

**Decided** (chosen by the colony; xero can change it):
- ALGO changes a copy only the keys and the arp use, not the song's instrument.
- ALGO is connection plus feedback as one 16-step list, not modulator TL or another parameter. It stops at both
  ends rather than wrapping.
- PRESETS goes through the song's instruments, not a fixed bank, and moves the editor's instrument with it rather
  than keeping a separate play-mode one.
- PRESETS starts the new instrument as itself: an ALGO setting doesn't carry over to it.
- `reload_song()` (HOME, a looped song starting again) keeps the copy; only loading a song drops it.
- Shown in the status line with `say()`, not a new view.
- No hold-OCT transpose.

**Still open.**
- What the stock firmware's PRESETS / ALGORITHM do while its arp runs has not been checked on the device.
- The keys play nothing outside EDIT with the arp off; when 2b's MIDI in plays notes, it should go through
  `live_note()` so it uses the same instrument.
- ARP into the tracker (4) writes `ed_instr`, the song's instrument, not ALGO's copy: a written arp sounds as the
  song's instrument does. Writing the copy as a new instrument would be a separate choice.
- Hardware: hearing it on the FM-1.

## 6. Battery indicator — in progress

**Done (branch `oplbox/battery-indicator`).** A 3-bar battery icon at the top right of every screen
(`batt_draw`), fed from ADC ch3 once a second (`batt_poll`, IIR over ~4 s, 4 counts of hysteresis). GLO outside
the editor shows `BATTERY <count> (<bars>/3)` in the status line. The host build shows it at `HOST_BATT` (575,
two bars).

**Open.**
- The thresholds 531 / 561 / 591 are raw counts from Felucca (`firmware/src/ui_draw.c` `batt_level`, "the stock
  thresholds") and SLOOP (same numbers). No source gives the divider ratio or the ADC reference, so there is no
  conversion to volts. Check on the FM-1: note the GLO count on a full battery, and again when the stock
  firmware's battery shows one bar / empty, then set `BATT_LEVEL`.
- Not tested on hardware yet.
- No sign of USB power: on USB the count is the charging battery's. Felucca shows a bolt when a USB host is
  configured (`usb.config && !usb.suspended`); OPLBOX could do the same.

## 7. WiFi on our own driver — stages 2-5 run on the FM-1, stage 6 built, waiting for an open network

xero: "i would rather keep it gpl and just build our own wifi firmware". OPLBOX stays GPL-3.0-only and does not
link JieLi's binary WiFi archives.

**Done.** `docs/WIFI.md`: licence provenance of the archives, what has source and what is binary only, the
registers and the internal SDIO bus the radio uses, the power-up and calibration sequence, what fm1-radio links,
OPLBOX's size, audio and flash limits, and a plan in nine stages (one task each).

**Found.** The MAC driver in `wl_wifi*.a` is compiled from Ralink's GPL-2.0-or-later Linux driver plus Linux's
GPL-2.0 `wext-core.c`, so its Apache-2.0 label is doubtful; the WiFi core's MAC is register-compatible with Ralink's
RT2800 family (Linux `rt2800.h`, GPL-2.0-or-later); the radio and internal bus (`wl_rf_common.a`) carry Ralink and
Linux header lineage in five of thirteen members (see 7c), and the archives are LLVM bitcode, readable as LLVM IR. JieLi's stack (about 330 KB of code) would not fit beside
OPLBOX (238 KB free) anyway.

**Next.** Stages 2 and 3 ran on xero's FM-1 (7e, 7f: the bring-up, the trims, a survey that counts on every
channel), and stages 4 and 5 on 2026-10-27 (7h: receiving, the parser, transmitting and the active scan work). Stage 6
is next: xero makes an open hotspot named OPLBOX on 2.4 GHz and the colony runs the page and reads it over USB
(`tools/fm1_wifi_report.py --run --out DIR`, docs/WIFI_STAGE4.md "Reading the results over USB"), no photos needed.
That run also brings back whole frames for the parser's fixtures. Stage 7's host part (IP on lwIP, 7g) and stage 8's handshakes (7d) are built ahead of the frames they need.

### 7a. Stage 1, register header — built, host-tested, not yet tried on hardware

**Built.** `firmware/hal/fm1_wifi_regs.h`: the CPU-side blocks (`JL_ANA` `WLA_CON0`-`39`, `JL_WL` with `WL_CON*`,
`WL_ANL_CON*`, `WF_CON*`, the internal SDIO host at 0x30F00-0x30F1C, IRQ 52) from the SDK's `WL82.h`/`hwi.h`; the
WiFi core's MAC registers in use with their `rt2800.h` names, addresses and masks (Linux v6.6); JieLi's own
0x0300-range registers and the frame data addresses; the four BBP indirect windows; the key tables at 0x4000,
0x6000, 0x6800, 0x6C00, 0x7000 with their entry sizes. Addresses and masks only, no register values.
`firmware/hal/fm1_wifi_hal.c`: CPU-side accessors (bounded index, read-only `WLA_CON31`-`39` refused) and
`fm1_wifi_cmd52`/`cmd53`/`mac_rd`/`mac_wr`, which return `FM1_WIFI_ENOTYET` until stage 2. The internal SDIO
host accessor accepts only its six documented offsets; the host test checks both gaps and other invalid offsets
touch no register. `tests/run_host.sh`
compiles both warning-free (`-Werror`), runs `tests/wifi_regs.c` against a fake bus (each accessor hits its
address) and `tests/wifi_regs.py`, which checks every address and field docs/WIFI.md gives (79) against the header.
The JieLi compiler builds `fm1_wifi_hal.c` too; nothing links it yet, so the image is unchanged.

**Found.** All 82 rt2800-named constants in the header equal Linux v6.6 `rt2800.h` (sha256 `eb7ac9bf…`). All 22
`MACRegTable` addresses have an `rt2800.h` name (docs/WIFI_REVIEW.md had this UNCHECKED); the values were not compared
when this section was written (settled in 7c: they equal Ralink's staging driver, 22 of 22).
The shared key table (16 × 32 B) ends at 0x6E00, not at the mode table's 0x7000.

**Decided** (chosen by the colony; xero can change it):
- WiFi-core addresses keep `rt2800.h`'s names unprefixed (`MAC_SYS_CTRL`), so stage 5-8 code reads like rt2x00;
  CPU-side ones carry `FM1_`. The SDIO host's registers and JieLi's own are named by address (`FM1_WL30_HOST_04`,
  `FM1_WF_REG_0308`): no source names them.
- No `MACRegTable` values in the header: stage 5 takes them from rt2800lib.c or Ralink's staging driver.
- `fm1_wifi_mac_rd/wr` assume little-endian on the SDIO bus until stage 2 reads `MAC_CSR0`.
- The HAL is not linked into the firmware until stage 2 calls it.

**Open.** What the SDIO host's registers do (stage 2). The clock-block register 0x10010 that `wf_clk_rst_init` writes
is not in the header yet (`MAC_WCID_BASE`, 0x1800, is since stage 8). The 0x30F00 and 0x0300-range addresses
come from `wl_rf_common.a` as facts, not code; whether stages 1 and 3 may rest on that archive was task 3's to settle (settled in 7c: facts only).
`tools/wifi_ir.py` and the extracted tables (the rest of stage 1) are not built.

### 7b. Register model and fixture format (for stages 2-5) — built, host-tested

**Built.** `tests/wifi/sdio_model.c`/`.h`, own code from docs/WIFI.md and the SDIO Simplified Specification, host
only. A fake of the internal SDIO host that stands in below `fm1_wifi_sdio_cmd`/`cmd52`/`cmd53` (the HAL built with
`-DFM1_WIFI_SDIO_MODEL` leaves its stubs out). It keeps the card's state the way the specification does (CMD5 until
ready, CMD3 gives the RCA, CMD7 selects, CMD52/53 only once selected, no function above the count CMD5 reports),
keeps a register file per function, makes I/O Ready follow I/O Enable, and ignores writes to the read-only CCCR
registers. It logs every command (function, address, data, direction, result) and answers reads from a script:
`ocr`, `rca`, `reg` (register contents), `next` (queued answers, for a polled busy bit or a FIFO), and `fail`
(the next matching command fails). The fixture format is plain text, one command or recorded raw 802.11 frame
(`frame rx|tx key=value… bytes`) per line. `??` matches any byte, and a model's log is written in the same format,
so a run can be kept as a fixture. Its grammar is in `sdio_model.h`. `wifi_fix_diff` compares a run with a fixture
and reports each record missing, extra or different, with its line. `build/host/wifi_model diff <want> <got>` runs
it on two files. In the HAL, `fm1_wifi_sdio_enum()` is wl30_sdio_enum's sequence, written from docs/WIFI.md and the
specification: CMD5 until C is set (100 at most), CMD3 again while the card offers RCA 0, CMD7 with the RCA the card
gave, then the CCCR and function-1 writes. It also adds `fm1_wifi_sdio_cmd` (a stub until stage 2), the CCCR
addresses (cross-checked against Linux `mmc/sdio.h`, facts only) and `FM1_WIFI_EBUS`/`ETIMEOUT`.
`tests/wifi_model.c` (in `tests/run_host.sh`) runs the enumeration against `tests/wifi/sdio_enum_card.txt` and diffs
it against `tests/wifi/sdio_enum_expect.txt`, the 11 commands of docs/WIFI.md written by hand from the document. It
also checks the error paths (card never ready, no function 1, RCA 0, a failed CMD52 or CMD7), the model's
specification rules, `mac_rd`/`mac_wr` as 4-byte CMD53s, the format's round trip and 27 malformed inputs. It runs
clean under ASan/UBSan. Of 11 deliberate bugs in the enumeration, the model and the diff, the first run caught 10;
a check was added for the one it missed (the diff ignoring read vs write).

**Decided** (chosen by the colony; xero can change it):
- The model works at command level (CMD52/53 with function and address), not on the SDIO host's MMIO registers at
  0x30F00: what those do is unknown until stage 2. Stage 2's real `cmd52`/`cmd53` program them, and stage 2 can
  extend the model downward when it knows them.
- The enumeration keeps JieLi's values where the specification leaves room: CMD5 argument `0x1F000000` (bits 25-28
  are reserved in the specification's CMD5 argument; marked unchecked in the header) and I/O Enable `0xFF` (bit 0
  reserved). It takes the RCA from the CMD3 response instead of JieLi's fixed `0x00010000`; on a card that offers RCA
  1, the two send the same command.
- No I/O Ready poll after I/O Enable, which the specification recommends: wl30_sdio_enum, as decoded, has none,
  and the fixture follows the document. Stage 2 adds it if the hardware needs it.
- The model's CMD53 is byte mode, incrementing address, up to 2048 bytes; it has no block mode yet, because the HAL's
  `cmd53` has no block or fixed-address argument yet.

**Open.** Whether the WiFi core answers CMD5/CMD3/CMD7 as the card script assumes (ready at once, RCA 1) is stage 2's
to see on the FM-1. Recorded frames for the format: stage 4 dumps the first real ones.

### 7c. Licence premise for stages 1 and 3, WIFI.md corrected — documents only

**Done.** docs/WIFI.md corrected where docs/WIFI_REVIEW.md marked it WRONG, and the `wl_rf_common.a` question
settled as far as the archive shows. New sections "What `wl_rf_common.a` carries" and "What stages 1 and 3 may take
from `wl_rf_common.a`"; corrections in "Licence provenance", "What fm1-radio actually links", "WiFi core registers",
stages 1, 3 and 5, and "Reproducing the measurements". docs/WIFI_REVIEW.md has a follow-up section listing them.

**Found.**
- `wl_rf_common.a`: 8 of its 13 members (radio, PHY, calibration: `wf_cfr_init`, `wf_pa_dpd`, `wf_phy_mac_init`,
  `wf_phy_mac_lib`, `wf_rf_cal`, `wf_rf_init`, `wf_soft_adj`, `wl_rfd_init`) name nothing third-party except the
  typedefs `u8`/`u16`/`u32`. The other 5 carry lineage: `wl_hw_init` and `wl_usb_to_sdio` were compiled with
  Ralink's `rtmp_mac.h`/`rtmp_comm.h`/`rtmp_type.h`/`mac_usb.h` (RXWI/TXWI/TXINFO descriptors, 802.11 header);
  `wl_sdio` and `wl_sdio_host` with Linux-shaped `mmc/*.h`, `device.h` and FreeRTOS V9.0.0 types (GPL-2.0 with the
  FreeRTOS exception, types only); `wl_usb_to_sdio` and `wl_sdio_host` define Linux USB and MMC core function names
  as JieLi's emulation; `wf_deep_sleep` defines two empty Ralink-named stubs. The bodies read are JieLi's (stubs, OS
  calls); the larger ones were not compared.
- The 22 `MACRegTable` values equal Ralink's own driver (Linux v2.6.32 staging `rtmp_init.c`, RT2870 build), 22 of
  22; JieLi drops 8 of Ralink's 30. rt2800.h has no values. rt2x00 (`rt2800lib.c` v6.6) uses the same literal for 6
  and a different `TX_RTY_CFG`; its other 15 are built from fields and were not compared.
- `wext-core.c.o` defines 7 functions, not 10; all 7 names are in Linux's `wext-core.c` (v2.6.33, v2.6.37). Bodies
  not compared.
- `wl_rf_common.a`'s named bytes in fm1-base's `wifi.elf` are 16,017, not 16,018 (`wl_usb_to_sdio` is 4988).
- The GPL wording now says what GPLv3 section 6 asks (the source given by one of its means), not "physically with".

**Verdict for the plan.** Stages 1 and 3 may go ahead on `wl_rf_common.a` as a source of facts: addresses, values,
order, bounds, table contents. `tools/wifi_ir.py` may extract constant (target, address, value) rows and constant
arrays from `wf_phy_mac_init`, `wf_cfr_init` and `wf_phy_mac_lib`, and with `--trace` the power-up write sequence as
data (placeholders for computed values), with SHA-256/member/function provenance and no control flow or IR comments.
It reads nothing from the five members with lineage, `wl_wifi*.a` or the Bluetooth archives. Stage 3's code is
written from a plain description of each step, not translated from IR.

**Decided** (chosen by the colony; xero can change it):
- No function is translated from `wl_rf_common.a`'s IR, not even from the eight clean members: their Apache-2.0
  label is not contradicted but not proven (no notice in the archive, a blank copyright line in the SDK's
  LICENSE). Facts only, and code written from a description.
- The generated tables are listed in LICENSING.md as register values read from the archive (facts), not as
  Apache-2.0 code.
- Stage 5 takes the MAC defaults from Ralink's staging driver (they equal JieLi's); where rt2x00 differs it picks one
  and says why.

**Open** (marked [unchecked] in WIFI.md). Where JieLi's `inc/linux/` and Ralink headers came from, and which
Linux version the MMC structures match; the bodies of `usb_submit_urb`, `sdio_recv_int_hdl`, `RTUSBDoubleWrite`
and the WEXT functions against upstream; rt2x00's 15 field-built defaults; `hsm.a`'s authorship. None of these
blocks stages 1-3 under the limits above.

### 7d. Stage 8 host part, WPA2 key primitives and EAPOL-Key handshakes — built, host-tested, not yet tried on hardware

**Built.** `firmware/wifi/wpa_crypto.c`/`.h`, own code from IEEE 802.11-2020 (12.7.1.2 PRF, 12.7.1.3 PTK, J.4
passphrase to PSK), RFC 8018 (PBKDF2) and RFC 3394 (key unwrap), on a BearSSL subset in `firmware/wifi/bearssl/`
(MIT; SHA-1, HMAC, AES decryption; upstream commit and changes in its README.md):
- the PMK as a job that `wpa_pmk_step(job, n)` advances n iterations at a time: PBKDF2-HMAC-SHA1(passphrase, SSID,
  4096, 32) is 8192 iterations of two SHA-1 compressions each. The job keeps the HMAC key state, not the passphrase,
  and is wiped when the key is taken or the job dropped. Passphrases outside 8-63 printable ASCII characters and
  SSIDs outside 1-32 bytes are refused (J.4.1);
- `wpa_prf_sha1` (PRF-n) and `wpa_ptk` (PRF-384 over Min/Max of the addresses and nonces, KCK/KEK/TK);
- `wpa_hmac_sha1`, for the EAPOL-Key MIC later;
- `wpa_aes_unwrap` (RFC 3394 2.2.2, KEK 16/24/32 bytes), which compares every byte of the integrity value and zeroes
  its output when the check fails.

`tests/wifi_crypto.c` (in `tests/run_host.sh`, 69 checks):
- vectors: the three J.4.2 PSKs, RFC 6070's 4096-iteration case, the three J.3 PRF-512 cases, RFC 3394 4.1-4.6,
  SHA-1 "abc" and HMAC RFC 2202 test 2;
- the PMK in steps of 64, 1, 7, 4095, 4096, 4097, 8192 and 100000 iterations, and a PTK computed with Python's
  hashlib from either side;
- the refusals: every changed byte of a wrapped key, integrity values one bit off in the first or last byte, a
  wrong KEK, a valid one-block wrap (RFC 3394 wants n >= 2) and a 15-byte KEK.

The 802.11 vectors are as wpa_supplicant 2.9's `crypto_module_tests.c` gives them; each was recomputed with Python
before it went in. The IEEE text itself was not read [unchecked]. The test also passes under ASan/UBSan. 19
deliberate bugs were planted in `wpa_crypto.c`. The test missed two at first: an integrity check that skipped one
byte, and a wrap of one block (n = 1) accepted. Both now have a check (vectors wrapped with Python), and the test
catches all 19.

**Size.** JieLi clang `-Os`: BearSSL subset 2,932 B, `wpa_crypto.c` 1,177 B, together 4,109 B of code and
read-only data, no static RAM. wpa_supplicant 2.9's SHA-1/HMAC/AES for the same job is about 8,914 B
(firmware/wifi/bearssl/README.md has the breakdown). Stack: a step uses about 0.7 KB [unchecked on the device].
`tools/build.py` now compiles `firmware/wifi/` on every build and prints its size, without linking it, so the
image is unchanged. Its register-access check also covers `firmware/wifi/` now; the vendored BearSSL is exempt
from the address-literal rule only, because AES's round constant 0x02000000 looks like an XIP address.

**Decided** (chosen by the colony; xero can change it):
- BearSSL, not wpa_supplicant's `src/crypto`: it is about a third of the size. Our own PBKDF2/PRF/unwrap instead of
  wpa_supplicant's: those run the 4096 iterations in one call and recompute the HMAC pads on each.
- BearSSL's `aes_small` (smallest, not constant-time) for the key unwrap. It only ever decrypts the GTK under the
  KEK, and CCMP runs in the MAC's hardware; `aes_ct` is the swap if that matters.
- `WPA_PMK_STEP` 64 iterations per main-loop pass is a guess [unchecked]: stage 8 times a step on the FM-1 and sets it
  so a pass stays well under a few milliseconds (the audio runs in its ISR either way). The caller passes the count,
  so stage 9 can change it without touching this code.
- The unwrap takes any KEK size BearSSL supports (16/24/32), so all six RFC 3394 vectors run; WPA2 uses 16.

**Built: the EAPOL-Key handshakes.** `firmware/wifi/wpa_eapol.c`/`.h`, the station (supplicant) side of the 4-way
and group key handshakes for WPA2-PSK with CCMP (AKM 2, key descriptor version 2: HMAC-SHA1-128 MIC, AES key wrap),
own code from IEEE 802.11-2020 12.7.2, 12.7.6 and 12.7.7 on `wpa_crypto.c`, no malloc. Not on the WiFi page and not
run on an FM-1.
- `wpa_eapol_init` takes the PMK, both MACs, the RSNE the station sent in its association request and the one from
  the beacon / probe response, and a nonce callback (so the tests are deterministic). `wpa_eapol_rx` takes one EAPOL
  frame (the 802.1X header onward, as it comes out of LLC/SNAP with ethertype 0x888E), writes the reply (msg 2,
  msg 4 or group msg 2) into the caller's buffer, and leaves a state (waiting for msg 1, waiting for msg 3, done,
  failed) and a reason.
- Checks: descriptor type and version, the key information bits of each message, the replay counter (above every
  one taken; it moves only on a valid MIC), the ANonce of msg 3 against msg 1's, msg 3's key length (16), the MIC on
  msg 3 and group msg 1, encrypted key data a multiple of 8 and at least 24 bytes, the RSNE in msg 3 byte for byte
  against the beacon's (no second RSNE), and the GTK KDE in the unwrapped key data (padding, other KDEs skipped).
  A frame that fails a check before the MIC is dropped and the state kept; a frame with a valid MIC that fails after
  it (RSNE, unwrap, no GTK) ends in FAILED with every key wiped, for the caller to deauthenticate.
- The PTK from msg 1 is held aside until msg 3 verifies under it. A repeated msg 3 or group msg 1 is answered but
  does not flag the same key for reloading, so reloading cannot reset the MAC's receive counters.
- Output for a later stage to load into the MAC's key tables (nothing here writes a register): `tk` (CCMP),
  `gtk`/`gtk_len`/`gtk_idx`/`gtk_tx` and `gtk_rsc`, with `new_keys` saying which changed on that frame.

`tests/wifi_eapol.c` (in `tests/run_host.sh`, 668 checks):
- the 4-way handshake recorded in Wireshark's sample capture `wpa-Induction.pcap` (SSID "Coherer", passphrase
  "Induction"): its msg 1 and msg 3 go in with the capture's SNonce, and the msg 2 and msg 4 that come out are the
  recorded ones byte for byte except the key length field (the recorded station sent 16; this code sends 0, which is
  what 12.7.6 asks for), with the MIC recomputed for that field. The TK is the one that decrypts the capture's CCMP
  data frame 99 (checked once with Python's `cryptography`, not in the test); the GTK (TKIP, 32 bytes, key index 2)
  is the KDE in msg 3. The frames' bytes are in the test, so it needs no network;
- a made-up AP in the test (AES-128 encryption and RFC 3394 key wrap of its own, checked against FIPS-197 C.1 and
  RFC 3394 4.1): 4-way handshake, group key handshake, a repeated msg 3 and group msg 1, a PTK rekey; the PTK and the
  group msg 2 compared with values computed with Python's `hmac`/`hashlib`;
- the refusals: bad MIC, replayed counters (msg 1, msg 3, group msg 1), descriptor type 254, key descriptor versions
  0-7 other than 2, wrong key information bits, the wrong ANonce, key length 32, RSNEs that differ from the
  beacon's (capabilities, group cipher, length, missing, a second one), every truncation of msg 1, key data lengths
  0-40 that are not multiples of 8 or under 24 and one of 264, a KEK that does not unwrap, no GTK KDE, two GTK KDEs,
  a 4-byte GTK, a KDE longer than the key data, a nonce source that fails, a reply buffer one byte short, and every
  single-bit change (bits 0 and 7) to msg 3 and group msg 1.

No PTK or EAPOL-Key MIC vector for AKM 2 could be sourced from IEEE 802.11-2020 Annex J: the Annex J vectors
wpa_supplicant cites are J.3/J.4 (already in `tests/wifi_crypto.c`), J.10 (SAE), J.12 (PASN) and J.13 (KDK), the
last three SHA-256 based; the IEEE text was not read [unchecked]. The recorded handshake stands in for one. The test
also passes under ASan/UBSan. 24 deliberate bugs were planted in `wpa_eapol.c`; it catches the 23 that change
behaviour (the 24th, `>` to `>=` in the counter compare, cannot: that compare only runs on bytes that differ). It
missed three at first (any KDE type taken as the GTK, two GTK KDEs, the TK not wiped on a failure after the
handshake); each has a check now.

**Size.** JieLi clang `-Os`: `wpa_eapol.c` 1,870 B of code and read-only data, no static RAM (about 6 KB with
`wpa_crypto.c` and the BearSSL subset); the context (`struct wpa_eapol`) is 424 B with 32-bit pointers, in the
caller's RAM. Stack: `wpa_eapol_rx` about 0.9 KB at its deepest (its own 324-byte frame, then the PRF's or the MIC's
HMAC contexts, about 0.4 KB) [unchecked on the device]. It uses only `memcpy` and `memset` from the C library.

**Built: the join (stage 8 on the page, docs/WIFI_STAGE8.md).** xero (2026-10-27, after the first FM-1 run found
only WPA2 networks): "i dont get why we cant just get wpa working". `firmware/wifi/fm1wifi_wpa.c` joins a WPA2-PSK
network: our RSN element in the association request, the handshakes over EAPOL data frames, the keys into the MAC's
tables (`fm1wifi_key_*` in `fm1wifi.c`, JieLi's layout: the AP at WCID 1, the GTK on WCID 0), group msg 2 encrypted by
the MAC. The network comes over USB (SysEx 0x64, `tools/fm1_wifi_report.py --ssid NAME --psk-from-nm CONNECTION`),
in RAM only, with its PMK cached beside it; the passphrase is never drawn, reported or written to flash. The page's
steps are RADIO, SURVEY, LINK, PASSIVE, ACTIVE, JOIN, 4-WAY, GROUP KEY, STAY 2S, REJOIN, and page 11 (new) has the
PMK time, the EAPOL frames seen, the key tables read back and what the MAC decrypted; the report adds the EAPOL frames
and 39 counters. Host-tested (`tests/wifi_wpa.c` 73 checks, eight key-table mutants caught; the simulator against a
WPA2 AP in the register model; the tool's `--psk-file` / `--psk-from-nm`). **Not run on an FM-1 yet.**

**Open.** The FM-1 run (`python3 tools/fm1_wifi_report.py --ssid YOURNET --psk-from-nm <connection> --run --out
DIR`): whether the key tables read back, whether the MAC encrypts with WIV 0 and counts the PN itself, whether its
RXD bits are rt2800usb's, the PMK time that sets `WPA_PMK_STEP`. The PMK cache is in RAM (the settings sector is stage
9's); no PMF, WPA3 or TKIP.

### 7e. Stage 2, first contact, as a standalone library — built, run on xero's FM-1

xero (2026-10-25): "i'd love if we could do wifi firmware updates and wifi song exports", and the WiFi support
should be generic enough to share by itself: a friend's webcam / video-out firmware on the same FM-1 (GPL too)
could stream over it instead of USB.

**Built.**
- `firmware/wifi/` is the **fm1wifi** library (its `README.md`: API, linking, licence note). `fm1wifi.c`: the
  core's power-up (JieLi's `wf_clk_rst_init` and `wdma_init` steps as a table of facts, docs/WIFI.md "Stage 2
  bring-up") and power-down (every register it changed written back), the MAC registers through the core's window
  at 0x30000 (refused before power-up, and for the frame FIFOs until stages 4-5), the BBP through `BBP_CSR_CFG` with
  each wait bounded at 1000 reads 1 us apart, a diagnostic snapshot (`MAC_CSR0`, `MAC_SYS_CTRL`, BBP 0; NO ANSWER when
  `MAC_CSR0` reads 0 or all ones), and the RAM pattern check. The stage 1 register header moved in as
  `fm1wifi_regs.h`; the stage 1 HAL's accessors are in `fm1wifi.c`.
- `fm1wifi_port.h`, the port: register access to the WiFi blocks, the WiFi bits of the shared `CLK_CON1`, a delay.
  `firmware/hal/fm1wifi_port_bsp.c` implements it on felucca-bsp with nothing of OPLBOX's in it.
- The **WiFi test page** in `oplbox.c` (README "WiFi test page"): SAVE outside EDIT opens it, PLAY there stops the
  sound, fills the free pool (end of the `.pool` buffers to the end of the pool region, 86,320 B; `app.ld` gains
  `_pool_top`), powers the core up and shows RESULT, `MAC_CSR0`, `MAC_SYS_CTRL`, BBP 0 and the RAM check (rechecked
  every 500 ms); SAVE powers it down and gives the sound back. SAVE outside EDIT did nothing before.
- Tests (`tests/run_host.sh`): the library compiles with only its own directory on the include path, and a check
  fails if any of its files includes something from outside it. `tests/wifi_model.c` runs it against the register
  model (`tests/wifi/sdio_model.c`, rewritten as the library's host port: a register file of CPU addresses, a
  baseband behind `BBP_CSR_CFG`, every access logged as `rd32`/`wr32`/`clk`/`delay` records) and diffs power-up
  against `tests/wifi/bringup_expect.txt` and the first reads against `diag_expect.txt`, both written by hand from
  docs/WIFI.md. It also checks: power-down restores all twelve registers and the clock bits; a stuck BUSY ends in
  TIMEOUT after exactly 1000 polls, before and after the request; a silent core (0 or all ones) is NO ANSWER; no
  access ever leaves the WiFi blocks; the RAM check counts changed bytes and the first address; the fixture format
  round-trips and refuses 19 malformed inputs. `tests/script_wifi.txt` drives the page in the simulator against the
  made-up core, a stuck baseband and a silent core; `run_host.sh` checks the page's text in each.
  `tools/build.py` links `fm1wifi.c` and the port, compiles every file of `firmware/wifi/` alone, and its register
  check keeps all access out of `firmware/wifi/` (the register map's addresses are exempt from the literal rule).
- Size: image +2,996 B (354,580 of 581,564), `.data`+`.bss` +48 B; pool and RAM code unchanged.

**Found.**
- The core is not enumerated over SDIO, and the brief's "real CMD52/CMD53 on the internal SDIO host" does not exist:
  nothing in the SDK calls `wl30_sdio_enum`, its `sdio_host_send_command` is `cpu.a`'s driver for the external
  SD controllers `JL_SD0`/`JL_SD1` on GPIO pins, and JieLi's CMD52 (`wl30_mmc_io_rw_direct`) only asserts. A MAC
  register at function 1 address A is the word at CPU address 0x30000 + A; only the two frame FIFOs go through the
  WL30 block's DMA. So 7b's enumeration (`fm1_wifi_sdio_enum`), its card model and fixtures, and the header's SDIO
  command and CCCR constants are gone; docs/WIFI.md "Internal SDIO enumeration" has the correction.
- `CLK_CON1` (0x10010) bits 15-14 are WiFi's; the same register holds the USB (1-0) and UART (11-10) clocks, so the
  port changes only those two bits, with interrupts off.

**Decided** (chosen by the colony; xero can change it):
- The library is `firmware/wifi/` itself, not a subdirectory: it is where docs/WIFI.md's plan already put the driver,
  and the WPA2 primitives there have no OPLBOX dependency either, so the whole library is one directory to copy.
- The page needs two presses (SAVE, then PLAY) so that a stray SAVE never powers the radio, and runs its own loop
  (the app is held while it is open; `platform_poll()` keeps USB and the updater alive).
- Power-down restores the registers' values from before power-up instead of writing zeros, so the core goes back to
  however the boot left it.
- The licence stays as it is (GPL-3.0-only, like the rest of OPLBOX). Which GPL the shared library ends up under is
  xero's decision (docs/WIFI.md "Standalone library"); stage 6's GPL-2.0-or-later code basis makes it GPL in any case.

**Risk on the FM-1.** Only when the page is opened and PLAY pressed: if the core's window does not answer, the first
read could stall the bus and hang the FM-1 on the page until it is powered off. Nothing is written to flash; boot,
the boot guard and the OCT− USB rescue are untouched (no change to `platform.c`), so it still boots and can be
rescued.

**On the FM-1** (xero, 2026-10-26 02:20 UTC, main c1fc5e8 flashed, the page after PLAY): `RESULT: OK`, `MAC_CSR0`
00300100, `MAC_SYS_CTRL` 00000003, `BBP 0` 00, `RAM CHECK: 0 CHANGED`, no hang. Read:
- `MAC_CSR0` 00300100: chipset 0x0030 (WL30) revision 0x0100.
- `MAC_SYS_CTRL` 00000003: `RESET_CSR` and `RESET_BBP` set, the MAC and baseband still held in reset, as expected:
  stage 2 does not run `wfhw_init`, whose first step releases them.
- `BBP 0` 00 with no TIMEOUT: the `BBP_CSR_CFG` handshake works; 0x00 is what rt2800 treats as "BBP not ready", which
  fits the baseband being in reset.
- `RAM CHECK: 0 CHANGED`: nothing wrote the free pool, but no DMA has run yet.

**Answered.** Whether the window answers before `wfhw_init`: yes (`MAC_CSR0` reads the chip id right after
`wf_clk_rst_init` and `wdma_init`).

**Still open.** What the bits power-up sets mean (`WF_CON0`, `WF_CON7`, the WL30 host block, `CLK_CON1` bits 15-14).
Which RAM the hardware writes: none so far, to be checked again once RX/TX DMA runs (stages 4-5).

### 7f. Stage 3, radio power-up and calibration — built, host-tested, waiting for hardware

**Built.**
- `tools/wifi_ir.py` (the rest of stage 1): turns the eight clean members of `wl_rf_common.a` into LLVM IR and runs it
  in a small interpreter against a register model, in `wfhw_init`'s order (docs/WIFI.md "Power-up and calibration
  sequence"), and writes the facts: `firmware/wifi/fm1wifi_tables.h` (15 straight-line tables of register steps plus
  3 small ones, the constant arrays, `wifi_conf.c`'s thresholds and RF RAM table; generated, committed) and
  `tests/wifi/radio_trace.txt.gz` (every write of the sequence against the model, 35,821 records). `--check` compares
  both with the SDK; `tests/run_host.sh` runs it when the SDK is there. No control flow, no IR text, nothing from the
  five members with lineage, `wl_wifi*.a` or the Bluetooth archives.
- `docs/WIFI_STAGE3.md`: the plain description of each step that is not a table (the AGC table, the PLL bank scan,
  the RX filter tune, the ADC/VGA search, the TX, RX DC and RX IQ trims, the PA configuration, RAPP, the RF RAM
  table, channel and TX power, the per-MCS gains), which the code is written from.
- In the fm1wifi library: `fm1wifi_radio_up` (with a calibration, `fm1wifi_cal_default` = fm1-base's), and
  `fm1wifi_set_channel`, `fm1wifi_set_power`, `fm1wifi_adc_peak`. Every hardware wait is bounded (1000 reads, then
  TIMEOUT and where it stopped). The trim and everything it calls run from RAM (`.ram_code.fm1wifi`, and the tables
  it reads), with interrupts masked by the port (`fm1wifi_port_trim`, new) and the sound off; `tools/build.py` fails
  the build if that section refers to anything outside RAM (tried: a constant table in it fails the build). Power-down
  also restores what stage 3 changed (`WLA_CON0`-`30`, `WL_ANL_CON0`-`3`).
- The WiFi test page now brings the radio up after stage 2's reads and has two pages (PLAY switches): page 1
  `MAC_SYS_CTRL` and BBP 0 after the resets are released, the PLL bank, the filter codes, the RAM check; page 2 the
  trim results and the ADC peak on channels 1-13 (README "WiFi test page", what good values look like).
- Tests: `tests/wifi_model.c` runs the bring-up against the register model and diffs its writes with the trace record
  for record (equal), checks the trims' results, that power-down puts all 80 CPU-side registers back, that the
  calibration bytes reach their registers, and that each of the four waits (filter tune, measurement, RF digital
  block, BBP BUSY) ends in TIMEOUT after 1000 polls. `tests/wifi_mutants.py` plants 8 bugs (two swapped writes, two
  dropped delays, four waits without their bound); the test catches all 8, the unbounded ones through the model's
  guard, which ends a run after 100,000 busy answers in a row instead of hanging. Under ASan/UBSan too.
- Size (against main 55f53ad): image +15,648 B (370,228 of 581,564), `.data`+`.bss` +200 B, RAM code +7,504 B
  (14,344 of 24,576), pool unchanged.

**Found.**
- No RSSI register in the clean members (per-packet RSSI is in `wl_hw_init`'s RX hook, from the RX descriptor): the
  page shows `rd_adc_peak`'s ADC peak per channel instead, a relative level. A calibrated RSSI comes with stage 4.
  Update (2026-10-26, FM-1 run of main c4aa202): the ADC peak reads only in the trim's loopback (0 on every channel in
  the receive state, even at VGA gain 41), so the page now surveys each channel as JieLi's AP channel select does:
  the false CCA count (`RX_STA_CNT1`) and busy time (`CH_BUSY_STA`) over 200 ms (`fm1wifi_channel_survey`; page 2's
  CCA lines and page 4; docs/WIFI_STAGE3.md "Receive level per channel"). Untested on an FM-1.
- JieLi's trims carry truncations (a best value kept as a signed byte, the middle bank falling back to a VCO reading
  when the scan finds no edge); the library keeps them so the writes match, and docs/WIFI_STAGE3.md says where.
- The FM-1's flash code cannot call RAM code directly (beyond the 23-bit call range), so the library calls across
  through a pointer, and the rest of the bring-up sits in RAM with the trim (`.ram_code.fm1wifi_init`).

**Decided** (chosen by the colony; xero can change it):
- Always a fresh trim, never a stored one (fm1-base found a stored trim made WiFi deaf); the receive thresholds and RF
  RAM table are the SDK's compiled branch (fm1-base's default), not the "moderate" one.
- The tables are the straight-line segments from all eight clean members, not only the three docs/WIFI.md first named
  for tables (still register values only); docs/WIFI.md says so.
- Interrupts masked for the trim (about half a second): no handler may run from the flash then; USB goes quiet and
  the page waits 15 ms after stopping the sound so both output halves hold silence. The watchdog (8 s) is fed by the
  port's delay.
- The register model's answers are made up, so each search lands somewhere particular; the trace and the test use
  the same model in Python and in C.

**Risk on the FM-1.** Only when the page is opened and PLAY pressed. The radio's calibration draws current and runs
with interrupts off; if something reads the flash meanwhile, or a wait never ends past its bound, the FM-1 could
crash or hang on the page until it is powered off. Nothing is written to flash; boot, the boot guard and the OCT−
USB rescue are untouched.

**Waiting for xero.** Flash, open the page (SAVE, PLAY), read page 1 and page 2 (PLAY switches). Then: whether the
bring-up completes (`RESULT`), whether BBP 0 comes out of reset, where the PLL and the trims land, what the receiver
sees per channel, and whether the RAM check stays at 0.

### 7g. Stage 7 host part, IP over WiFi on lwIP — built, host-tested, not yet tried on hardware

**Built.**
- lwIP 2.1.3 (BSD-3-Clause) in `firmware/wifi/lwip/src/`, the release's files unchanged: `git archive` of tag
  `STABLE-2_1_3_RELEASE` (commit 6ca936f6) from an lwIP clone already on this machine (the Pico SDK's submodule), the
  16 `.c` files IPv4, ARP, DHCP, ICMP and UDP need and the 63 headers they include, with `COPYING`. Not the JieLi
  SDK's copy: JieLi changed 27 of its 37 core `.c` files. `SHA256SUMS` lists every vendored file and
  `tests/run_host.sh` checks it. `CREDITS.md`, `LICENSING.md` and the directory's README.md say where it is from.
- `firmware/wifi/lwip/port/lwipopts.h` (`NO_SYS=1`, raw API, no TCP or IPv6) and `arch/cc.h`, ours.
- `firmware/wifi/fm1wifi_net.c`/`.h`: the network interface. In: data and QoS data frames (QoS control, HT control
  with the Order bit) with FromDS, addr1 = our MAC or a group address, addr2 = the BSSID, addr3 = the source, RFC 1042
  SNAP; everything else dropped (other MACs or BSSIDs, ToDS, protected frames, fragments, A-MSDUs, our own
  broadcasts coming back, other LLC headers). Out: ToDS data frames, addr1 = BSSID, addr2 = our MAC, addr3 = the
  destination, written in front of lwIP's pbuf without a copy (a chained one is copied into lwIP's heap). EAPOL
  (0x888E) goes to a callback instead of lwIP, and `fm1wifi_net_eapol_send` sends the handshake's replies, for the
  WPA2 code (7d's next step). DHCP start and stop, `fm1wifi_net_poll` for the timers, state, address and counters.
- `sys_now()` (milliseconds) joins `firmware/wifi/fm1wifi_port.h`; the host test has its own clock.
- `tests/run_host.sh`'s include rule for `firmware/wifi/` now admits lwIP's header names (`lwip/`, `lwip/priv/`,
  `lwip/prot/`, `netif/`, `arch/cc.h`, `lwipopts.h`) and nothing else; `tools/build.py` compiles `fm1wifi_net.c`
  with lwIP's include paths and exempts `wifi/lwip/src/` from the address-literal rule only (lwIP's TCP MSS
  constant 0x02040000 looks like an XIP address), as for BearSSL.

`tests/wifi_net.c` (508 checks; also clean under ASan/UBSan) plays the AP, a DHCP server and two pinging hosts as
802.11 data frames: DISCOVER retried after 2 and 4 s while the server is silent, OFFER (broadcast), REQUEST with
options 50 and 54, ACK (unicast) gives 192.168.4.23/24 and the gateway, a gratuitous ARP; ARP requests for our
address answered, others not; ICMP echo answered in plain, QoS and QoS+HT control frames, 56 and 1,400 bytes
(chained), and from a host lwIP has to ARP for first, with the IP and ICMP checksums and every 802.11 header field
checked; a bad ICMP checksum not answered; 15 kinds of frame dropped; EAPOL to the callback and not to lwIP, and
out; a send hook that refuses; DHCP stop sends a RELEASE (after ARPing for the server); down, and a restart on
another BSSID; 300 pings answered in a row (the pools do not run dry). 15 deliberate bugs planted by hand in
`fm1wifi_net.c`: 14 caught; the one missed restores the Ethernet header in a pbuf after sending, which lwIP never
reads again.

**Size** (JieLi clang `-Os`, objects not linked): RAM 6,839 B, all static (lwIP's heap `MEM_SIZE` 4,096, the receive
pool 4 x 512 B, ARP table 4 entries; `lwipopts.h` and `firmware/wifi/lwip/README.md` have the breakdown); code and
read-only data 14,959 B without `--gc-sections` (lwIP 13,411, `fm1wifi_net.c` 1,548). Not added to the firmware
build: lwIP's files need `string.h`, and the C library's `memcmp` (plus `memmove`, `strlen`, `strncmp`, `atoi`
unless `--gc-sections` drops their users), which `firmware/src/libc.c` does not have yet.

**Decided** (chosen by the colony; xero can change it): upstream lwIP rather than the SDK's; no TCP yet (UDP is
there; TCP is the files `tcp*.c` from the same tag when song transfer needs it); no IP fragmentation or reassembly;
`DHCP_DOES_ARP_CHECK` off (no ARP probe of the offered address); protected frames dropped, so the driver must hand
frames already decrypted by the MAC with the Protected bit cleared; the TX sequence number counted here, which the
hardware may replace.

**Open.** Untested on an FM-1 (it needs received frames: stages 4-6), not linked into OPLBOX and not on the WiFi page
yet. Stack depth in `fm1wifi_net_input` is not measured [unchecked]. The FM-1 test (an address from the hotspot,
ping answered) is stage 7's hardware part.

### 7h. Stages 4-6, frames, scans and joining an open network — stages 4-5 run on xero's FM-1, stage 6 waiting

xero (2026-10-26): "can you get a few steps going here? i'm tired of doing this one by one". Stages 4, 5 and 6 are
built together so one FM-1 run tests all of them (docs/WIFI_STAGE4.md).

- **Built.** `fm1wifi.c`: the MAC's 22 defaults and the address, the RX FIFO read and the TX FIFO write through the
  WL30 host block's DMA (registers, bits and limits read from `wl_sdio`/`wl_usb_to_sdio` as facts), bounded waits,
  a per-channel scan driver and the station's driver. `fm1wifi_frame.c` (new, pure): the RX descriptor and
  beacon/probe-response parser, the scan list, the TX image, probe request / authentication / association /
  deauthentication frames, and the station's state machine (timeouts, beacon loss, deauthentication, leaving,
  rejoining). The WiFi page runs the link, a passive and an active scan, joins the open network OPLBOX, stays 2 s,
  leaves and rejoins; pages 5-10 show each step, the scan list, the first four frames' bytes, the core's counters and
  the join. The address comes from the flash part's unique ID (no settings sector needed). The crash record names
  the new steps (`WIFI AT RX READ`, `TX SEND`, ...).
- **Tests.** `tests/wifi_frame.c` (86 checks: parser, scan list, TX image, frames byte for byte, station, and the
  first four frames from the FM-1, 36 bytes each) and
  `tests/wifi_link.c` (92 checks: the DMA register by register, passive and active scans against scripted access
  points in the register model, a join and what ends it, every wait bounded, the crash record during a DMA). The
  page runs on the host against the made-up core, an RX DMA that never finishes and a refused association.
- **On the FM-1** (xero, 2026-10-27, main 82f7fa6 flashed 02:20 UTC, no open OPLBOX network up; read off photos of
  the ten pages, so a digit may be misread): page 5 `RADIO OK`, `SURVEY OK 13 OF 13 CH`, `LINK OK`,
  `PASSIVE OK 12 NETWORKS`, `ACTIVE OK 12 ANSWERS`, `JOIN FAIL NO OPLBOX`, `STAY 2S` and `REJOIN NOT REACHED`;
  page 6 15 networks (the seven on the screen all WPA2, channels 1 and 6, RSSI -57 to -88, four probed); page 9
  `WL30 04 00000037>00010037`, `TXRXQ 00000000>00000000`, `FRAMES 34 BAD 0 TO ME 12`, `CRC ERR 44 PHY ERR 21`,
  `DUP 0 OVERFLOW 0`, `SENT 26 ERR 0`, `TX OK 26 FAIL 0 RETRY 0`, `MAC CE:62:BC:AE:4D:05 UID`. So frames reach the
  FIFO with no IRQ and no 0x0328, the RXWI's fields the parser reads are rt2800's, the TX gate opens, probe requests
  go out at CCK 1 Mbit/s and APs answer them. The four raw frames (pages 7-8) and what they show are in
  docs/WIFI_STAGE4.md "The first run on the FM-1": N is the RXWI plus the MPDU rounded up to 4 bytes, and group-addressed
  data frames of other networks pass `RX_FILTER_CFG` (the library drops them).
- **Untested on hardware**: the join (stage 6: authentication, association, beacon loss, leaving, rejoining) and any
  unicast frame we send that wants an acknowledgement. Open: 0x30F04 bit 16 (set after the run), the RXWI's other
  bytes, what RSSI0 is against another receiver, whether word 1 bits 15-4 are the sequence number.
- **Results over USB** (xero, 2026-10-27, after photographing ten pages: "you should be able to build something to dump
  this back"): SysEx 0x62 reads the page's report (every line of the ten pages, every network, the counters and up
  to twelve whole frames of 512 bytes at most), 0x63 starts a run as SAVE then PLAY would (refused in EDIT). `tools/fm1_wifi_report.py --run --out DIR` does both
  from the computer the FM-1 is plugged into and writes each frame as a file. Host-tested (the page's report text
  against the pages the simulator draws, the commands through `slot_reply`'s framing, the tool against
  `wifi_sysex.c` on a pipe, a run driven over USB in the simulator); not yet run on an FM-1. The pages stay as the
  fallback.
- **Stage 8 on the page** (7d, docs/WIFI_STAGE8.md): the run joins the WPA2 network set over USB, eleven pages now.
  Host-tested, not run on an FM-1 yet. **Not on the page yet**: stage 7 (IP over lwIP), host-built.

## 8. Bluetooth LE MIDI on code we can ship — runs on the FM-1 (Cordio and NimBLE, MIDI both ways with BlueZ); phones, WiFi together and pairing open

xero (2026-10-25): "ideally bluetooth as well". The goal is BLE MIDI beside USB and TRS, on GPL-3.0-compatible code.

**Status (2026-10-28).** The controller is Cordio's LE link layer from source (Apache-2.0, `firmware/ble/cordio/`) on our
own radio driver and PAL, the host is Apache NimBLE (`firmware/ble/nimble/`), the BLE-MIDI service is ours. No JieLi
Bluetooth archive is linked (xero, 2026-10-27: no JieLi binary in the image). At e8811d7 every step of the Bluetooth
test run passed on xero's FM-1 with BlueZ on xlap as the central: the connection held, GATT resolved, a Note On went
each way (docs/BLUETOOTH.md "The runs after 6d8b497", docs/BLUETOOTH_CORDIO.md). The sections below up to Stage 2 are
the record as written at the time [history].

**Stage 0 done (2026-10-25).** `docs/BLUETOOTH.md`: licence provenance of the SDK's Bluetooth archives, the controller's HCI interface, what
the SDK's C sources show of its registers and RF path, what is shared with WiFi, coexistence, candidate host stacks
with licences, flash and RAM against the 238 KB, fm1-radio's notes 01-06 as prior art, and a plan in seven stages.
Documents only; nothing was flashed.

**Found.**
- `btstack.a` (JieLi's host) exports BlueKitchen BTstack's public API names and file names, and the controller's
  `hci_transport.h` declares BTstack's `hci_transport_t`. BTstack's licence adds a no-commercial-use clause, so it is
  not GPL-compatible. No BlueKitchen notice is in the SDK; the lineage comes from names only.
- `btctrler.a` (the controller, 107 members) names only micro-ecc (BSD-2-Clause); its Apache-2.0 label is not
  contradicted and not proven, as for `wl_rf_common.a`. It speaks standard HCI through a function-call transport.
- `crypto_toolbox_Osize.a` ships with GPL-2.0-or-later headers from CycloneCrypto Open (Oryx Embedded) in the SDK;
  fm1-radio links it for both radios. WIFI.md did not mention it.
- The SDK C sources define no Bluetooth baseband registers (`bsfr_base` 0x20000 has no block); only field names
  survive in the archive's debug information. The BLE controller needs `wf_rf_common_init`, which starts the WiFi
  core's clocks and the SDIO host's DMA, so the registers in `fm1_wifi_regs.h` matter to Bluetooth too.
- `wlc_main` and the coexistence modes are in `cpu.a` (`wlc.c`): a priority arbiter programmed from nine mode tables;
  the SDK header says modes 3 and 4 (WiFi first) do not take from BLE.
- NimBLE's host, peripheral-only and without pairing, compiles with JieLi's clang to 41 KB of code and 6 KB of static
  RAM (unlinked). fm1-radio measured JieLi's whole BLE stack at +124 KB code; with the 120 KB WiFi target that is
  above the 238 KB free.

**Decided** (chosen by the colony; xero can change it):
- Host: Apache NimBLE, peripheral-only, no pairing first. Our own minimal ATT/GATT host is the fallback if the port or
  the size is a problem. Not JieLi's `btstack.a`, not BTstack.
- Controller [superseded 2026-10-27]: was JieLi's `btctrler.a` for diagnostic builds only, never shipped, until the
  licence gate was answered. Now Cordio's link layer from source; nothing links `btctrler.a`, so no gate is pending.
- Bluetooth and WiFi run one at a time at first; simultaneous use is stage 6.
- The BLE-MIDI profile is our own code, written after reading the specification.

**Stage 1 done (host only, 2026-10-26).** `firmware/ble/hci.c`/`.h`: the HCI packets (command, event, ACL), H4
framing, and a host that resets the controller, sets both event masks, reads the buffer size (LE's own, or the shared
one when LE has none) and the address, sets the advertising parameters and payloads, advertises, takes LE Connection
Complete, sends L2CAP PDUs in fragments only as the controller frees buffers (Number Of Completed Packets), puts
incoming fragments back together, follows a connection update, and advertises again after either side disconnects.
`tests/ble/hci_model.c` is the fake controller (script, packet log, fixture diff); `tests/ble_hci.c` (108 checks)
diffs a whole session against `tests/ble/session_expect.txt`, runs it again with answers inside the send call, and
covers a refused command, a silent controller (2 s timeout), a Hardware Error, no command credit, a link lost with
buffers held, and the ACL drop rules; `tests/ble_mutants.py` makes it catch 17 deliberate bugs. 3 KB of code, no
static RAM, compiled by `./build.sh` but not linked. Whether JieLi's controller answers the same way was to be stage 4's
question [history]; JieLi's controller is no longer used.

**Stage 2 done (host only, 2026-10-26).** `firmware/ble/ble_midi.c`/`.h`: the BLE-MIDI profile, our own code from the
specification (MMA/AMEI RP-052, BLE-MIDI 1.0, which was read; midi.org's download is members-only, so the copy read
was the PDF in `github.com/stuffmatic/zephyr-ble-midi`). It has the service and characteristic UUIDs (checked against
the specification's strings), the characteristic's properties, the empty first read, and the advertising data and scan
response; an encoder that batches messages into packets of the size given, with 13-bit timestamps and their wrap,
running status where the specification allows it and a timestamp byte where it requires one, and SysEx split over
packets (no timestamp byte in a continuation, one before 0xF7); a decoder that rebuilds the times, drops and counts
what the rules forbid and carries on; and the bridge to the app's input: `ble_midi_get` has `midi_get()`'s contract
over a 64-message queue, and a whole SysEx goes to a sink. `tests/ble_midi.c` (419 checks: packets byte for byte,
round trips of random messages and SysEx at ten packet sizes, every dropped case, the bridge, 40,000 random packets;
also clean under ASan/UBSan) and `tests/ble_midi_mutants.py` (32 deliberate bugs, all caught) run in
`tests/run_host.sh`. 2,064 B of code, no static RAM; compiled by `./build.sh`, not linked. The specification has no
packets in hex, so the test's vectors are our reading of its rules; a misreading would pass every test. **Untested on
hardware and against any real central** (a Mac, an iPhone): that is stage 5. The earlier note that Apple asks for
11.25 ms is not in the specification, which says 15 ms or less; Apple's guidelines were not read.

**Since then.** Stage 3 (NimBLE, host-tested) and the Cordio path are done, and stages 4 and 5 ran on Cordio's
controller with BlueZ on xlap as the central (docs/BLUETOOTH.md "Cordio path as built", "The runs after 6d8b497").

**Next.** Stage 5 with xero's devices: a Mac, an iPhone, an Android phone (none is recorded as tried). Wire BLE MIDI into
the app's MIDI paths (today only the Bluetooth test run calls `ble_midi_get`). Stage 6, WiFi and Bluetooth together.
Stage 7, pairing, if a phone insists.

**Open.** Whether a Mac or an iPhone accepts the stage 2 packets and the connection interval. Whether an iPhone
connects without bonding. Coexistence index 8. The receive interrupt (everything runs from the main loop today).
No licence decision is pending for Bluetooth: nothing links JieLi's controller.

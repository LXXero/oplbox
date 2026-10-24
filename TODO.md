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
  (0.4.2). Its docstring still says "Build Dinghy … dinghy.bin … dinghy.fwsc".
- Docs still written for Dinghy: `docs/API.md` ("# Dinghy API", `dinghy.c`), `docs/MAKE-IT-YOURS.md`,
  `docs/RULES.md` (`dinghy.c`, `build/dinghy.dis`) and `LICENSING.md` (`firmware/src/dinghy.c`, "Dinghy is
  free software"). README.md refers to a `CREDITS.md` that is not in the tree.
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
  9 × 32 KiB (`SLOT_BASE`, `oplbox.c:197`). A Dinghy slot holds 16 KiB − 12 bytes; four of the 18 built-in
  songs are bigger than that (TRADINTR 28510 B, ECHOS 26260, DR_TALE 21014, GOLDVAL 20953). So OPLBOX would
  keep its own slots and change `FL_RANGE_OK`, which moved to `store.h` in 0.4.
- The MIDI clock in would give the arpeggiator (item 3) an external clock, and TRS MIDI IN would give live
  recording (item 2) a second input.

**Open questions.**
- Keep tracking upstream? If yes: add Dinghy as a remote (done: `upstream`, fetch-only), merge or cherry-pick 0.4's
  platform and HAL files, and decide whether to offer OPLBOX's platform changes (`FL_RANGE_OK` and the PR #4
  line) back upstream. If no: say in README.md that OPLBOX is based on Dinghy 0.3 plus PR #4 and is no
  longer synced.
- Which parts of 0.4 are wanted on their own if the whole release is not taken: the MIDI clock in, TRS MIDI
  IN, the test suite.
- If tracking: record in CHANGES.md which upstream commit each port came from.
- Either way: update the Dinghy-era docs above to name `oplbox.c`, and write or drop the `CREDITS.md`
  reference. **Done:** the docs name `oplbox.c`, and `CREDITS.md` is written.

## 2. The 0.5 items — xero said yes to all three

CHANGES.md line 34 (0.4): "Not yet: pulling/pushing .CMF files over USB (0.5), live recording while playing,
undo."

### 2a. Pull and push `.CMF` slot files over USB

**Today.** The slots are plain `.CMF` files behind a 16-byte header (magic "OPLS", length, CRC):
`slot_len()` :244, `slot_save()` :254, `slot_program()` :219, `slot_ptr()` :218. The host build writes
`build/host/slotN.cmf` (:207); the device has no way out. USB is MIDI only (`usb.c`; CDC and UAC are off).
`usb.c:sysex_byte()` recognises only two SysEx messages, the UBOOT key (F0 22 24 35 7D F7) and the M-UPGRADE
key (F0 22 24 35 7F F7). The updater's frames go to `sx_frame[640]` for `ota.c`. `midi_get()` gives the app
channel and realtime messages only, and `app_main()` throws them away (:724).

**Where.** A new SysEx command in `usb.c:sysex_byte()` (or a third key next to the two above), 7-bit packed
the way `ota.c` frames are. A handler in `oplbox.c` that reads a slot with `slot_ptr()`/`slot_len()` and
writes one with `macsop_parse()` (to check it) and `slot_program()`. A host command in `tools/`, next to
`fm1_install.py`, which already uses mido for SysEx.

**Open questions.**
- New command bytes or the M-UPGRADE framing? The command bytes must not collide with stock or Felucca
  SysEx.
- Push goes straight to flash (an erase stalls the sound, like SAVE, :700) or into RAM as the current song?
- Reject files that are bigger than a slot (32 KiB − 16) or that `macsop_parse()` refuses (more than 64
  patterns or instruments, `macsop.h`).
- Can the FM-1 Firmware Hub (web MIDI) do it too, or only the Python tool?
- Push into the slot that is playing: reload it (`load_song()`, :285) or refuse?
- USB-MIDI SysEx throughput for a 32 KiB slot. 0.4.2 fixed the FM-1 dropping off USB on SysEx, so this needs
  a test on hardware.

### 2b. Live recording while playing

**Today.** In EDIT the tracker screen follows the cursor, not the playback (:472). A key writes the note at
the cursor and steps (:823–834). `audition()` (:689) does nothing while `playing`, so a key pressed during
playback is written to the cell but is not heard. REC is the loop-mode toggle and is ignored while editing
(:794), so it is free in EDIT.

**Where.** `app_main()`'s pattern editor block (:805). While playing and recording, write at the player's
position (`pos_order`/`pos_row`, set in `tick_row()` :88) instead of at the cursor, and play the key through
the write queue (`opl_write()` :72) on a channel the song is not using at that moment.

**Open questions.**
- Rounding: a key pressed late in a row goes into that row, or into the next one if it is past half of it?
  The row is 55 / 110 / 220 ms at speed 1 / 2 / 3.
- Which column does it record into: the cursor's column, or the next free one?
- Key-up writes a key off (`==`) or leaves the note ringing?
- Record MIDI in too (needs `app_main()` to stop discarding `midi_get()`, :724)?
- REC as record arm in EDIT, with its LED (:896)?
- The player and the key share the column's channel: which one wins until the next row?
- The main loop writes cells while the ISR reads the same pattern. A cell is written whole (`macsop_cell.u`),
  which is safe for one cell; say so in a comment.

### 2c. Undo

**Today.** Every edit changes `song` in place: cells (`c->u = t.u`, :816–837), the order list (:847–863),
instruments (:876–885), speed and rhythm (:848–849). Nothing keeps the old value. `song` is about 186 KB in
the 336 KiB `.pool` (app.ld), next to `slot_buf` (32 KiB), so a second copy of the song does not fit. A log of
(address, old value) entries does, since a cell is one `uint32_t`.

**Where.** A small undo log in `oplbox.c` written by every edit site above. Each edit site already sets
`moved = 1`, so they are easy to find.

**Open questions.**
- Which button: ENV and LFO (`platform.c:21`) are not used anywhere in OPLBOX.
- How many steps, and does a key held while scrolling count as one step?
- Inserting or deleting an order position moves the whole list: log the list (198 bytes) instead of single
  entries.
- Does the log survive SAVE, a song change, a power-off? (A song change reloads `song` from flash, :770, so
  the simplest answer is no.)
- Redo?

## 3. A working arpeggiator, with the stock FM-1's settings

**The stock arpeggiator** (M-VAVE FM-1 user manual, ARP section and the ARP mode knob table):
- Mode: Up, Down, Inclusive (low → high → low, endpoints played twice), Exclusive (endpoints once), Random,
  Order (the order the keys were pressed), Repeat (each note repeated before the next).
- Page 1 (SELECT turns pages): K1 Mode, K2 Octave 1–4, K3 Rate, K4 Tempo 30–300 BPM.
- Page 2: K1 Gate 20–100 %, K2 Swing 50–75 %, K3 Latch on/off.
- Rates for OPLBOX: 1/4, 1/8, 1/8T, 1/16, 1/32. The manual does not list the stock rate values; check them on
  stock firmware.
- ARP toggles the arpeggiator and opens its settings. With SEQ running, ARP shows "Disable SEQ to enter".

**Today.** There is no arpeggiator. `BTN_ARP` is used only while editing: on the song screen it makes a new
empty pattern (:860), on the instrument screen a new instrument copied from the current one (:884). On the
tracker screen (edit or not), the jukebox and the scope, ARP does nothing. Outside EDIT the keys do nothing
either, except on the scope, where the first nine white keys mute channels (:800).

**Where.** A new arp state and step function in `oplbox.c`. The note on/off goes through
`macsop_play_note()` (macsop.c), the same path as `audition()`. The timing: the ISR already counts native
samples for the row clock (`row_acc`, `tick_row()` :83–91). An arp clock beside it in `refill()` (:142) keeps
time in the audio stream; a clock in `app_main()` from `fm1_ms` would jitter by the main loop's latency.

**What it conflicts with.**
- ARP on the song and instrument screens (new pattern / new instrument). Either the arp is not available
  there, or those two functions move to another button.
- The stock settings page uses SELECT and K1–K4. Outside EDIT, SELECT picks the song (:765); K1–K4 are free.
  In EDIT, K1–K4 and SELECT are the cursor and the cell values.
- On the scope screen, the first nine white keys are the channel mutes.
- Channels: while a song plays, the player uses all 9 channels (or 6 + drums). The arp needs one of them.
- "Disable SEQ to enter" does not apply: in OPLBOX, SEQ switches screens.

**Open questions.**
- Which channel does the arp play on while a song plays: a fixed one (9?), one the song leaves silent, or a
  muted one (`mute_mask`, :275)?
- Which instrument: the song's (`ed_instr`, :378) or a separate bank (item 5)?
- Tempo: its own 30–300 BPM as on stock firmware, follow the song's row rate, or follow a MIDI clock (needs
  Dinghy 0.4's clock in, item 1)?
- Where do the settings live (pages on the tracker screen, a new view in the `V_*` enum at :661), and are
  they kept in the settings sector (`set`, :708)?
- Gate and swing finer than the arp clock's tick cost ISR time; the ISR has a tight budget
  (`ISR_BUDGET_US`, the overload guard at :189).

## 4. Save an arpeggio into the tracker as a track

A bassline generator and similar: the arp's output is written as ordinary note rows (note, instrument, volume,
pitch 32) in one column of a pattern, so the saved song is still a plain `.CMF` that FXDRIVER / SoundFX Macs
Opera plays. No new data in the file.

**Today.** The cell is `{note, instr, vol, pitch}` (`macsop.h`). A note sounds until a key off (note 4, `==`)
or the next note in its column. A pattern has 64 rows (row 64 only holds end marks). The format has no effect
column, no per-pattern tempo and no tempo command. The only timing is the song's speed 1–3: 18.2 / 9.1 / 4.55
rows per second (`SPR_Q16`, :61; the song screen's K2, :848). Up to 64 patterns and 99 order positions.

**Where.** The write goes through the same cell code as the pattern editor (:823–837): a loop that runs the arp
step function from item 3 offline and writes cells from the cursor down, then continues into new patterns
(:860) when it runs past row 63.

**Format limits.**
- Every arp step must be a whole number of rows. With N rows per quarter note, the tempo is
  60 × rows per second / N. At speed 1: N = 4 (row = 1/16) is 273 BPM; N = 8 (row = 1/32) is 136.5 BPM. At
  speed 2 the tempos halve, at speed 3 they halve again.
- 1/32 needs at least 8 rows a quarter. 1/8T needs N divisible by 3. All five rates at once need N = 24:
  45.5 BPM at speed 1 and slower at the others. In practice, the rate picks the tempo: 1/32 and the triplet
  rates only work at some song speeds, and a stock tempo (30–300 BPM) is kept only if it matches one of these
  values.
- Gate is a key off N × gate rows after the note, rounded to a row. A step of one row cannot have a gate
  shorter than 100 %.
- Swing is alternating long and short steps, so it needs at least 2 rows a step and rounds to whole rows.
- One column is one voice, and the arp is monophonic, so this fits. In rhythm mode only columns 1–6 are
  melodic.

**Open questions.**
- How long: one pattern, a number of bars, or until the key is released?
- Write into the cursor's column of the current pattern (overwriting what is there), or always into a new
  pattern?
- Should velocity from the keys go into the volume byte, or use `ed_vol`?
- Does SAVE then need a warning when the song's speed does not give the arp's tempo?
- Record live while the song plays (needs 2b), or render offline from a held chord and the arp settings?

## 5. Change the instrument while playing (with an arp running too)

**Stock firmware.** PRESETS scrolls through 128 presets and loads each one at once. ALGORITHM picks one of 32
6-operator algorithms; holding OCT− / OCT+ and turning ALGORITHM transposes −12…+12 semitones.

**OPL2.** Two operators a channel, no 4-op algorithms. The per-channel equivalents are the connection bit
(FM: operator 1 modulates operator 2; or additive) and feedback 0–7, both in register 0xC0 + channel, both
already fields in `macsop_instr` (`feedback`, `connection`) and rows 25–26 of the instrument screen
(`ins_field()` :597).

**Options for ALGO.**
- Connection and feedback as one list: FM with feedback 0…7, then additive with feedback 0…7 (16 steps).
- Modulator TL (operator 1 `tl`, 0–63): the brightness of an FM sound, the most audible single parameter.
- Others: operator 1 MULT, the waveforms (`wave` 0–3), the release rates.

**Today.**
- Outside EDIT, PRESETS and ALGO do nothing (`pre` and `alg` are read at :729 and used only in the editors).
- On the tracker screen in EDIT: ALGO is the volume of the cell and of new notes (:821), PRESETS is the step
  after a note, 0–16 (:811).
- On the instrument screen: PRESETS picks the instrument (:872), and a change heard at once is already there.
  `macsop_refresh_instr()` (:881) sends the new registers to every channel playing that instrument.

**How the play-mode mapping would differ from the editor's.** Outside EDIT: PRESETS = which instrument the
keys / the arp play, ALGO = the sound (one of the options above). In EDIT the two knobs keep their editor
meaning (volume, step), so the same knob does different things in the two modes. The other choice is to move
the editor to match play mode and give step and volume to other controls; K1–K4 are taken in the tracker
editor (column, row, transpose, instrument).

**Open questions.**
- Does turning ALGO while playing change the song's instrument (heard on every channel playing it, and saved
  with SAVE), or a copy only the keys / arp use, which a song reload drops?
- What the stock firmware's PRESETS / ALGORITHM do while its arp runs has not been checked on the device.
- PRESETS goes through the song's instruments (`n_instr`, up to 64, often under 10), or a fixed bank, e.g.
  every instrument of the 18 built-in songs?
- Keep stock's hold OCT−/OCT+ + ALGORITHM for transpose? Outside EDIT, OCT−/OCT+ are previous / next song
  (:756), so holding them would also change the song.
- Show the change on screen like stock (the preset list, the algorithm diagram): a line in the status area
  (`say()`, :385) or a new view?

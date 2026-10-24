# OPLBOX TODO

What is planned after 0.4.2, in order. Each item says what exists today, which code it touches, and what is
still open. Line numbers are for `firmware/src/oplbox.c` at 0.4.2 (a4b62d9).

## 1. Upstream: Dinghy (zvenson)

Upstream: https://github.com/zvenson/dinghy (default branch `main`).

**Today**

- This checkout has one remote, `origin` (`git@github.com:LXXero/oplbox.git`). There is no `upstream` remote
  here; xero set one up on another machine. Adding it here is a separate step (`git remote add upstream
  https://github.com/zvenson/dinghy`), not done in this change.
- The history shares no commit with Dinghy: the first commit, `6353ff6 Release 0.1`, adds the whole tree
  (68 files) at once. `git merge` with upstream has no merge base, so changes from Dinghy come over by hand
  (as the 0.4.1 fix did).
- Which Dinghy version 0.1 started from is not recorded anywhere in the tree.

**How far it has diverged**

- The app: Dinghy's `firmware/src/dinghy.c` became `firmware/src/oplbox.c` (a different program: the Macs
  Opera player, the tracker editor, the flash slots). Added beside it: `macsop.c`/`.h`, `dbopl2.c`/`.h`,
  `dbopl2_tables.h`, `opl2fast.c`, `opl3.c`/`.h`, `songs.h`, `host/`, `tools/gen_songs.py`.
- The platform (`platform.c`, `usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`,
  `uac_fir.h`, `firmware/hal/`, `firmware/loader/`, `tools/`) is still Dinghy's / Felucca's / SLOOP's code with
  OPLBOX changes:
  - `tools/build.py`: `PRODUCT = "FM-1_960"` (OPLBOX's 96N identity instead of Dinghy's 99N), the output names
    `oplbox.*`, and since 0.4.2 the OPL core compiled as its own object at -O2 while the app and platform stay
    at -Os like Dinghy.
  - `fm1_cstart()`: the screen's pins are set up before the OCT− rescue check, taken from dinghy PR #4
    (sgarrettnelson). README.md (Install) and CHANGES.md (0.4.1) cite that PR.
- Docs still written for Dinghy: `docs/API.md` ("# Dinghy API", examples refer to `dinghy.c`),
  `docs/MAKE-IT-YOURS.md`, `docs/RULES.md` (`dinghy.c`, `build/dinghy.dis`), the header of `tools/build.py`
  ("Build Dinghy … dinghy.bin … dinghy.fwsc"), and `LICENSING.md` (`firmware/src/dinghy.c`, "Dinghy is free
  software"). README.md refers to a `CREDITS.md` that is not in the tree.

**Open questions**

- Keep tracking upstream, or treat OPLBOX as a fork that only picks up platform fixes by hand? Dinghy had a
  major overhaul in 0.4; nobody has yet compared it with this tree. A first step is a diff of upstream's
  platform files (`platform.c`, `usb.c`, `ota.c`, `recovery.c`, `tools/build.py`) against ours, listing which
  upstream changes are fixes we want (USB, rescue, updater) and which only serve `dinghy.c`.
- If we keep tracking: add the `upstream` remote here (as on xero's other machine), and record in
  CHANGES.md which upstream commit each port came from.
- Update the Dinghy-era docs (API.md, RULES.md, LICENSING.md, build.py header) to name `oplbox.c`, and
  write or drop the `CREDITS.md` reference.

## 2. 0.5: the items CHANGES.md lists as "not yet"

CHANGES.md line 34 (0.4): "Not yet: pulling/pushing .CMF files over USB (0.5), live recording while playing,
undo." xero wants all three.

### 2a. Pull and push .CMF slot files over USB

**Today**

- Slots: nine 32 KiB slots at `SLOT_BASE 0x98000` (`oplbox.c:196–260`): a 16-byte header (magic `OPLS`, length,
  CRC32) and a plain `.CMF`. `slot_len()`, `slot_save()`, `slot_program()` (header written last, so a cut save
  stays empty). The host build writes `build/host/slotN.cmf`; the device has no way out.
- USB: `usb.c` assembles SysEx (`sysex_byte()`, line 675); a frame reaches the main loop through
  `ota_frame_get()` and replies go out with `ota_wire_send()` (line 818), both under `FELUCCA_OTA`. The
  updater's protocol (`ota.c`: `F0 pack7(00 59 cmd len24 body chk) F7`) and the UBOOT soft key
  (`F0 22 24 35 7D F7`) use this path. `oplbox.c:723–725` drains incoming MIDI and drops it.
- `tools/fm1_install.py` already talks SysEx to the device (`--info`, install).

**Work**: a SysEx command set for list / read slot / write slot (7-bit packed, chunked, CRC-checked), a
handler in `app_main()` that takes frames the updater does not claim, and a host tool (a new
`tools/fm1_slots.py`, or subcommands in `fm1_install.py`). Writing a slot erases flash, which stalls the sound
(see `settings_save` at line 914): do it with playback paused, like SAVE does (`save_song()`, `chip_off()`).

**Open questions**

- How to tell OPLBOX's frames from the updater's in `ota_frame_get()` without breaking installs (a different
  command byte in the same `00 59` framing, or our own manufacturer ID).
- Push into the playing slot: reload it (`load_song()`) or refuse?
- USB-MIDI SysEx throughput for a 32 KiB slot; 0.4.2 fixed USB dropping off on SysEx, so this needs a test on
  hardware.

### 2b. Live recording while playing

**Today**: keys enter notes only in EDIT on the tracker screen, at the cursor, then step by `ed_step`
(`oplbox.c:823–834`, `cursor_step()` line 685). While playing in EDIT the screen follows the cursor, not the
playback, and `audition()` (line 689) plays nothing while `playing` is set.

**Work**: a record state (REC is free while editing: `oplbox.c:794` uses it only when `!edit`) where a key
writes its note into the row the player is on (`pos_order`, `pos_row`, set by the ISR in `tick_row()`) on the
cursor's channel, and is heard at once.

**Open questions**

- Quantize to the current row or to the nearest row (a key just before the next row lands on it)?
- Key release: write a key off (`note 4`) at the row where the key goes up?
- The player's writes and the key's audition share one channel: which one wins until the next row.
- The cell writes come from the main loop while the ISR reads the same pattern; cells are written whole
  (`macsop_cell.u`), which is safe for one cell, but say so in a comment.

### 2c. Undo

**Today**: no undo; every edit writes `song` in place (`.pool`, 190 KB of the RAM pool for the song model).

**Work**: an undo log of (pattern, row, channel, old cell) entries for cell edits, plus whole-record entries for
order list and instrument edits; a key to undo (candidates: a button combination, since every button already
has a job in EDIT).

**Open questions**

- How much RAM is left after the pool for a log, and how many steps that buys.
- Which key or combination undoes; redo too?
- Undo across SAVE, or clear the log on save?

## 3. A working arpeggiator, with the stock firmware's modes

Wanted: all the settings the stock M-VAVE FM-1 arpeggiator has: rates 1/4, 1/8, 1/8T, 1/16, 1/32, note length
(gate), and the stock firmware's other arp settings.

**Today**

- No arpeggiator. `BTN_ARP` (`platform.c:22`) is read in two places, both only in EDIT:
  - song screen: a new, empty pattern at this position (`oplbox.c:860–864`);
  - instrument screen: a new instrument, a copy of this one (`oplbox.c:884–886`).
- In play mode (not EDIT) ARP does nothing; nor do `BTN_ENV`, `BTN_LFO`, K1–K4, ALGO and PRESETS
  (`alg` and `pre` are read at line 729 but used only by the editors).
- Notes from the keys only play in EDIT (tracker or instrument screen), through `audition()` →
  `macsop_play_note()`, which keys one note on one channel.

**Work**: an arp engine clocked from the row clock or its own timer, held-key list from `fm1_in.notes`, notes out
through `macsop_play_note()` on a channel the song does not use.

**Conflicts with ARP**

- If ARP toggles the arpeggiator everywhere, "new pattern" and "new instrument" need another key in the song
  and instrument screens. If the arp is play-mode only, ARP keeps its editor jobs and only means "arp on/off"
  outside EDIT; then the arp cannot run while editing.
- The keys: outside EDIT, the first nine white keys mute channels on the scope screen (`oplbox.c:800–802`), so the
  arp cannot take keys on that screen.

**Open questions**

- The stock arp's full list of settings is not confirmed: the M-VAVE manual's arpeggiator section has not been
  read yet (the user manual PDF circulates on Amazon's listing). Likely candidates to check against it:
  direction (up, down, up/down, random, as played), octave range, latch/hold.
- Which channel the arp plays on while a song plays: a song can use all 9 channels (or 6 + 5 drums in rhythm
  mode), so there may be none free.
- Clock: the song's row rate (18.2 / 9.1 / 4.55 rows/s) or a tempo of its own; follow incoming MIDI clock
  (today all MIDI in is drained and dropped, `oplbox.c:723–725`)?
- Gate below one row: the player keys notes per row; a gate shorter than a row needs a timer of its own in the
  ISR, which runs on a tight budget (`ISR_BUDGET_US`, the overload guard at line 189).

## 4. Save an arpeggio into the tracker as a track

Wanted: record the arp's output as ordinary note rows in a pattern (a bassline generator and similar), so the
saved song stays a plain `.CMF` that FXDRIVER / SoundFX Macs Opera play.

**Today**: cells are `{note, instr, vol, pitch}` (`macsop.h:27`); `note 4` is key off, `note 1` ends the
pattern. `macsop_write()` writes any song the model holds, and the editor already writes cells this way
(`oplbox.c:823–837`).

**Format limits**

- Time is rows only. The song has one speed for the whole song, 1–3 = 18.2 / 9.1 / 4.55 rows per second
  (`macsop.h:38`, `SPR_Q16` at `oplbox.c:61`). There is no effect column and no tempo change.
- So an arp rate is a whole number of rows. 1/16 = 1 row at speed 1 means a quarter note is 4 rows = 273 BPM.
  1/8T (a third of a quarter) and 1/32 (an eighth of a quarter) are whole rows together only if a quarter
  is a multiple of 24 rows (45.5 BPM at speed 1). In practice 1/32 and the triplet rates are tied to the song
  speed and to how many rows a quarter note gets.
- Note length (gate) is also whole rows: a key off cell after N rows. A gate shorter than one row cannot be
  written.
- 64 rows per pattern (`MACSOP_ROWS`), 64 patterns (`MACSOP_MAX_PATTERNS`), 99 order positions. A long arp
  spills over patterns.
- One cell per channel per row: the arp takes one channel of the pattern.

**Open questions**

- Write over an existing channel, or only into empty channels / a new pattern (`BTN_ARP`'s song-screen job
  already makes new patterns)?
- Record live while the song plays (needs 2b) or render offline from a held chord and the arp settings?
- Volume per step: cells carry `vol`, so accents could be written; does the stock arp have accents to copy?

## 5. Change the instrument while playing (and while the arp runs)

Wanted, as on the stock firmware: PRESETS scrolls through the instruments, ALGORITHM changes the sound.

**Today**

- In the tracker editor: ALGO is the volume of the cell and of new notes (`oplbox.c:821–822`), PRESETS is the
  step after a note, 0–16 (`oplbox.c:811`). In the instrument editor PRESETS picks the instrument
  (`oplbox.c:872`) and K1 changes the selected parameter, heard at once through `macsop_refresh_instr()`
  (`oplbox.c:881`). In play mode (not EDIT) neither knob does anything.
- OPL2 has no 4-operator algorithms. Per channel it has the connection bit (FM: op 1 modulates op 2; or
  additive: both heard) and feedback 0–7 on op 1. The instrument model already holds both
  (`macsop_instr.feedback`, `.connection`, `macsop.h:19`; params 25 and 26 in `ins_field()`, line 597).

**Options for ALGO** (play mode)

- Step through the 16 combinations of connection × feedback (FM fb 0–7, then ADD fb 0–7).
- Modulator TL (`op[0].tl`, 0–63): the brightness of an FM sound, the most audible single control.
- Others to consider: modulator MULT, the wave selects (`wave`, 0–3 on OPL2 when wave select is on).

**How the play-mode mapping differs from the editor's**

- Play mode: PRESETS = instrument (as in the instrument editor), ALGO = sound (connection/feedback or TL).
- Tracker editor: PRESETS = step, ALGO = volume. Keep that, or move the editor to match play mode and give step
  and volume to other knobs (K1–K4 are taken by cursor, row, transpose, instrument).

**Open questions**

- Which channel(s) the change applies to while a song plays: the arp's channel, the channel the keys play on,
  or every channel using that instrument (`macsop_refresh_instr()` already does the last).
- Is a play-mode change an edit of the song (saved with SAVE) or a temporary change that a song reload drops?
- The stock firmware's PRESETS / ALGORITHM behaviour while its arp runs has not been checked against the device
  or its manual.

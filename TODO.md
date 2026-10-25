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

## 7. WiFi on our own driver — stage 0 done (the map)

xero: "i would rather keep it gpl and just build our own wifi firmware". OPLBOX stays GPL-3.0-only and does not
link JieLi's binary WiFi archives.

**Done.** `docs/WIFI.md`: licence provenance of the archives, what has source and what is binary only, the
registers and the internal SDIO bus the radio uses, the power-up and calibration sequence, what fm1-radio links,
OPLBOX's size, audio and flash limits, and a plan in nine stages (one task each).

**Found.** The MAC driver in `wl_wifi*.a` is compiled from Ralink's GPL-2.0-or-later Linux driver plus Linux's
GPL-2.0 `wext-core.c`, so its Apache-2.0 label is doubtful; the WiFi core's MAC is register-compatible with Ralink's
RT2800 family (Linux `rt2800.h`, GPL-2.0-or-later); the radio and internal bus (`wl_rf_common.a`) show no third-party code,
and the archives are LLVM bitcode, readable as LLVM IR. JieLi's stack (about 330 KB of code) would not fit beside
OPLBOX (238 KB free) anyway.

**Next.** Stage 1 (register header and extracted tables, host only), then stage 2, the first hardware test: xero
flashes a diagnostic build and reads four register values and a RAM pattern check off the screen.

### 7a. Stage 1, register header — built, host-tested, not yet tried on hardware

**Built.** `firmware/hal/fm1_wifi_regs.h`: the CPU-side blocks (`JL_ANA` `WLA_CON0`-`39`, `JL_WL` with `WL_CON*`,
`WL_ANL_CON*`, `WF_CON*`, the internal SDIO host at 0x30F00-0x30F1C, IRQ 52) from the SDK's `WL82.h`/`hwi.h`; the
WiFi core's MAC registers in use with their `rt2800.h` names, addresses and masks (Linux v6.6); JieLi's own
0x0300-range registers and the frame data addresses; the four BBP indirect windows; the key tables at 0x4000,
0x6000, 0x6800, 0x6C00, 0x7000 with their entry sizes. Addresses and masks only, no register values.
`firmware/hal/fm1_wifi_hal.c`: CPU-side accessors (bounded index, read-only `WLA_CON31`-`39` refused) and
`fm1_wifi_cmd52`/`cmd53`/`mac_rd`/`mac_wr`, which return `FM1_WIFI_ENOTYET` until stage 2. `tests/run_host.sh`
compiles both warning-free (`-Werror`), runs `tests/wifi_regs.c` against a fake bus (each accessor hits its
address) and `tests/wifi_regs.py`, which checks every address and field docs/WIFI.md gives (79) against the header.
The JieLi compiler builds `fm1_wifi_hal.c` too; nothing links it yet, so the image is unchanged.

**Found.** All 82 rt2800-named constants in the header equal Linux v6.6 `rt2800.h` (sha256 `eb7ac9bf…`). All 22
`MACRegTable` addresses have an `rt2800.h` name (docs/WIFI_REVIEW.md had this UNCHECKED); the values were not compared.
The shared key table (16 × 32 B) ends at 0x6E00, not at the mode table's 0x7000.

**Decided** (chosen by the colony; xero can change it):
- WiFi-core addresses keep `rt2800.h`'s names unprefixed (`MAC_SYS_CTRL`), so stage 5-8 code reads like rt2x00;
  CPU-side ones carry `FM1_`. The SDIO host's registers and JieLi's own are named by address (`FM1_WL30_HOST_04`,
  `FM1_WF_REG_0308`): no source names them.
- No `MACRegTable` values in the header: stage 5 takes them from rt2800lib.c or Ralink's staging driver.
- `fm1_wifi_mac_rd/wr` assume little-endian on the SDIO bus until stage 2 reads `MAC_CSR0`.
- The HAL is not linked into the firmware until stage 2 calls it.

**Open.** What the SDIO host's registers do (stage 2). The clock-block register 0x10010 that `wf_clk_rst_init` writes
and `MAC_WCID_BASE` (0x1800, needed by stage 8) are not in the header yet. The 0x30F00 and 0x0300-range addresses
come from `wl_rf_common.a` as facts, not code; whether stages 1 and 3 may rest on that archive is task 3's to settle.
`tools/wifi_ir.py` and the extracted tables (the rest of stage 1) are not built.

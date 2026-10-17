# OPLBOX 0.4 (identity FM-1_964) — the editor

- **EDIT**: the tracker screen becomes an editor. K1/K2 cursor, SELECT order position, keys enter notes (and
  audition them when paused), K3 transpose, K4 instrument, ALGO volume, PRESETS step, FX key-off, SCL clear,
  GLO end-of-pattern, OCT-/+ keyboard octave, PLAY from the cursor, HOME to row 0. Edits are live.
- **Song screen** (SEQ while editing): the order list; K1 pattern at a position, GLO insert, SCL delete, ARP new
  pattern, K2 speed, K3 rhythm mode.
- **Instrument screen** (SEQ again): every parameter of an instrument on a list, K1 changes it (channels
  playing it update at once), name editing, ARP copies an instrument, keys audition on channel 1.
- **SAVE**: nine 32 KiB flash slots (0x98000-0xE0000). Slots show in the jukebox after the 18 built-in songs;
  an empty slot opens as a new blank song. Slots hold plain .CMF files.
- The song model: patterns as cells in RAM (190 KB of the pool), a lossless .CMF reader/writer: all 18 originals
  round-trip byte for byte; the register stream from the cell model equals the old player's on every song.
- Proof against the real thing: a song edited and saved here, played by Stone Age's FXDRIVER in DOSBox, gives
  the FM-1's register stream exactly (host/proof/: the .CMF and the DOSBox capture; host/rowcmp3.py).
- Register writes from the main loop (auditions, instrument edits) go through a queue the audio interrupt
  drains between blocks, so editing never races the chip.
- Not yet: pulling/pushing .CMF files over USB (0.5), live recording while playing, undo.

Audio path unchanged from 0.3.4 (dbopl core, mutes, scope).

# OPLBOX 0.3.4 (identity FM-1_963)

- Fix: mutes survived on screen but not in the chip after a restart (song end in LOOP SONG, HOME). Now kept.

# OPLBOX 0.3.3 (identity FM-1_963)

- Scope screen: the first nine white keys (F3-G4) mute / unmute channels 1-9. Red label, dim trace,
  key LED lit while muted; the channel keeps running so unmuting is seamless. Cleared on song change.
  (Mute is a mask in the mix, bit-exact with DOSBox when nothing is muted.)

# OPLBOX 0.3.2 (identity FM-1_963)

- Scope labels: instrument number first ("06 produc"), `--` for the driver's default patch.

# OPLBOX 0.3.1 (identity FM-1_963)

- **Scope screen** on FX: nine oscilloscopes, one per channel, labelled with the channel's instrument;
  rhythm songs tap bass drum / hi-hat + snare / tom + cymbal into boxes 7-9. Auto-ranged, zero-crossing
  triggered, 20 frames a second; the audio interrupt pays about 5 % more for the taps while it is up.

# OPLBOX 0.2 (identity FM-1_962)

0.1 on hardware: the audio interrupt ran over its budget every time (100 %+), half-speed stutter, and the
main loop — buttons, screen, USB, the updater — starved with it.

- OPL core: DOSBox `dbopl` (one OPL2, integers only, tables precomputed for 49716 Hz), sample loop and
  wave tables in RAM. Emulator: 9–13 % of the core (0.1: 62 % emulated, over 100 % real).
  Verified sample for sample against the unmodified DOSBox code over all 18 songs.
- Overload guard: a quarter second over budget → playback pauses itself, screen says
  `AUDIO OVERLOAD - PAUSED`, the device stays reachable over USB. PLAY resumes.
- MIDI input is drained: 0.1 never read the ring, so ~40 incoming notes / clocks made the USB endpoint NAK
  and locked SysEx (the installer handshake) out until a power-cycle. Found by Smalley.
- CPU readout: three digits (0.1 showed 103 % as "03%"), amber past 85 %, red over budget.
- Unity gain (0.1 was 1.5x and clipped the loud LINEL songs), clipping at the DAC instead of wrapping.
- Nuked-OPL3 kept as the cycle-exact build option (`-DOPL_CORE=OPL_NUKED`), for the second core later.

Install: `python3 fm1_install.py oplbox-0.2.fwsc` (needs `pip3 install mido python-rtmidi`), or the
FM-1 Firmware Hub with the local file. From a 0.1 that does not answer: README, "If it will not start".


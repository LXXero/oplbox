# Unreleased

- Bluetooth stage 1, host only (docs/BLUETOOTH.md): `firmware/ble/hci.c`, the host side of HCI for a BLE peripheral,
  our own code from the Bluetooth Core Specification. It packs and decodes command, event and ACL packets, reads H4
  framing from a byte stream, starts a controller (Reset, both event masks, the buffer size, the address, the
  advertising parameters and data, advertising on), takes one connection, moves L2CAP PDUs over ACL in fragments with
  the controller's flow control, and advertises again after a disconnection. `tests/ble_hci.c` runs it against a fake
  controller (`tests/ble/hci_model.c`) and diffs a whole session with a hand-written trace; 17 deliberate bugs are
  caught. 3 KB of code, no static RAM; compiled by `./build.sh`, not linked, untested on an FM-1 (no controller runs
  yet).

- IP over WiFi for stage 7, host part: lwIP 2.1.3 (BSD-3-Clause, the release's files unchanged, IPv4/ARP/DHCP/ICMP/UDP
  only, in `firmware/wifi/lwip/`) and `firmware/wifi/fm1wifi_net.c`, a network interface that turns 802.11 data frames
  into Ethernet frames for lwIP and back and hands EAPOL frames to the WPA2 code. `tests/wifi_net.c` plays an AP, a
  DHCP server and two pinging hosts as 802.11 frames: an address from DHCP (retried while the server is silent), ARP
  and ping answered, foreign frames dropped. 6.8 KB of RAM, about 15 KB of code; not linked into the firmware and
  not on the WiFi page yet, untested on an FM-1.

- WiFi page: the 13-channel scan now surveys each channel as JieLi's AP channel select does (`wl_wifi.a`:
  `APAutoSelectChannel`, `RT30xx_ChipSwitchChannel`, `UpdateChannelInfo`): the channel set with `TX_BAND_CFG` on
  2.4 GHz and `TX_PIN_CFG` = 0x00050302, 200 ms, then the false CCA count (`RX_STA_CNT1`) and the busy time
  (`CH_BUSY_STA`), in the new `fm1wifi_channel_survey`. Page 2 shows the false CCA counts in place of the ADC peaks,
  a new page 4 both counts per channel. The ADC peak stays on page 3: the FM-1 run of main c4aa202 showed it reads
  only in the trim's loopback (49 and 53 there, 0 in the receive state even at the highest VGA gain), so it could not
  measure the antenna (docs/WIFI_STAGE3.md "Receive level per channel"). Host-tested against made-up counts;
  untested on an FM-1.

- WiFi stage 3's first run on xero's FM-1 (2026-10-26, main 80a6917) crashed: CPU exception, PC 01C02674 in `rmw()`,
  right after a register write returned. No register-side cause was found (the library's writes equal JieLi's code's
  and stay inside the WiFi blocks); the likely cause is the supply: OPLBOX ran the WiFi load at the boot loader's VDC14
  and SYSVDD, at which felucca-bsp notes some units misread flash and SRAM. The WiFi page now raises the rails to
  M-VAVE's levels (VDC14 1.60 V, SYSVDD 1.38 V, the DC-DC, the WiFi 1.2 V regulator on automatic) before the core
  comes up. The library records the step it is in and its last register write in `.noinit` (`fm1wifi_where`), and the
  crash screen now also shows EMU_MSG, DBG_MSG, sp, the four ETM trace PCs, whether the RAM code still equals its flash
  copy, and the WiFi step and last write for a crash inside the bring-up (README "If it crashes", docs/WIFI_STAGE3.md
  "First run on an FM-1"). Host-tested; not run on an FM-1.

- WiFi stage 3, the radio's power-up and calibration, in the fm1wifi library: `fm1wifi_radio_up` resets the MAC and
  baseband, loads their tables, sets up the radio (crystal caps from a calibration), scans the PLL's banks, makes a
  fresh trim (RX filter, TX LO/DC/gain/phase, RX DC per gain range, RX gain/phase), configures the PA, loads the RF
  RAM table and sets channel 6 and TX power 6, in JieLi's `wfhw_init` order; every wait is bounded. The trim runs from
  RAM with interrupts masked, and the build fails if that RAM code refers to the flash. The register facts come from
  `tools/wifi_ir.py`, which runs the eight clean members of JieLi's `wl_rf_common.a` as LLVM IR against a register
  model and writes them as tables (`firmware/wifi/fm1wifi_tables.h`, generated) and as a reference trace; the steps
  with loops are written from the plain description in `docs/WIFI_STAGE3.md`. On the host the library's writes equal
  JieLi's code's, record for record (35,821), and 8 planted bugs are all caught. The **WiFi test page** brings the
  radio up after stage 2's reads and has a second page (PLAY): the PLL bank, the filter codes, the trim results and
  the receiver's ADC peak on channels 1-13 (README "WiFi test page"). Stage 2's values from xero's FM-1 are recorded
  in docs/WIFI.md and TODO.md 7e (`MAC_CSR0` 00300100). Image +15,648 B, RAM code +7,504 B. Not run on an FM-1.

- WiFi stage 2, as a library: `firmware/wifi/` is now **fm1wifi**, a WiFi driver any FM-1 firmware can link (its
  `README.md`): no OPLBOX code, headers or globals, everything board-specific through `fm1wifi_port.h`, implemented
  for felucca-bsp in `firmware/hal/fm1wifi_port_bsp.c` (usable as is by another firmware) and for the host tests by
  the register model. It powers the WiFi core up and down (JieLi's `wf_clk_rst_init` and `wdma_init` steps, as
  facts), reads its MAC registers through the core's memory window at 0x30000 and its baseband through
  `BBP_CSR_CFG` with bounded waits. Found on the way: the core is not enumerated over SDIO; the SD command
  enumeration and the stage 1 header's SDIO command constants are gone (docs/WIFI.md). The **WiFi test page**: SAVE
  outside EDIT, then PLAY, shows `MAC_CSR0`, `MAC_SYS_CTRL`, BBP register 0, the result and a RAM check of the free
  pool (README "WiFi test page"). The image grows by 2,996 B. Host-tested (`tests/wifi_model.c` against
  `tests/wifi/bringup_expect.txt` written by hand, `tests/script_wifi.txt`), not run on an FM-1.

- Arpeggiator SYNC: turning K4 (tempo) left past 30 BPM gives `SYNC`, the song's own tempo: one row is a 1/16, the
  steps are started by the row clock on the row's first sample while the song plays (no drift; 1/4 on the pattern's
  rows 0, 4, 8 …), a new chord waits for the grid's next step, swing and gate as before, and with the song stopped it
  free-runs at the song's speed; a speed change or a new song retimes it at once. ARP in EDIT then writes what the
  synced arp played, on its own rows (1/32 a row a step). Kept in the settings sector as 0 BPM: an older OPLBOX
  reads that as out of range and takes 120; older sectors still load. Host-tested (`tests/script_arp_sync.txt`,
  `tests/script_arp_sync_write.txt`), not yet on hardware.

- Platform: the chip and board layer is now felucca-bsp d6b8f77 (`firmware/bsp/`, MPL-2.0, vendored), in place of
  the Felucca copies in `firmware/hal/`, `firmware/loader/`, `usb.c`, `ota.c` and `libc.c`. The update can install
  M-VAVE's V16, the update loader no longer erases a song slot that looks like an update record, and it shows a
  progress screen. USB audio has a deeper ring (+5.8 ms). `tests/loader_test.c` is new. Not run on hardware (TODO.md 1).
- Change the instrument while playing: outside EDIT, PRESETS picks which of the song's instruments the keys and the arp play, and ALGO steps a copy of it through FM feedback 0–7 then additive feedback 0–7; the song keeps its own; the status line says which. The instrument screen's CONNECT now says FM for the file's 1 (it had FM and ADD swapped). Host-tested (`tests/script_live_instr.txt`), not yet on hardware.
- The arp's settings screen names the keys' instrument and latch: its last two lines show the channel, the notes held and `LATCH ON` / `LATCH OFF`, and the instrument PRESETS / ALGO picked (`I03 t.detert`, `I03 FM FB 5`), which stays after the status line's two seconds; it changes as the knobs turn. Latch was only on page 2, K3, with nothing on page 1 to say so. `tests/script_live_instr.txt` checks both lines.
- The arp screen's status line shows the song's title instead of repeating the instrument: there PRESETS / ALGO no longer flash `I07 …` for 2 s (the footer already names it), and the status area's second line shows the current song's title, clipped to 18 characters, in place of `LOOP SONG` / `ALL IN TURN`; it changes at once when the song does. Other views keep the message and the loop mode. `tests/script_live_instr.txt` checks it.
- OCT− / OCT+ change the keys' octave on every screen, not the song (SELECT still picks the song): `OCT0`–`OCT6` as EDIT shows it, flashed in the status line, and on the arp screen's last line. A playing arp, held or latched, moves with it from its next step (its notes are worked out again from the keys, so +3 then −3 lands back where it started, and a key let go still takes its own note out). The octave is kept in the settings sector (12 bytes now; a 10- or 2-byte sector from an older OPLBOX still loads, the octave at OCT3). `tests/script_octave.txt` checks it on the host, not yet on hardware.
- Write the arpeggio into the tracker: ARP in the pattern editor turns the arp's last chord and settings into notes and key offs in the cursor's column, 1–8 patterns (arp page 2, K4), going on in new patterns; steps round to whole rows and the status line gives the tempo that results; one ENV undoes it. Host-tested (`tests/script_arp_write.txt`), not yet on hardware.
- Pull and push song slots over USB: `tools/fm1_slot.py pull|push SLOT FILE`, SysEx commands 0x60/0x61 in the updater's framing (docs/API.md). A push is checked as a .CMF (`macsop_check`) and refused for the slot in the player or a file bigger than a slot. Host-tested, not yet on hardware.
- docs/BLUETOOTH.md: the map for BLE MIDI (stage 0): the SDK's Bluetooth archives and their licence provenance (JieLi's host is BTstack-shaped and not GPL-compatible), the controller's HCI interface and registers as far as the SDK shows them, WiFi coexistence, NimBLE measured at 41 KB of code, flash and RAM against the WiFi plan, and seven stages. Documents only.
- WPA2 key primitives for WiFi stage 8 (`firmware/wifi/wpa_crypto.c` on a 2.9 KB BearSSL subset): the PMK from the passphrase in main-loop steps, the 802.11 PRF and PTK, AES key unwrap; `tests/run_host.sh` checks them against IEEE 802.11 J.3/J.4 and RFC 3394. `tools/build.py` compiles `firmware/wifi/` and prints its size (4.1 KB, not linked). Host only, no EAPOL yet.
- docs/WIFI.md: `wl_rf_common.a` checked member by member (8 of 13 clean, 5 with Ralink/Linux header lineage) and what stages 1 and 3 may take from it (facts, not code); WEXT count 7, `MACRegTable` values traced to Ralink's driver, a symbol total and the GPL source wording corrected. Documents only.
- WiFi register model for stages 2-5: a host fake of the internal SDIO host (`tests/wifi/sdio_model.c`), a plain-text fixture format for command sequences and raw 802.11 frames, a diff helper, and the HAL's `fm1_wifi_sdio_enum()` replayed against docs/WIFI.md's `wl30_sdio_enum` sequence in `tests/run_host.sh`. Host only.
- WiFi stage 1, register header: `firmware/hal/fm1_wifi_regs.h` (SDK and Linux `rt2800.h` addresses and masks) and accessors in `fm1_wifi_hal.c`, checked against docs/WIFI.md by `tests/run_host.sh`. Not linked yet, not run on hardware.
- WiFi stage 1 guard: the internal SDIO host accessor now permits only the six documented offsets; the host test verifies the gaps at `0x08` and `0x0C` touch no register.
- Arpeggiator: ARP outside EDIT turns it on and opens its settings (LFO opens them / turns the page). Held keys
  feed it; modes up, down, inclusive, exclusive, random, order, repeat; 1–4 octaves; 1/4 1/8 1/8T 1/16 1/32;
  30–300 BPM; gate 20–100 %; swing 50–75 %; latch. Its clock runs in the audio ISR beside the row clock. While a
  song plays it takes a channel the pattern leaves silent, else a muted one, else the last melodic one, and the
  player skips that column (`macsop.skip`). Settings kept in the settings sector (10 bytes; an older sector still
  loads). `tests/script_arp.txt` checks it on the host (new script checks `is` and `within`). Not run on hardware.
- Scope screen, rhythm songs: boxes 7-9 are now bass drum / snare / hi-hat + tom + cymbal (before: bass drum /
  hi-hat + snare / tom + cymbal) and are labelled `BD`, `SD`, `HH TM CY`. Box 9 was dark in 17 of the 18 songs:
  no melodic song uses a ninth channel, and the tom and cymbal almost never sound (see the commit message).
  The mute keys follow the boxes. The mix is unchanged.
- Undo: ENV in EDIT takes back the last edit on the tracker, song and instrument screens (cells, order list,
  patterns and instruments added, speed, rhythm, instrument fields and names). A log of what the song held
  before each edit, 32 steps, oldest dropped; an insert or delete in the order list keeps the whole list
  (198 bytes). A knob turned on and on within half a second is one step. No redo. A song change clears it;
  SAVE does not, and the log is not saved.
- Live recording: REC in EDIT arms it (LED, `REC` in the status line). While the song plays, a key writes its
  note into the cursor's column at the player's row, rounded to the nearest row (past half a row goes into the
  next one), and sounds. Key-up writes nothing. Each recorded note is one undo step. REC outside EDIT is
  still the loop-mode toggle.
- `tests/run_host.sh` also runs `tests/script_undo.txt` and `tests/script_rec.txt`: the host build checks pattern
  cells and the order list in memory (new script commands `save`, `same`, `differs`, `note`, `empty`) and exits 1 when a check
  fails. Not run on hardware: the firmware build needs the JieLi toolchain, which is not on this machine.

- Battery icon at the top right of every screen: 3 bars from ADC ch3, read once a second and smoothed, with a
  little hysteresis so it does not flicker; red outline when empty. The thresholds are raw counts (531 / 561 /
  591, Felucca's and SLOOP's), not volts, and not yet checked on hardware. GLO outside the editor shows the
  smoothed count in the status line, to tune `BATT_LEVEL` by. The jukebox's and the editor's top lines are
  centred in 220 px now, left of the icon. The host build (`tests/host.c`) reads a fixed count, `HOST_BATT`.

- Docs: the Dinghy-era names (`dinghy.c`, `dinghy.fwsc`, "Build Dinghy") now say OPLBOX, with Dinghy credited as the
  base; `CREDITS.md` added; Dinghy is a fetch-only `upstream` remote, nothing merged from it.

- `tests/run_host.sh` checks what it makes and exits 1 on a regression: the screen at 3000 ms is not blank, the
  battery icon shows 2 bars of 3 for `HOST_BATT` 575, and the WAV is 6.5 s of 16-bit stereo 44.1 kHz and not
  silent. Before, it only wrote the files and always passed.

# OPLBOX 0.4.2 (identity FM-1_964)

- USB: a SysEx from the computer (the installer's identity query, `fm1_install.py --info`, the first stage of an
  install) dropped the FM-1 off USB: a burst of babble, a disconnect, a re-enumeration a second later; plain MIDI
  did not. The app was built at -O2 with the platform inside it, and the USB stack babbles at -O2 (Dinghy keeps
  -Os for that reason, its tools/build.py). Now the app and the platform build at -Os like Dinghy and the OPL
  core (`dbopl2.c`) is its own object at -O2, with its own header; the scope taps moved with it. Installs over
  a running OPLBOX no longer need the USB rescue.
- `tests/run_host.sh` compiles the core separately too.

# OPLBOX 0.4.1 (identity FM-1_964)

- USB rescue: the screen's pins are set up before the OCT- check at power-on. On some boards no key reads as
  pressed until that happens, so holding OCT- never opened the rescue (dinghy PR #4, sgarrettnelson). One line
  in fm1_cstart(); nothing else changed.

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


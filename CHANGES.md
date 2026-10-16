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


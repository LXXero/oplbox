# Credits

OPLBOX is based on Dinghy by zvenson, https://github.com/zvenson/dinghy (a copy of Dinghy 0.3 plus the change
below). It stands on the work of the people and projects named here. Licences and copyright lines are in
[LICENSING.md](LICENSING.md); this file only says who did what.

## Base

- **Dinghy**, zvenson, https://github.com/zvenson/dinghy: the base of this firmware, its wiring (`platform.c`),
  its host mode (`tests/`) and its docs.
- **Felucca**, Leo Kuroshita (@kurogedelic), Hügelton Instruments, https://github.com/hugelton/Felucca: the
  platform (boot, USB, the updater, the rescue, the screen, the panel).
- **SLOOP**, isod89, https://github.com/isod89/sloop-fm1: the changes to that platform that Dinghy builds on.

## Changes taken from others

- **dinghy PR #4**, sgarrettnelson: `fm1_lcd_hw_init()` before the OCT- check at power-on, so the USB rescue
  opens on boards where no key reads as pressed until the screen's pins are set up (`firmware/src/platform.c`,
  OPLBOX 0.4.1). Not in Dinghy 0.4.
- **Smalley**: found that MIDI input was never drained, which locked SysEx out until a power-cycle (fixed in
  OPLBOX 0.2).

## Third-party code and data

- **AdPlug**, `cmfmcsop.cpp`, Sebastian Kienzl (LGPL-2.1): the Macs Opera player follows it.
- **DOSBox** `dbopl` (The DOSBox Team, GPL-2.0-or-later), reduced to an OPL2 as `firmware/src/dbopl2.c`.
- **Nuked-OPL3**, Alexey Khokholov (LGPL-2.1): `firmware/src/opl3.c`, the cycle-exact core.
- **Terminus** font 8x16 (SIL OFL 1.1).
- **Thomas Detert** (Stone Age, Eclipse 1992) and **Michael Tschögl** (LINEL 1990-92): the 18 built-in songs.
- **JieLi AC79 SDK** (Apache-2.0): the boot files read from your SDK checkout at build time.
- **designburg**: the signet in `assets/logo.svg`.

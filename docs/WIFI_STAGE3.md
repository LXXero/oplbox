# WiFi stage 3: radio power-up and calibration, step by step

This is the plain description `firmware/wifi/fm1wifi.c`'s stage 3 code is written from (docs/WIFI.md, "What stages
1 and 3 may take from `wl_rf_common.a`": facts from the archive, code from a description). Each step says which
register gets which value, what is read and when a loop ends. Where JieLi's code waits without a bound, the step
says so and the library bounds it. Its first run on an FM-1 crashed: "First run on an FM-1" at the end.

The facts were read with `tools/wifi_ir.py`, which turns the eight clean members of `wl_rf_common.a` (SHA-256
`ec50bb31…`) back into LLVM IR and runs that IR against a register model. Its output is the straight-line part of
the power-up as tables of register steps (`firmware/wifi/fm1wifi_tables.h`, generated, committed) and the whole
sequence's register writes against the model (`tests/wifi/radio_trace.txt.gz`, the host test's reference). The
parts with loops, or with values that depend on what the hardware reads back, are the steps described below; the
library writes those by hand and runs the tables for the rest.

## Names used here

- `WLA_CONn` is JL_ANA word n at 0x11900 + 4n (the radio's analog controls), `WF_CONn` JL_WL word 16 + n at
  0x14040 + 4n, `WL_ANL_CONn` 0x14028 + 4n (the RF digital block's port). "Field bits h-l of R = v" is one
  read-modify-write of R changing only those bits; every listed step is its own read and write, in the order given.
- **BBP windows** [no standard term]: four register files behind BBP registers. A window write sets the window's
  address register to the address, its data register to the value, then its strobe register to 1. A window read sets
  the address register, then the strobe to 2, then reads the window's read register. The registers (BBP numbers):

  | Window | strobe | address | data | read |
  | --- | --- | --- | --- | --- |
  | CCA | 0xC8 | 0xC9 | 0xCA | 0xCB |
  | RX 11n | 0xCC | 0xCD | 0xCE | 0xCF |
  | TX back end (TXBE) | 0xD0 | 0xD1 | 0xD2 | 0xD3 |
  | RX front end (RXFE) | 0xD4 | 0xD5 | 0xD6 | 0xD7 |

  "TXBE 92 = x" is a window write; "read TXBE 92" a window read.
- **Status read**: write the selector into `WLA_CON26` bits 31-28, write `WLA_CON30` = 1 eight times, then
  `WLA_CON30` = 0, then read `WLA_CON30`. Only selector 1 is used. In its answer bit 5 is "filter tune done",
  bits 15-8 the filter tune's code, and bit 18 | bit 17 << 1 the PLL's VCO comparison ("VCO result", 0-3).
- **Measurement** (`dem_filter_dc`, with a kind 0-3): RXFE 21 = 1, RXFE 22 = 1, RXFE 21 = 3, RXFE 22 = 3; wait until
  read RXFE 0x8D has bits 1-0 clear (JieLi: no bound; the library: up to 1000 reads, 1 us apart after the first);
  then
  - kind 0 and 3: I = the signed 16 bits RXFE 0x81 << 8 | RXFE 0x82, Q = RXFE 0x84 << 8 | RXFE 0x85; the result is
    I² + Q²;
  - kind 1: the signed 24 bits RXFE 0x81 << 16 | 0x82 << 8 | 0x83;
  - kind 2: the signed 24 bits RXFE 0x8A << 16 | 0x8B << 8 | 0x8C.
- **ADC peak**: RXFE 23 = 0x82, wait 1000 us, read RXFE 0x80 (the peak), RXFE 23 = 2.
- **VGA gain g** (0-41): the four bytes b0-b3 of row g of `FM1WIFI_VGA`: `WLA_CON4` bit 22 = b0 bit 0, bit 23 =
  b0 bit 1, bit 24 = b0 bit 2, bit 25 = b1 bit 0, bit 26 = b1 bit 1; `WLA_CON5` bits 17-15 = b2 bits 2-0, bit 12 =
  b2 bit 4, bit 13 = b2 bit 5, bit 14 = b2 bit 6, bits 7-5 = b3 bits 2-0. Ten read-modify-writes in that order.
- **PLL**: cap c is `WLA_CON14` bits 25-19 = c (7 bits); feedback divider f is `WLA_CON15` bits 12-5 = f (8 bits);
  "restart" is table `PLL_RESTART` (three bits cleared, then set again 5 us, 5 us apart, then 100 us); the VCO
  detector is `WLA_CON16` bit 24 (1 open, 0 closed).
- Setters for the trims (each is the writes listed, in order):
  - TX LO I v: `WLA_CON7` bits 14-8 = v & 0x7F; TX LO Q v: `WLA_CON8` bits 6-0 = v & 0x7F.
  - TX DC I d (a signed 10-bit value): TXBE 8 = (d >> 2) & 0xFF, then TXBE 98 = (read TXBE 98 & 0xFC) | (d & 3).
    TX DC Q: TXBE 9 = (d >> 2) & 0xFF, then TXBE 98 = (read TXBE 98 & 0xF3) | ((d & 3) << 2).
  - TX gain I g (10 bits): TXBE 4 = (g >> 2) & 0xFF, then TXBE 97 = (read & 0xFC) | (g & 3). TX gain Q: TXBE 6, then
    TXBE 97 = (read & 0xCF) | ((g & 3) << 4).
  - TX phase I p: TXBE 5, then TXBE 97 = (read & 0xF3) | ((p & 3) << 2). TX phase Q: TXBE 7, then TXBE 97 =
    (read & 0x3F) | ((p & 3) << 6). (Same split: bits 9-2 in the first register.)
  - RX gain I/Q: RXFE 9 / RXFE 11 = the byte. RX phase I/Q: RXFE 10 / RXFE 12 = the byte.
  - Analog RX DC I d (signed): if d ≥ 0, `WLA_CON2` bits 15-7 = d & 0x1FF, then bit 16 = 1, and the code kept is
    d | 0x200; else bits 15-7 = (-d) & 0x1FF, then bit 16 = 0, and the code kept is -d. Analog RX DC Q: the same with
    bits 25-17 and bit 26.
  - "CCA set for the trim": CCA 64 = read CCA 64 & 0x7F; CCA 65 = read CCA 65 & 0x7F; CCA 40 = 0; CCA 39 = 5;
    BBP 4 = BBP 4 & 0xE7.

## The sequence

`fm1wifi_radio_up` runs, after stage 2's power-up (JieLi's `wfhw_init` order, docs/WIFI.md "Power-up and calibration
sequence"; table names are `FM1WIFI_T_*` in `fm1wifi_tables.h`):

1. Tables `PHY_MAC_RST` (MAC_SYS_CTRL = 3, MAC 0x030C = 30 then 0, MAC_SYS_CTRL = 12, BBP 22 = 0, 21 = 0x31,
   21 = 0x30, 25 = 0), `SIFS` (six MAC timing registers), `MAC_0308` (MAC 0x0308 = 0x9407), `CFR0` (TX back end).
2. `PHY_INIT_A` (the PHY reset and the MAC timing again), **AGC table**, `PHY_INIT_B` (307 window writes: CCA, RX
   front end, RX 11n, TX back end).
3. `RF_INIT` (292 register steps: the radio's analog set-up, the crystal's load caps from the calibration into
   `WLA_CON12` bits 16-13 and 22-19, the radio's mode 5, a PLL restart), **PLL bank scan** for divider 121,
   `RF_PLL` (the fractional divider, a restart, 80 us).
4. **The trim**, from RAM with the sound off: **RX filter tune**, `TRIM_TX`, **TX trim**, `TRIM_RXDC`, **RX DC trim**,
   `TRIM_RXIQ`, **RX IQ trim**. Always a fresh trim: JieLi's stored-trim path (`wifi_trim_is_already`,
   `quick_start_wifi_trim`) is not taken, as fm1-base found a stored trim made WiFi deaf.
5. `RF_DONE` (the radio's mode 0: receive), **PA configuration**, `RF_GAIN`, **VGA gain 0**.
6. `PHY_INIT_A`, **AGC table**, `PHY_INIT_B` again, `CFR1`, **RAPP**.
7. **RF RAM table**, `BSB_AUTO` (`wf_rf_bsb_auto_ctl`), **TX power 6**, **channel 6**, **per-MCS gains**.

JieLi's sequence also has steps with no register write the library needs: `keep_tx_iq_set` and `rf_dc_get_reload`
only read registers into variables for later re-trims, `tx_rx_bw_set_save` and the trim region calls do nothing on
this path. They are left out. The bold steps:

### AGC table

For i = 0 to 127, with w = `FM1WIFI_DAGC[i]`: CCA 47 = i; CCA 43 = the low byte of w as a signed number, limited to
-64..63; CCA 78 = bits 15-8 of w; CCA 84 = bits 23-16; CCA 85 = bits 31-24; CCA 47 = i | 0x80. Then CCA 47 = 0,
CCA 84 = 0xC0, CCA 85 = 1, CCA 78 = 66, CCA 43 = 66, CCA 44 = 66, CCA 121 = 0, CCA 122 = 32.

### PLL bank scan (divider f)

1. Open the VCO detector.
2. Find the middle bank: divider 73; for m = 7, 23, 39, … while m < 120: for each cap c in (m, m - 7, m + 8): set
   cap c, restart, read the VCO result. If the three results are equal, go on to the next m; otherwise the middle
   bank is m | 15. If they never differ, it is the last centre result | 15 (JieLi's code returns that reading; kept
   so the sequence matches). Keep the middle bank as limit[47].
3. For i = 0 to 46: set cap (middle - i) & 0xFF. With l the lowest divider still below the VCO's range (starting at
   73, carried from bank to bank) and x starting at l: set divider x, restart, read the VCO result; 1 means x is
   above the range: the top of this bank h is x, stop; 2 means below: l = x; anything else: inside. Then x + 1; past
   171 the scan stops with h = 171 (and l = 171 if the last result was 2). limit[i] = (h of the previous bank, 73 for
   the first, + l - 3) / 2, rounded towards zero.
4. Set divider f. With i the first index from 1 to 47 whose limit[i] > f (compared unsigned), minus 1, or 47 if
   none: bank = limit[47] - i, 8 bits. Set cap bank. Close the detector. (Index 47 is the middle bank itself, so it
   takes part in the comparison, as in JieLi's code.)

Bounded: at most 47 banks of at most 99 dividers.

### RX filter tune

`WLA_CON1` bits 11-8 = 8; `WLA_CON20` bit 27 = 1. For k = 0, 1, 2: `WLA_CON26` bits 9-1 = 254, 145, 80 for k = 0, 1,
2; then bits 17-10 = 0x1E; bits 22-18 = 0; bit 23 = 0; bit 0 = 1 (start). Wait until the status read's bit 5 is set
(JieLi: no bound; the library: up to 1000 status reads, 1 us apart after the first). A second status read gives the
code (bits 15-8): k = 0 is the 6 MHz code, 1 the 10 MHz, 2 the 20 MHz. `WLA_CON26` bit 0 = 0.

Then `WLA_CON20` bit 27 = 0; `WLA_CON8` bits 17-10 = code 10; `WLA_CON4` bits 21-14 = code 10; and for `WLA_CON23`,
then `WLA_CON24`: bits 7-0 set to 0xFF, bits 15-8 = code 6, bits 23-16 = code 10, bits 31-24 = code 20.

### VGA search (`adc_pp_tune`, mode 0)

g = 0; VGA gain 0; ADC peak; p = ADC peak (the second reading counts). While p ≤ 47 and g ≤ 35: g = g + 1, VGA
gain g, ADC peak, p = ADC peak. Then VGA gain g once more. At most 36 steps.

### TX trim

1. Table `PHY_RST`; CCA 21 = 0; CCA 20 = 0; CCA set for the trim; table `PHY_LOOPBACK`; TXBE 0 = 3, 95 = 16, 1 = 20,
   2 = 0, 3 = 0; RXFE 0 = 62, 19 = 30, 20 = 56, 20 = 52.
2. TX gain I 1023, gain Q 1023, phase I 0, phase Q 0, DC I 0, DC Q 0; TX LO I 64, LO Q 64; VGA search; TX LO I 64.
3. LO I: best = measurement 0, choice 64; for v = 0 to 127: TX LO I v, m = measurement 0, if m < best: best = m,
   choice = v. TX LO I choice.
4. LO Q: TX LO Q 64; the same over LO Q; TX LO Q choice.
5. DC I: DC I 0, DC Q 0; VGA search; DC I 0; best = measurement 0, choice 0; for n = -200 to 200: v = n as a signed
   byte (n wraps: -200 is 56); DC I v; m = measurement 0; if m < best: best, choice = v. DC I choice.
6. DC Q: DC Q 0; the same over DC Q; DC Q choice.
7. Gain: RXFE 19 = 60; gain I 1023, gain Q 1023; best = measurement 0, choice 0; for n = -423 to 423: if n < 0 gain
   I 1023 + n and gain Q 1023, else gain I 1023 and gain Q 1023 - n; m = measurement 0; if m < best: best, choice =
   n as a signed byte. If choice < 0: gain I 1023 + choice, gain Q 1023, and the phase goes on I; else gain I 1023,
   gain Q 1023 - choice, and the phase goes on Q.
8. Phase: phase I 0, phase Q 0; best = measurement 0, choice 0; for n = -200 to 200: set the chosen side's phase to
   n; m = measurement 0; if m < best: best, choice = n as a signed byte. That side's phase = choice.

The "as a signed byte" truncations are JieLi's: a best value past ±127 would be written wrapped. Kept, so the result
matches.

### RX DC trim

1. Table `PHY_RST`; CCA 21 = 0; CCA 20 = 0; CCA set for the trim; table `PHY_LOOPBACK`; TXBE 0 = 3, 95 = 16; RXFE 0 =
   62, 27 = 56, 8 = 0xC0, 19 = 0, 20 = 56, 20 = 52; VGA gain 40.
2. For each gain range r = 0 to 3: `WLA_CON3` bits 7-6 = 0 for r = 0, else 1; `WLA_CON4` bits 11-8 = 0, 0, 3, 9;
   `WLA_CON4` bits 6-0 = 127, 127, 69, 14. Analog DC I 0, analog DC Q 0. With i = q = 0 and step 256, 12 times:
   m = measurement 1; if m > 0 and i > -510: i = i - step; else if m < 0 and i < 510: i = i + step; analog DC I i;
   m = measurement 2; the same for q; analog DC Q q; step = step / 2, at least 1. Then analog DC I i and analog DC Q
   q once more, keeping their codes as range r's (rx_dc[r]).

### RX IQ trim

1. Table `PHY_RST`; CCA 21 = 0; CCA 20 = 0; CCA set for the trim; table `PHY_LOOPBACK`; TXBE 0 = 3, 95 = 16, 1 = 20,
   2 = 0, 3 = 17; RX gain I 255, gain Q 255, phase I 0, phase Q 0; RXFE 0 = 62, 20 = 56, 20 = 4, 19 = 30; VGA search;
   RX gain I 255, gain Q 255.
2. Gain: best = measurement 3, choice 0; for n = -105 to 105: if n < 0 gain I 255 + n and gain Q 255, else gain I
   255 and gain Q 255 - n; m = measurement 3; if m < best: best, choice = n. If choice < 0: gain I 255 + choice, gain
   Q 255, phase on I; else gain I 255, gain Q 255 - choice, phase on Q.
3. Phase: phase I 0, phase Q 0; best = measurement 3, choice 0; for n = -50 to 50: that side's phase n; m =
   measurement 3; if m < best: best, choice = n. That side's phase = choice.

### PA configuration

From the calibration's 7 PA bytes p0-p6 (default 1, 7, 4, 7, 11, 1, 7): `WLA_CON9` bits 26-24 = p0 & 7; `WLA_CON10`
bits 2-0 = p1 & 7; bits 11-9 = p2 & 7; bits 17-15 = p3 & 7; bits 25-22 = p4 & 15; `WLA_CON11` bits 2-0 = p5 & 7. p6
has no register here (it goes into the RF RAM table).

### RAPP (`rapp_init`; "cfr"/"rapp" may mean crest-factor reduction and a Rapp PA model, not confirmed)

1. TXBE 81 = read TXBE 81 & 0xFC.
2. For i = 0 to 127, with d = `FM1WIFI_RAPP_RAM[i]`, at most 511: TXBE 92 = read TXBE 92 | 0x10; TXBE 93 = (read
   TXBE 93 & 0x80) | i; r = read TXBE 92; TXBE 94 = d & 0xFF; TXBE 92 = (r & 0xDF) | ((d >> 8 & 1) << 5); TXBE 92 =
   read TXBE 92 & 0xEF.
3. Thresholds, each read-then-write: r = read 88; TXBE 89 = 160; TXBE 88 = (r & 0xF8) | 0. r = read 81; TXBE 82 =
   0x64; TXBE 83 = 0x2D; TXBE 81 = (r & 0xF3) | 0x0C. r = read 88; TXBE 90 = 160; TXBE 88 = (r & 0xC7) | 0.
   r = read 81; TXBE 84 = 0x64; TXBE 85 = 0x2D; TXBE 81 = (r & 0xCF) | 0x30. r = read 88; s = read 92; TXBE 91 =
   0x90; TXBE 88 = (r & 0x3F) | 0x40; TXBE 92 = (s & 0xFE) | 0. r = read 81; TXBE 86 = 0xD6; TXBE 87 = 0x3F;
   TXBE 81 = (r & 0x3F) | 0x80. (The values are JieLi's 160, 0x32D64, 160, 0x32D64, 400, 0x23FD6 split into fields.)
4. TXBE 81 = (read TXBE 81 & 0xFC) | 2.

### RF RAM table (`wl_anl_actl_init(0)`)

"Wait" is: while `WL_ANL_CON0` bits 22-20 are not all 0, write `WL_ANL_CON3` = 0 and read again (JieLi: no bound; the
library: up to 1000 times, 1 us apart). "Kick" is `WL_ANL_CON0` bit 4 set five times, then cleared five times (ten
read-modify-writes).

1. `WL_ANL_CON0` = 0, `CON1` = 0, `CON2` = 0, `CON0` = 0x10000. Wait. `CON1` = 0, `CON2` = 0, `CON0` = 0x10000. Kick.
2. For i = 0 to 255, with {hi, lo} = `FM1WIFI_RAM_LUT` entry i:
   - Wait. If 128 ≤ i < 224 (AGC entries): with r = 0 for i < 133, 1 below 180, 2 below 186, else 3, hi = (rx_dc[r]'s Q
     code << 22) | ((rx_dc[r]'s I code & 0x3FF) << 12) | (hi & 0xFFF). If i ≥ 224 (PA entries): hi = (hi & ~0x3F) |
     p5 | p6 << 3. `CON1` = hi; `CON0` = i << 8 | 0x1000E; kick.
   - Wait. If i < 128 (frequency entries): lo = lo | (the PLL bank for divider (lo >> 21) & 0xFF, as step 4 of the
     bank scan finds it) << 9. If i ≥ 224: lo = (lo & 0xC06071FF) | p4 << 26 | p3 << 23 | p2 << 18 | p1 << 15 |
     p0 << 9. `CON1` = lo; `CON0` = i << 8 | 0x1000D; kick.
3. `WL_ANL_CON0` = 0 six times, then 0x10000.

### TX power, channel, per-MCS gains

- TX power p: `WF_CON7` bits 23-16 = p, at most 6.
- Channel c (1-13): `WF_CON7` bits 7-0 = c + 101.
- Per-MCS gains: start from `FM1WIFI_MCS_GAIN`'s 20 bytes; each nonzero calibration byte replaces its entry. TXBE
  11 = g0, 13 = g1, 15 = g2, 17 = g3, 27 = g5, 29 = g7, 31 = g9, 33 = g11, 19 = g12, 21 = g14, 23 = g16, 25 = g19.

## What runs from RAM

docs/WIFI.md: the trim draws about 500 ms of high current and a flash fetch meanwhile reads corrupted data. So step 4
(the trim and everything it calls: the window and BBP accesses, the status read, the measurement, the setters, the
table runner) is in section `.ram_code.fm1wifi`, the tables and arrays it reads in `.ram_code.fm1wifi_data`, and the
port's register access, delay and trim hook in `.ram_code.fm1wifi_port` (felucca's `app.ld` copies `.ram_code*`
into RAM); `tools/build.py` fails the build if the trim's section refers to anything outside RAM. The port is told
before and after (`fm1wifi_port_trim`); felucca's port masks interrupts meanwhile, so no handler runs from flash,
and its delay is the BSP's RAM-safe busy wait that feeds the watchdog. The caller stops the sound first. The rest of
the bring-up is in `.ram_code.fm1wifi_init` only because the FM-1's flash code cannot call RAM code directly.

## The register model's answers (host only)

`tests/wifi/sdio_model.c` and `tools/wifi_ir.py` answer the reads with made-up values, so each search lands
somewhere particular (none is measured): the filter tune is done at once with code (bits 9-1 of `WLA_CON26`) × 5 / 9;
the VCO result is 2 below 250 - 2 × cap, 0 for the next five dividers, 1 above; the measurement is a linear function
of the trim settings in force (TX: LO, DC, gain and phase imbalance; RX IQ: gain and phase imbalance; otherwise the
analog DC against a target per gain range); the ADC peak grows with the VGA fields and varies with the channel, but
only with the PHY in loopback (BBP 25 = 0x18, the trim) and is 0 otherwise, as the FM-1 read it ("ADC peak 0 on every
channel" below; which the hardware really does is not known); every wait ends at once. With the defaults the trims land on TX LO 57/41, TX DC -1/0, TX gain imbalance 5 (gain Q 1018),
TX phase Q 2, RX gain I 235, RX phase I 7, RX DC I 123 and Q -77 in range 0, PLL bank 64.

## First run on an FM-1 (2026-10-26): the crash in `rmw()`

OPLBOX main 80a6917 (`oplbox.fwsc` SHA-256 `e3736f02…`, rebuilt byte-identical from that commit) was flashed at
05:50 UTC and the bring-up started on the WiFi test page. The red crash screen showed vector 1 (CPU exception), PC
01C02674, RETS 01C02674.

**What the numbers say.** 0x01C02674 is in `rmw()` (`.ram_code.fm1wifi`), the instruction after `call r7` with r7 =
`fm1wifi_port_wr32`, i.e. right after the write of the read-modify-write returned. RETS = PC means that call ran
and returned (`fm1wifi_port_wr32` is a leaf and returns to RETS), and the exception was raised at the instruction it
returned to, `sp += 8`, which cannot fault itself. So either the write's bus error arrived late (an imprecise fault on
a store), or the instruction fetched at 0x01C02674 was not `sp += 8` (an illegal instruction from RAM code that was
overwritten or misread). The screen showed neither EMU_MSG nor DBG_MSG, so the two cannot be told apart from it.
The first `rmw()` of the whole bring-up is table `RF_INIT`'s first step (`WLA_CON12`, the crystal caps): every step
before it goes through the core's window, not `rmw()`. So the crash was in `RF_INIT` or later.

**The addresses, checked.** Run against the register model, the bring-up writes these CPU-side registers, first
write first: `WLA_CON12` (`RF_INIT`), `WLA_CON20`, `WLA_CON0`-`CON11`, `CON13`-`CON17`, `WLA_CON25`, `CON26`, `CON30`
(the status read), `CON21`, `CON23`, `CON24`, then `WL_ANL_CON0`-`CON2` (the RF RAM table) and
`WF_CON7` (power and channel). All are inside the blocks `fm1wifi_port.h` names (`WLA_CON0`-`30` at 0x11900, JL_WL at
0x14000), none of them a read-only `WLA_CON31`-`39`, and `WLA()`, `ANL()` and the table runner's register numbers
(below 64 `WLA_CONn`, from 64 JL_WL word n - 64) give the addresses JieLi's code uses: the host test diffs the whole
sequence against `tools/wifi_ir.py --trace`'s run of `wl_rf_common.a` record for record, so no write is out of order,
at a wrong offset or missing against the archive. What the archive does not show is what JieLi's system does before
`wf_rf_common_init`. In the SDK: `WL_CON0` bit 1 (`wifi_rf_on`, "bt_en & wf_en") is set by `wlc_main`, after the
radio's bring-up, so JieLi's bring-up runs without it too; no clock gate or reset outside `wf_clk_rst_init` (which
stage 2 does, and which ran on this FM-1) is touched first. No register-side cause was found in the code.

**The supply, the likely cause (not confirmed).** felucca-bsp's `fm1_sys.h`: "At the boot loader's [levels], under
load (both cores, or WiFi in fm1-radio), some units read flash and SRAM wrong." M-VAVE's firmware raises VDC14 to
1.60 V and SYSVDD (the core and SRAM rail) to 1.38 V as it starts, moves the 1.3 V supply onto its DC-DC and puts the
WiFi 1.2 V regulator on automatic; the bsp has these as `fm1_vdc14_raise`, `fm1_sysvdd_raise` and `fm1_power_stock`,
and OPLBOX called none of them (TODO.md: "there, not called"). The radio's analog set-up and the trim are the WiFi
load, the code at the fault runs from SRAM, and a misread instruction fetch raises exactly this kind of exception at
a PC in RAM code. So the WiFi page now raises the rails (`platform_supply_full`, platform.c) before the core is
powered up. Remaining candidates, if it crashes again: a late bus error from one of the writes above (DBG_MSG's bus
bits would show it), and the RAM code overwritten by the hardware (fm1-base saw the radio write into its RAM; the new
crash screen compares the RAM code with its copy in the flash).

**The next crash says where it was.** The library keeps `fm1wifi_where` in `.noinit` (fm1wifi.h): the step it is in
(set before each step of the sequence above, the trims' searches each a step of their own) and the address and value
of the last CPU-side register write, recorded just before each write is made; a magic word says a call is running.
The crash screen (`fm1_fault`, platform.c) shows vector, PC and RETS as before, plus EMU_MSG, DBG_MSG, sp, the four
ETM trace PCs, whether the RAM code still equals its flash copy, and, for a crash inside the WiFi library, the step
and the last write (README.md "If it crashes"). `tests/wifi_model.c` checks the record at every register write of the
bring-up: magic set, the address and value the write's, the steps in the order above each with writes of their own,
`WLA_CON12` first written in `RF_INIT`, `WL_ANL_CON0` only in the RF RAM table, magic clear after every call (a
TIMEOUT too). Not run on an FM-1 yet.

## ADC peak 0 on every channel (2026-10-26): not settled by the code, a diagnostic added

OPLBOX main d03636c ran stage 3 cleanly on xero's FM-1 (page 1 RESULT OK, MAC_SYS_CTRL 0000000C, BBP 0 = 60, PLL BANK 51
MID 63, FILTER 251 127 55; page 2 TX LO 49 60, TX DC -13 2, TX GAIN +19 PHASE -15 Q, RX GAIN +6 PHASE -7 Q, RX DC I
+202 +212 +178 +154, Q -324 -340 -208 -96). The ADC peak read exactly 0 on all 13 channels, with access points near
the board.

**What the code shows.** The read itself is JieLi's: in `wl_rf_common.a`'s IR, `rd_adc_peak` is RXFE 23 = 0x82,
`delay_us(1000)`, the byte `rx_frontend_rd(0x80)`, RXFE 23 = 2, the same 8-bit read `adc_peak()` makes (and the host
test diffs its writes against the archive). But JieLi reads it in one place only: `rd_adc_peak` is in section
`.rf.text.cache.L2.rf_trim`, its one caller is `adc_pp_tune` (the VGA search), whose callers are `txiq_tune_soft`
and `rxiq_tune_soft`; no other member of the archive, and no other SDK archive, reads RXFE 0x80 (`rx_frontend_rd` is
used by `wf_rf_cal.c.o` only). So JieLi reads the peak only inside the trim: the PHY in loopback (table
`PHY_LOOPBACK`, BBP 25 = 0x18), the transmitter making the test signal, the RX front end set for the trim (RXFE 0 =
62, 19 = 30, 20 = 52 or 4). The page's scan reads it after the bring-up, where `PHY_INIT_B` has put the receive state
back (BBP 25 = 0, RXFE 0 = 0x3A, 19 = 0, 20 = 0, 23 = 0), the VGA gain is 0 (`wfrf_flt_vga_gain(0)`, the lowest)
and `BSB_AUTO` has handed the baseband gains to the hardware's automatic control. That is a state JieLi never reads
the detector in. Whether the detector only runs in loopback, or runs but reads below 1 at VGA gain 0 without the
trim's tone (WiFi frames come in bursts, and a 1 ms window between beacons may hold none), the code cannot tell. So
nothing in the read was changed; a diagnostic was added instead.

**The diagnostic** (page 3 of the WiFi page, README.md): the VGA gain each of the trim's three VGA searches settled on
and the last peak it read (`fm1wifi_radio.vga`, `vga_peak`), and `fm1wifi_rx_probe` on channel 6 after the scan: BBP
25, RXFE 0, 19, 20, RXFE 0x80 before the peak starts, RXFE 23 read back after 0x82 is written, and the peak at VGA gain
0 and at 41 (the table's highest; the gain is set back to 0). How to read it:
- VGA GAIN below 36 with VGA PEAK above 47: the peak reads fine in the trim's loopback, so 0 is the receive state's
  answer (the detector off outside loopback, or the signal below it); then PEAK AT VGA 41 says which.
- VGA GAIN 36 36 36 with peaks of 0: the peak reads 0 in the trim too (the read or the loopback is wrong), and the
  three VGA searches ran to their limit.
- RXFE 23 not 82: the RX front end window did not take the write.

The host model answers the peak only in loopback (`tests/wifi/sdio_model.c`), so the simulated page shows the
all-zero scan and the page 3 lines; `tests/wifi_model.c` checks the probe's values, that it leaves the VGA gain where
it was, and its `fm1wifi_where` step (RX PROBE). Not run on an FM-1 yet.

# The BLE radio's registers and RAM layout, as facts for a clean-room driver

OPLBOX is GPL-3.0-only. `docs/BLUETOOTH.md` ("Cordio path as built") drops JieLi's closed `btctrler.a` and runs Arm
Cordio's link layer (Apache-2.0) on the FM-1 instead. Cordio is portable C that talks to a chip through a small
hardware layer (`PalBb*`, `PalTimer*`, `PalCfg*`, `PalCrypto*`). That layer is ours to write for JieLi's baseband,
and it needs facts: addresses, bit fields, write order, RAM layouts. This file records those facts, in the same
form and under the same rules as `docs/WIFI.md`'s stage 1 and `tools/wifi_ir.py`: numbers with a provenance, no code.

Read on 2026-10-27. "[unchecked]" marks what nobody has verified; "[measure on FM-1]" marks what the IR does not
settle and only a run on the device can. Nothing below was run on an FM-1.

| Source | Version read |
| --- | --- |
| JieLi AC79 SDK, `~/fw-AC79_AIoT_SDK` | d179b448, tag `AC79NN_SDK_V1.2.1_2023-12-13` |
| `cpu/wl82/liba/btctrler.a` | SHA-256 `9ef42624…73de` |
| `cpu/wl82/liba/cpu.a` (member `wlc.c.o` only) | SHA-256 `00cea21d…2c62` |
| Arm/Packetcraft Cordio, https://github.com/packetcraft-inc/stacks (Apache-2.0), `platform/include/pal_*.h` only | 3656312 (2020-11-11) |

## Short answer: does the IR give enough to advertise?

**It gives enough to write the first driver, not enough to promise that it transmits.** What it settles, with
numbers, in the sections below:

- the power-up writes of the Bluetooth side of the radio (section B), the analog and modem block's reset values (C),
  the baseband's control block (D), and the RAM block the baseband reads its work from (E): the instance table, the
  per-link parameter block field by field (named, with offsets), the TX and RX buffer headers, the channel and PLL
  tables (F);
- the exact order JieLi's driver uses to put one link into the advertising state and start it (G), and the smallest
  transmit sequence the IR has, JieLi's own direct-test transmitter (K);
- the interrupt registers and what the two handlers do with them (H).

For a non-connectable advertisement (ADV_NONCONN_IND on channels 37, 38 and 39, no receive) that is a complete list
of what JieLi writes. For a connectable one (ADV_IND, then SCAN_REQ and CONNECT_IND received) the receive-side
facts are there too (RX buffers, the filter register, the RX status words, what software does with a CONNECT_IND),
but the timing inside an event (the 150 µs turnaround, the receive windows) is done by the baseband and no number for
it appears in the IR except the timing constants in D.1 and D.5.

**What is not settled** (each needs an FM-1 measurement, in the order they will bite):

1. `wf_rf_common_init`, the first call of `bt_rf_init`, lives in `wl_usb_to_sdio.c.o`, which `docs/WIFI.md` does not
   clear as a source of facts, and was not read. It brings up the WiFi core's clocks and the shared analog radio.
   OPLBOX's own WiFi power-up (`fm1wifi_radio_up`) does the same job for the same analog block.
   [measure on FM-1] whether the BLE sequence works after `fm1wifi_radio_up`, before it, or needs a BLE-only subset.
2. The calibration inputs. `bt_reload_trim` copies five values (`keep_BT_CMPUK1`..`3`, `keep_BT_OFSI`, `keep_BT_OFSQ`)
   into the modem block; the IR shows where they go, not what they hold on this device (they come from the chip's
   calibration store). The PLL bank table (`rf` field 20, 32 bytes) and the crystal offset (`rf` field 13) are filled
   by code outside the BLE members. [measure on FM-1] their values; zero is the reset state of both.
3. What each bit of the control block means. The IR shows which bits JieLi sets and clears, in which order, not what
   they are called. D lists them as constants. Where a use shows the meaning (interrupt enable and pending, the
   anchor register file), it is stated and marked "from use".
4. Whether the baseband starts without the coexistence arbiter (`wlc_wreg(5, 0x98)` in `bt_process_before_init`, and
   `wlc_main` when `wf_rf_common_init` returns nonzero). [measure on FM-1]
5. The units of `delay(n)` and `hw_udelay(n)` [unchecked]: `docs/WIFI.md` already treats the CPU's `delay` as microseconds
   at a known clock; the BLE members use the same function.
6. Whether the radio actually puts a carrier out. Section K is the cheapest way to find out: JieLi's own transmit
   test, reproduced, sends an unmodulated carrier or a fixed packet on a fixed channel without a link layer.

**How the baseband works, which decides the shape of Cordio's hardware layer.** It is not a "transmit this packet now"
radio. It is a small per-link engine: the CPU fills a parameter block in RAM (access address, CRC init, channel map,
buffer pointers, interval, window), writes a few indexed registers, and the hardware runs the whole event on its own
clock: it counts slots (625 µs) down to the anchor, transmits the buffers, opens the receive window, turns around
after 150 µs, hops, and raises one interrupt per event (and one per received packet). Cordio's hardware layer
instead expects the opposite split: the link layer decides a time and a packet and the layer transmits it
(`PalBbBleTxData`, `PalBbBleRxData`, `PalBbBleTxTifsData`). Section "What Cordio's hardware layer needs" maps one
onto the other.

## Clean-room rules

The same rules as `docs/WIFI.md` "What stages 1 and 3 may take from `wl_rf_common.a`", applied to `btctrler.a`:

- Only facts leave the archive: addresses, bit positions, constants, the order of writes, the layout and the names of
  RAM structures. The names are the ones the IR carries as value names (`ANCHOR`, `CRCWORD0`, `TXPTR0` …). No control
  flow, no function body, no IR text is copied or re-expressed as C. `tools/ble_ir.py` prints facts as steps
  ("write this constant to this address", "read-modify-write with this mask") and a layout table, and nothing else.
- What a driver written from this file does is **ours**: a register map, a table of sequences, a layout. Where the
  hardware needs a decision JieLi's code encodes as logic (when to resend, how to pick the next channel), the driver
  makes it from the Bluetooth Core Specification, not from `btctrler.a`.
- Not read: `wl_usb_to_sdio.c.o`, `hw_dma.c.o` (only its function names and sizes), `lbuf.c.o`, `device_manager.c.o`,
  the BR/EDR members. Cordio's `platform/include/pal_*.h` were read for function names; no Cordio file under
  `platform/targets/` was read for register content (it is Nordic's).
- `tools/ble_ir.py --check` reproduces every number quoted below that is marked "from IR" and fails when the archive
  is not the one named above.

Members read, all from `btctrler.a` except the last, with the source path the archive records:

| Member | Records | Used for |
| --- | --- | --- |
| `RF_ble.c.o` | `port/wl82/baseband/ble/RF_ble.c` | the baseband: init, links, TX and RX, IRQs, the parameter block |
| `RF_ble5.c.o` | `port/wl82/baseband/ble/RF_ble5.c` | field names of the parameter block only (LE 5 extended advertising is not used) |
| `RF.c.o` | `port/wl82/baseband/common/RF.c` | power-up, PLL on/off, clock gating |
| `analog.c.o` | `port/wl82/baseband/common/analog.c` | the analog and modem block, trim, PLL table |
| `analog_ble.c.o` | `port/wl82/baseband/ble/analog_ble.c` | TX power and AGC words |
| `btagc.c.o` | `port/wl82/baseband/common/btagc.c` | AGC block |
| `le_low_power.c.o` | `port/wl82/baseband/ble/le_low_power.c` | sleep and resume writes |
| `direct_test_dut.c.o` | `port/wl82/baseband/ble/direct_test_dut.c` | the direct-test transmitter and receiver |
| `cpu.a`: `wlc.c.o` | `lib/driver/cpu/wl82/wlc.c` | the coexistence command register (`wlc_wreg`) |

## Memory map: what the BLE path touches

Addresses as the IR uses them. "Known" says whether `docs/WIFI.md` or `firmware/wifi/fm1wifi_regs.h` already maps it,
so the WiFi driver and a BLE driver do not write the same register under two names.

| Range | What (WL82.h name where it has one) | BLE path writes it in | Known to the WiFi driver? |
| --- | --- | --- | --- |
| 0x10010 | `CLK_CON1` (`JL_CLOCK`), bits 15-14 | `bt_rf_init`, `bt_rf_close` | **yes**, `FM1_CLK_CON1_WIFI` (0xC000): the same two bits. Shared register: one writer at a time |
| 0x11924, 0x11950 | `JL_ANA` `WLA_CON9`, `WLA_CON20` | `bt_pll_reset` (test paths only) | **yes**, `FM1_WLA_CON(n)`; WiFi power-up writes both (`fm1wifi_tables.h`): clash for the test paths, see B.3 |
| 0x13B00 | not named in WL82.h | `pseudo_random_genrate` reads it | no [unchecked what block it is] |
| 0x14000 | `JL_WL` `WL_CON0` | `bt_rf_init`, `bt_rf_close`, `bt_clk_close`, `RF_poweroff_recover`, `bt_process_before_init`, `wlc_main` | **yes**, `FM1_WL_CON(0)` is defined; no WiFi code or table writes it [unchecked: `wf_rf_common_init` may] |
| 0x14004 | `JL_WL` `WL_CON1` | `wlc_wreg` / `wlc_rreg` (the coexistence command register) | `FM1_WL_CON(1)` exists; no WiFi code writes it |
| 0x20000, 0x20050 | `bsfr_base` ("bt; wire less"), no block named | `bt_rf_init`, `bt_process_before_init` | **no** |
| 0x28000-0x28054 | the LE baseband's control block | `RF_ble` (D) | **no** |
| 0x2FC00-0x2FCA0 | analog and modem block | `analog.c` (C) | **no** |
| 0x2FC40, 0x2FC50, 0x2FC6C, 0x2FC78, 0x2FC7C | BT clock, power and control words | `bt_rf_init`, `bt_process_before_init`, `le_low_power`, `btagc_init` | **no** |
| 0x2FC80, 0x2FC84, 0x2FC88, 0x2FCBC | the baseband's RAM window (E.1, F.2) | `le_hw_init`, `bta_pll_config_init` | **no** |
| 0x2FD80-0x2FD9C | AGC block | `btagc_init`, `btagc_ram_init`, `analog_rx_control` | **no** |
| 0x50048 | `JL_IOMAP` | `bt_process_before_init` writes 0x101 | **yes**, listed in docs/WIFI.md (IO mapping); the WiFi driver does not write it [unchecked] |

No address above overlaps the WiFi core window (0x30000-0x3FFFC, `FM1_WIFI_CORE`) or the internal SDIO host (0x30F00).
The Bluetooth blocks are 0x20000-0x2FFFF, wholly below it. The one register written by both stacks with different
bits is `CLK_CON1`; the one with a plausible clash is `WL_CON0` (B.2) and `WLA_CON9`/`WLA_CON20` (B.3).

## The facts

"From IR" always means: `tools/ble_ir.py`, member and function as named. A step is "write", "set bits"
(read-modify-write, set), "clear bits" (read-modify-write, clear) or "field" (clear a mask, then set a value).

### B. Power-up of the Bluetooth side

**B.1 `bt_rf_init`** (`RF.c`), the order JieLi uses. The first two and the last are not register writes:

| # | Step | Provenance |
| --- | --- | --- |
| 1 | call `wf_rf_common_init()` and keep its return value | from IR; callee not read (see "not settled" 1) |
| 2 | call `set_bt_version(12)` (a byte in the controller's own state) | from IR |
| 3 | set two bytes in the controller's `rf` struct: field 10 = 24 (looks like the oscillator in MHz [unchecked]), field 11 = 10 | from IR |
| 4 | `CLK_CON1` (0x10010): clear bits 15-14 | from IR |
| 5 | `delay(240)` | from IR |
| 6 | `WL_CON0` (0x14000): set bit 7 | from IR |
| 7 | `WL_CON0`: field bits 20-16 = 0x0C | from IR |
| 8 | `delay(rf.field10 × 10)` = 240 | from IR |
| 9 | `WL_CON0`: set bit 0 | from IR |
| 10 | `delay(240)` | from IR |
| 11 | 0x2FC40: set bit 10 | from IR |
| 12 | 0x20000: set bits 31-30 | from IR |
| 13 | `delay(240)` | from IR |
| 14 | 0x2FC40: write 0xFFFF | from IR |
| 15 | 0x2FC40: clear bits 9 and 8 and 1 (mask 0x302) | from IR |
| 16 | 0x2FC78: write 0xF | from IR |
| 17 | 0x2FC78: set bits 9-4 (0x3F0) | from IR |
| 18 | `bt_rf_analog_init()` (C) | from IR |
| 19 | if step 1 returned nonzero: `wlc_main()` (the coexistence arbiter, B.4) | from IR |

Caller: `hci_controller_init`, before the controller's thread starts and before `ll_init`, which calls `le_hw_init` (D.1)
through a table of operations [from IR; the call is an indirect call through `ble_ops`].

**B.2 `bt_process_before_init`** (`RF.c`; its caller is not in `btctrler.a` [unchecked when it runs relative to B.1]):

1. `wlc_wreg(5, 0x98)` (B.4)
2. `WL_CON0`: field bits 27-24 = 2
3. `WL_CON0`: set bit 8
4. 0x2FC50: field bits 5-2 = 2
5. 0x20050: write 0x87, then write 0xAC
6. 0x2FC50: set bit 1
7. `JL_IOMAP` (0x50048): write 0x101

**B.3 `bt_pll_reset(mode)`** (`RF.c`; called with 1 by the direct-test transmitter and `bt_set_single_carrier`'s callers,
0 by the receiver):
mode 0: `WLA_CON9` (0x11924): clear bit 2, then bit 0, then bit 1 (one write each); `WLA_CON20` (0x11950): clear bits 3-0.
mode nonzero: `WLA_CON9`: set bit 2, then 0, then 1; `WLA_CON20`: set bits 3-1 (0xE). Only the test paths call it
(the direct-test transmitter and receiver, the single-carrier sender); advertising and connections do not. These are
the same two registers `docs/WIFI.md` lists for PA and `rf_on`/`rf_off`, and `firmware/wifi/fm1wifi_tables.h` has WiFi's
power-up leave `WLA_CON9` bits 2-0 = 7 and `WLA_CON20` = 0: the mode-nonzero write of `WLA_CON9` agrees with that, the
`WLA_CON20` write does not. A BLE test run must not follow WiFi power-up without re-writing both [unchecked].

**B.4 The coexistence command register** (`cpu.a`, `wlc.c.o`): `wlc_wreg(type, data)` writes 0x14004 =
`type << 4 | data << 16 | 1`; `wlc_rreg(type)` writes 0x14004 = `type << 4 | 2` and the result is read from a register
the IR does not show [unchecked which]; `wlc_pti_wreg` is two `wlc_wreg` calls (type 6, then type 7). `wlc_main` sets
`WL_CON0` bits 1-0 and then calls `wlc_init` with three of nine tables indexed by the coexistence mode
(`docs/BLUETOOTH.md` "What `wl_rf_common.a` shares with WiFi, and coexistence"); `wlc_init` was not read.

**B.5 Power-down** (`RF.c`): `bt_rf_close`: `WL_CON0` clear bits 4-0, `delay(1000)`, `WL_CON0` = 0, `delay(1000)`,
`CLK_CON1` set bits 15-14, `delay(1000)`. `bt_clk_close(mode, flag)` and `RF_poweroff_recover` are sequences on
`WL_CON0` alone (`tools/ble_ir.py --steps RF bt_clk_close RF_poweroff_recover`).

### C. The analog and modem block, 0x2FC00-0x2FCA0

**C.1 `bt_rf_analog_init`** (`analog.c`), in order. A software step first: `bt_fre_offset_value_init()` (not read), and
`rf`'s 243-word table is filled (F.2). Then, all from IR:

| Address | Value written | Note |
| --- | --- | --- |
| 0x2FC88 | address of the PLL table (F.2) | |
| (AGC) | `btagc_init()` (C.3) | |
| 0x2FC48 | 0xB9 | |
| 0x2FC00 | 0x00FFD144 | |
| 0x2FC04 | 0x4E143CDF | |
| 0x2FC08 | 0x03100000, or 0x32100000 when single-carrier mode is configured | bits 9:0 and 19:10 are the modem's TX I/Q offsets, see C.2 |
| 0x2FC0C | 0x80808080 | |
| 0x2FC10 | 0x80008080 | bits 7:0, 15:8, 23:16 are the TX I/Q compensation, see C.2 |
| 0x2FC14 | 0x80 | |
| 0x2FC18, 0x2FC1C | 0, 0 | |
| 0x2FC20 | 0x36555537 | |
| 0x2FC24 | 0x00034041 | |
| 0x2FC28 | 0x0005C000 | |
| (trim) | `update_mdm_txiq_cmp()` (C.2) | |
| 0x2FC9C | field bits 3-0 = 8 | |
| 0x2FC98 | field bits 7-0 = 0x36; bits 23-16 = 0x36; bits 15-8 = 0x02; bits 31-24 = 0x02 | one write each, in this order |
| 0x2FC9C | field bits 23-16 = 0x36; bits 31-24 = 0x36 | |
| (delay) | `delay(10)` | |
| 0x2FCA0 | field bits 7-0 = 0x36; bits 15-8 = 0x36 | |

**C.2 Trim and the two special modes** (`analog.c`):

- `update_mdm_txiq_cmp` and `bt_reload_trim` write 0x2FC10 bits 7-0 = `keep_BT_CMPUK1`, bits 15-8 = `keep_BT_CMPUK2`,
  bits 23-16 = `(-keep_BT_CMPUK3) & 0xFF`, and 0x2FC08 bits 9-0 = `keep_BT_OFSI & 0x3FF`, bits 19-10 = `keep_BT_OFSQ & 0x3FF`;
  `bt_reload_trim` then writes 0x2FC0C = 0x80808080, calls `bta_pll_config_init(0)` (F.2) and `rf_dc_set_reload()` (not read).
  The five `keep_*` values are RAM variables set outside the BLE members. [measure on FM-1] what they hold.
- `bt_set_single_carrier` (the unmodulated-carrier test): 0x2FC10 bits 23-0 cleared (three writes, 8 bits each),
  0x2FC08 bits 9-0 and 19-10 cleared, then 0x2FC08 set bits 9-8 (0x300) and bits 19-18 (0xC0000), 0x2FC0C = 0,
  `bta_pll_config_init(1)` (F.2), `rf_dc_set_max()` (not read).
- `bqb_2m_setting` writes 0x2FC24 = 0x006A4040; `bsb_normal_setting` writes 0x2FC24 = 0x006B4040 (both from IR; what the
  difference is [unchecked]).
- `bt_dut_reset_mdm`: 0x2FC08 clear bits 31-24, then write back what it read.

**C.3 AGC block** (`btagc.c`, `btagc_init`): `btagc_ram_init()` first (below); 0x2FC20 clear
bits 31-30; 0x2FD80 = 0x000F0000; 0x2FD84 = 0x1872BF14; 0x2FD88 = 0x1872BF55; 0x2FD8C = 0x1872BF00; 0x2FD90 = 0;
0x2FD94 = 0; `delay(100)`; 0x2FD80 set bit 0, clear bit 20; 0x2FC7C set bit 0. `analog_rx_control` reads 0x2FD8C (the
receive strength, `(0x3F ^ (reg | 0xFFC0)) - table[...]`, accumulated over 20 packets: a software RSSI average).

**`btagc_ram_init`, the gain table** (from IR, read 2026-10-27): 0x2FD98 = 0, then 128 writes to 0x2FD9C: a constant table
of 64 eight-byte entries (`agc_tlb`, in `.bt_rf_const`) walked from entry 63 down to entry 0, each entry's low word then
its high word. The 128 words, in the order written, are `firmware/ble/fm1ble_agc.h` (`tools/ble_ir.py --agc` prints them;
`--check` fails when the header differs). From use 0x2FD98 resets a write position and 0x2FD9C takes the next word
[unchecked]; what each word holds is [unchecked] (a receive gain table by its member and its place before the block's
enable). The driver left it out up to 3b39ab4, and the FM-1 received no packet in any advertising event (SCAN_REQs and a
CONNECT_IND from xlap; oplbox-ble-run-3b39ab4); the run's RX trials (docs/BLUETOOTH_PAGE.md) tell whether it is needed.

### D. The baseband control block, 0x28000-0x28054

**D.1 `le_hw_init`** (`RF_ble.c`) in order; all from IR:

1. 0x2FC80: set bit 0.
2. 0x28000 = 1.
3. 0x28034 = 0x0A01. (The direct-test variant writes 0x1401, D.5.)
4. Allocate the RAM block (E.1). Write its start (CPU address) to 0x2FC84 and its end (start + size) to 0x2FCBC.
5. Request IRQ 45 (`ble_event_irq_handler`) and IRQ 29 (`ble_rx_irq_handler`), both at priority 2.
6. 0x28000: set bit 13, set bit 14, clear bit 4.
7. 0x28008: field bits 9-0 = 0x46 (70), then set bits 15-12.
8. 0x2800C: field bits 9-0 = 0x46.
9. 0x28010: field bits 7-0 = 0x3E, field bits 15-8 = 0x3E.
10. 0x28014: field bits 7-0 = 0x1E (30).
11. 0x28018: field bits 7-0 = 0x14 (20), field bits 15-8 = 0x14.

The last five are timing constants in microseconds [unchecked]: `__hw_set_widen_drift` computes a window start as
`625 - (0x2800C & 0x3FF) - (0x28014 & 0xFF)` plus the clock count, which is a slot-relative offset (625 µs is one slot).

**D.2 The register file behind 0x28020 and 0x2801C (the "anchor" registers)**, from IR (`__set_ble_anchor_con`,
`__get_ble_anchor_con`). Per-link values are not memory-mapped: they are 16-bit registers of a small file addressed by
an index (0-15) and a hardware id (0-7):

- write: 0x28020 = value, then 0x2801C = `index << 10 | hw_id << 4 | 5`, with interrupts masked;
- read: 0x2801C = `index << 10 | hw_id << 4 | 2`; the value comes back in **0x28024** (the only function that reads
  0x28024 is `__get_ble_anchor_con`; [unchecked] whether the read needs a delay after the command). The direct-test code
  uses hw_id 0 and writes the constant 0x1805 (= index 6, hw_id 0, write) to 0x2801C.

What each index holds, from the setters that write it (names are ours, from use):

| Index | Written by | Value | From use |
| --- | --- | --- | --- |
| 0 | `__set_anchor_cnt` | (slots − 1) & 0xFFFF; 0 when slots is 0 | slot counter to the next event, low 16 bits |
| 0xE | `__set_anchor_cnt`, `__set_anchor_cnt_disable` | (slots − 1) >> 16 \| 0x8000; 0 to stop | high bits, bit 15 = counter enable |
| 7 | `__set_anchor_cnt` (writes 0 first), `__set_bitoffset` | bit offset | offset inside the slot |
| 1 | `__set_interval` | interval / 625 (slots), low 16 bits | |
| 0xF | `__set_interval` | `event_enable << 15 \| (interval / 625) >> 16` | bit 15 = `evt_en`: 1 for advertising and connections, 0 in the direct test [unchecked meaning] |
| 2 | `__set_hw_state` | `latency_en << 11 \| state << 12 \| latency` | state codes in D.3 |
| 4 | `__set_winoffset` | `winoffset \| 0x8000`, or 0 | transmit window offset |
| 6 | `__set_adv_channel_index`, `__set_scan_channel_index`, `__set_fixed_channel_index` | advertising: `hop_inc << 8 \| ((map & 1) ^ 1) << 6 \| (map >> 2) << 7 \| first_channel \| 0x4000`; scan: `ch \| 0x2100`; fixed: `ch \| 0x2000` | channel select |
| 8 | `__set_adv_random_slot` | 0xC000 | random delay on advertising events |
| 5 | `__set_event_instant` | the instant | connection update instant |
| 3, 9, 16 | connection setup and latency code | see `--steps RF_ble le_hw_ioctrl ble_event_irq_handler` | [unchecked meaning] |

**D.3 State codes** (written by `__set_hw_state`, index 2, bits 15-12), from the calls: 0 standby (`le_hw_standby`,
`__set_hw_state` is not called, the byte at +0x152 is set to 0), **2 advertising** (`le_hw_advertising`), 3 initiating
(`le_hw_initiating_common`), 6 connection, central side [unchecked which role] (also the direct-test transmitter), 7 connection,
peripheral side [unchecked] (also the direct-test receiver). Scanning writes its state in `le_hw_scanning_common` (not traced).

**D.4 Interrupt registers**, from use in `__set_hw_irq(hw_id, enable)`, `check_event_pending` and the two handlers:

| Register | Bits | Meaning from use |
| --- | --- | --- |
| 0x28028 | bit `id` (0-7) and bit `id+8` | enable: event done, and packet received, for hw id `id` |
| 0x28030 | same bits | pending flags (read; masked with 0x28028 by the handlers) |
| 0x2802C | same bits | clear: the handler sets the bit, `__set_hw_irq` sets it when enabling |
| 0x2804C | bit `id`; bit `id+8` | enable and clear for a second group (the anchor timeout, `ble5_*`) |
| 0x28050 | bit `id` | pending flags of the second group |
| 0x28038 | bit 1 | busy: `ble_hw_disable` polls until it is 0; bit 31 set raises the handler's "set bit 6" acknowledge; bits 8-9 and the value 0x1800 in bits 31-10 are the sleep handshake (D.6) |

The event handler (IRQ 45) loops over hw ids: if `0x28028 & 0x28030` has bit `i`, it sets bit `i` of 0x2802C, then runs the
per-state bookkeeping (the state byte at +0x152 selects it: it sends queued data again with `ble_hw_tx`, re-arms the scan
channel with `__set_scan_channel_index`, widens the window, sets the next priority). The
receive handler (IRQ 29) loops likewise on bit `i+8`, and for the id: reads RXPTR0/RXPTR1 (E.3), processes the RX
buffer(s) the hardware filled, then calls the TX bookkeeping.

**D.5 The direct-test variants** (`direct_test_dut.c`) write 0x28034 = 0x1401 (not 0x0A01), 0x28008 and 0x2800C field
0x44 (not 0x46), 0x28010 bits 7-0 = 0x64 and bits 15-8 = 0x7C (not 0x3E, 0x3E), the rest the same; this sets the
timing constants for the test's fixed spacing [unchecked].

**D.6 Sleep and resume** (`le_low_power.c`, written only if the driver supports low power): sleep probe: 0x28038 clear bit
2, set bit 5, set bit 0, then wait for bit 8 and expect bits 31-10 = 0x1800; timeout post: 0x28038 set bit 8, 0x28054 field
bits 12-0 = 0x96 (150), 0x28038 set bit 9, wait for bits 31-10 = 0x1800; resume: 0x28040 and 0x28044 = the clock count (low 16, high 16),
0x2803C = a remainder, 0x28038 set bit 2; 0x2FC6C bit 11 is polled by the post-suspend. A first driver can leave all of
this out.

### E. The RAM block the baseband reads

**E.1 The block** (`le_hw_init`, `alloc_baseband_hw_ram`), allocated once, all addresses relative to its start `S`:

| Offset | Size | Contents |
| --- | --- | --- |
| 0x00 | 8 × u16 | **instance table**: entry `n` is `((entity_addr − S) \| 0x8000)` for hw id `n`; 0 = unused. Written by `ble_hw_inst_set`; the value must be < 0xFFFF (the code asserts it) |
| 0x10 | pointer | SW: the array of per-link software records |
| 0x14 | pointer | SW: the array of entities |
| 0x18, 0x1C | pointers | SW: the RX and TX buffer areas |
| 0x20 | byte | SW: number of open links |
| 0x24 | `nums × 0x1C` | SW: one 28-byte record per link (first field: pointer to its entity) |
| then | `nums × 0x268` | **entities**, 616 bytes each: the per-link parameter block (E.2) at offset 0, then software state |
| then | `rx_nums × 0x126 + 0x23` | RX buffer area: buffers of 0x14 + length |
| then | TX area | TX buffers of 0x18 + length |

`nums` is `config_btctler_le_hw_nums`, `rx_nums` is `config_btctler_le_rx_nums`: configuration constants of the controller;
a driver chooses them. The block's start goes to 0x2FC84 and its end to 0x2FCBC (D.1 step 4). `PHY_TO_BLE(addr)` is
`addr − S` and is asserted to be below 0xFFFF; every pointer the hardware reads is therefore a 16-bit offset into
this block, and **the whole block must fit in 64 KB**. `BLE_TO_PHY` is the inverse.

**E.2 The per-link parameter block** (first 324 bytes of the 616-byte entity; offsets in bytes; names are the IR's;
`tools/ble_ir.py --layout`). "Set by" is the one setter that writes it in the advertising path:

| Offset | Name | Size | Set by / value in the advertising path |
| --- | --- | --- | --- |
| 0x000 | ANCHOR | u16 | 0 at open |
| 0x002 | BITOFF | u16 | 0 at open |
| 0x004 | ADVIDX | u16 | 0xD388 at open (bits 15-14 = 3, bits 13-0 = 5000); `le_hw_advertising` writes `(adv.byte4 << 14) \| (adv.word2 × 625)` (G step 5) [unchecked what the two fields are; 5000 µs = 5 ms is the default] |
| 0x006 | FORMAT | u16 | bit 3 set by `__set_local_addr`; bit 2 set when advertising starts (`le_hw_advertising`); DTM: bits 11-10 and bit 13 [unchecked meaning] |
| 0x008 | OPTCNTL | u16 | 0x28 at open, then bit 9 set; bit 4 cleared by `__set_local_addr_match_enable`; bit 3 cleared by `__set_remote_addr_match_enable` |
| 0x00A | BDADDR0 | u16 | **access address, low 16 bits: 0xBED6** for advertising (the field is named for a device address in the IR; its use is the access address: the direct test writes 0x4129 here and 0x7176 below, the access address 0x71764129) |
| 0x00C | BDADDR1 | u16 | access address high 16 bits: 0x8E89 for advertising |
| 0x00E | TXTOG | u16 | 0 |
| 0x010 | RXTOG | u16 | 0 |
| 0x012 | TXPTR0 | u16 | offset (E.1) of TX buffer 0's payload (`tx + 0x14`), or 1 when unused |
| 0x014 | TXPTR1 | u16 | same for TX buffer 1 |
| 0x016 | TXAHDR0 | u16 | advertising header of buffer 0: `((hdr >> 2) & 0x30) \| (hdr & 0xF)` where `hdr` is the buffer's word at +0x10 (E.4) |
| 0x018 | TXAHDR1 | u16 | same for buffer 1 |
| 0x01A | TXDHDR0 | u16 | data header of buffer 0 (see E.4); 1 at open |
| 0x01C | TXDHDR1 | u16 | same for buffer 1 |
| 0x01E | RXPTR0 | u16 | offset of RX buffer 0's data (`rx + 0x14`) |
| 0x020 | RXPTR1 | u16 | same for RX buffer 1 |
| 0x022 | WINCNTL0 | u16 | window size, low 16 bits: 0x32 (50) when advertising |
| 0x024 | WINCNTL1 | u16 | window size, high 16 bits: 0 |
| 0x026-0x02C | RXAHDR0/1, RXDHDR0/1 | u16 ×4 | written by the hardware: the header word of the received advertising or data PDU for buffer 0 / 1 |
| 0x02E | CHMAP0 | u16 | channel map bytes 0 and 1 (byte 0 low) |
| 0x030 | CHMAP1 | u16 | channel map bytes 2 and 3 |
| 0x032 | CHMAP2 | u16 | `(used_channel_count << 5) \| (map byte 4 & 0x1F)` |
| 0x034 | LASTCHMAP | u16 | read by the receive handler: the channel the last packet came on [unchecked] |
| 0x036 | RXMAXBUF | u16 | 0xFF at open: the longest PDU accepted |
| 0x038, 0x03A | RXSTAT0/1 | u16 | written by the hardware per RX buffer: bits 3-0 = PDU type, bit 4 (0x10)... (E.5) |
| 0x03C | FILTERCNTL | u16 | 0 at open; bit 3 set when the advertising filter policy is 1 or 3, bit 4 when it is 2 or 3 (E.6) |
| 0x044 | CRCWORD0 | u16 | CRC init low 16 bits: 0x5555 |
| 0x046 | CRCWORD1 | u16 | CRC init high 8 bits: 0x55 (advertising) |
| 0x048 | WIDEN0 | u16 | window widening, `widen / 625` (slots); 0 at open |
| 0x04A | WIDEN1 | u16 | `(widen % 625) \| 0x5000`; 0x51E7 at open (the advertising value: `0x1E7` = 487) |
| 0x04C-0x050 | TARGETADRL/M/U | u16 ×3 | the peer address (6 bytes, little endian), 0 at open |
| 0x052-0x056 | LOCALADRL/M/U | u16 ×3 | the local address (6 bytes), set by `__set_local_addr` |
| 0x058 | EVTCOUNT | u16 | event counter, read by the receive handler |
| 0x05C | IFSCNT | u16 | 0x8295 at open [unchecked: inter-frame spacing] |
| 0x05E | RFPRIOSTAT | u16 | priority status; 1 at open (0x1E in the direct test) |
| 0x060 | RFPRIOCNTL | u16 | priority control; 0x101 at open (0x21E in the direct test); `__set_le_prio` writes the priority here |
| 0x062 | INTFRAME | u16 | bits 1 and 2 set at open |
| 0x064, 0x065 | TXBUF0CNTL, TXBUF1CNTL | u8 | |
| 0x066, 0x067 | RXBUF0CNTL, RXBUF1CNTL | u8 | bit 0 cleared at open |
| 0x068, 0x090, 0x0B8, 0x0E0 | four tables of 40 bytes | u8 ×40 | the channel tables (F.1) |
| 0x108-0x110, 0x114-0x118 | EXTPHYDRF0/1, EXTIFSDRF0/1, EXTPKTDRF, EXTCTL0/1, … | u16 | LE 5 extended and coded-PHY fields: not used by advertising on 1M |
| 0x112 | WINCNTL2 | u16 | **the receive window, written on every path** (from IR, read 2026-10-27): `le_hw_advertising` writes 0x32 here as well as WINCNTL0 (G step 2); `__set_winsize(hw, nwinsize, winsize)` writes WINCNTL0/1 = `nwinsize` and WINCNTL2 = `winsize`, and every caller passes `nwinsize` = 0x32 (50): scanning (`__set_scan_window`) with `winsize` = the scan window in slots × 625, a connection (`le_hw_ioctrl`) with the transmit window `(WinSize + 1) × 1250` and later windows; the direct-test receiver writes 0x96. So WINCNTL2 is the time the receiver listens, in µs from those uses [unchecked unit]; WINCNTL0 stays 0x32. 0 up to 3b39ab4 in OPLBOX's driver (see C.3's last paragraph) |
| 0x120, 0x128 | (no IR name) | u32 ×2 | **TX power for buffer 0 and 1**: `0x10 + level` (E.7) |
| 0x130 | PLL_COMP | i32 | `(−rf.field13 << 20) / 24000`; `rf.field13` is the crystal offset (0 at reset) |
| 0x13C | RSSI2 | u16 | |

Everything above offset 0x144 and the rest of the entity (up to 616) is software state (`ble4_hw`: state byte at +0x152,
latency, buffer pointers at +0x178..+0x184, the advertising data length at +0x20E and the data at +0x20F, the scan
response length at +0x22E and data at +0x22F, the pending-TX flags at +0x24E) and is never read by the hardware as far as
the IR shows [unchecked]. The three bytes `0x250`-`0x254` are a copy of CHMAP.

**E.3 Open sequence of one entity** (`le_hw_open`, `__set_hw_frame_init_4_2`): allocate a record and an entity;
write the entity's offset into the instance table (E.1) and the entity's first word; clear all of E.2's words to the
values in the table above (0 unless stated: ANCHOR, BITOFF, ADVIDX = 0xD388, WIDEN0 = 0, WIDEN1 = 0x51E7, access
address 0x8E89BED6 in BDADDR1:BDADDR0, CRCWORD = 0x555555, OPTCNTL = 0x28 then bit 9, TXPTR0 and TXPTR1 = 1, INTFRAME bits
1 and 2, RXBUF0CNTL and RXBUF1CNTL bit 0 cleared, PLL_COMP, RFPRIOSTAT = 1, RFPRIOCNTL = 0x101, TXTOG, RXTOG, TXAHDR0/1 = 0,
TXDHDR0 = 1, TXDHDR1 = 1, RXMAXBUF = 0xFF, IFSCNT = 0x8295, FILTERCNTL = 0, TARGETADR = 0); apply the AGC and TX power words
(E.7); fill the channel tables for all 37 data channels and the three advertising channels (F.1); then put the link in
standby (state byte 0, `ble_hw_disable`). It does not start anything.

**E.4 TX buffers** (`struct ble_tx`, allocated 0x18 + payload length): offset 0x00 a list head (software, 8 bytes), 0x08 and 0x0C
two software pointers, **0x10 a 16-bit header word**, **0x12 an 8-bit payload length**, 0x13 a byte (software), **0x14 the
payload**. Header word bits (from `__adv_pdu` and `ble_hw_tx`): bits 3-0 PDU type (0 ADV_IND, 1 ADV_DIRECT_IND, 2 ADV_NONCONN_IND,
4 SCAN_RSP, 6 ADV_SCAN_IND, written as `adv_type & 0xF`), bit 6 TxAdd (the local address type), bit 7 RxAdd (the peer's, for
ADV_DIRECT_IND), bit 5 is where `set_tx_pdu_channel_selection_algorithm` (not read) works [unchecked: the ChSel bit]. The **payload begins with the
6-byte advertiser address, which software copies in** (from the entity's local address); the advertising data follows;
and the length byte counts both (`adv_len + 6`). ADV_DIRECT_IND payload = advertiser address then initiator address (length 12).
`ble_hw_tx(buffer, index)` writes the offset of `buffer + 0x14` to TXPTR0 or TXPTR1 (only when the length is nonzero),
TXAHDR = `((hdr >> 2) & 0x30) | (hdr & 0xF)` and **TXDHDR = `length << 8 | (old TXDHDR & 4) | ((hdr >> 6) & 8) | ((hdr >> 4) & 3)`**
(from IR, read 2026-10-27 from the `ble_hw_tx` body's stores: `length` is the buffer's byte at +0x12, 0 when the length is 0),
for advertising buffers as much as for data ones: the PDU's length goes into TXDHDR bits 15-8, as the received length comes
back in RXDHDR bits 15-8 (E.5). [unchecked] whether the hardware takes the length from TXDHDR or from +0x12; JieLi writes both.
`ble_hw_tx` is called with the buffer index the hardware will send next: see "TX and RX toggles" below. The
scan response is TX buffer 1 with type 4 and length `scan_rsp_len + 6`; for ADV_DIRECT_IND buffer 1 is a second copy.

**E.5 RX buffers** (`struct ble_rx`, allocated 0x14 + 0xFF for advertising): offset 0x00 list head (software); **0x08 a 16-bit word
the software builds from the hardware's RX status words**: bits 3-0 = RXAHDR bits 3-0 (PDU type); bit 6 = RXAHDR bit 7;
bit 7 = RXAHDR bit 6 (the two address-type bits swapped: software later compares bit 7 with the peer's address type);
bit 10 = RXDHDR bit 4; bit 9 = RXDHDR bit 3; bit 8 = RXDHDR bit 2; bits 5-4 = RXDHDR bits 1-0; RXAHDR bit 5 goes to
`record_rx_chsel_algorithm`; 0x0A the payload length (RXDHDR bits 15-8); 0x0B a byte copied from a
software field; 0x0C the RSSI (`ble_analog_get_rssi`, the AGC block, C.3); 0x0E the event counter; 0x10 four bytes; **0x14 the
received payload** the hardware writes (for CONNECT_IND: the initiator address at +0x14, the advertiser address at +0x1A,
then the connection parameters, as the specification lays them out). The hardware reports each packet through
RXAHDR/RXDHDR/RXSTAT of the entity (buffer 0 or 1) and the "packet received" interrupt bit; software reads those words
and copies the fields above. RXSTAT: bit 8 is "this packet acknowledges ours" (`rx_ack`), bits 3-0 give the result
(1 = good [unchecked], bit 2 and bit 3 = error classes that make software set the length to 0). [unchecked] the exact bit table.

**E.6 `FILTERCNTL`** (`__set_adv_device_filter_param`): for filter policy 0 neither bit; 1: bit 3 (0x08); 2: bit 4 (0x10); 3: both.
`OPTCNTL` bit 3 cleared = remote address match enabled; bit 4 cleared = local address match enabled (`__set_remote_addr_match_enable`,
`__set_local_addr_match_enable`) [from IR; the polarity is read from the clear, the meaning is [unchecked]]. `FORMAT` bit 8 cleared
= scan request enabled, `__set_scan_req_enable`.

**E.7 TX power and AGC words** (`analog_ble.c`): `ble_txpwr_normal_set(entity, select, level)` stores a software byte (+0x1CF),
clamps the level to 0..`bt_get_pwr_max_level()` and to `rf.field0 >> 12` (the chip's maximum BLE level, 0 at reset), and writes
**0x120 and 0x128 = 0x10 + level** as 32-bit words. `ble_agc_normal_set` writes a software byte (+0x1D0) from a field of the block
(bits 13-8 of the word at 0x13C) and no register. Both are called at open with level 25 for AGC and the maximum for power.

**TX and RX toggles** (from IR, read 2026-10-27: `__set_hw_frame_init_4_2`, `ble_event_irq_handler`, `ble_rx_irq_handler`).
TXTOG (+0x0E) and RXTOG (+0x10) are written by software once, 0 at open, and never again; both handlers read them. The event
handler, in the advertising state, re-points the two TX buffers at the end of every event: the advertising PDU with
`ble_hw_tx(..., TXTOG & 1)` and the second PDU (the scan response) with index `(TXTOG & 1) ^ 1`. The receive handler takes the
RX buffers in the order `RXTOG & 1`, then the other. So the hardware changes TXTOG and RXTOG [from use: the bit table is
unchecked], and bit 0 of TXTOG names the TX buffer it sends next. Two consequences for a driver: (1) after each advertising
event the advertising PDU must be at TXPTR[TXTOG & 1], or the next event sends the scan response (or whatever the other
buffer holds) as its advertisement; (2) a change of TXTOG is the hardware's own record that it sent a buffer, the only
transmit evidence in the IR (there is no transmit interrupt: D.4 has the event-done and packet-received bits only).

**The TX slots in a connection** (from IR, read 2026-10-28: `tools/ble_ir.py --steps RF_ble __hw_rx_buf_process ble_hw_tx
__hw_tx_process ble_event_irq_handler`). On a CONNECT_IND the receive path writes TXAHDR0 and TXAHDR1 = `(rx word >> 2) &
0x10` (the ChSel bit; 0 for channel selection algorithm 1), **TXDHDR0 = `((TXTOG << 2) & 4) | 1` and TXDHDR1 =
`((TXTOG << 2) & 4) ^ 5`**: two empty PDUs (LLID 1) whose bit 2 differs, clear in the slot TXTOG & 1 names (the one sent
first), and frees the advertising buffers. `ble_hw_tx` keeps TXDHDR bit 2 when it fills a slot (`old & 4`, E.4), puts the
buffer header's bit 9 in bit 3 (the bit the receive path fills from RXDHDR bit 3, the SN on the air: E.5) and, in a last
store, a per-link software flag in bit 4. So bit 2 is the slot's own and the hardware's [unchecked: NESN put in by the
hardware, or a sequence bit per slot]; software does not write it after the CONNECT_IND. After every packet received the
receive handler calls `__hw_tx_process`, which reads TXTOG bit 0, TXBUF0CNTL bit 0 and TXBUF1CNTL bit 0 (+0x064, +0x065)
and calls `__hw_tx_buf_proess` for the slot TXTOG & 1 names and for the other: JieLi refills the TX slots from its queue
after a packet, ahead of the next one, not between a packet and T_IFS. In a connection the event handler
(`ble_window_autozoom`) writes the window for the next event into WINCNTL0/1 (+0x22, +0x24), or through
`__hw_set_widen_drift` (window + 2 × (widening + 100 us), the widening growing by 100 us per missed event; 5 us with
`config_vendor_le_bb` set), which also writes index 7 (the bit offset | 0x8000) and index 0x10. OPLBOX's driver up to
f0e6d08 wrote a response into both slots with TXDHDR = length << 8 | the header's bits 4-0 (Cordio's NESN over bit 2)
after the packet came; fm1ble.h FM1BLE_TX_JIELI and FM1BLE_TX_INTIME write the IR's words.

**The TX power level** (`cpu/wl82/liba/btctrler.a` `RF.c.o` `bt_max_pwr_set`, from IR; `apps/common/config/user_cfg.c`, SDK
source): `bt_max_pwr_set(pwr, pg_pwr, iq_pwr, ble_pwr)` stores `ble_pwr << 12 | (iq_pwr & 0xF) << 8 | pg_pwr << 4 | (pwr & 0xF)`
as `rf` field 0, so `rf.field0 >> 12` in E.7 is the BLE level the application set. The SDK's applications call it with
`ble_power` = 6 unless the configuration store holds another value (user_cfg.c: "0-11", default 6), so JieLi's BLE advertises at
TXPWR = 0x16. Level 0 (0x10), which a driver that reads `rf.field0` as 0 at reset ends up with, is the lowest level.

### F. Channels and the PLL table

**F.1 Channel tables** (`__set_channel_map`, `__dut_set_channel_map`): four 40-byte tables in the parameter block. For data channel
`i` (0-36): table 0x068 = `i`; table 0x0B8 = **frequency offset from 2402 MHz in MHz** = `(i + 1) × 2` for channels 0-10 and
`(i + 1) × 2 + 2` for channels 11-36, i.e. channel 0 = 2404 MHz and channel 11 = 2428 MHz. Used channels (map bit set) are
also written compactly: table 0x090 = `i`, table 0x0E0 = the same offset, with `CHMAP2`'s count. Advertising channels live at
indices 37-39 of all four tables: table 0x068 and 0x090 = 37, 38, 39; table 0x0B8 and 0x0E0 = **0, 24, 78** (2402, 2426, 2480 MHz).
`get_bta_pll_bank` is called for every entry (with the byte `81 + 2i`, or `83 + 2i` from channel 11 on, and `2 × offset + 79` for the three
advertising channels) but its result is not stored in the parameter block.

**F.2 The PLL table** (`bt_rf_analog_init`, `bta_pll_config_init(single_carrier)`): in the controller's RAM, a table of 81 entries of
3 words (243 words, `rf` field 21); 0x2FC88 is set to its address. Entry `n` (0-80, the frequency offset in MHz from 2402):
word 0 = `(n << 8) | n`; words 1 and 2 = 0, or 0xFFFFAAAB each when `single_carrier` is set. `get_bta_pll_bank(freq)` returns
`table[31] − i + 1` for the first index `i` in 1..30 whose byte in `rf` field 20 exceeds `freq`, else 0; that 32-byte
table is zero at reset and is filled by code outside the BLE members. [measure on FM-1] whether an all-zero bank table
tunes the right channel; if not, the table is calibration data like B's.

### G. Advertising: the order JieLi uses

`le_hw_advertising(link, parameters)` after `le_hw_open` and `__set_local_addr` (E.3). All from IR:

1. `__set_le_prio(link, 0x1A)` (0x13 when classic Bluetooth is in sniff mode): the priority goes to RFPRIOCNTL/RFPRIOSTAT (E.2).
2. CRCWORD0 = 0x5555, CRCWORD1 = 0x55; WINCNTL0 = 0x32, WINCNTL1 = 0, **WINCNTL2 = 0x32** (and a software copy at +0x14C).
3. `__set_interval(link, interval_slots × 625, 1)`: index 1 = slots, index 0xF = `0x8000 | slots >> 16` (D.2).
4. `__set_hw_state(link, 2, 0, 0)`: state 2 (advertising), no latency (D.2/D.3).
5. `__set_adv_device_filter_param`: FILTERCNTL bits 3 and 4 (E.6). ADVIDX = `(byte 4 of the advertising parameters << 14) | (word 2 of
   them × 625)` [unchecked what the two fields are; 625 = 0x271].
6. If the interval is above 10 slots (6.25 ms) and the type is not ADV_NONCONN_IND: `__set_adv_random_slot`: index 8 = 0xC000.
7. `__set_adv_channel_index(map)`: index 6 gets the hop value and the first channel (D.2).
8. Allocate two RX buffers (0xFF payload, E.5) and set RXPTR0/RXPTR1 (`ble_hw_rx`, offsets).
9. Allocate two TX buffers (payload 0x25 = 37 bytes), build the advertising PDU in buffer 0 (`__adv_pdu`: type, `adv_len + 6`, TxAdd,
   advertiser address, data), the second PDU (ADV_DIRECT_IND repeats, otherwise the scan response with type 4 and length
   `scan_rsp_len + 6`) in buffer 1, and `ble_hw_tx(link, buffer, 0)`, `ble_hw_tx(link, buffer, 1)` (E.4).
10. A software byte (+0x24E, pending-TX flags) = 0; `__set_local_addr_match_enable` (OPTCNTL bit 4 cleared).
11. `ble_power_get(link)` (the low-power gate: keeps the clock running while the link is enabled).
12. `ble_hw_enable(link, 1)`: enable the link's interrupts (`__set_hw_irq(id, 1)`: D.4) and `__set_anchor_cnt(link, 1)`: index 7 = 0,
    index 0 = 0, index 0xE = 0, then index 0 = 0, index 0xE = 0x8000 (slot 1 − 1 = 0, counter enabled): the first event is armed for the next slot.
13. If more than one link is open, `ble_slot_schedule()` arbitrates the slots (not needed with one link).
14. Last write: `FORMAT` bit 2 set (after the link is enabled).

After each event the event interrupt (IRQ 45) runs its per-state bookkeeping (D.4). Whether the hardware repeats the event every interval on its own
(index 0xF bit 15 was set in step 3) is [unchecked]; the direct test (K) uses the interrupt path instead.

To stop: `le_hw_close` → `suspend_init`, `ble_power_put`, `ble_hw_disable` (index 0xE = 0, then wait until 0x28038 bit 1 clears, then
`__set_hw_irq(id, 0)`), free the buffers, clear the instance table entry.

### H. Receive and connection (summary)

On a received packet (IRQ 29, bit `id+8` of 0x28030): the handler copies RXAHDR/RXDHDR/RXSTAT of the buffer the hardware used (0 or 1) into
the software RX buffer (E.5), refills RXPTR with a fresh buffer, and for a CONNECT_IND in advertising state runs `set_hw_channel_selection_algorithm`
and sets the peer address (`TARGETADR*`) and TXAHDR/TXDHDR for the connection, frees the advertising buffers; the connection's own parameters
are then written by `le_hw_ioctrl` through `__set_local_addr`, `__set_peer_addr`, `__set_event_instant`, `__set_winsize`,
`__set_channel_map` (CHMAP and the channel tables), `__set_conn_dead_times`, `__set_winoffset`, `__set_widen`, `__set_interval` (slots × 625 → index 1/0xF),
`__set_ble_anchor_con(hw, 6, hop/first channel)`, `__set_hw_state(7, latency_en, latency)` for a peripheral and the same for a central with state 6
(`tools/ble_ir.py --steps RF_ble le_hw_ioctrl`). The hop increment sits in index 6 bits 12-8 and the next channel is computed by the hardware.
The 150 µs turnaround, retransmission and acknowledgement bits (`TXTOG`/`RXTOG`) are hardware [unchecked bit table]. The receive window of a
connection event is WINCNTL2 (E.2), with WINCNTL0 = 0x32.

### J. The operation table `ble_ops` (the interface the baseband port exposes)

`RF_ble.c` ends in a constant table of 17 function pointers, in this order, which the link layer calls instead of touching the hardware: init
(`le_hw_init`), destroy, open, close, standby, advertising, tws advertising, scanning, tws scanning, initiating, handler register, send packet,
ioctrl, upload data, is-init-enter-connection, send packet (high priority), send single carrier. This is the boundary Cordio's hardware layer
replaces: every fact in D, E and G is behind one of these entries. The ones OPLBOX's first driver needs: init, open, close, standby,
advertising, send packet, ioctrl (connection parameters).

### K. The smallest transmit sequence in the IR: the direct-test transmitter

`le_transmitter_test` (`direct_test_dut.c`) transmits fixed packets on a fixed channel with no link layer: HCI LE Transmitter Test. In order, all from IR:

1. `bt_set_single_carrier()` (C.2) for the unmodulated-carrier packet type (0xF0, read from the compare, the branch not traced), `bt_reload_trim()` (C.2),
   and `bt_pll_reset(1)` (B.3) when the FCC mode is set.
2. Open the link with the direct-test variants (`dut_le_hw_open`: the D.1 writes with D.5's constants, `dut_hdl` block, E.1 with the same sizes, IRQs 45 and 29).
3. `le_test_frame_init(level)`: the E.2 defaults of E.3 except ADVIDX = 0xD388, WIDEN1 = 0x5014, access address 0x71764129 (BDADDR0 = 0x4129, BDADDR1 = 0x7176),
   CRCWORD = 0x555555, OPTCNTL = 0x3E, INTFRAME bits 1 and 2, FORMAT bit 5 cleared, TX power words (E.7), RFPRIOCNTL = 0x21E, RFPRIOSTAT = 0x1E, RXMAXBUF = 0xFF,
   IFSCNT = 0x8295; then the channel tables (F.1; the offset gets 2 MHz added for LE 2M).
4. FORMAT: bit 13 cleared; 0x28000 set bit 2; `hw_udelay(10)`; `__set_fixed_channel_index(ch)` (index 6 = `ch | 0x2000`); 0x28000 clear bit 2. [unchecked: this pulse
   latches the channel into the synthesiser]
5. Allocate one TX buffer of the test length; TXPTR0 and TXPTR1 = its offset; TXAHDR0 = TXAHDR1 = the payload pattern code; TXDHDR0 = TXDHDR1 = `length << 8`.
6. `__set_hw_state(6, 0, 0)`; `__set_interval(interval_slots, 0)`; 0x2802C set bits 0 and 8; 0x28028 set bits 0 and 8; `__set_anchor_cnt(slots)`.

This is the sequence to reproduce first on the FM-1: it needs no receive path, no scheduler, one buffer and nine registers, and a spectrum analyser or a phone's
BLE scanner at 2402 MHz (channel 37) shows whether it works. (The direct-test transmitter sends data PDUs with the test access address, not ADV_NONCONN_IND; a
phone scanner will not list it, a spectrum analyser or a second FM-1 in receive-test mode will see it.)

## What Cordio's hardware layer needs, and what fills it

Cordio's functions are in `platform/include/pal_*.h` (names only were read). "Facts" says which section above fills it. Cordio's controller calls them with
absolute times and whole packets; the baseband counts 625 µs slots and runs the event itself, so most `PalBb*` functions are a translation, written by us
from the Core Specification and these facts.

| Cordio function | What it must do | Filled by | Left for the FM-1 |
| --- | --- | --- | --- |
| `PalBbInit`, `PalBbBleInit` | one-time hardware init | B.1, B.2, C.1-C.3, D.1, E.1, E.3 | the order against `fm1wifi_radio_up` (not settled 1); AGC; calibration inputs (not settled 2) |
| `PalBbEnable`, `PalBbBleEnable` | clocks and interrupts on | B.1 step 4-17, D.4 | |
| `PalBbDisable`, `PalBbBleDisable` | clocks off | B.5 | |
| `PalBbLoadCfg` | take the controller's configuration (buffer counts, RF path compensation) | E.1 (`nums`, `rx_nums`), E.7 | the RF path compensation: no value in the IR [measure on FM-1] |
| `PalBbRegisterProtIrq`, `PalBbSetProtId` | route the timer and radio interrupts to the controller | D.4 (IRQs 45 and 29, priority 2) | |
| `PalBbGetCurrentTime`, `PalBbGetTimestamp`, `PalTimerInit/Start/Stop/GetCurrentTime` | a free-running microsecond counter and a one-shot compare | partly: the slot counter is anchor index 0/0xE (D.2) and the bit offset index 7 | whether the counter free-runs and at what rate; the register that holds sub-slot time [measure on FM-1]; the MCU's own timer (a `JL_TIMER`, outside the BLE members) is the fallback |
| `PalBbBleSetChannelParam` | access address, CRC init, channel, PHY, TX power | AA: BDADDR0/1; CRC: CRCWORD0/1; channel: index 6 (D.2) and F.1 tables; power: 0x120/0x128 (E.7); PHY 1M only | whitening channel; 2M and coded PHY need `RF_ble5.c` facts (not read) |
| `PalBbBleSetDataParams` | connection parameters: hop, interval, window, latency | D.2, H | |
| `PalBbBleSetOpParams` | advertising/scan/connection kind and timing | D.3 state codes | scan states (state codes for scanning) |
| `PalBbBleTxData`, `PalBbBleTxTifsData` | transmit a packet now / 150 µs after the last receive | E.4, G steps 8-12, K | "now" means: choose slot count and bit offset for the target time; TIFS is the hardware's own turnaround, so a TIFS transmit would be the second TX buffer of the same event (our design from G step 9, not a fact of the IR) |
| `PalBbBleRxData`, `PalBbBleRxTifsData` | receive a packet | E.5, RXPTR/RXMAXBUF, H, WINCNTL2 (E.2) | whether Cordio's timeout in WINCNTL2 is the window the chip opens (µs taken from use) |
| `PalBbBleCancelData`, `PalBbBleCancelTifs` | abort | G stop sequence | |
| `PalBbBleEnableDataWhitening`, `PalBbBleEnablePrbs15` | test modes | `FORMAT` bits in K (bit 13) | bit table [measure on FM-1] |
| `PalBbBleInlineEncryptDecryptSetDirection`, `…SetPacketCount` | hardware CCM | not read (`ble_hw_encrpty` uses `ccm_memory_v2`) | only for connections with encryption; none for MIDI without pairing |
| `PalBbBleLowPower` | sleep | D.6 | not needed first |
| `PalCfgLoadData` | device address, UUID | none (the address is software: LOCALADR) | where the FM-1 keeps a device address: nowhere known; make one up and store it |
| `PalCryptoGenerateRandomNumber` | random numbers | 0x13B00 is read by `pseudo_random_genrate` [unchecked] | what 0x13B00 is; OPLBOX has `firmware/hal/fm1_rand.h` already |
| `PalCryptoAesEcb`, `PalCryptoAesCmac`, `PalCryptoGenerateP256…` | AES and ECC | none | only with pairing; BearSSL's AES is in `firmware/wifi/bearssl/` |
| `PalRadioGetActualTxPower`, `PalRadioGetSupTxPower`, … | TX power levels in dBm | E.7: level 0..`rf.field0 >> 12` | the dBm per level: no table in the IR for BLE [measure on FM-1] |
| `PalSys*` | critical sections, assert | none (CPU interrupt masking, outside the radio) | |

## Facts count

Counted by a script over sections B to K of this file: 109 table rows (a row is one register, one field, one state code or one
region) and 38 numbered steps, 147 in all. The memory-map table above adds its own rows and is not counted. `tools/ble_ir.py --check`
re-derives 30 constants, 15 layout offsets and the AGC gain table against `firmware/ble/fm1ble_agc.h` (46 facts), the ones the
advertising and receive paths depend on.

## Reproducing

```sh
python3 -I tools/ble_ir.py --check                       # 46 facts reproduce; fails on another archive
python3 -I tools/ble_ir.py --agc                         # the AGC gain table, 128 words in the order written
python3 -I tools/ble_ir.py --layout                      # E.2: field, offset, size, IR name
python3 -I tools/ble_ir.py                               # B, C, D, G, K step views (about 1300 lines)
python3 -I tools/ble_ir.py --registers                   # every address above 0x10000 by member and function
python3 -I tools/ble_ir.py --steps RF_ble le_hw_ioctrl   # any function of one member
```

It needs `ar`, a system `llvm-dis` (LLVM 23 was used) and the SDK at `$AC79_SDK`.

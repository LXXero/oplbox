# Bluetooth LE MIDI on code OPLBOX can ship

OPLBOX is GPL-3.0-only. The goal here is BLE MIDI: the FM-1 shows up as a Bluetooth MIDI device to a Mac, an iPhone,
a Windows PC or an Android phone, and MIDI flows both ways beside USB and TRS. This file is stage 0, the map, in
the same shape as `docs/WIFI.md`: what the SDK's Bluetooth libraries are, what is known about their licence, what
the controller's hardware looks like from the SDK's own C sources, which host stacks could sit on top, what it all
costs against OPLBOX's free space, and a plan in stages small enough for one task each.

Read on 2026-10-25. "[unchecked]" marks what nobody has verified. This is evidence, not a legal opinion. Nothing
below was run on an FM-1.

| Source | Version read |
| --- | --- |
| JieLi AC79 SDK, `~/fw-AC79_AIoT_SDK` | d179b448, tag `AC79NN_SDK_V1.2.1_2023-12-13` |
| fm1-radio, https://github.com/charlesvestal/fm1-radio, `notes/01`-`06` and `LICENSING.md` | c9113b5 (2026-10-24) |
| fm1-base, https://github.com/charlesvestal/fm1-base, `ble/` directory listing only | aa2b3bc |
| Apache NimBLE, https://github.com/apache/mynewt-nimble (sparse checkout of `nimble/host`, `nimble/include`, `porting/`, `ext/tinycrypt`) | e00b85cc (2026-10-23) |
| BlueKitchen BTstack, `LICENSE` only | `master`, read 2026-10-25 |

How it was measured: the SDK archives were unpacked with `ar x`. Almost every member is LLVM bitcode, so `llvm-dis`
and `llvm-nm` (system LLVM) give function names, types and source file names. `strings -n 4` was counted for
copyright, licence and third-party names the way WIFI.md did. NimBLE was compiled with JieLi's own `pi32v2/bin/clang`
at `-Os -ffunction-sections` (the flags `tools/build.py` uses). The commands are at the end.

## Short answer

- **Controller.** JieLi's `btctrler.a` is the only Bluetooth controller we have. It is binary only and speaks
  standard HCI through a function-call transport, so any HCI host can sit on top of it. Its licence label (Apache-2.0
  on the SDK's root `LICENSE`) is as unproven as the WiFi libraries' (see "Licence provenance"). Writing our own
  controller needs the baseband's registers, which no SDK C source defines: that is a large job with a lot of
  FM-1 time, and it is not planned here.
- **Host.** JieLi's `btstack.a` uses the public API names of BlueKitchen's BTstack, whose licence is not
  GPL-compatible. Do not use it. Recommended: **Apache NimBLE's host, peripheral-only, pairing off at first**
  (Apache-2.0, compatible with GPL-3.0), with the BLE-MIDI service written by us. Measured: 41 KB of code and 6 KB of
  static RAM before linking. A hand-written minimal ATT/GATT host would be smaller (about 4 KB, an estimate) and is the
  fallback.
- **Cost.** fm1-radio measured JieLi's whole BLE stack at +124 KB of code. OPLBOX has 238,772 B free, and the WiFi
  plan targets under 120 KB. Both together do not fit unless one of them shrinks (see "Flash and RAM").
- **Blocker for shipping.** A GPL-3.0 binary that contains the controller archive cannot offer its source. The
  diagnostic and host stages need no decision. Shipping it needs xero's decision (options in "Open questions").

## Licence provenance

### What the SDK says

- The only licence statement is the SDK root `LICENSE`: the Apache License 2.0, text only, with the appendix's
  template line `Copyright [yyyy] [name of copyright owner]` filled in for nobody (as WIFI.md found).
- The Bluetooth headers that ship with the SDK carry a JieLi copyright line instead: `Copyright:(c)JIELI
  2011-2016/2017/2019/2020 @ , All Rights Reserved.` (18 lines across `include_lib/btstack` and
  `include_lib/btctrler`). No file in those two directories names Apache-2.0, BSD, GPL or BlueKitchen.
- The Zephyr-derived Bluetooth mesh sources the SDK ships as source (`lib/utils/btmesh`, `include_lib/utils/btmesh`)
  do carry notices: `SPDX-License-Identifier: Apache-2.0`, `Copyright (c) 2017 Intel Corporation` and a BSD-style
  block.

### What the archives carry

`strings -n 4` on each archive (case-insensitive unless noted):

| Archive | Members | Size | "copyright" | "GPL"/"GNU" (case-sensitive) | "licen[cs]e" | Third-party names found |
| --- | --- | --- | --- | --- | --- | --- |
| `btctrler.a` | 106 + 1 | 5,157,416 | 0 | 0 | 0 | `uECC` (11): micro-ecc |
| `btstack.a` | 70 + 1 | 3,064,140 | 0 | 1 (inside a vCard identifier, not a licence) | 0 | BTstack public API names (below) |
| `rcsp_stack.a` | 5 | 91,038 | 0 | 0 | 0 | none (JieLi's phone-app protocol) |
| `lib_sig_mesh.a` | 29 + 1 | 1,187,436 | 0 | 0 | 0 | `tinycrypt` (5), Zephyr file names |
| `crypto_toolbox_Osize.a` | 12 + 1 | 421,662 | 0 | 0 | 0 | `uECC` (108) |
| `lib_ccm_aes.a` | 3 + 1 | 33,128 | 0 | 0 | 0 | none (mbedTLS-style file names `aes.c`, `aes_tab.c`, `ccm_memory.c`) |
| `wl_rf_common.a` | 13 + 1 | 321,802 | 0 | 0 | 0 | none (WIFI.md has the member-by-member result) |

Not Bluetooth, to avoid mixing them up: `profile.a` is JieLi's cloud-account profile (`internet_profile/profile.c`,
an AI-service licence key check). `libmatter.a` (578 members, 128 MB) is the Matter stack; it was listed, not opened.

### The host: `btstack.a` is BTstack-shaped

- Its exported names are BlueKitchen BTstack's public API: of 1,931 defined symbols, 83 start with `att_`, 68 with
  `gatt_client`, 69 with `l2cap_`, 110 with `sm_`, 68 with `btstack_`, 43 with `rfcomm_`, 71 with `sdp_`. Spot checks
  that exist as exported functions: `att_set_db`, `att_set_read_callback`, `att_set_write_callback`,
  `att_server_init`, `att_server_notify`, `att_server_can_send_packet_now`, `gatt_client_discover_primary_services`,
  `sm_set_authentication_requirements`, `btstack_linked_list_add`.
- Its member names are BTstack's source file names: `att_db`, `att_server`, `att_dispatch`, `gatt_client`, `sm`,
  `gap`, `l2cap`, `l2cap_signaling`, `rfcomm`, `sdp_util`, `ancs_client`, `le_device_db_memory`, `btstack_memory`,
  `btstack_linked_list`, `run_loop`.
- The controller's own transport header, `include_lib/btctrler/hci_transport.h`, declares BTstack's
  `hci_transport_t` and `btstack_uart_block_t`, down to the declarations `btstack_uart_block_posix_instance` and
  `btstack_uart_block_windows_instance` (BTstack's own POSIX and Windows UART drivers, which a JieLi chip has no use
  for).
- No BlueKitchen notice appears in the SDK or in the archives (`bluekitchen`: 0 hits in `include_lib`, `apps/common/ble`
  and `btstack.a`). JieLi's headers carry JieLi's own copyright line.
- BlueKitchen's `LICENSE` (read 2026-10-25) is a BSD-style text with a fourth clause: "Any redistribution, use, or
  modification is done solely for personal benefit and not for any commercial purpose or for monetary gain", and ends
  "Please inquire about commercial licensing options". That is a restriction on use that GPLv3 does not allow to be
  added to a GPL work, so BTstack is **not GPL-compatible**. Whether JieLi holds a commercial licence for its copy is
  unknown. Names are evidence of lineage, not proof of copying: the bodies were not compared.

### The controller: `btctrler.a`

- 107 members, the Bluetooth link controller for BR/EDR (Classic) and LE: `lmp*`, `bredr_*`, `ll_*` (310 `ll_`
  symbols, including the full HCI command handler set `ll_hci_*`), `hci_ll*`, `hopping_channel`, `ll_afh`, `tws_*`
  (true-wireless earbuds), `ll_cis`/`ll_bis` (LE audio), `aes_hw`, `btagc`, `RF`/`RF_ble`/`RF_ble5`/`bredr_RF`.
- The only third-party name is micro-ecc (`uECC_*`, header `uECC_new.h`; its source carries "Copyright 2014/2015,
  Kenneth MacKay. Licensed under the BSD 2-clause license" in the SDK's `crypto_toolbox/micro-ecc/` headers). No
  Ralink, Linux, Zephyr, GPL or GNU names. The vendor extension `ll_wifi_detect` exists (see "Coexistence").
- Everything else in it reads as JieLi's own. That is a finding about names, like the eight clean `wl_rf_common.a`
  members in WIFI.md: the Apache-2.0 label is **not contradicted and not proven**.

### The crypto helper: `crypto_toolbox_Osize.a`

- Its headers in the SDK (`include_lib/utils/crypto_toolbox/crypto.h`, `endian.h`, `hmac.h`, `sha256.h`) say
  "Copyright (C) 2010-2014 Oryx Embedded ... This file is part of CycloneCrypto Open" under **GPL-2.0-or-later**.
  The archive itself has no "Oryx" or "Cyclone" string. Its members are `Crypto_hash`, `hmac`, `sha256`, `rijndael`,
  `aes_cmac`, `bigint`, `ecdh`, `endian`, `uECC_new`. The BLE controller needs only the uECC and AES-CMAC members
  (fm1-radio's probe: 20 symbols), and fm1-radio links the whole archive for both Bluetooth and WiFi.
- This was not in WIFI.md. It matters most for the claim "the archives are Apache-2.0": the SDK ships GPL-2.0-or-later
  headers beside one of them. [unchecked] whether the matching members were compiled from that source.

## The controller's interface: HCI

`btctrler.a` exports `hci_transport_controller`, `hci_transport_h4_controller_instance`, `hci_controller_init`,
`hci_controller_run_loop` and `btctrler_hci_cmd_to_task`. Its `hci_transport_t` has `init`, `open`, `close`,
`register_packet_handler(handler(packet_type, packet, size))`, `can_send_packet_now`, `send_packet(packet_type,
packet, size)`. That is the standard HCI packet interface (command, ACL data, event), with function calls instead of
a UART. Any HCI host can use it: JieLi's host does, and a replacement host would implement the other end of the same
`hci_transport_t`. [unchecked] whether the controller accepts a plain host-side `send_packet` without `btctrler_task`
running under JieLi's OS; fm1-radio runs both tasks on a small cooperative scheduler of its own (their note 02).

fm1-radio's link probe found what the controller needs from the outside: 87 symbols for a BLE-only build, 115
with `wl_rf_common.a` (note 02). That shim is the real work: an OS layer (two tasks that block), timers, buffers
(`lbuf`), interrupts, the settings store and the radio's calibration constants.

## The controller's registers and RF path, as far as the SDK's C sources show

Short version: the SDK's C sources show almost none of it. The baseband's registers are not defined in any C header
that ships. They can be read only as type names in the archive's debug information.

- `include_lib/driver/cpu/wl82/asm/WL82.h` defines `bsfr_base 0x20000` ("bt; wire less") and no block at it. There
  is no `JL_BT`, `JL_BLE` or `BT_CON` register struct. The blocks WL82.h does define for the wireless side are the
  ones WIFI.md already lists: `JL_ANA` (`WLA_CON0`..`WLA_CON30`, 0x11900) and `JL_WL` (0x14000: `WL_CON0`..`7`,
  `LOFC_CON`, `LOFC_RES`, `WL_ANL_CON0`..`3`, `WF_CON0`..`9`, `SD_CON0`..`2`, `SD_CTU_CON`, `SD_CTU_CNT`, `SD_CPTR`,
  `SD_DPTR`). The radio is shared, so these are the same registers the WiFi driver uses.
- IRQ numbers (`hwi.h`): `IRQ_BLE_RX_IDX` 29, `IRQ_BREDR_IDX` 40, `IRQ_BT_CLKN_IDX` 41, `IRQ_BT_DBG_IDX` 42,
  `IRQ_WL_LOFC_IDX` 43, `IRQ_BLE_EVENT_IDX` 45, and for the WiFi core `IRQ_WF_IDX` 52. fm1-radio's note 04: the SDK
  requests 29 and 45 at priority 2; the handlers (`ble_rx_irq_handler`, `ble_event_irq_handler`) are plain C with the
  `interrupt` attribute and call only library-internal functions and `hw_dma_empty`.
- The baseband frame descriptor appears in `btctrler.a`'s debug information as a struct from
  `lib/btctrler/port/wl82/baseband/classic/bredr_frame.h` (not shipped): `ctrl_frame` with `bdaddr0`..`2`, `bchl`/`m`/`h`,
  `txpwr`, `txfreq`, `rxfreq`, `rxrssi`, `txptr0`/`1`, `rxptr0`/`1`, `linkcntl0`/`1`, `txphdr0`/`1`, `rxstat0`/`1`,
  `clk_offset`, `lclk_offseth`, `bufcntl`, `txbuf0cntl`.., `twscntl`, `twsword0`..`3`; and `common_rf_param`, `PLL_COMP_t`,
  `mdm_set_t`, `RSSI0`..`3` from `.../baseband/common/RF.h`. Field names only: no addresses, no bit meanings. The
  `lib/btctrler/port/wl82/baseband/ble/` directory (six source files, with member names such as `RF_ble`, `RF_ble5` and
  `analog_ble`) is not shipped, so LE has no register names at all except through these.
- RF path and shared calibration: `wl_rf_common.a` provides the radio bring-up for both Bluetooth and WiFi. The
  controller calls `wf_rf_common_init`, defined in `wl_usb_to_sdio.c.o` (one of the five members WIFI.md found to carry
  Ralink/Linux header lineage). That function calls `wfhw_init`, `wf_clk_rst_init` and `wdma_init`: it brings up the
  WiFi core's clock and reset (`WF_CON*`, 0x10010) and the SDIO host's DMA (0x30F00, 0x30F04), even for a BLE-only
  build. Also needed from the same archive: `wf_get_phcom_cnt` (`wf_deep_sleep.c.o`), the RF constants (`STF_*`,
  `DS_*`, `DSSS_*`, `wl_rfd_ram_lut`), the trim and crystal getters (`wifi_get_xosc`, `wifi_get_rf_trim_data`,
  `wifi_get_pa_trim_data`, `wifi_get_mcs_dgain`) and a few soft-double helpers (28 symbols beyond the BLE-only 87,
  fm1-radio's `notes/02-undefined-with-radio.txt`).
- `CONFIG_RF_TRIM_CODE_AT_RAM`: fm1-radio's note 06 says the RF trim code (about 500 ms of high current) must run from
  RAM, not from flash, or the chip crashed 0.7 s after the stack started (a wrong high bit in a return address,
  inside `adc_pp_tune`/`rd_adc_peak`). Confirmed on the FM-1 by fm1-ctl. The masking of other interrupts during start
  (note 05) was a workaround before that.

## What `wl_rf_common.a` shares with WiFi, and coexistence

- Shared: all 13 code members. The BLE build touches 10 of its symbols (fm1-radio note 01). `wf_rf_common_init` brings
  up the WiFi core's clocks, so **BLE needs the WiFi register blocks that OPLBOX's `fm1_wifi_regs.h` already
  describes** (`WF_CON0`-`4`, `WF_CON7`, 0x10010, 0x30F00, 0x30F04).
- `wlc_main` is called by `wl_usb_to_sdio.c.o`. It is **not** in `wl_rf_common.a`, `wl_wifi.a`, `wl_wifi_sta.a` or
  `ext_wifi_dev.a`. It is defined in `cpu.a` (JieLi's chip support, binary, bitcode), member `wlc.c.o`, source file
  `lib/driver/cpu/wl82/wlc.c`, which WIFI.md does not mention. That member also holds `switch_rf_coexistence_config_table`,
  `_lock`, `_unlock`, `get_rf_coexistence_config_index` and `set_rf_coexistence_mesh_config`.
- What `wlc_main` does (IR, names and constants only): sets bits 1:0 of `WL_CON0` (0x14000), then calls
  `wlc_init(prio_table, weight_table, cca_busy_en, index)` with entries from three tables of nine, indexed by the
  current coexistence mode. Each `prio_init_N` has 46 entries of 4 bytes; `weight_init_table` is a table of nine
  pointers and `cca_busy_en_table` holds nine `u16`. The member also defines register accessors `wlc_wreg`, `wlc_rreg`,
  `wlc_pti_wreg` and `wlc_pti_rreg`; `wlc_init`'s body was not read. This reads as a **hardware priority arbiter
  between Bluetooth and WiFi, programmed from per-mode tables** [unchecked]; it is the kind of block other chips call
  PTA (packet traffic arbitration). The string "PTA" appears nowhere in `btctrler.a`,
  `wl_rf_common.a` or `cpu.a`; the name is ours.
- The SDK header `rf_coexistence_config.h` documents the modes (Chinese comments; translation ours): 0 balanced 1 (WiFi
  connected to a router, before EDR pairing); 1 balanced 2 (after EDR pairing, mostly receiving); 2 Bluetooth has
  absolute priority (WiFi only in the gaps); 3 WiFi has absolute priority, "used at power-up to join the router quickly",
  "does not take from BLE", hurts EDR; 4 WiFi has relative priority, "does not take from BLE"; 5 balanced 3 (EDR
  sending); 6 balanced 4; 7 balanced 5. The header lists modes 0-7; the tables have nine entries. [unchecked] what
  index 8 is.
- `btctrler.a` also has `ll_wifi_detect.c` (function names for a vendor LL control exchange, `__slave_ll_send_vendor_wifi_detect_req`
  and a report callback; `ll_update_ch_done_wfdet_callback` suggests it feeds the channel map) and fields `coexistence_timer0`/`1` in its BR/EDR
  link structure. Names only.
- **For OPLBOX** (LE only, no EDR): modes 3 and 4 say they do not take from BLE, so a MIDI link should survive WiFi
  traffic in those modes. That is the manual's claim, not a test. The first plan runs BLE and WiFi one at a time
  (below); simultaneous use is a later stage that needs the FM-1 and a WiFi driver that can already transmit.

## Candidate host stacks

| Stack | Licence | GPL-3.0 compatible | Notes |
| --- | --- | --- | --- |
| JieLi `btstack.a` | BTstack-shaped, JieLi label Apache-2.0, unproven | doubtful | binary only; BTstack lineage (above) |
| BlueKitchen BTstack, from source | BSD-style plus a no-commercial-use clause; commercial licence on request | **no** | the fourth clause is a further restriction |
| Apache NimBLE host | Apache-2.0 (`LICENSE`, 12,553 B) | yes (Apache-2.0 into GPL-3.0; not the other way) | portable host, used with many controllers; needs a small OS port |
| Zephyr's Bluetooth host | Apache-2.0 | yes | tied to Zephyr's kernel and build; too much to carve out |
| Own minimal ATT/GATT plus BLE-MIDI | ours (GPL-3.0-only) | yes | smallest, only this one profile; interoperability corners are ours to find |

### Measured: NimBLE host, peripheral-only

NimBLE `e00b85cc` was compiled with JieLi's clang (`-target pi32v2 -Os -ffunction-sections -fno-builtin`), not linked.
Config: the Linux example's `syscfg.h` with `BLE_ROLE_CENTRAL=0`, `BLE_ROLE_OBSERVER=0`, `BLE_SM_LEGACY=0`,
`BLE_SM_SC=0` (no pairing), one connection, and a stub `console/console.h`. The 56 files that make the host:
`nimble/host/src/*.c` (ATT, GAP, GATT client and server, HCI, L2CAP), the RAM store, `host/util`, the GAP and GATT
services, `porting/nimble/src` (mbuf, mempool, msys, endian, mem, port) and the dummy NPL. All 56 compile.

| Section | Bytes |
| --- | --- |
| code (`.text*`) | 41,090 |
| read-only data | 1,990 |
| initialised data | 320 |
| static RAM (`.bss`) | 5,911 |

Largest: `ble_att_svr` 5,402, `ble_gap` 5,292, `ble_gatts` 4,288, `ble_gattc` 3,484, `os_mbuf` 2,536. Unlinked, no
dead-code removal, so an upper bound for code. Not in the number: the HCI glue to the controller, an OS port beyond the
dummy NPL, the mbuf pools (a design choice: for example 12 blocks of 292 B is 3.5 KB), the BLE-MIDI service, and the
Security Manager. With pairing on, `ble_sm_alg.c` wants mbedTLS's `aes.h`, `cmac.h` and `ecp.h`; it did not compile
here. That would need an AES and AES-CMAC (the repo has BearSSL's AES under `firmware/wifi/bearssl/`) and, for Secure
Connections, an ECDH. NimBLE expects its own task and event queue; OPLBOX is a polled main loop, so the port runs the
event queue from the loop. [unchecked] how big that port is.

### Estimated: our own minimal GATT and BLE-MIDI [estimate, not built]

A BLE-MIDI peripheral needs: HCI (reset, LE event mask, advertising parameters, data and enable, connection and
disconnection events, flow control by `Number Of Completed Packets`, connection update), an L2CAP LE signalling
reply (the connection parameter update request, to ask for a short interval), an ATT server (MTU exchange, Find
Information, Find By Type Value, Read By Type, Read By Group Type, Read, Read Blob, Write Request and Command, Handle Value
Notification, Error Response) with a fixed database of two services, and an SMP reply that refuses pairing. About 3-6 KB
of code and under 1 KB of RAM is a guess from NimBLE's ATT server being 5.4 KB for all its procedures. What it costs
instead is testing: Apple's, Windows' and Android's centrals each find different corners of discovery and MTU
handling.

### The BLE-MIDI profile

Written by us from "Specification for MIDI over Bluetooth Low Energy (BLE-MIDI)" 1.0, MMA/AMEI RP-052, November 1,
2015, which stage 2 read (the section numbers in `firmware/ble/ble_midi.h` are its). midi.org gives the download only
to members, so the copy read was the ten-page PDF in `github.com/stuffmatic/zephyr-ble-midi` (`BLE-MIDI-spec.pdf`);
it is not copied into this repository. What the document says, and what the code does about it:

- **Service and characteristic (section 3).** Service `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`; one MIDI Data I/O
  characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3` with write (write without response required), read (must
  answer the first read with no payload, section 5) and notify; encryption is "recommended", not required, so no
  pairing is a legal first step. The earlier text here, from memory, had these right.
- **Connection interval (section 4): 15 ms or less.** The earlier text here said Apple asks for 15 ms or less,
  starting at 11.25 ms; the document says only 15 ms or less (12 units of 1.25 ms), and the 11.25 ms is not in it
  [unchecked: Apple's Accessory Design Guidelines were not read]. The device must also support MTU exchange
  (section 6); a packet is at most the ATT MTU minus 3 bytes (20 bytes at the default MTU of 23).
- **Packet (section 7).** A header byte `1 0 tttttt` (bit 6 reserved, 0; the top 6 bits of a 13-bit millisecond
  timestamp), then timestamp bytes `1 ttttttt` (the low 7 bits) before every status byte, then the messages. A
  message is full (status and data) or running status (data only, allowed after a full message of 2 bytes or more
  that is neither System Common nor Real-Time, with the same status, in the same packet); a running-status message
  may leave out its timestamp byte (then it takes the previous message's), except after a System Common or
  Real-Time message; the end of a packet cancels running status; Real-Time messages are not interleaved inside other
  messages, only inside SysEx.
- **Timestamps (section 9).** 13 bits, 0 to 8191 ms, increasing; when a timestamp low is smaller than the one before
  it in the packet, the receiver adds one to timestampHigh (it is not sent). The sender must keep the messages of one
  packet within that rule; timestamps are in the sender's clock and are never in the future; relating them to the
  receiver's clock is not in the document.
- **SysEx (section 8).** Only SysEx spans packets. The start byte has a timestamp byte before it; a continuation
  packet is a header and data bytes with no timestamp byte (that is how the decoder knows it is one); a Real-Time
  message inside a SysEx has its own timestamp byte and the data resumes right after it; the last packet has a
  timestamp byte before 0xF7. Not supported (the document says so on its preface): multiple virtual cables and clock
  synchronisation.

## Flash and RAM cost against the 238 KB

OPLBOX's free space (WIFI.md "OPLBOX's constraints"): 238,772 B of app image, 61,528 B of `.data`+`.bss`, 88,176 B of
pool, 17,736 B of RAM code.

| Piece | Code | Static RAM | Pool | Source |
| --- | --- | --- | --- | --- |
| JieLi's whole BLE stack plus fm1-radio's shim and MIDI profile | +124 KB | +20 KB | +48 KB (3 task stacks of 8 KiB, 24 KiB heap) | fm1-radio note 06, measured on the FM-1 |
| Same, link probe before the shim | about 97 KB (`.bt_stack_code` 22 KB, `.text` 20 KB, ...), 18 KB read-only data | about 4 KB bss | | fm1-radio note 02 |
| NimBLE host, peripheral-only, no pairing | 41 KB (unlinked upper bound) | 6 KB | mbuf pools by design | this file |
| Own minimal host | about 3-6 KB | under 1 KB | small | estimate |
| Our WiFi driver (target, WIFI.md) | under 120 KB with lwIP | under 48 KB | | WIFI.md |

- The 124 KB for JieLi's stack is the controller and the host together. How it splits is **not measured**; the
  controller is 5.1 MB of bitcode with TWS, EDR and LE audio the probe leaves out. Replacing JieLi's host with NimBLE
  would save some of the 124 KB, not all.
- **Both radios do not fit as measured**: 124 KB of Bluetooth plus a 120 KB WiFi target is 244 KB, above 238,772 B.
  Options: the WiFi driver comes in under 114 KB; the Bluetooth cost drops with NimBLE (the host part) and a
  trimmed shim; or the two are not in the image at once (one image per radio, chosen from a setup page, as fm1-radio
  keeps Bluetooth off at boot).
- RAM fits alone (68 KB for JieLi's stack against about 150 KB free in `.data`+`.bss`+pool) and tightly beside the
  WiFi target (48 KB). The task stacks are the first thing to shrink; fm1-radio says their real depth is in its log.
- Audio timing: the stack's interrupts (priority 7 in fm1-radio's mapping, maskable by `cli` on this chip, probed) and
  its 500 ms trim must not break the audio interrupt's `ISR_BUDGET_US` 5805. fm1-radio audio pauses about a second at
  start. Bluetooth start-up is the same kind of muted event as WiFi's trim.

## fm1-radio's notes 01-06 as prior art

All of this was written by someone else on a different codebase (MPL-2.0 on Felucca's platform); nothing is copied
here. Read for what carries over.

| Note | What it holds | What carries over |
| --- | --- | --- |
| `01-symbols` | `btctrler.a` + `btstack.a` define about 4,800 symbols and reference 386 they do not define; the SDK runs the stack as two OS tasks, `btctrler` (priority 19) and `btstack` (18) | the closure of what a BLE build links |
| `02-link-probe` | 87 symbols to provide for BLE alone, 115 with the radio; table of groups (libc, interrupts, OS, timers, buffers, power, storage, hooks, radio); size about 97 KB of code | the shim's list of work; the "unknown": the factory trim and crystal values |
| `03-device-results` | the first device runs: calibration loops for seconds with no watchdog feed; crashes at `om_render`, wild PCs ending in `0x470`; priority 7 interrupts | what to log when it crashes |
| `04-analysis` | the wild PCs are a return address with one high bit wrong; JieLi's two BLE handlers and `request_irq` read; the instrumented build and probe for `IPMASK` | priority 7 is maskable by `cli` (measured in 05); nested interrupts inside Felucca's wrappers |
| `05-midi-works` | 2026-10-08: the FM-1 advertises as "FM-1" with the BLE MIDI service; a Mac connects through Audio MIDI Setup; notes in, MIDI from the Mac reaches the engine | a working end state: GATT database, advertising, packet parsing, batching, one central tested |
| `06-controller-handoff` | the plan for a BLE controller firmware; start-up masking; BT interrupts at priority 7; `low_power_get` must return non-null; costs; pairing (`sm_set_request_security(0)`; a Mac connects without pairing, iOS may want bonding); the root cause: RF trim code in flash, fixed by running it from RAM | all of it; the pairing line matters for the "no pairing first" decision |

- fm1-base (`aa2b3bc`) holds the shim (`ble/shim/`: `os.c`, `systimer.c`, `misc.c`, `rf_const.c`, `ctx.S`, `shim.h`)
  and `ble/midi/le_midi.c` under MPL-2.0 (fm1-radio `LICENSING.md`). MPL-2.0 is a GPL-compatible licence (its
  secondary-licence clause), so reading them and copying files with their SPDX lines and attribution is allowed
  in principle. This task read none of the files [unchecked]; each use needs its own `LICENSING.md` entry.
- fm1-radio's licence note 06 says shipping a GPLv3 binary that links the Apache-labelled binary libraries cannot
  offer corresponding source and needs a GPLv3 section 7 permission from the platform's copyright holders, a platform
  rewrite, or source-only distribution. The platform candidates in `docs/BASE.md` (fm1-base,
  felucca-bsp) are MPL-2.0, which removes the platform half of that problem and leaves the binary libraries.
- fm1-radio does not ship JieLi's libraries; its build copies them from each user's own SDK. That is the
  "source-only distribution" option.

## Plan

One task per stage, each on its own branch, merged before the next begins unless marked parallel. "Host" is what a host
test proves. "FM-1" is what needs xero to flash a build and read numbers off the screen.

### Stage 1: HCI packet layer and a fake controller (host only)

- `firmware/ble/hci.c`/`hci.h`: encode and decode command, event and ACL packets for the commands listed under "Estimated: our own
  minimal GATT and BLE-MIDI".
- `tests/ble/hci_model.c`: a host fake controller in the style of `tests/wifi/sdio_model.c`: answers commands from a
  script, logs every packet, plain-text fixture format, diff helper.
- **Host**: the command sequence for advertise, connect and disconnect replays against the fixture. **FM-1**: nothing.
- **Done (2026-10-26)**, see "Stage 1 as built" below.

### Stage 1 as built

Own code from the Bluetooth Core Specification (v5.4 section numbers in `firmware/ble/hci.h`; opcodes, event codes and
field layouts cross-checked against Apache NimBLE's `include/nimble/hci_common.h`, values only). Nothing in it comes from
`btstack.a` or the SDK's Bluetooth headers. The transport is our own function pair, `send(type, packet, size)` and a
packet handler `hci_host_receive(type, packet, size)`, the shape of the controller's function-call interface above.

- **Packets.** Command (opcode, length, parameters), event (code, length, parameters) and ACL data (12-bit handle,
  Packet_Boundary flag, length) packed and decoded; an event whose length byte disagrees with its size, or that is
  shorter than its fields, is refused. H4 framing (`hci_h4_feed`): the indicator byte, then the packet, read from a byte
  stream in any pieces; a stray byte or an over-long packet is skipped and counted.
- **Start**, one command at a time, each after the previous one's Command Complete with status 0, and only with a
  command credit (Num_HCI_Command_Packets): Reset; Set Event Mask (Disconnection Complete, Hardware Error, LE Meta);
  LE Set Event Mask (LE Connection Complete, LE Connection Update Complete); LE Read Buffer Size, and Read Buffer Size
  when LE has no buffers of its own; Read BD_ADDR; LE Set Advertising Parameters (ADV_IND, public address, channels
  37-39); LE Set Advertising Data and Scan Response Data (31 bytes each, built with `hci_ad_add`); LE Set Advertising
  Enable. A refused command, a 2 s silence (also when the transport refuses every send) or a Hardware Error ends the
  start with the command and status, for a stage 4 page to show.
- **Connection.** LE Connection Complete makes the host connected (advertising stops on its own); LE Connection Update
  Complete updates the interval, latency and timeout. An L2CAP PDU (up to 256 bytes of payload) goes out in fragments
  of the controller's ACL size (PB 00 first, then 01), one per free buffer; Number Of Completed Packets gives buffers
  back. Incoming fragments (PB 10 first, then 01) are put back together by the L2CAP basic header's length; a
  fragment with no start, another handle, an over-long PDU or a start that cuts one off is dropped and counted.
  Disconnection Complete (either side; `hci_host_disconnect` sends Disconnect) gives every buffer back and advertising
  starts again.
- **The host's state moves on before each send**, so it works whether the controller answers inside the send call or
  later, from its own task; which one `btctrler.a` does is not known.
- **The fake controller**, `tests/ble/hci_model.c`: answers commands as the specification describes (Invalid HCI
  Command Parameters for an advertising interval out of range or data over 31 bytes, Command Disallowed for
  advertising while connected, Unknown HCI Command otherwise), sends events only when the masks let it, plays a
  central from a script (connect, update, L2CAP PDUs in fragments, raw fragments, disconnect, Hardware Error, held
  buffers), checks what the host sends (parameter lengths, credits, buffer size and count, Packet_Boundary order,
  handles), and logs every packet in a fixture format.
- **Tests**, `tests/ble_hci.c`, 110 checks: the packets byte for byte; a session diffed against
  `tests/ble/session_expect.txt` (written by hand from the specification), once with queued answers and once with
  answers inside the send call; the failures above; a link lost while the controller holds both buffers.
  `tests/ble_mutants.py`: 18 deliberate bugs in `hci.c`, all caught.
- **Size**: 3,017 B of code and read-only data, no static RAM (the `hci_host` struct, about 1 KB, is the caller's).
  Compiled by `./build.sh` with the app's flags, not linked.
- **Untested on hardware.** Nothing here has talked to `btctrler.a`. Stage 4 shows whether its answers match: the same
  opcodes accepted, how many command credits it gives, its LE buffer size and count, whether it sends LE Meta events
  after these masks, and whether it calls back inside `send_packet`.
- Not in stage 1: LE Connection Update from the peripheral (the L2CAP connection parameter request is stage 3's),
  encryption events, random addresses, extended advertising.

### Stage 2: BLE-MIDI profile (host only, parallel with stage 1)

- `firmware/ble/ble_midi.c`: encode and decode BLE-MIDI packets (13-bit timestamps, running status, SysEx split
  across packets, batching), the GATT database for the service and characteristic.
- Tests against the specification's rules (it has no packets in hex) and a round trip with `midi_get()`'s
  USB-MIDI events. **FM-1**: nothing. Built: see "Stage 2 as built" below.

### Stage 3: host stack port (host only, after 1 and 2)

- NimBLE host, peripheral-only, no pairing, in `firmware/ble/nimble/` with its licence files, provenance README and a
  `LICENSING.md`/`CREDITS.md` entry. A port layer that runs the event queue from the main loop, static mbuf pools,
  an HCI transport that is a function pair (same shape as `hci_transport_t`).
- **Host**: NimBLE against stage 1's fake controller: advertise, accept a connection, discover the MIDI service,
  notify and write. Size recorded by `tools/build.py`, unlinked, as for `firmware/wifi/`.

### Gate: the controller licence (no code; xero decides)

- Whether OPLBOX may ship a binary containing `btctrler.a`, `wl_rf_common.a`, `cpu.a` and `crypto_toolbox_Osize.a`,
  or only builds that link them from the user's own SDK (fm1-radio's way), or neither. Diagnostic builds in stages 4-6
  are never shipped, so they need no decision.

### Stage 4: controller start-up on the FM-1 (needs FM-1)

- A diagnostic build that starts JieLi's controller on the cooperative scheduler pattern of fm1-radio's shim (written
  by us; fm1-base's `ble/shim` as reference under MPL-2.0), the trim code in `.ram_code`, audio muted during start.
  The setup page shows: did `wf_rf_common_init` return, the trim result, the HCI reset response, `LE Read Buffer Size`
  (the controller's real ACL buffer size and count), and the crash note fm1-radio built (if it crashes).
- **FM-1**: xero flashes the build, opens the page, reads out the numbers. **Host**: nothing new.

### Stage 5: advertise, connect, exchange MIDI (needs FM-1)

- Stage 3's host on stage 4's controller: advertise "OPLBOX" with the service UUID, connect from a Mac, an iPhone and
  an Android phone (one each, or what xero has), echo notes both ways, record connection interval, latency and
  dropped notes; wire the notes into the existing MIDI paths (keys and the arp out, USB and TRS in).
- **FM-1**: xero connects the devices and reports. Pairing stays off; if an iPhone needs bonding, stage 5 says so.

### Stage 6: WiFi and Bluetooth together (needs FM-1, after WiFi stage 4)

- Run the coexistence modes (`switch_rf_coexistence_config_table`, modes 3 and 4 first) while WiFi scans and BLE MIDI
  streams; count lost connection events and MIDI packets. If it does not hold, BLE and WiFi stay one-at-a-time.
- **FM-1**: needs a WiFi driver that can already receive (WiFi stage 4).

### Stage 7: pairing (optional, needs FM-1 and stage 5)

- Only if a phone insists. NimBLE's Security Manager with an AES and AES-CMAC provider (BearSSL's AES is already in
  `firmware/wifi/bearssl/`), just-works legacy pairing first, the bond in the settings sector.

### Not planned: our own controller

- Needs the baseband's register map and the RF sequences for LE: no source shows them, only field names in the
  archive's debug information. It would be measured on the FM-1 with the stock controller running as a reference.
  Revisit if the licence gate says no.

### Order and what runs in parallel

Stages 1 and 2 are independent. Stage 3 needs both. The gate can be answered at any time. Stages 4 onward need the
FM-1, in order; 6 also needs WiFi stage 4. Stages 1-3 need nothing from xero's hardware.

### Stage 2 as built

`firmware/ble/ble_midi.c`/`.h`, our own code from the specification above and nothing else: not from `btstack.a`, not
from Apple's headers, not from another BLE-MIDI implementation (Espressif's and Zephyr's pages were read only to find
the specification's copy). It includes no C library header, like `hci.c`.

- **The GATT database.** The two UUIDs as text and as the 16 bytes ATT carries (least significant first), the
  characteristic's properties (`0x16`: read, write without response, notify), the connection interval limit, the empty
  first read, the advertising data (Flags, then the 128-bit service UUID, 21 bytes) and the scan response (the device
  name, shortened to 29 characters when longer). It is data and builders, not an ATT server: stage 3 puts the NimBLE
  host's attribute table on top of these constants.
- **The encoder** (`ble_midi_enc`, one packet buffer the caller owns). `ble_midi_enc_msg(ts, status, d1, d2)` batches
  messages into one packet while they fit the size given (ATT MTU minus 3, at least 5) and while the receiver could
  rebuild each 13-bit time: a time that would need a second wrap, or an earlier one, starts a new packet. It leaves
  the status out for running status and the timestamp byte out when the time is the previous message's, and puts both
  back where the specification requires (after System Common and Real-Time, after a SysEx). `ble_midi_enc_sysex`
  takes a whole 0xF0 ... 0xF7 message and splits it, filling each packet, with no timestamp byte in the
  continuations and one before 0xF7. `ble_midi_enc_flush` hands the packet to the send function (a notification);
  the caller flushes once per connection interval. A send that fails drops that packet and counts it. Real-Time
  messages cannot be put inside a SysEx being encoded (the SysEx is one call): the decoder accepts them there, the
  encoder never makes them.
- **The decoder** (`ble_midi_dec`, no buffers: it hands out what it finds). Per packet it rebuilds each message's
  13-bit time and gives messages (status and data bytes) and SysEx chunks (first and last flags) to two callbacks. It
  drops and counts what the rules forbid and carries on with the rest of the packet: a bad header, data bytes with no
  status, running status after the end of a packet or after a Real-Time byte with no timestamp, a message cut by the
  end of a packet or by the next timestamp, a status byte inside a SysEx (which ends it unfinished), a stray 0xF7, and
  the undefined statuses 0xF4 and 0xF5. A SysEx open at the end of a packet stays open for the next one.
- **The bridge to the app's MIDI input** (`ble_midi_in`). `ble_midi_in_write` is the GATT write handler;
  `ble_midi_get` has `midi_get()`'s contract (`platform.c`: 1 and `(status, data 1, data 2)`, the data bytes a message
  lacks read 0) over a 64-message queue, so the app's input loop reads Bluetooth and USB the same way once stage 5 calls
  both. A full queue drops the newest message and counts it. SysEx does not go through `midi_get()` on USB either (the
  update and song-slot code take a whole frame): a complete BLE SysEx goes to a sink function as one 0xF0 ... 0xF7
  message, or is dropped and counted when it is longer than the buffer the caller gave. Timestamps are not kept; a
  message is acted on when it arrives.
- **Tests**, `tests/ble_midi.c`, 419 checks: the UUID bytes checked against the specification's strings by a second
  conversion; packets byte for byte, worked out by hand from the rules because the document has field tables and
  figures but no packets in hex (the one-message, two-message and running-status figures on its page 3, the wrap, 8191
  to 0, running status around Real-Time bytes, SysEx over two packets and ending exactly at the end of a packet);
  round trips of 250 random messages (channel, System Common, Real-Time, SysEx up to 89 bytes, mostly close in time,
  sometimes 400 ms apart, from random start times) at ten packet sizes from 5 to 244 bytes and three seeds, each
  message back with its bytes and its 13-bit time; every case the decoder drops; the bridge (the queue's wrap and
  overflow, a SysEx across packets with Real-Time inside it, too long, never ended, a reset, no sink, no buffer, the
  encoder into the bridge); and 40,000 random packets (also run once under AddressSanitizer and UBSan: clean).
  `tests/ble_midi_mutants.py`: 32 deliberate bugs in `ble_midi.c`, all caught.
- **Size**: 2,064 B of code and read-only data, no static RAM (the encoder, decoder and queue are the caller's: the
  queue 64 x 3 B plus the SysEx buffer). Compiled by `./build.sh` with the app's flags, not linked.
- **Untested on hardware**, and untested against any real central: a Mac or an iPhone has never read these packets.
  The vectors come from our reading of the specification, so a misreading would pass every test here; the first
  check is stage 5's.
- Not in stage 2: the ATT server and GATT attribute table (stage 3), pairing, the connection interval request
  (L2CAP, stage 3), MTU exchange, timestamps smoothing on receive, and a bridge from the app's `midi_send()` to the
  encoder (one call with `fm1_ms` as the time, wired in stage 5).

## Cordio path as built

Decision (xero, 2026-10-27): no JieLi binary in the image. The controller is Arm/Packetcraft Cordio's link layer
(Apache-2.0, https://github.com/packetcraft-inc/stacks), which the names inside `btctrler.a` (`lctrComputeAccessAddr`,
`lctrComputeCrcInit`, `lctrCalcTotalAccuracy`, `lctrBisCalcGroupSessionKey`) point to as the source of about 20 of its
functions. Apache-2.0 is compatible with GPL-3.0-only when LICENSE and NOTICE are kept. The chip's radio driver is
ours, clean-room, from facts only.

Status of the six steps (2026-10-27). **Nothing below ran on an FM-1.** Host tests: `tests/run_host.sh`.

1. **NimBLE host, peripheral-only, no pairing: done and host-tested.** `firmware/ble/nimble/` (129 files, unchanged, commit
   e00b85cc, `README.md` and `SHA256SUMS`), our polled port (`ble_nimble_npl.c`, `nimble_port/`), the transport as a function
   pair and the BLE-MIDI GATT service in `ble_nimble.c`. `tests/ble_nimble.c` (five scenarios in separate processes, 135 checks)
   against `tests/ble/hci_model.c`: NimBLE's start-up commands, advertising with the profile's data, a central that walks the
   GATT database (MTU, services, the characteristic's properties 0x16, the CCCD), subscribes, writes MIDI (with and without a
   response), reads the empty first value, is sent MIDI (one notification in six ACL fragments through two controller
   buffers), the connection interval request (an L2CAP update to 15 ms or less, because a peripheral cannot ask the
   controller), disconnection from either side and advertising again, and a start that never completes ends in FAILED and
   stops the host's retries (NimBLE alone would retry forever, each try holding the loop for 2 s). 36 deliberate bugs in the
   port are caught (`tests/ble_nimble_mutants.py`). The fixture `tests/ble/nimble_expect.txt` was captured from a run and
   read against the specification, not written from it.
2. **Cordio link layer from source: vendored, configured, compiled for the FM-1 CPU and host-tested on stubs.**
   `firmware/ble/cordio/` (143 files, unchanged, commit 3656312), `firmware/ble/cordio_port/` (our configuration, WSF and HCI
   glue, the stub PAL, the work order `PAL_NEEDED.md`). `tests/ble_cordio.c`: 40 checks, the HCI commands, the refusals and
   the first advertising transmit with the host's PDU. Three open points in docs/BLUETOOTH_CORDIO.md. Not tested: NimBLE and
   Cordio together.
3. **Radio driver and register map: written, host-tested against a model made from the same facts.**
   docs/BLUETOOTH_RADIO.md (the facts; `tools/ble_ir.py --check` reproduces 35 of them from `btctrler.a`) and
   `firmware/ble/fm1ble.c` (B, C, D, E, G of that file: power-up, analog block, baseband, RAM block, one advertising link,
   polling, stop) on `firmware/hal/fm1ble_port_bsp.c`. `tests/ble_radio.c` (100 checks) shows the driver writes what the
   document says in its order; it cannot show the chip does what the document says. Guesses are marked in the code: the order of
   B.2 against B.1, the hop increment, the calibration (none), re-arming the next event, the receive buffer choice, and
   `wf_rf_common_init`, which the second variant of the test run replaces with our own WiFi power-up. The chip's Cordio PAL
   is **not written** (PAL_NEEDED.md), so the driver advertises on its own and Cordio is not in the radio's path yet.
4. **First FM-1 proof: ready to run, not run.** The test run's `adv_air` step counts the baseband's event flags while the
   driver advertises ADV_IND (service UUID in the data, "OPLBOX" in the scan response) and the radio's SCAN_REQ and
   CONNECT_IND captures show whether a phone's scanner reached it. If the facts were not enough for a first transmit, the
   report's raw readbacks say which register to look at. Connections and MIDI over the air need the PAL and the
   receive path (H); not started.
5. **Test page, USB report, tool: done and host-tested.** docs/BLUETOOTH_PAGE.md; `tools/fm1_ble_report.py --run --out DIR`
   over SysEx 0x65 (report) and 0x66 (run); `tests/ble_report_tool.py`. Eight steps (radio, block, hci, adv_set, adv_air,
   connect, midi_in, midi_out), one line each with raw bytes, 24 counters; a failed step's note says what to do next. The host
   build runs a scripted fake (`tests/ble/fm1ble_test_host.c`) to test the page and the tool, the firmware runs
   `firmware/ble/fm1ble_test.c` on the driver.
6. **Size: measured.** NimBLE + Cordio + C library, linked with `--gc-sections` on a stub PAL: 69,606 B of code and read-only
   data, 19,588 B of static RAM. The app today is 416,156 B of 581,564 B and 62,864 B of 98,304 B of RAM, so carrying all of it
   is about 486 KB and 82.5 KB: **WiFi and Bluetooth fit together**, no separate Bluetooth image. The stack is not in the
   app's link yet (no controller to run it on).

The first FM-1 run, by the control session over USB: `python3 tools/fm1_ble_report.py --run --out DIR`, then a phone's BLE
scanner for "OPLBOX". What would settle the open points: the report's counters (events with and without re-arming, the
variant that worked), the CONNECT_IND bytes, and the PAL's first run.

## Open questions

- Whether OPLBOX may ship the controller binary (the gate above); and who to ask. Who would answer for JieLi is
  not known.
- Whether the controller accepts a plain host-side `send_packet` without JieLi's OS (stage 4 answers it).
- What coexistence index 8 is, and what the 46-entry priority tables encode. They are constants in `cpu.a`'s IR; the
  same rule as WIFI.md's tables applies (facts only, no translated code).
- How much of the 124 KB belongs to the controller and how much to JieLi's host (a link map would say).
- Whether an iPhone connects without bonding (fm1-radio found a Mac does).
- Whether an iPhone or a Mac wants more than the specification says: whether they need the connection interval at
  the 11.25 ms Apple's guidelines may ask for (not read), whether they read the characteristic before they write,
  and how they react to a notification with a timestamp in the past (stage 5).
- Whether the licence finding about `crypto_toolbox_Osize.a` (GPL-2.0-or-later headers beside the archive) should be
  passed on to fm1-radio's and fm1-base's authors, as WIFI.md asks of the WiFi finding.

## Reproducing the measurements

```sh
S=$(mktemp -d); L=~/fw-AC79_AIoT_SDK/cpu/wl82/liba
for a in btctrler btstack rcsp_stack lib_sig_mesh crypto_toolbox_Osize cpu; do
  mkdir -p $S/$a && (cd $S/$a && ar x $L/$a.a); done
strings -n 4 $L/btctrler.a | grep -c -i copyright                     # the table's columns, per archive
llvm-nm --defined-only $S/btstack/*.o | awk '{print $3}' | sort -u | grep -c '^att_'
llvm-dis $S/cpu/wlc.c.o -o - | grep -E '^define|prio_init_table'         # wlc_main and the nine tables
llvm-dis $S/wl_rf_common/wl_usb_to_sdio.c.o -o - | awk '/@wf_rf_common_init\(/,/^}/'   # three calls (unpack wl_rf_common too)
# NimBLE size: sparse checkout of nimble/host nimble/include porting ext/tinycrypt at e00b85cc, then for each .c:
#   $JIELI_TOOLCHAIN/pi32v2/bin/clang -target pi32v2 -Os -ffunction-sections -fno-builtin -c \
#     -isystem $SDK/include_lib/newlib/include -I<overlay with syscfg.h and a stub console/console.h> \
#     -Inimble/include -Inimble/host/include -Inimble/host/src -Iporting/nimble/include \
#     -Iporting/npl/dummy/include -Inimble/host/util/include -Inimble/host/services/gap/include \
#     -Inimble/host/services/gatt/include -Inimble/host/store/ram/include \
#     -DMYNEWT_VAL_BLE_ROLE_CENTRAL=0 -DMYNEWT_VAL_BLE_ROLE_OBSERVER=0 -DMYNEWT_VAL_BLE_SM_LEGACY=0 -DMYNEWT_VAL_BLE_SM_SC=0
#   then add up the section sizes with $JIELI_TOOLCHAIN/common/bin/objdump -h
```

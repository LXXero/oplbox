# WiFi without JieLi's binary libraries

OPLBOX is GPL-3.0-only. To get WiFi it needs its own driver instead of linking JieLi's prebuilt WiFi archives.
Whoever distributes a GPL program has to give its recipients the complete corresponding source. GPLv3 section 6
allows several ways to do that (with the binary, a written offer, network access), but for those archives there is
no source to give: the SDK ships them as binaries only, with no source and no offer of it. This file maps what the
driver has to replace, then splits the work into stages small enough for one task each.

Read on 2026-10-24. Corrected on 2026-10-25 after docs/WIFI_REVIEW.md: the WEXT count, the `wl_rf_common.a`
provenance (new section "What `wl_rf_common.a` carries"), the source of the MAC defaults, a symbol total and the
GPL source wording. "[unchecked]" marks what nobody has verified yet. This is evidence, not a legal opinion.
Nothing below was run on an FM-1.

| Source | Commit read |
| --- | --- |
| JieLi AC79 SDK, `~/fw-AC79_AIoT_SDK` | d179b448, tag `AC79NN_SDK_V1.2.1_2023-12-13` |
| fm1-radio, https://github.com/charlesvestal/fm1-radio | c9113b5 (2026-10-24) |
| fm1-base, https://github.com/charlesvestal/fm1-base | aa2b3bc (2026-10-24), submodule `bsp` at d6b8f77 |
| Linux, `drivers/staging/rt2860` (Ralink's driver) and `drivers/net/wireless/ralink/rt2x00/rt2800.h` | v2.6.32, v2.6.37, v6.6 |
| Linux, read 2026-10-25 as single files: `drivers/staging/rt2860/common/rtmp_init.c`, `rtmp_def.h`, `rtmp.h` (v2.6.32); `net/wireless/wext-core.c` (v2.6.33, v2.6.37); `rt2x00/rt2800lib.c`, `rt2800.h` (v6.6) | as listed |

How it was measured: the archives were unpacked with `ar x`. Almost every member is LLVM bitcode, not machine
code (see "Archive inventory"), so `llvm-dis` (system LLVM 23) turns them into LLVM IR with function names,
argument names and source line numbers. The JieLi toolchain's `objdump` also runs here (LLVM 4.0.1); no
disassembly was needed. fm1-base's WiFi example (`examples/wifi`) and its template were built in /tmp, and so was
OPLBOX (this tree, `./build.sh`), to get real sizes and the real link closure. The commands are listed in
"Reproducing the measurements" at the end.

## Licence provenance

The plan below depends on the licence of JieLi's archives, so this comes first.

### What the SDK says

- The SDK root has one `LICENSE`: the Apache License 2.0, text only. Its appendix still has the template line
  `Copyright [yyyy] [name of copyright owner]`, filled in for nobody.
- `README.md` (Chinese) has no licence statement. Its only related lines list protocol support ("lwip、mbedtls、
  http/https、websocket、coap、nopoll、curl、mqtt、ftp、uip、iperf") and, line 101, "支持脱离802.11协议，直接利用底层RF收发数据包"
  (it can send and receive packets on the raw RF without the 802.11 protocol).
- `cpu/wl82/liba/` and `cpu/wl82/` have no LICENSE, NOTICE, COPYING or README. No file anywhere in the SDK names
  a different licence for the prebuilt archives. The only other licence files near the network code are for
  bundled source projects (`lib/net/uC-HTTPc/LICENSE` and `NOTICE`, `lib/net/mbedtls_2_2_1/LICENSE`:
  "Unless specifically indicated otherwise in a file, files are licensed under the Apache 2.0 license",
  `lib/net/mbedtls_2_26_0/LICENSE`: Apache-2.0).

### What `strings` finds in the archives

`strings -n 4` on each archive, searched for copyright lines, licence names and vendor and project names:

| Archive | "copyright" | "GPL" / "GNU" | "licen[cs]e" | "Ralink" | "RTMP" (Ralink's prefix) |
| --- | --- | --- | --- | --- | --- |
| `wl_wifi.a` | 0 | 0 | 0 | 76 | 642 |
| `wl_wifi_sta.a` | 0 | 0 | 0 | 60 | 581 |
| `wl_wifi_ap.a` | 0 | 0 | 0 | 61 | 508 |
| `wl_wifi_sfc.a` | 0 | 0 | 0 | 76 | 642 |
| `wl_wifi_sta_sfc.a` | 0 | 0 | 0 | 60 | 581 |
| `ext_wifi_dev.a` | 0 | 0 | 0 | 2 | 0 |
| `wl_rf_common.a` | 0 | 0 | 0 | 0 | 3 |
| `wpasupplicant.a` | 0 | 0 | 2 (`wpa_supplicant_license`, an identifier) | 0 | 4 |
| `hsm.a` | 0 | 0 | 0 | 0 | 0 |

There are no copyright lines at all: comments are not compiled into bitcode. What the strings do carry is the
build machine's source paths and full debug information (types, function and variable names). The names found:

- **Ralink** (every `wl_wifi*.a`): identifiers such as `COUNTER_RALINK`, `RalinkCounters`,
  `MlmeResetRalinkCounters`, `CmmRxnonRalinkFrameIndicate`, `fAllStationAsRalink`, `APRalinkIe`, and the whole
  `RTMP*` / `Rtmp*` / `Mlme*` / `Asic*` naming of Ralink's Linux driver.
- **Atheros, Broadcom** (every `wl_wifi*.a`): `bLastAtheros`, `bCurrentAtheros`, `bNowAtherosBurstOn`,
  `bIAmBadAtheros`, `bBroadComHT`, `BROADCOM` (an IE variable). These are the interoperability flags inside
  Ralink's driver that work around other vendors' access points, not code from those vendors.
- **Realtek, Marvell, MediaTek, Ralink, HiSilicon** (`ext_wifi_dev.a`): one enum of external WiFi modules,
  `RTL8189E RTL8189F RTL8188E RTL8188F RTL8822B RTL8822C RTL8822E RTL8852B MARVEL_88W8782 MARVEL_88W8801
  MEDIATEK_7601U RALINK_3070 WL_30S9082 RTL8192E RTL8192F RTL8821CS HI3881 NONE_MODULE`. `ext_wifi_dev.a` drives
  an external module through `wifi_connect.c`/`wifi_dev.c`; it is not on the internal radio's path, and
  fm1-radio does not link it.
- **Linux kernel**: source paths `wifi_sdk/lib/net/device/wireless/inc/linux/{if.h, if_ether.h, completion.h,
  device.h, fs.h, module.h, net/iw_handler.h, hdlc/ioctl.h, asm/atomic.h, byteorder/swab.h}` and
  `WIFI_todo/wext-core.c`.
- **wpa_supplicant 2.9**: every member of `wpasupplicant.a` comes from
  `lib/net/device/wireless/wpa_supplicant-2.9/{src/common, src/crypto, src/drivers, src/l2_packet, src/rsn_supp,
  src/utils, wpa_supplicant}`. `wl_wifi*.a` calls into it (`wpa_supplicant_main`, `wpa_supplicant_req_scan_ssid`).
  `hostapd_and_wpasupplicant.a` (not used here) is a hostap tree built for AP mode.
- **mbedTLS 2.2.1**: `include_lib/net/mbedtls_2_2_1/mbedtls/aes.h` in `wl_wifi*.a` (it calls `mbedtls_aes_*`).
- **lwIP**: only event and hook names (`lwip_event_cb`, `wait_lwip_stop_send`, `LWIP_WIRELESS_DHCP_BOUND_TIMEOUT`).
- **AirKiss** (Tencent WeChat's WiFi provisioning): `air_kiss.c` and its `AIRKISS_*` states, in `wl_wifi*.a`.
- No Espressif, Qualcomm, Intel, OpenSSL or PolarSSL strings.

### What the archives were compiled from

Each bitcode member records its source file. The members of `wl_wifi_sta_sfc.a` come from:

| Source directory (under `/jks/workspace/wifi_sdk_all_export_gitlab/wifi_sdk/lib/net/device/`) | Members |
| --- | --- |
| `wireless/jl_wifi/MODULE/common` (`mlme.c`, `cmm_wpa.c`, `connect.c`, `assoc.c`, `rtmp_init.c`, `rtusb_io.c`, ...) | 46 |
| `wireless/jl_wifi/MODULE/sta`, `MODULE/chips` (`rt30xx.c`, `rtmp_chip.c`), `MODULE/os/linux` | 16 |
| `wireless/jl_wifi/NETIF/os/linux`, `UTIL/common`, `UTIL/os/linux` | 7 |
| `wireless/WIFI_todo` (`wext-core.c`, `skbuff.c`, `tasklet.c`, `netlink_socket.c`, `wifi_connect.c`, `air_kiss.c`, ...) | 17 |
| `wireless/jl_wifi/mp_test` | 1 |

The file names (`rtmp_*`, `cmm_*`, `rtusb_*`, `rt30xx.c`, `sta_ioctl.c`) are those of Ralink's Linux station
driver for its RT2870/RT3070 USB chips. Two checks against published copies:

- **Ralink's driver.** Linux 2.6.32 carried Ralink's own driver in `drivers/staging/rt2860`. All 16 functions of
  its `common/action.c` are defined in JieLi's `action.c.o`. Against the 2.6.37 copy (after the staging
  clean-ups renamed some functions): `assoc` 19 of 19 JieLi functions present there, `cmm_data` 29/30,
  `cmm_asic` 25/27, `ba_action` 30/33, `cmm_sanity` 14/16, `connect` 26/30, `mlme` 67/79. Ralink's file header
  in that tree reads:

  > (c) Copyright 2002-2007, Ralink Technology, Inc.
  >
  > This program is free software; you can redistribute it and/or modify it under the terms of the GNU General
  > Public License as published by the Free Software Foundation; either version 2 of the License, or (at your
  > option) any later version.

- **The Linux kernel.** JieLi's `wext-core.c.o` (in `wl_wifi_sta_sfc.a`) defines 7 functions:
  `wireless_send_event`, `get_wireless_stats`, `call_commit_handler`, `wext_handle_ioctl`, `ioctl_standard_call`,
  `iw_handler_get_iwstats`, `ioctl_standard_iw_point`. All 7 names are functions of Linux's
  `net/wireless/wext-core.c` in v2.6.33 (the first release with that file) and v2.6.37. That file's header reads
  "(As all part of the Linux kernel, this file is GPL)"; the kernel's licence is GPL-2.0-only. The function bodies
  were not compared [unchecked], so this is lineage by names, source path and WEXT log strings, not a proven copy.
  (An earlier version of this file said 10 functions; the object defines 7.)

`wl_rf_common.a` (radio, PHY, internal bus) is not free of third-party names, as an earlier version of this file
said: five of its thirteen members were compiled with Ralink's and Linux-shaped headers. The next section lists
them. `hsm.a` (`lib/net/HSM/src/{hsm.c, camera.c}`) shows no third-party name; its authorship is [unchecked].

### What `wl_rf_common.a` carries

Checked 2026-10-25 on the SDK's `cpu/wl82/liba/wl_rf_common.a` (sha256 `ec50bb31af02ee3e…`). Each bitcode member
was turned back into IR with the JieLi toolchain's own clang 4.0.1 (`clang -x ir -S -emit-llvm`): system LLVM 23's
`llvm-dis` rejects the debug information of `wf_deep_sleep`, `wl_sdio` and `wl_usb_to_sdio` and drops it. Every
member's own source file is under `jl_wifi/wl_port/wl82/`, JieLi's port directory. The table lists what else each
one was compiled with, by the debug information's file names and types, and which third-party names it defines.
The SDK's own C headers (`WL82.h`, `cpu.h`, `power_interface.h`, `spinlock.h`, `os_type.h`, `wifi_connect.h`) and
newlib's are left out.

| Member | Functions | Other headers | Third-party names |
| --- | ---: | --- | --- |
| `wf_cfr_init` | 16 | `inc/linux/types.h` | typedefs `u16`, `u32` only |
| `wf_pa_dpd` | 3 | `inc/linux/types.h` | `u8` only |
| `wf_phy_mac_init` | 3 | none | none |
| `wf_phy_mac_lib` | 15 | `inc/linux/types.h` | `u32` only |
| `wf_rf_cal` | 40 | `inc/linux/types.h` | `u32` only |
| `wf_rf_init` | 22 | none | none |
| `wf_soft_adj` | 6 | none | none |
| `wl_rfd_init` | 6 | `inc/linux/types.h` | `u8` only |
| `wf_deep_sleep` | 24 | none | defines `RT28xxUsbStaAsicForceWakeup` (a function declared in Ralink's `rtmp.h`, Linux v2.6.32 staging) and `ForceSleepReady`, both weak and empty |
| `wl_hw_init` | 12 | `inc/linux/types.h`; Ralink's `MODULE/include/chip/rtmp_mac.h`, `UTIL/include/rtmp_comm.h`, `rtmp_type.h` | Ralink's RX descriptor `RXWI_STRUC`, `HEADER_802_11`, `FRAME_CONTROL`, `UCHAR`/`USHORT`/`UINT32`, used by `WifiRecvPkgHook` (per-packet AGC and frequency-offset tracking) |
| `wl_usb_to_sdio` | 27 | `inc/linux/types.h`, `inc/linux/usb.h`; Ralink's `rtmp_mac.h`, `MODULE/include/chip/mac_usb.h`, `rtmp_type.h` | defines Linux USB core names as its emulation over the internal SDIO bus (`usb_register`, `usb_submit_urb`, `usb_free_urb`, `usb_kill_urb`, `usb_get_dev`, `usb_put_dev`, `usb_suspend`, `usb_resume`, `usb_reset_device`, `usb_deregister`; `struct urb`, `usb_device`, `usb_interface`); Ralink's TX descriptors `TXWI_STRUC`, `TXINFO_STRUC`, `HT_FBK_CFG0_STRUC`, `LG_FBK_CFG0/1_STRUC`; Ralink-style names `RTUSBDoubleWrite` and the empty `ra_mtd_read_nm`/`ra_mtd_write_nm` |
| `wl_sdio` | 12 | `inc/linux/types.h`, `device.h`, `mmc/card.h`, `mmc/host.h`, `mmc/sdio_func.h`; FreeRTOS V9.0.0 `FreeRTOS.h`, `portmacro.h` | Linux MMC structures with Linux-style field lists (`mmc_card`, `mmc_cid`, `mmc_csd`, `mmc_ext_csd` with fields such as `raw_pwr_cl_ddr_200_360`, `sdio_func`, `sdio_cccr`, `sdio_cis`, `sd_scr`, `sd_ssr`, `sd_switch_caps`); FreeRTOS static types (`StaticSemaphore_t`, `StaticQueue_t`, ...) |
| `wl_sdio_host` | 18 | `inc/linux/types.h`, `mmc/host.h`; FreeRTOS as above | defines Linux MMC core names `mmc_claim_host`, `mmc_release_host`, `mmc_hostname`, `mmc_set_timing`, `mmc_delay` |
| `version.z.o` | ELF | | `lib_wl82_rf_version` only |

What that shows, and what it does not:

- The eight radio, PHY and calibration members (`wf_cfr_init`, `wf_pa_dpd`, `wf_phy_mac_init`, `wf_phy_mac_lib`,
  `wf_rf_cal`, `wf_rf_init`, `wf_soft_adj`, `wl_rfd_init`) name nothing third-party apart from the typedefs
  `u8`/`u16`/`u32` from JieLi's `inc/linux/types.h`. Nothing found contradicts the Apache-2.0 label for them.
- The other five carry Ralink and Linux lineage: Ralink's descriptor and header structures, Linux's MMC and USB
  structures, Linux and Ralink function names. Neither `inc/linux/` nor Ralink's `rtmp_*.h`/`mac_usb.h` is in the
  SDK; only their file names and type layouts survive in the debug information. Whether JieLi's copies are Linux's
  and Ralink's files or rewritten ones is [unchecked], and so is the Linux version the MMC structures match.
- The Linux- and Ralink-named functions that were read have bodies written for JieLi's system: empty stubs, or
  calls into JieLi's OS (`mmc_delay` is `os_time_dly(cnt * 100 / 1000)`, `mmc_claim_host` is `os_mutex_pend`,
  `mmc_hostname` returns a name field, `mmc_set_timing` calls `host_set_timing`). The larger ones (`usb_submit_urb`,
  `sdio_recv_int_hdl`, `RTUSBDoubleWrite`, which routes MAC addresses by range) were not compared with any Linux or
  Ralink function [unchecked]. `RTUSBDoubleWrite`, `ForceSleepReady` and `ra_mtd_*` are not in the three Ralink
  files read; where they come from is [unchecked].
- `wl_sdio` and `wl_sdio_host` take FreeRTOS's static types through the SDK's `os_type.h`. FreeRTOS V9.0.0, as
  shipped in the SDK's `include_lib/system/os/FreeRTOS/`, is GPL-2.0 with the FreeRTOS linking exception. Only type
  definitions reach these members.
- The archive has no copyright or licence string. The only licence statement covering it is the SDK root's
  Apache-2.0 text, whose copyright line was never filled in. So the label is unproven for the whole archive, and
  doubtful for the five members above.

### What stages 1 and 3 may take from `wl_rf_common.a`

Two kinds of thing can be read out of the archive:

- **Facts about the hardware**, from any member: register addresses, the values written, the bits polled and how
  long, the order of the writes, the contents of constant tables, delays. They describe the chip, and stages 1-3 need
  them. `firmware/wifi/fm1wifi_regs.h` already holds the addresses.
- **Code**: the functions' structure and logic. None is translated from IR into C, not even from the eight clean
  members (chosen by the colony; xero can change it). Their Apache-2.0 label is not contradicted but not proven
  either, and writing from a description costs little more. Stage 3's code is written from a plain description of
  each step (which register, which value, what is read, when a loop ends), kept in this file or a stage-3 document,
  and from what the FM-1 shows. A closer port of the eight clean members would be a change to this rule, not to
  the evidence.
- From the five members with Ralink or Linux lineage, addresses and values only. Stage 2's bring-up and the core's
  window are such facts from `wl_sdio` (`wf_clk_rst_init`, `wdma_init`, `wl30_mmc_io_rw_extended`: registers, bits,
  order, delays; "Stage 2 bring-up"), and `fm1wifi.c` is written from that list. The RX/TX descriptors (stages
  4-5) come from Linux rt2x00's `rt2800.h`/`rt2800lib.c` (GPL-2.0-or-later), not from JieLi's structures.

What stage 1's `tools/wifi_ir.py` may extract:

- From `wf_phy_mac_init.c.o` (`wf_phy_init`, `wf_sifs_reset`), `wf_cfr_init.c.o` (`rapp_init`, `cfr_int`, the
  `rapp_set_thr_*` functions) and `wf_phy_mac_lib.c.o` (`wf_phy_rst`, `wf_phy_mac_rst`): the constant writes in call
  order, as (target, address, value) rows. The target is a MAC register (`sdio_mac_wreg`), a BBP register
  (`bbp_set`) or one of the four indirect windows (`cca_reg_set`, `rx11n_reg_set`, `tx_backend_wr`,
  `rx_frontend_wr`). Also the contents of constant global arrays in those members.
- With `--trace`, for stage 3's host test: the register writes of the power-up call tree (`wf_rf_init`,
  `wf_rf_trim`, `wf_rf_cal`'s tune functions, `wf_soft_adj`, `wl_rfd_init`, `wf_pa_dpd`) as data. A value that
  depends on a reading is written as a named placeholder, not as the expression that computes it.
- Its output is arrays of numbers with a comment naming the archive's SHA-256, the member, the function and the
  IR line range. It emits no control flow, no IR comments and no identifiers other than register names from
  `WL82.h`, `rt2800.h` or `fm1wifi_regs.h`.
- It reads none of `wl_hw_init`, `wl_usb_to_sdio`, `wl_sdio` and `wl_sdio_host`, and never `wl_wifi*.a` or the
  Bluetooth archives.
- The generated tables are committed. LICENSING.md lists them as register values read from `wl_rf_common.a`
  (facts, no code), not as Apache-2.0 code.

Verdict for the plan: stages 1 and 3 may go ahead on `wl_rf_common.a` as a source of facts within the limits above.
Neither needs the archive's code.

### What this means

- **Nothing found contradicts the Apache-2.0 label for the SDK's C sources, `hsm.a`, and eight of
  `wl_rf_common.a`'s thirteen members. For its other five members the label is unproven, and for the MAC driver in
  `wl_wifi*.a` it is doubtful.** `wl_wifi*.a` is compiled from Ralink's GPL-2.0-or-later driver and Linux's
  GPL-2.0 `wext-core.c`, and `wl_rf_common.a`'s five were compiled with Ralink's and Linux-shaped headers. JieLi's
  Apache-2.0 notice cannot relicense that code. This is not a legal opinion; it is what the file names, function
  names, debug information and the upstream headers show.
- `wpasupplicant.a` is wpa_supplicant 2.9. Upstream it is BSD-3-Clause, and the source is public (w1.fi). The SDK
  ships only the binary.
- For OPLBOX:
  - Copying or translating code out of `wl_wifi*.a` is ruled out twice over: there is no source to give with it, and
    what it was compiled from includes GPL-2.0-only code (`wext-core.c`), which cannot be combined into a
    GPL-3.0-only program. Facts read from it are another matter (register addresses, values, the order of
    operations), but the driver does not need it as a code source anyway (next point).
  - The code bases that are compatible with GPL-3.0-only: Ralink's driver as published in Linux staging
    (GPL-2.0-or-later), Linux's rt2x00/rt2800 driver (`rt2800.h` has `SPDX-License-Identifier: GPL-2.0-or-later`),
    wpa_supplicant (BSD-3-Clause), lwIP (BSD-3-Clause), mbedTLS 2.x (Apache-2.0), and the SDK's own C sources
    (Apache-2.0; Apache-2.0 code may be included in a GPL-3.0 work). `wl_rf_common.a` is a source of facts only
    ("What stages 1 and 3 may take from `wl_rf_common.a`").
  - The WEXT ioctl layer (`wext-core.c`, `sta_ioctl.c`, wpa_supplicant's `driver_wext.c`) is not reused. OPLBOX
    talks to its own driver through a small C interface.
- Stages affected: stage 6 (station MAC layer), stage 7 (lwIP glue) and stage 8 (WPA2) take their code basis from
  the GPL-2.0-or-later and BSD sources above, not from the archives. Stages 1 to 5 (registers, internal bus,
  radio calibration, raw frames) take facts from `wl_rf_common.a` (addresses, values, order) and code from the SDIO
  specification, rt2x00 and our own descriptions.
- Aside: fm1-radio and fm1-base ship firmware that links `wl_wifi_sta_sfc.a` under its Apache-2.0 label
  (fm1-base `LICENSING.md`). The finding above applies to that too.

## Source versus binary in the WiFi path

| Piece | In the SDK as | Licence | fm1-radio (fm1-base `wifi = true`) uses |
| --- | --- | --- | --- |
| lwIP 2.1.3 (IP, TCP, UDP, DHCP, sockets) | source, `lib/net/lwip_2_1_3` (146 `.c`); also prebuilt `lwip_2_1_3(_sfc).a` | BSD-3-Clause | source, `src/api`, `src/core`, `src/core/ipv4`, `src/netif`, `port`, `apps/sock_api`, `apps/dhcp_srv`, `apps/ntp` |
| lwIP to WiFi glue, `port/wireless_ethernetif.c`, `port/LwIP.c` | source | Apache-2.0 (JieLi) | source |
| mbedTLS 2.2.1 and 2.26.0 | source, `lib/net/mbedtls_2_*` | Apache-2.0 | no (fm1-radio uses BearSSL, MIT, for HTTPS) |
| RF tuning constants and the RF RAM table (`STF_*`, `DS_*`, `DSSS_*`, `wl_rfd_ram_lut[256][2]`) | source, `apps/common/net/wifi_conf.c` (1539 lines) | Apache-2.0 | copied: `ble/shim/rf_const.c` (generated by `tools_rfconst.py`), `ble/wifi/rf_moderate.c` |
| Driver configuration (`WL_*` constants, `WL_STA_DAT` profile string) | source, `wifi_conf.c` | Apache-2.0 | copied into `ble/wifi/wifi_shim.c` |
| Calibration getters (`wifi_get_xosc`, `wifi_get_pa_trim_data`, `wifi_get_mcs_dgain`) | source, `apps/common/config/user_cfg.c:526-566` | Apache-2.0 | rewritten in `ble/shim/misc.c:497-533` |
| wpa_supplicant 2.9 (station side) | **binary only**, `wpasupplicant.a` | BSD-3-Clause upstream | `wpasupplicant.a` |
| hostapd + wpa_supplicant (AP side) | **binary only**, `hostapd_and_wpasupplicant.a` | BSD-3-Clause upstream | no |
| 802.11 MAC driver and MLME (Ralink's) and the Linux compatibility layer | **binary only**, `wl_wifi*.a` | see above | `wl_wifi_sta_sfc.a` |
| Radio, PHY, internal SDIO bus (JieLi's) | **binary only**, `wl_rf_common.a` | Apache-2.0 label, unproven ("What `wl_rf_common.a` carries") | `wl_rf_common.a` |
| External-module glue | **binary only**, `ext_wifi_dev.a` | Apache-2.0 (label) | no |
| State machine helper | **binary only**, `hsm.a` | Apache-2.0 | `hsm.a` (`hsm.c` only) |

### Archive inventory

Counts from `llvm-nm` over the unpacked members ("exported" = defined global symbols, "undefined" = referenced
and defined in no member of the same archive). Each archive also holds one small ELF object, `version.z.o`.

| Archive | Members (bitcode + ELF) | Size | Exported | Undefined |
| --- | --- | --- | --- | --- |
| `wl_wifi.a` | 104 + 1 | 8,574,238 | 1543 | 128 |
| `wl_wifi_sfc.a` | 104 + 1 | 8,574,242 | 1543 | 128 |
| `wl_wifi_sta.a` | 87 + 1 | 6,182,374 | 1328 | 122 |
| `wl_wifi_sta_sfc.a` | 87 + 1 | 6,182,370 | 1328 | 122 |
| `wl_wifi_ap.a` | 90 + 1 | 6,296,084 | 1241 | 115 |
| `ext_wifi_dev.a` | 2 + 0 | 74,202 | 23 | 59 |
| `wl_rf_common.a` | 13 + 1 | 321,802 | 209 | 90 |
| `wpasupplicant.a` | 42 + 1 | 2,623,914 | 606 | 56 |
| `hsm.a` | 2 + 1 | 26,168 | 26 | 2 |

- `wl_wifi.a` = `wl_wifi_sta.a` + the AP files (`ap*.c`, `p2p_cfg.c`; 17 members). The `_sfc` variants have the
  same members built for code in flash and small buffers ("no-SDRAM" builds); their bitcode differs.
- Sizes are mostly debug information: the whole station driver links to about 200 KB of named code (below).

### What fm1-radio actually links

fm1-base's build (`tools/build.py:328-330`) links `wl_wifi_sta_sfc.a`, `wpasupplicant.a`, `hsm.a`,
`crypto_toolbox_Osize.a`, `lib_ccm_aes.a`, `wl_rf_common.a`, plus lwIP from source and its shim (`ble/shim/*.c`,
`ble/wifi/*.c`). The members below have symbols in the linked `wifi.elf` of fm1-base's `examples/wifi`. Bytes are
the sizes of the named symbols that survived LTO; inlined code is counted in its caller, so they are a lower bound.

- `wl_rf_common.a`: **13 of 13** code members. `wl_usb_to_sdio` 4988 B, `wf_phy_mac_init` 3014, `wf_rf_cal` 2716,
  `wf_cfr_init` 1714, `wf_rf_init` 964, `wf_soft_adj` 841, `wl_sdio` 812, `wf_phy_mac_lib` 576, `wl_sdio_host`
  168, `wf_deep_sleep` 116, `wl_hw_init` 53, `wl_rfd_init` 48, `wf_pa_dpd` 7. Total 16,017 B named (recounted
  2026-10-25 from the same `wifi.elf`; an earlier version said 4989 and 16,018).
- `wl_wifi_sta_sfc.a`: **58 of 87**, 207,679 B named. Largest: `rtmp_init` 35,245, `cmm_mac_usb` 25,010,
  `cmm_data` 11,446, `sync` 10,840, `mlme` 9,863, `rtmp_init_inf` 9,642, `rt_usb` 9,336, `rtmp_data` 8,412,
  `wifi_connect` 7,766, `rtmp_timer` 6,662, `assoc` 6,274, `connect` 6,156, `sta_ioctl` 5,246, `sta_cfg` 5,108.
  Not linked: the AES/ARC4/DH/big-integer files, WAPI, WPS (`wsc*`), MAT, ATE, `cfg80211*`, `rt30xx`, `frq_cal`,
  `jl_todo`, `todo`, `spinlock`, `wait`, `rt_usb_util`, `client_wds`, `cmm_wpa_adhoc`.
- `wpasupplicant.a`: **35 of 42**, 79,127 B named. `wpa_supplicant` 20,450, `events` 12,860, `scan` 8,526,
  `driver_wext` 7,218, `sha1-internal` 4,320, ... The driver interface is `driver_wext.c` (Linux Wireless
  Extensions, served by JieLi's copy of `wext-core.c`).
- `hsm.a`: `hsm.c` only, 220 B.

Whole-image cost: fm1-base's WiFi example is 416,624 B of image, `.data`+`.bss` 122,080 B and pool 305,988 B.
Its template without WiFi is 82,152 B, 27,844 B and 69,384 B. So JieLi's stack with lwIP and the shim costs
roughly 330 KB of code and 330 KB of RAM. (The two apps differ, so this is an estimate.)

## The hardware the driver talks to

The radio has three layers, and the IR shows which code drives each.

1. **Analog radio and PLL**: JieLi's own registers, written directly by `wl_rf_common.a`.
2. **An internal SDIO bus** between the CPU and the WiFi core ("WL30" in JieLi's names: `wl30_mmc_io_rw_direct`,
   `wl30_mmc_io_rw_extended`, `wl30_sdio_enum`). Despite the names, the CPU reaches the core's registers directly,
   in a memory window, and only the frame FIFOs go through a DMA engine; the core is never enumerated with SDIO
   commands (stage 2; "Internal SDIO enumeration", correction).
3. **The WiFi core's MAC and baseband**, reached as SDIO function 1 registers. Its MAC registers are the
   Ralink RT2800 family's: same addresses and bit fields as Linux's `rt2800.h`. `rt2800.h` has no default values;
   JieLi's 22 MAC defaults equal those of Ralink's own driver (below).
   Ralink's USB driver code talks to it through `wl_usb_to_sdio.c`, which turns USB vendor requests and bulk
   transfers into SDIO transfers.

### CPU-side registers (memory-mapped)

Names from `include_lib/driver/cpu/wl82/asm/WL82.h` (`lsfr_base` 0x10000, `psfr_base` 0x50000,
`map_adr(grp, adr) = (64*grp + adr)*4`). Addresses as the SDK code uses them.

| Range | Block (WL82.h) | Registers used | Used by |
| --- | --- | --- | --- |
| 0x11900-0x11978 | `JL_ANA` (`lsfr_base + map_adr(0x19, 0)`) | `WLA_CON0`..`WLA_CON30` | radio on/off (`wf_rf_en_set`: CON0-CON11, CON14; `rf_on`/`rf_off`: CON0-CON11, CON13-CON17, CON20), RX DC and IQ (`set_anl_rxdc_i/q`, `rd_rx_iq_set`: CON2, CON3, CON4), TX LO and IQ (`txlo_cali_set`, `txlo_calq_set`, `rd_tx_iq_set`: CON7, CON8), PA (`pa_config_set`: CON9-CON11), crystal load caps (`wf_read_xosc`/`wf_write_xosc`: CON12), PLL (`set_pll_rn_en` CON13-14, `set_pll_cap` CON14, `set_pll_fbds` CON15, `open/close_rf_pll_vc_det` CON16), filter bandwidth (`rd_bw_set`: CON23-24), gains (`wfrf_flt_vga_gain`, `wfrf_rx_agc_gain`: CON1, CON3-CON5), status reads (`wlrf_get_`: CON26, CON30) |
| 0x14000-0x14064 | `JL_WL` (`lsfr_base + map_adr(0x40, 0)`) | `WL_CON0`, `WL_CON2`, `WL_CON3`, `WL_ANL_CON0`..`3`, `WF_CON0`..`WF_CON9` | clock and reset (`wf_clk_rst_init`: WF_CON0-CON4, CON7, plus 0x10010 in the clock block), interrupt enable and pending (`wfhw_iqr_en`, `wifi_isr`: WF_CON4), power (`wifi_rf_on/off`: WL_CON0, WF_CON0-CON7), channel and TX power (`wifi_set_channel`, `wifi_set_pwr`: WF_CON7), PLL offset (`wf_rf_pll_offset_adj`: WF_CON9), deep sleep (WL_CON0, WL_CON3, WF_CON7), the RF digital block's indirect register port (`wl_rfd_reg_wr/rd`, `wl_rfd_sync`, `wl_rfd_actl_ram_init`: WL_ANL_CON0-CON3) |
| 0x30F00-0x30F1C | not named in WL82.h (`0x20000` is `bsfr_base`, "bt; wire less") | 0x30F00, 0x30F04, 0x30F10-0x30F1C | the internal SDIO host: `wl30_mmc_io_rw_extended` (0x30F04, 0x30F10-0x30F1C), `wdma_init` (0x30F00, 0x30F04), `sdio_recv_int_hdl` (0x30F04) |
| 0x50000-0x5000C, 0x50048, 0x5101C | `JL_PORTA`, `JL_PORTB`, `JL_IOMAP` | GPIO | external PA enable (`wifi_hw_pa_enable`: PA1/PA2; fm1-base sets `WIFI_PA_ENABLE = 0`), debug signal output, IO mapping (`usb_register`) |

The interrupt is `IRQ_WF_IDX` = 52 (`hwi.h`), requested at priority 2 by `wfhw_iqr_init`. Its handler
`wifi_isr` only posts a semaphore; `sdio_irq_thread` does the work in a task.

### Internal SDIO enumeration (`wl30_sdio_enum`)

`sdio_host_send_command(arg, cmd)`, decoded (command index in bits 8-13 of the second argument):

1. CMD5 (IO_SEND_OP_COND), arg 0x1F000000, twice.
2. CMD3 (SEND_RELATIVE_ADDR), then CMD7 (SELECT_CARD), arg 0x00010000.
3. CMD52 writes to function 0 (CCCR): 0x02 I/O Enable = 0xFF, 0x07 Bus Interface Control = 0x02 (4-bit bus),
   0x04 Int Enable = 0x03 (master and function 1).
4. CMD52 writes of 0 to function 1 registers 0x10, 0x11, 0x12, 0x13 (WiFi-core specific).

**Correction (stage 2, 2026-10-25): the WiFi core is not enumerated, and these commands never reach it.** Nothing in
the SDK calls `wl30_sdio_enum`: no member of any archive in `cpu/wl82/liba/` references it (`nm -A *.a | grep
wl30_sdio_enum` finds only its definition), and `wf_rf_common_init` in `wl_usb_to_sdio` brings the core up with
`wf_clk_rst_init`, `wdma_init` and `wfhw_init` alone. The `sdio_host_send_command` it calls is in `cpu.a`'s `sd.c.o`,
JieLi's driver for the external SD/SDIO controllers `JL_SD0` (0x11400) and `JL_SD1` (0x11500) on GPIO pins (the
SDK's `demo_wifi_ext` uses it for an external WiFi module; `sdio_host_init.h` lists their pin groups); it writes the
command into a RAM buffer whose address goes to `CPTR` and polls bit 7 of `CON0`. It does not touch the WiFi core's
blocks. And `wl30_mmc_io_rw_direct`, JieLi's CMD52, only asserts. The internal core's registers are reached directly
instead ("WiFi core registers", below), and stage 2 follows `wf_clk_rst_init` and `wdma_init` ("Stage 2 bring-up").
`sd.c.o` was read for these facts only; its debug information names only the SDK's own headers (`WL82.h`, `cpu.h`,
`sdmmc.h`, `port_waked_up.h`). The SD command enumeration built for stage 2 before this was found
(`fm1_wifi_sdio_enum`, TODO.md 7b) is gone.

### WiFi core registers (SDIO function 1)

`sdio_mac_wreg(addr, val)` = CMD53-style `wl30_mmc_io_rw_extended(write=1, fn=1, addr, 4 bytes)`;
`sdio_mac_rreg` the same with write=0. On the CPU side there is no bus transfer behind that (stage 2,
`wl30_mmc_io_rw_extended`'s IR): function 1 address A is the word at CPU address `0x30000` + A, read or written with
one plain 32-bit load or store; the function argument is ignored. Only the two frame FIFOs differ: a write to
address 0 is a DMA through the WL30 host block (0x30F10 = buffer + 4, 0x30F14 = increment << 31 | length, then wait
for bit 20 of 0x30F04 and set bit 10 to clear it), a read from 12 likewise (0x30F18, 0x30F1C, bit 21, bit 11).
The addresses, checked against Linux's `rt2800.h`:

| Address | rt2800.h name | Where JieLi uses it |
| --- | --- | --- |
| 0x1004 | `MAC_SYS_CTRL` | `wf_phy_mac_rst` writes 3 (reset CSR and BBP), `wf_phy_rst` writes 12 (enable TX and RX) |
| 0x101C | `BBP_CSR_CFG` | `bbp_set`/`bbp_rd`: polls bit 17 (`BUSY`), writes `reg<<8 \| val \| 0x80000` (`BBP_RW_MODE`) then `\| 0xA0000` (`\| BUSY`); a read uses `0x90000`/`0xB0000` (`READ_CONTROL`). Exactly rt2800's field layout |
| 0x0408, 0x040C, 0x0438, 0x043C | `PBF_CFG`, `PBF_MAX_PCNT`, `TXRXQ_PCNT`, `PBF_DBG` | `PBF_CFG` and `PBF_MAX_PCNT` set from `MACRegTable`; the queue counts read in `usb_submit_urb` and `get_txrxq_pcnt` (with 0x0434, not in rt2800.h) |
| 0x1500, 0x1504 | `TX_SEC_CNT0`, `RX_SEC_CNT0` | read in the TX path |
| 22 registers of `MACRegTable` | `BCN_OFFSET0/1`, `LEGACY_BASIC_RATE` (0x13F), `HT_BASIC_RATE`, `RX_FILTER_CFG` (0x17F97), `TX_LINK_CFG`, `MAX_LEN_CFG`, `TX_RTY_CFG` (0x47D01F0F), `AUTO_RSP_CFG`, the six `*_PROT_CFG`, `TXOP_CTRL_CFG`, `TX_RTS_CFG`, `EXP_ACK_TIME`, `TXOP_HLDR_ET`, `PWR_PIN_CFG` | `NICInitializeAsic` (`rtmp_init.c`): all 22 addresses are named in rt2800.h. The values are not from rt2800.h (which has none); they are below |
| 0x4000, 0x6000, 0x6800, 0x6C00, 0x7000 | `PAIRWISE_KEY_TABLE_BASE`, `MAC_IVEIV_TABLE_BASE`, `MAC_WCID_ATTRIBUTE_BASE`, `SHARED_KEY_TABLE_BASE`, `SHARED_KEY_MODE_BASE` | `AsicAddPairwiseKeyEntry`, `AsicUpdateWCIDIVEIV`, `AsicUpdateWcidAttributeEntry`, `AsicAddSharedKeyEntry`: the MAC encrypts and decrypts in hardware |
| 0x1100, 0x1104, 0x1330, 0x1334, 0x1338, 0x1348 | `XIFS_TIME_CFG`, `BKOFF_SLOT_CFG`, `TX_SW_CFG0/1/2`, `TX_TIMEOUT_CFG` | `wf_sifs_reset` (JieLi's values) |
| 0x0308, 0x030C, 0x0324, 0x0328 | none: JieLi's own | 0x0308 = 0x9407 in `wfhw_init` and `wf_phy_init`; 0x030C = 30 then 0 in `wf_phy_mac_rst`; 0x0328 written around `request_irq` in `wfhw_iqr_init`; 0x0324 written in the receive handler `sdio_recv_int_hdl` |
| function 1, address 0 (write) and 12 (read) | none | frame data: `usb_submit_urb` writes TX frames to address 0, `sdio_recv_int_hdl` reads RX frames from 12 |

Where the 22 `MACRegTable` values come from (checked 2026-10-25, method in "Reproducing the measurements"):

- All 22 (address, value) pairs in JieLi's `rtmp_init.c.o` equal the `MACRegTable` of Ralink's driver in Linux
  v2.6.32 (`drivers/staging/rt2860/common/rtmp_init.c`, GPL-2.0-or-later), as built for RT2870 with
  `HW_BEACON_OFFSET` 0x200 (`MAX_LEN_CFG` = `MAX_AGGREGATION_SIZE` 3840 | 0x1000 = 0x1F00, from `rtmp_def.h`).
- Ralink's table has 30 entries. JieLi leaves out 8: `MAC_SYS_CTRL` (set in `wf_phy_mac_rst`), `XIFS_TIME_CFG`,
  `BKOFF_SLOT_CFG`, `TX_SW_CFG0`, `TX_SW_CFG1`, `TX_TIMEOUT_CFG` (set with JieLi's values in `wf_sifs_reset`),
  `LED_CFG` and `WPDMA_GLO_CFG`.
- Linux rt2x00 (`rt2800lib.c` v6.6) writes six of them as the same literal values (`LEGACY_BASIC_RATE`,
  `HT_BASIC_RATE`, `PBF_MAX_PCNT`, `PBF_CFG`, `EXP_ACK_TIME`, `PWR_PIN_CFG`). It differs for `TX_RTY_CFG`: retry limits
  2 and 2 instead of 15 and 31. It builds the other 15 from bit fields, and those were not compared [unchecked].

The baseband is reached through `BBP_CSR_CFG`, and four further indirect register windows sit on top of BBP
registers [no standard term for these windows]: `cca_reg_set(a, d)` = BBP 0xC9 = a, 0xCA = d, 0xC8 = 1;
`rx11n_reg_set` = 0xCD/0xCE/0xCC; `tx_backend_wr` = 0xD1/0xD2/0xD0; `rx_frontend_wr` = 0xD5/0xD6/0xD4. These are
JieLi's, not RT2800's (an RT2800 has its own RF chip on `RF_CSR_CFG`; JieLi's channel switch, `RT30xx_ChipSwitchChannel`,
calls `wifi_set_channel` in `wl_rf_common.a` instead).

No MCU firmware is loaded: `rt28xx_init` never calls a firmware loader (`RTUSBFirmwareWrite` is defined but
unused), and the archives hold no firmware image. `AsicSendCommandToMcu` does nothing and returns 1.

### Stage 2 bring-up

What `firmware/wifi/fm1wifi.c` does to power the core up, as a list of facts read from `wl_sdio.c.o`'s
`wf_clk_rst_init` and `wdma_init` (register, bits, order, delays; no code). `tests/wifi/bringup_expect.txt` is
this list written out by hand as a fixture, and `tests/wifi_model.c` diffs the library's run against it. Each step
is one read-modify-write, one change at a time, as the SDK makes them. What each bit means is not known.

| Step | Register | Change |
| --- | --- | --- |
| 1 | `CLK_CON1` (0x10010), bits 15-14 | cleared (the register is shared with USB and UART: the port changes these two bits only) |
| 2 | `WF_CON0`-`WF_CON4` (0x14040-0x14050) | each written 0 |
| 3 | `WF_CON0` | bit 8, bit 6, bit 0 set; 1 us; bit 2; 1 us; bit 3; 1 us; bit 1; 50 us |
| 4 | `WF_CON7` (0x1405C) | bits 7-0 = 5; bits 15-8 cleared; bits 23-16 cleared; bits 26-24 set; bit 30 set; bit 31 cleared |
| 5 | `WF_CON3` | bit 24 set |
| 6 | WL30 host 0x04 (0x30F04) | bits 11, 10, 9, 5, 4 set; bit 3 cleared; bits 2, 1, 0 set |
| 7 | WL30 host 0x00 (0x30F00) | bit 10 set; bits 9, 8 cleared; bits 7-5 = 3; bit 4 cleared; bits 3, 2, 1, 0 set |

`wdma_init` then enables the external PA only when `WIFI_PA_ENABLE` is set; fm1-base sets it to 0, so stage 2 skips
it. Before step 1 the library reads and keeps every register it changes; power-down writes them back in the order
WL30 0x00, 0x04, `WF_CON7`, `WF_CON4`-`WF_CON1`, `WF_CON0`, then restores `CLK_CON1`'s two bits, so the core goes back
to how the boot left it (held in reset, if it was). Not done in stage 2: `wfhw_init` (the MAC and PHY reset and
tables, stage 3). Stage 2 then reads `MAC_CSR0` and `MAC_SYS_CTRL` through the window, and BBP register 0 through
`BBP_CSR_CFG`: wait for BUSY clear, write `0x90000` | reg << 8, then the same | BUSY, wait for BUSY clear, the value
in bits 7-0. Each wait reads at most 1000 times, 1 us apart. JieLi's `bbp_rd` waits without a bound.

### Power-up and calibration sequence

From the call order in the IR (constants are the arguments passed):

```
usb_register
  wf_rf_common_init
    wf_clk_rst_init          clock 0x10010, WF_CON0-4/7: enable, reset pulses (delay_us 1,1,1,50)
    wdma_init                internal SDIO DMA (WF_CON3, 0x30F00/0x30F04); reads WIFI_PA_ENABLE (0 in fm1-base)
                             for wifi_hw_pa_enable (PA1/PA2 as outputs, WL_CON2 |= 0x103)
    wfhw_init
      wf_phy_mac_rst         MAC_SYS_CTRL=3; 0x30C=30,0; wf_phy_rst (MAC_SYS_CTRL=12; BBP 22=0, 21=49, 21=48, 25=0)
      wf_sifs_reset; 0x308=0x9407
      cfr_int(0); wf_phy_init  CCA table: cca_reg_set(0..86, constants), rx11n/tx_backend/rx_frontend tables
      wf_rf_init
        clk_get("osc") = 24 MHz; wifi_get_xosc -> WLA_CON12
        wf_rf_en_set(5); set_pll_rn_en; wfa_pll_bank_scan(121); set_pll_rn_en; delay_us(80)
        if a stored trim is valid (wifi_trim_is_already):
          quick_start_wifi_trim   68-byte record from wifi_get_rf_trim_data, CRC16 over 64 bytes, then
                                  TX LO I/Q, TX gain/phase/DC, RX analog DC, RX gain/phase
        else wf_rf_trim:
          enter_wifi_rf_trim_region; wf_rx_tune_test; wf_rf_en_set(2); rx_tx_cal_attenuation(1);
          txiq_tune_soft; wfrf_hp_close; wf_rf_en_set(0); rxdc_tune_soft; wfrf_hp_open;
          wf_rf_en_set(3); rx_tx_cal_attenuation(0); rxiq_tune_soft; exit_wifi_rf_trim_region
          (result saved with wifi_set_rf_trim_data)
        wf_rf_en_set(0); pa_config_tune (wifi_get_pa_trim_data); keep_tx_iq_set; tx_rx_bw_set_save;
        wfrf_flt_vga_gain(0)
      wf_phy_init; cfr_int(1); rapp_init (a RAM and thresholds per 11b, 11g/n, BPSK through
                             tx_backend_wr; "cfr"/"rapp" suggest crest-factor reduction and a Rapp PA model,
                             not confirmed)
      wl_anl_actl_init(0)    RF digital block: wl_rfd_sync, then wl_rfd_actl_ram_init loads wl_rfd_ram_lut
                             through WL_ANL_CON* (FEQ entries 0-127, AGC 128-223, APC from 224)
      wf_rf_bsb_auto_ctl; wifi_set_pwr(6); wifi_set_channel(6); pa_mcs_dgain_init (wifi_get_mcs_dgain)
      rf_dc_get_reload; a 3000 ms timer for temperature drift (wl_temperature_drift_freq_trim)
  wlc_main (coexistence; empty in fm1-base)
  gpio_direction_input(18..24); msleep(100)
  rtusb_probe (Ralink driver start: rt28xx_init -> NICInitializeAsic -> MACRegTable ...)
  wl30_sdio_claim_irq (IRQ 52)
```

`wfa_pll_bank_scan` = open the VCO detector, start at the middle bank, then set cap and feedback divider,
read the VCO result, repeat, settle on a bank, close the detector. `rd_adc_peak`/`adc_pp_tune` read the
receiver's ADC peaks during the trim.

Constraints the sequence carries, all learned the hard way by fm1-radio:

- The trim draws about 500 ms of high current, and a flash fetch during it reads corrupted data. JieLi's demos
  put the trim code in RAM (`CONFIG_RF_TRIM_CODE_AT_RAM`). Every BLE crash in fm1-radio's notes 03-05 was this,
  until fm1-ctl moved `*.rf.text.cache.L2.rf_trim` and the delays into RAM (`06-controller-handoff.md`, last
  section; fm1-base `device/app_wifi.ld`). In fm1-base's WiFi build the RAM-resident radio code is about 3.5 KB
  (`txiq_tune_soft` 748 B, `rxdc_tune_soft` 416, `rxiq_tune_soft` 386, `wf_rf_en_set` 376, ...).
- fm1-base holds the sound for the radio's first second (`plat.h:120`).
- A trim stored by a Bluetooth start made the access point unable to hear WiFi; a fresh trim at every start
  joined at once (`ble/shim/misc.c:524-529`, 2026-10-16). fm1-base therefore never stores the trim.
- "The radio hardware writes somewhere in [the pool] (unfound), and moving the stacks / heap / lwIP pools by even
  4.6 KiB broke its reception" (fm1-base `device/app_wifi.ld`). Something holds a fixed RAM address or a stale
  DMA pointer. Our driver has to find it before it lays out its buffers (stage 2).

### Where the tuning values come from

| Value | Source | Default when absent |
| --- | --- | --- |
| Detection thresholds `STF_*`, `DS_*`, `DSSS_*`; the 256-entry RF table `wl_rfd_ram_lut` | SDK source `apps/common/net/wifi_conf.c` (two branches: default and "WIFI RX moderate strategy") | compiled in |
| Crystal load caps, 2 bytes | syscfg item `VM_XOSC_INDEX` (106) | `{0x0b, 0x0b}` (SDK board default; fm1-radio notes 03 saw `wifi_get_xosc 11/11`) |
| PA trim, 7 bytes | `VM_WIFI_PA_DATA` (107) | `{1, 7, 4, 7, 11, 1, 7}` |
| Per-MCS digital gain, 20 bytes | `VM_WIFI_PA_MCS_DGAIN` (108) | from the SDK's `cfg_tool.bin` (fm1-base `misc.c:509-510`) |
| Trim result, 68 bytes with CRC16 | `VM_WIFI_RF_INIT_INFO` (187) | a fresh trim |
| Reference clock | `clk_get("osc")` | 24 MHz |

On a stock FM-1 these items would sit in M-VAVE's VM area. Whether M-VAVE wrote per-device values at the factory is
unknown; fm1-radio works on the defaults.

### OS services the binary stack needs

The members fm1-radio links reference 163 symbols defined outside the four archives. fm1-base supplies them
(`ble/wifi/wifi_shim.c` 67, `ble/shim/misc.c` 39, `rf_const.c` 15, `os.c` 12, `systimer.c` 5, lwIP 6). The other
19 are in no fm1-base object: libc from the toolchain, mbedTLS, or references the linker dropped with their callers.
Grouped:

| Group | Symbols |
| --- | --- |
| Tasks and blocking | `os_sem_create/pend/post/del/set/valid`, `os_mutex_create/pend/post/del/valid`, `os_q_*`, `os_time_dly`, `os_current_task(_rom)`, `thread_fork`, `thread_kill`, `thread_rpc`, `vPortYield` |
| Timers and time | `sys_timer_add_to_task`, `sys_timer_del`, `sys_timeout_add_to_task`, `usr_timeout_add/del`, `sys_timer_get_ms`, `jiffies` (10 ms), `OSGetTime`, `time_lapse`, `udelay`, `mdelay`, `msleep`, `delay_us` |
| Interrupts | `request_irq`, `unrequest_irq`, `local_irq_disable/enable`, `__local_irq_*` |
| Memory, libc | `malloc`, `zalloc`, `calloc`, `realloc`, `free`, `kmalloc/kzalloc/kfree`, `vmalloc`, string and ctype functions, `snprintf` family, `qsort`, `rand`, `errno`, `__assert_func`, `abort` |
| Settings store | `syscfg_read`, `syscfg_write` |
| Calibration | `wifi_get_xosc`, `wifi_get_pa_trim_data`, `wifi_get_mcs_dgain`, `wifi_get_rf_trim_data`, `wifi_set_rf_trim_data`, `clk_get`, `adc_add_sample_ch`, `adc_get_value`, `CRC16` |
| Power | `low_power_get/put/request/exit_request`, `low_power_hw_unsleep_lock/unlock`, `check_low_power_hw_if_locked`, `low_power_check_bt_exist`, `low_power_get_config`, `P33_SYSTEM_RESET` |
| Configuration constants | the `WL_*` debug and queue sizes, `MAX_LEN_OF_BSS_TABLE`, `GET_WL_STA_DAT` (the profile string), `wifi_lowpower_mode`, ... |
| Network glue | `Init_LwIP`, `lwip_netif_set_up`, `lwip_renew`, `lwip_dhcp_release_and_stop`, `dhcps_init/uninit`, scan-record and filter hooks |
| Crypto | `mbedtls_aes_init/setkey_dec/crypt_cbc/free` |

Our own driver needs almost none of this: it runs from OPLBOX's main loop without tasks, so the task, semaphore
and Linux-emulation groups disappear.

### What fm1-radio's notes and probe already cover

- `notes/01`-`06` and `probe/` are about **Bluetooth** (the BLE MIDI work that came before Radio): the link probe
  that found the 87 (115 with `wl_rf_common.a`) symbols a BLE build needs, the shim, the crashes during RF
  calibration and their cause (trim code fetched from flash). They do not cover WiFi. What carries over: the
  calibration crash and its fix, IRQ priorities (a JieLi handler at priority 7 is still maskable by `cli` on
  this chip; measured), and that `wl_rf_common.a` is shared by Bluetooth and WiFi.
- `notes/aac-crash/` is about the AAC decoder.
- fm1-radio's WiFi findings live in fm1-base's code comments instead: the RAM layout warning
  (`device/app_wifi.ld`), never storing the trim (`ble/shim/misc.c`), the moderate RX branch
  (`ble/wifi/rf_moderate.c`), what each library call expects (`ble/wifi/wifi_shim.c`, e.g. `udelay` must sleep, not
  spin, or reception starves), and `tools_rfconst.py`, which extracts the RF constants from `wifi_conf.c`.
- Nobody has documented the registers, the internal SDIO bus or the MAC's RT2800 lineage. This file is the first.

## OPLBOX's constraints

From `docs/BASE.md` and a build of this tree (f36fe5d, `./build.sh` on this machine):

| Resource | OPLBOX today | Free | JieLi's stack would need |
| --- | --- | --- | --- |
| App image (XIP slot) | 342,792 of 581,564 B | 238,772 B | about 330 KB: does not fit |
| `.data`+`.bss` | 36,776 of 98,304 B | 61,528 B | about 94 KB more |
| Pool | 255,888 of 344,064 B | 88,176 B | about 237 KB more |
| RAM code (`.ram_text`+`.ram_code`) | 6,840 of 24,576 B | 17,736 B | about 3.5 KB (the trim) |

- **Size budget for the driver**: everything (driver, station MAC layer, WPA2, lwIP) has to fit in the free
  238 KB of code and about 140 KB of RAM, with headroom for the song features. Target: under 120 KB of code and
  under 48 KB of RAM. A minimal lwIP (UDP, TCP, DHCP, no sockets API) is roughly 40-50 KB of code.
- **Audio**: the ALNK0 interrupt renders 256 frames at 44.1 kHz and must finish within `ISR_BUDGET_US` 5805
  (`oplbox.c:59`). WiFi must never run inside it or block it. IRQ 52 must run at a lower priority than ALNK0
  and only set a flag; all SDIO transfers run in the main loop; busy-waits (`BBP_CSR_CFG` polling) have a bound.
  The trim (about 500 ms, no flash fetches) runs with the audio muted, as fm1-base does for one second. PBKDF2
  (stage 8) runs a few iterations per main-loop pass.
- **Flash**: song slots 9 × 32 KiB at 0x98000-0xE0000, settings sector 0xFC000 (`BASE.md`). The free window
  0xE7000-0xFBFFF can take one 4 KiB sector for WiFi settings (SSID, PMK, MAC address) without touching the
  slots.
- **No RTOS**: OPLBOX is a main loop (Felucca's platform). The driver is a polled state machine.
- **Bluetooth**: OPLBOX has none. If it ever gets BLE, `wl_rf_common` is shared, and WiFi/BT coexistence
  (`wlc_main`, PTA) becomes a question.
- **Stage 2's cost** (`./build.sh`, against main ddd5b5c): the image grows by 2,996 B to 354,580 of 581,564 B
  (`fm1wifi.c`, its port and the page), `.data`+`.bss` by 48 B to 39,152 of 98,304 B; the pool (257,744 B) and the
  RAM code (6,840 B) are unchanged. Against the driver's budget (under 120 KB of code, under 48 KB of RAM): about 3 KB
  and 48 B so far.

## Standalone library

xero wants the WiFi support shareable on its own (2026-10-25): his friend's webcam / video-out firmware runs on the
same FM-1 and could stream over WiFi (RTSP or similar) instead of USB. So the driver is a library, `fm1wifi`, in
`firmware/wifi/` (its `README.md` has the API and how to link it), not OPLBOX code:

- **One directory.** `firmware/wifi/` holds the driver (`fm1wifi.c`, `fm1wifi.h`, the register map
  `fm1wifi_regs.h`), the port header (`fm1wifi_port.h`), and the WPA2 primitives (`wpa_crypto.c` on its BearSSL
  subset), which already had no OPLBOX dependency. Nothing in it includes a file from outside it, uses an OPLBOX
  global, or touches a register itself.
- **The port.** Everything the library needs from the board goes through `fm1wifi_port.h`, which the firmware that
  links it implements: 32-bit register access to the WiFi blocks, the WiFi bits of the shared `CLK_CON1`, and a
  microsecond delay. Interrupt hookup (IRQ 52) joins it at stage 4, when the library first needs it. OPLBOX's
  implementation on felucca-bsp is `firmware/hal/fm1wifi_port_bsp.c`, a file of its own with nothing of OPLBOX's in
  it, so another FM-1 firmware on felucca-bsp can compile it as is. The host tests have a second one,
  `tests/wifi/sdio_model.c`.
- **Checked.** `tests/run_host.sh` compiles the library with only its own directory on the include path (and the
  model's for the tests) and fails if any of its files includes something from elsewhere; `tools/build.py`
  compiles each of its files alone with the JieLi toolchain, and its register check keeps every access out of
  `firmware/wifi/` (the register map's addresses are numbers handed to the port).
- **The layering for the later stages**, each keeping the same no-OPLBOX rule: the driver (stages 2-3: power,
  registers, calibration) → raw frame TX/RX (stages 4-5: the FIFOs, RX/TX descriptors, channel) → the station MAC
  (stage 6: scan, authenticate, associate; stage 8: WPA2 keys) → a netif-style packet interface: Ethernet frames in
  and out, the MAC address, link up/down. An IP stack such as lwIP plugs into that interface (stage 7 is OPLBOX's
  glue to lwIP), so a third party can run their own TCP, UDP or RTSP on it. OPLBOX's own uses (song transfer,
  updates, stage 9) sit on top like any other caller.
- **Licence: an open decision for xero.** Today every file in the library is GPL-3.0-only, like the rest of OPLBOX.
  Stage 6 takes its code basis from GPL-2.0-or-later sources (Ralink's staging driver, Linux rt2x00; "What this
  means" above), so the shared library as a whole will be GPL-licensed. Which GPL version (GPL-3.0-only as now, or
  GPL-2.0-or-later so a GPL-2.0 firmware could use it) is xero's to choose; nothing here picks or changes one. xero
  said on 2026-10-25 that his friend's firmware is GPL as well.

## Plan

Each stage is one task, on its own branch, merged before the next begins unless marked parallel. "Host" is what
a host test can prove; "FM-1" is what needs xero to flash a build and report. The driver goes under
`firmware/wifi/` (the fm1wifi library, "Standalone library") and its host tests under `tests/`. The build allows
register access only in `firmware/bsp/` and `firmware/hal/`, so the library hands every access to its port, and
OPLBOX's port is `firmware/hal/fm1wifi_port_bsp.c`.

### Stage 1: register map and extracted tables (host only)

- `firmware/hal/fm1_wifi_regs.h`: the CPU-side blocks above (`JL_ANA` `WLA_CON*`, `JL_WL`, the SDIO host at
  0x30F00, IRQ 52), the WiFi core's MAC registers (the rt2800 subset in use, with rt2800.h names), the BBP
  indirect windows.
- `tools/wifi_ir.py`: reads the user's SDK (`$AC79_SDK/cpu/wl82/liba/wl_rf_common.a`), runs `llvm-dis` and writes
  the constant register sequences (`wf_phy_init`'s CCA and front-end tables, `wf_sifs_reset`, `rapp_init`) as C
  tables with a provenance comment. Only from `wl_rf_common.a`: the MAC defaults (`MACRegTable`) come from rt2x00
  or Ralink's staging driver, not from `wl_wifi*.a`. What it may read and write is listed in "What stages 1 and 3
  may take from `wl_rf_common.a`". Generated output is committed (register values as facts, noted in
  `LICENSING.md`), and a host test re-runs the extraction and checks the tables still match.
- `wifi_conf.c`'s constants and `wl_rfd_ram_lut`, as fm1-radio's `tools_rfconst.py` does (Apache-2.0).
- **Host**: tables compile, extraction is reproducible. **FM-1**: nothing.

### Stage 2: internal bus and first contact (needs FM-1)

- The core's power-up and power-down ("Stage 2 bring-up": the `WF_CON*` clock and resets and the WL30 host
  block's DMA set-up), its MAC registers through the window at 0x30000, a bounded `bbp_rd`/`bbp_set`. Not the SD
  command enumeration first planned here: the core is not enumerated ("Internal SDIO enumeration", correction).
- A diagnostic page that prints the reads of `MAC_CSR0` (0x1000, ASIC version), `MAC_SYS_CTRL` and BBP register 0.
  It also fills the free pool with a pattern before WiFi starts and reports any byte that changes, to find the RAM
  the hardware writes (the fm1-radio layout warning).
- **Host**: a register model (the library's port on the host, logging every register access) checks the sequence.
  **FM-1**: xero flashes, opens the page, reads out four numbers and the pattern check.
- **Built** (TODO.md 7e): the fm1wifi library ("Standalone library") and the page. Outside EDIT, **SAVE** opens it;
  nothing touches the radio until **PLAY** there, which stops the sound, fills the free pool (from the end of the
  last `.pool` buffer to the end of the pool region, about 84 KB), powers the core up and shows `RESULT`,
  `MAC_CSR0`, `MAC_SYS_CTRL`, `BBP 0` and `RAM CHECK` (bytes changed, and the first one's address), checking the
  pool again every half second. **SAVE** leaves: the core is powered down, the sound comes back. Host-tested;
  not run on an FM-1.
- What could go wrong on the FM-1: the page's first read of the core's window after power-up. If the core's clock
  or bus is not running, that load might never complete and the FM-1 would hang on the page until it is powered
  off; nothing is written to flash, and the boot, the boot guard and the OCT− USB rescue are untouched (the page
  only runs when opened), so the FM-1 still boots and can be rescued.

### Stage 3: radio power-up and calibration (needs FM-1)

- Clock/reset, PHY reset and tables (stage 1), crystal caps, PLL bank scan, the trim (TX IQ, RX DC, RX IQ), PA
  configuration, the RF RAM table, `wifi_set_channel`/`wifi_set_pwr`. Trim code and its delays in `.ram_code`,
  audio muted during it.
- Written from a plain description of each step, made from the facts `tools/wifi_ir.py --trace` records, not
  translated from `wl_rf_common.a`'s IR ("What stages 1 and 3 may take from `wl_rf_common.a`").
- **Host**: the register model replays our sequence and diffs it against the sequence extracted from the IR
  (`tools/wifi_ir.py --trace`). **FM-1**: the diagnostic page shows the PLL bank, trim results and the RSSI of the
  noise floor per channel 1-13. Optional cross-check: a diagnostic build of fm1-base's WiFi example (never
  shipped) dumps `WLA_CON*`/`WF_CON*` after its own start, for comparison with ours on the same FM-1.

### Stage 4: receive raw frames, passive scan (needs FM-1)

- IRQ 52, RX queue count (`TXRXQ_PCNT`), FIFO read from function 1, RXINFO/RXWI parsing (rt2800 RX descriptor
  layout, checked on real frames), `RX_FILTER_CFG` for beacons. Channel hop 1-13, collect beacons, build the
  SSID/BSSID/RSSI/security list.
- **Host**: the parser on recorded frames (the FM-1 dumps a few raw frames over USB SysEx or onto the screen in
  hex; they become fixtures). **FM-1**: the page lists the networks around, the first visible milestone.

### Stage 5: transmit raw frames, active scan (needs FM-1)

- TXINFO/TXWI (rt2800 TX descriptor), FIFO write, the MAC defaults (Ralink's staging-driver values, which equal
  JieLi's 22 `MACRegTable` entries; where rt2x00 differs, as for `TX_RTY_CFG`, stage 5 picks one and says why), MAC address (`MAC_ADDR_DW0/1`, kept in the
  WiFi settings sector, made once from the chip ID so the router sees one FM-1), retries by the MAC
  (`TX_RTY_CFG`). Send probe requests.
- **Host**: descriptor encoding tests. **FM-1**: probe responses arrive (hidden networks too); optionally xero
  checks with a phone's WiFi analyser that the FM-1's MAC address shows up.

### Stage 6: 802.11 station, open networks (needs FM-1)

- Open-system authentication, association, BSSID and filters, rate set, beacon-loss and deauth handling,
  disconnect and rejoin. A small state machine written from IEEE 802.11-2020 (clauses 11.3, 9.3.3), taking MAC
  programming from rt2x00 and Ralink's staging driver where needed (both GPL-2.0-or-later).
- **Host**: the state machine with frame fixtures and timeouts. **FM-1**: joins an open hotspot (a phone).

### Stage 7: IP over WiFi (parallel with stage 8 after stage 6; needs FM-1)

- lwIP 2.1.3 from source (BSD-3-Clause; the SDK's copy or upstream), `NO_SYS=1` raw API polled from the main
  loop, a netif doing 802.11 data frames to Ethernet frames (LLC/SNAP), DHCP, ARP, ICMP. A small `lwipopts.h`
  sized to the budget.
- **Host**: the netif and lwIP run in the host build against a loopback that replays 802.11 data frames; DHCP
  against a scripted server. **FM-1**: gets an address on the open hotspot and answers ping.

### Stage 8: WPA2-PSK (parallel with stage 7 after stage 6; needs FM-1)

- EAPOL-Key 4-way and group handshakes, PBKDF2-SHA1 for the PMK (4096 iterations, a few per main-loop pass, cached
  per SSID in the settings sector), PTK/GTK derivation, AES key unwrap, keys into the MAC's hardware tables
  (0x4000, 0x6000, 0x6800, 0x6C00, 0x7000) so CCMP runs in hardware.
- Code basis: wpa_supplicant 2.9's `src/rsn_supp` and `src/crypto` (BSD-3-Clause) or BearSSL (MIT), whichever is
  smaller. The whole of wpa_supplicant (79 KB in fm1-radio) is too big.
- **Host**: IEEE 802.11 Annex J test vectors (PBKDF2, PRF), and a recorded handshake replayed to the code.
  **FM-1**: joins xero's WPA2 network.
- Built so far (host part, TODO.md 7d): `firmware/wifi/wpa_crypto.c` with the stepped PMK, the PRF and PTK and AES key
  unwrap, on a BearSSL subset (2,932 B, against about 8,914 B for wpa_supplicant's SHA-1/HMAC/AES; 4,109 B with
  our code). The EAPOL-Key handshakes are still to do.

### Stage 9: OPLBOX integration (needs FM-1)

- A WiFi page in OPLBOX's UI (on/off, scan list, join, password entry on the keys, state), the settings
  sector, a build check that holds the driver to its size budget, the audio interplay (mute only during the
  trim), and the first use of the network (song transfer, which is outside this plan).
- **Host**: the UI in the host simulator. **FM-1**: a full session: power on, join, play a song, power off, rejoin.

### Order and what runs in parallel

Stages 1 to 6 are a chain: each needs the previous one on hardware. Stages 7 and 8 run in parallel after 6.
Stage 9 can start its UI on the host while 7 and 8 are being built. In parallel with stage 1 (host only), the
register model and fixture format that stages 2 to 5 test against can be built.

### Open questions

- What the bits power-up sets mean (`WF_CON0`, `WF_CON7`, the WL30 host block, `CLK_CON1` bits 15-14), and whether
  the core's window reads anything before `wfhw_init` (stage 2's page answers the second).

- Which RAM address the hardware writes (stage 2's pattern check).
- What the JieLi registers 0x0300-0x032C in the WiFi core do beyond interrupts (stage 2-4).
- Whether the factory left per-device calibration on stock FM-1s, and where (compare a stock firmware dump).
- Whether the RX descriptor matches rt2800's RXWI exactly or JieLi changed it (stage 4, from real frames).
- The licence finding above: whether to tell fm1-radio and fm1-base's authors.
- Where JieLi's `inc/linux/` and Ralink headers in `wl_rf_common.a` came from, and which Linux version the MMC
  structures match (only matters if the eight clean members are ever to be ported rather than described).

## Reproducing the measurements

```sh
S=$(mktemp -d); L=~/fw-AC79_AIoT_SDK/cpu/wl82/liba
for a in wl_wifi_sta_sfc wl_rf_common wpasupplicant hsm ext_wifi_dev; do
  mkdir -p $S/$a && (cd $S/$a && ar x $L/$a.a); done
file $S/wl_rf_common/*.o                         # LLVM IR bitcode
llvm-dis $S/wl_rf_common/wf_rf_init.c.o -o -     # IR with names and line numbers
llvm-nm --defined-only $S/wl_rf_common/*.o       # exported symbols
strings -n 4 $L/wl_wifi_sta_sfc.a | grep -c -i ralink
grep -o 'inttoptr (i32 [0-9]* to ptr)' wf_rf_init.ll | sort | uniq -c   # MMIO addresses (decimal)
```

`wl_rf_common.a` member by member, and the `MACRegTable` comparison (2026-10-25):

```sh
C=~/.jieli/toolchain/common/bin/clang      # clang 4.0.1, the version that wrote the bitcode
for f in $S/wl_rf_common/*.c.o $S/wl_wifi_sta_sfc/rtmp_init.c.o $S/wl_wifi_sta_sfc/wext-core.c.o; do
  $C -target pi32v2 -x ir -S -emit-llvm -O0 $f -o ${f%.o}.ll; done
grep -o 'DIFile(filename: "[^"]*"' $S/wl_rf_common/wl_sdio.c.ll | sort -u     # headers it was compiled with
grep '^define ' $S/wl_wifi_sta_sfc/wext-core.c.ll | wc -l                     # 7
grep '@MACRegTable = ' $S/wl_wifi_sta_sfc/rtmp_init.c.ll                      # 22 (address, value) pairs
```

The pairs were then matched by address against Ralink's `MACRegTable` (names resolved through `rt2800.h` v6.6, with
Ralink's `RX_FILTR_CFG` as `RX_FILTER_CFG`) and against the literal `rt2800_register_write` calls in `rt2800lib.c`.

The link closure: build fm1-base's `examples/wifi` (`JIELI_TOOLCHAIN=~/.jieli/toolchain
AC79_SDK=~/fw-AC79_AIoT_SDK python3 tools/build.py examples/wifi`), `nm -S` its `build/wifi.elf`, and look up
each archive member's defined symbols in it.

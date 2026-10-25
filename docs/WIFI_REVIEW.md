# Independent review of `WIFI.md`

Reviewed 2026-10-24 against the assigned worktree, the AC79 SDK at
`d179b4484759423312073f5fbb232501aa491047`, fm1-radio at
`c9113b5eceb9d6c295e32c86e5d769ed8efa67d6`, fm1-base at
`aa2b3bc8f7900cd1820947cb93ca0e8ecc4189e7`, and Linux v6.6's `rt2800.h`.
This is an evidence review, not a legal opinion. `CONFIRMED` means the stated
artifact or source fact reproduced; it does not promote a source comment into
an independently repeated hardware experiment.

## Verdict summary

| Claim | Section of `WIFI.md` | Verdict |
| --- | --- | --- |
| SDK, fm1-radio and fm1-base revisions named in the input table | Opening source table | CONFIRMED |
| fm1-base BSP submodule pin (`d6b8f77`) | Opening source table | CONFIRMED |
| Historical Linux source-body revisions named in the source table | Opening source table | UNCHECKED |
| OPLBOX declares GPL-3.0-only; SDK root `LICENSE` is Apache-2.0 with the unfilled copyright template; README and WiFi archive directories contain no separate license notice | Licence provenance — What the SDK says | CONFIRMED |
| The nine archive copyright and Ralink/RTMP string counts; explicit license-name scan finds two identifier hits in `wpasupplicant.a` | Licence provenance — What `strings` finds | CONFIRMED |
| Interpretations of vendor/protocol strings (Atheros/Broadcom flags, AirKiss, absent vendor names) | Licence provenance — What `strings` finds | UNCHECKED |
| Station archive source-file paths and counts by directory | Licence provenance — What the archives were compiled from | CONFIRMED |
| The station archive's `action.c.o` is derived from the Ralink Linux v2.6.32 file, all 16 upstream bodies match, and the upstream license header applies | Licence provenance — What the archives were compiled from | UNCHECKED |
| JieLi's `wext-core.c.o` contains ten function definitions | Licence provenance — The Linux kernel | WRONG |
| JieLi's WEXT implementation is copied from the named historical Linux file and its licensing consequence | Licence provenance — The Linux kernel | UNCHECKED |
| `wl_rf_common.a` contains no Ralink/Linux names | Licence provenance — What the archives were compiled from | WRONG |
| `wl_rf_common.a` is entirely JieLi-owned Apache-2.0 code | Licence provenance — What this means | UNCHECKED |
| `hsm.a` source paths point to JieLi's HSM source tree; its exact authorship/license is Apache-2.0 | Licence provenance — What the archives were compiled from | UNCHECKED |
| A GPL binary must be distributed physically “with” its complete source | Licence provenance — opening paragraph / What this means | WRONG |
| The stated GPL compatibility conclusions for `wl_wifi*.a`, `wl_rf_common.a`, and stages 1–8 | Licence provenance — What this means | UNCHECKED |
| SDK lwIP and mbedTLS source trees/license files, and the upstream wpa_supplicant BSD license | Source versus binary in the WiFi path | CONFIRMED |
| Every other component-row attribution (glue licenses, BearSSL use, external WiFi, hostapd, exact HSM/WiFi archive license) | Source versus binary in the WiFi path | UNCHECKED |
| fm1-base's WiFi link set and source-built lwIP directories | Source versus binary / What fm1-radio actually links | CONFIRMED |
| All nine archive sizes, member totals, exported-symbol totals, and intra-archive unresolved totals | Archive inventory | CONFIRMED |
| `_sfc` membership and the 17-member AP delta | Archive inventory | CONFIRMED |
| `_sfc` archive member payloads differ from non-`_sfc` members | Archive inventory | CONFIRMED |
| fm1-base WiFi archive members retained after LTO and their named-code byte totals | What fm1-radio actually links | CONFIRMED |
| `wl_rf_common.a` named-symbol total is 16,018 B | What fm1-radio actually links | WRONG |
| Per-symbol byte sizes, largest-symbol ranking, and complete discarded-member list | What fm1-radio actually links | UNCHECKED |
| fm1-base WiFi/template image, RAM and pool figures, and the approximate incremental cost | What fm1-radio actually links | CONFIRMED |
| CPU register bases at 0x11900, 0x14000, 0x50000 and 0x5101C; IRQ number 52 | The hardware the driver talks to — CPU-side registers | CONFIRMED |
| The internal SDIO host uses 0x30F00–0x30F1C | The hardware the driver talks to — CPU-side registers | CONFIRMED |
| `wl30_sdio_enum` command ordering and CCCR writes | The hardware the driver talks to — Internal SDIO enumeration | CONFIRMED |
| Checked MAC register addresses/fields line up with Linux v6.6 `rt2800.h`; `MACRegTable` contains the listed 22 pairs | The hardware the driver talks to — WiFi core registers | CONFIRMED |
| Every one of the 22 table addresses is named in `rt2800.h` | The hardware the driver talks to — WiFi core registers | UNCHECKED |
| `rt2800.h` supplies the MAC default register values | The hardware the driver talks to — WiFi core registers | WRONG |
| JieLi's MAC is in the RT2800 family and every `MACRegTable` value equals a Linux/Ralink default | The hardware the driver talks to — WiFi core registers | UNCHECKED |
| Encryption table addresses and BBP indirect windows | The hardware the driver talks to — WiFi core registers | CONFIRMED |
| No firmware image is embedded or loaded; `RTUSBFirmwareWrite` is unused; MCU command stub returns 1 | The hardware the driver talks to — No MCU firmware | CONFIRMED |
| Named RF initialization/calibration functions and selected constants exist in archive IR | Power-up and calibration sequence | CONFIRMED |
| The full RF initialization call order and every detailed register-use mapping | Power-up / register map | UNCHECKED |
| SDIO Simplified Specification applicability | Internal SDIO enumeration | UNCHECKED |
| FM-1 high-current/flash-corruption, one-second sound hold, saved-trim failure, and 4.6 KiB pool-layout effects | Power-up and calibration sequence | UNCHECKED |
| Exact 68-byte trim layout/CRC, trim duration/current, RF RAM code size, and default-only FM-1 operation | Power-up / tuning values | UNCHECKED |
| Calibration indices, data lengths, source defaults, and source locations | Where the tuning values come from | CONFIRMED |
| The exact “163 external OS-service symbols” and provider breakdown | OS services the binary stack needs | UNCHECKED |
| fm1-radio notes/probe are BLE-focused; fm1-base contains the cited WiFi shim and layout notes | What fm1-radio's notes and probe already cover | CONFIRMED |
| This is the first documentation anywhere of the registers, SDIO bus, and RT2800 lineage | What fm1-radio's notes and probe already cover | UNCHECKED |
| Current OPLBOX image, `.data`+`.bss`, pool and RAM-code values | OPLBOX's constraints | CONFIRMED |
| Flash layout, main-loop design and declared audio timing constraints | OPLBOX's constraints | CONFIRMED |
| Stage outcomes, RAM discovery, packet formats, calibration and WiFi operation on FM-1; 40–50 KB minimal lwIP estimate | Plan / Open questions / OPLBOX's constraints | UNCHECKED |
| IEEE 802.11 clause references and claims about which implementation stages are needed | Plan | UNCHECKED |

## Evidence

### Scope and licensing

`/home/xero/git/oplbox/LICENSING.md:3` declares GPL-3.0-only. In the SDK,
`sed -n '1,4p' LICENSE` prints `Apache License`, `Version 2.0`; the appendix
still contains `Copyright [yyyy] [name of copyright owner]`. `rg -ni
'licen[cs]e|GPL|Apache' README.md` returns no matches. A `find` for
`LICENSE*`, `NOTICE*`, `COPYING*` and `README*` below `cpu/wl82` finds no
WiFi-archive notice. These observations confirm what files are present; they
do not establish that the root license can override third-party material in
an archive.

The reproduced scan for explicit license labels/copyright markers was:

```sh
strings -n 4 "$L/$archive.a" | grep -Eci \
  'GNU General Public License|GPL-[0-9]|SPDX-License-Identifier|BSD-[0-9]|Apache License|Copyright[[:space:]]'
```

It returned zero for each archive. The separate broad substring scan
`strings -n 4 "$L/$archive.a" | grep -ci 'licen[cs]e'` returns 2 for
`wpasupplicant.a` (`wpa_supplicant_license`, an identifier), zero for the
other eight archives. The report's vendor counts also reproduced using its command
`strings -n 4 "$L/$archive.a" | grep -c -i ralink`:

| Archive | `Ralink` | `RTMP` |
| --- | ---: | ---: |
| `wl_wifi.a`, `wl_wifi_sfc.a` | 76 each | 642 each |
| `wl_wifi_sta.a`, `wl_wifi_sta_sfc.a` | 60 each | 581 each |
| `wl_wifi_ap.a` | 61 | 508 |
| `ext_wifi_dev.a` | 2 | 0 |
| `wl_rf_common.a` | 0 | 3 |
| `wpasupplicant.a` | 0 | 4 |
| `hsm.a` | 0 | 0 |

The zero Ralink-token count for `wl_rf_common.a` does not support the broader
claim that it contains no Linux/Ralink names. `llvm-dis` on
`wl_rf_common/wl_hw_init.c.o` reports debug files
`.../MODULE/include/chip/rtmp_mac.h`, `.../UTIL/include/rtmp_type.h`,
`.../UTIL/include/rtmp_comm.h`, and `.../inc/linux/types.h`. Its IR also
contains RTMP-prefixed structures and names. Those are concrete third-party
lineage indicators beyond a filename or symbol match. The corresponding
header bodies were not available for comparison in this run, so this does
**not** prove those headers were copied or establish their license. It does
falsify “no Ralink/Linux name” and leaves the archive's Apache-only status
unproven.

The other vendor and protocol names in the report were not independently
traced to their implementations. In particular, strings alone cannot show
that the Atheros/Broadcom tokens are only compatibility flags, that AirKiss
is Tencent's implementation, or that absent Espressif/Qualcomm/Intel tokens
mean those vendors' code is absent. Those interpretations remain UNCHECKED.
fm1-base's `LICENSING.md` labels the WiFi archives, including
`wl_rf_common.a`, Apache-2.0. That reproduces the presence of the label, not
its authority over code with the RTMP/Linux header lineage above.

This changes the review's license conclusion: `WIFI.md` cannot currently
claim that translating `wl_rf_common.a` is allowed under Apache-2.0. Stages
1–3 use its tables or IR, so their provenance must be cleared or replaced by
independently documented facts before treating the archive as a permitted
code source. The separate `wifi_conf.c` tables have visible SDK source and
root-license evidence, but that does not clear imported header material in
the archive. Stages 4–5's published RT2800 register definitions remain a
separate usable reference; this audit did not compare all packet descriptors
or defaults. The proposed “do not copy WEXT” boundary remains sensible. No
stage was experimentally validated on an FM-1.

The sentence that a GPL binary “has to come with its complete source” is too
absolute. GPLv3 permits several ways to provide corresponding source, including
a written offer and equivalent network access; it need not be physically
bundled beside each binary ([GPLv3 section 6](https://www.gnu.org/licenses/gpl.en.html#section6)).
That wording correction does not make these archives safe to link: this SDK
search found no corresponding archive source or source offer.

### Ralink and WEXT provenance

The fm1-base gitlink records BSP commit
`d6b8f771795be04d77e15d72a79f474a723796d7`, matching the source table. Fresh
Linux clones could not materialize the source files: v2.6.32 and v6.6
checkout attempts stopped with `Disk quota exceeded`; v2.6.37 was fetched
without checkout. The v2.6.32 repository resolved to commit
`22763c5cf3690a681551162c15d34d935308c8d7`; v6.6 resolved to
`ffc253263a1375a65fa6c9f62a893e9767fbebfa`. fm1-radio cloned at the expected
commit. fm1-base's fresh clone resolved the expected `aa2b3bc` commit but
also could not materialize missing blobs. Therefore the historical Ralink
body/header comparison and the exact old WEXT file/license comparison are
unchecked here.

`llvm-dis` on `wl_wifi_sta_sfc/action.c.o` shows source filename
`.../MODULE/common/action.c`, 25 definitions, and non-name content including
the byte table `BuildIntolerantChannelRep.RegClassArray` and literals such
as `ACT - BuildIntolerantChannelRep` and
`BA - Send ADDBA request`. This is evidence of the archived module's contents,
not a comparison against all 16 functions in Linux v2.6.32. Function names,
the `action.c` path, and similar strings/constants alone do not establish
copying. No exact body diff was possible, so neither the all-16 claim nor the
v2.6.37 counts (`assoc` 19/19, `cmm_data` 29/30, `cmm_asic` 25/27,
`ba_action` 30/33, `cmm_sanity` 14/16, `connect` 26/30, `mlme` 67/79) nor
the GPL-2.0-or-later attribution was reproduced. These claims stay UNCHECKED.

The WEXT count is directly contradicted by `llvm-dis
.../wl_wifi_sta_sfc/wext-core.c.o -o - | grep -c '^define '`: output `7`.
The seven definitions include `wireless_send_event`, `get_wireless_stats`,
`call_commit_handler`, `wext_handle_ioctl`, `ioctl_standard_call`,
`iw_handler_get_iwstats`, and `ioctl_standard_iw_point`. The object has
WEXT-specific log strings and Linux-shaped event/ioctl routines, so lineage is
plausible, but the requested exact historical-source body comparison was
blocked by the same checkout quota. The current upstream WEXT source carries
`SPDX-License-Identifier: GPL-2.0`; that does not substitute for checking the
historical file's own header. See the [upstream WEXT source](https://github.com/torvalds/linux/blob/master/net/wireless/wext-core.c).

`rt2800.h` says `SPDX-License-Identifier: GPL-2.0-or-later` and supplies
register names, addresses, and bit masks. It does not supply the MAC defaults
in `MACRegTable`; those would have to be compared with driver initialization
code such as Ralink `rtmp_init.c` or Linux `rt2800lib.c`. Thus “same defaults
as `rt2800.h`” is wrong as a source attribution, even though the 22 JieLi
address/value pairs themselves reproduce below. This does not disprove that
some values equal Ralink defaults.

The legal compatibility conclusions for `wl_wifi*.a`, WEXT, and
`wl_rf_common.a` remain unchecked pending exact source and license comparison.
In particular, the archive evidence is insufficient to declare the
`wl_rf_common.a` translation permitted or to present the staged plan's
Apache-only premise as settled.

### Archive source paths, strings, and inventory

The assigned reproduction unpacked the five documented archives with
`ar x` into `/tmp/wifi-review-codex/sdk`. `file` reports each ordinary
`.o` member in `wl_rf_common.a` as `LLVM IR bitcode` and its `version.z.o` as
ELF. `llvm-dis .../wf_rf_init.c.o -o -` reports source filename
`.../jl_wifi/wl_port/wl82/wf_rf_init.c`, target triple `pi32v2`, and debug
paths. Source filename counts from all 87 station bitcode members:

| Debug source directory | Members |
| --- | ---: |
| `MODULE/common` | 46 |
| `MODULE/sta` | 9 |
| `MODULE/chips` | 2 |
| `MODULE/os` | 5 |
| `NETIF/os` | 4 |
| `UTIL/common` | 1 |
| `UTIL/os` | 2 |
| `WIFI_todo` | 17 |
| `mp_test` | 1 |

The totals and source groups in `WIFI.md` reproduce. They identify compile-time
paths, not source availability or authorship.

`ar t` plus `llvm-nm --format=posix` over each archive reproduced the complete
inventory (exported means unique defined global names; unresolved means
unique undefined global names not defined elsewhere in that archive):

| Archive | Bytes | Members (bitcode + ELF) | Exported | Undefined | Unresolved |
| --- | ---: | ---: | ---: | ---: | ---: |
| `wl_wifi.a` | 8,574,238 | 104 + 1 | 1,543 | 837 | 128 |
| `wl_wifi_sfc.a` | 8,574,242 | 104 + 1 | 1,543 | 837 | 128 |
| `wl_wifi_sta.a` | 6,182,374 | 87 + 1 | 1,328 | 701 | 122 |
| `wl_wifi_sta_sfc.a` | 6,182,370 | 87 + 1 | 1,328 | 701 | 122 |
| `wl_wifi_ap.a` | 6,296,084 | 90 + 1 | 1,241 | 702 | 115 |
| `ext_wifi_dev.a` | 74,202 | 2 + 0 | 23 | 75 | 59 |
| `wl_rf_common.a` | 321,802 | 13 + 1 | 209 | 164 | 90 |
| `wpasupplicant.a` | 2,623,914 | 42 + 1 | 606 | 401 | 56 |
| `hsm.a` | 26,168 | 2 + 1 | 26 | 6 | 2 |

`ar t wl_wifi.a` minus `ar t wl_wifi_sta.a` is exactly 17 members:
`ap*.c.o` plus `p2p_cfg.c.o`. The available full and station `_sfc` archives
have identical member-name sets to their non-`_sfc` counterparts. Hashing each
same-named raw archive member (`ar p` plus SHA-256) found different payloads
for 54/105 full-driver members and 43/88 station-driver members, confirming
that `_sfc` is not byte-identical even though membership is unchanged.

### Dependencies and fm1-base image

The source facts reproduce as follows:

- SDK lwIP source is `lib/net/lwip_2_1_3`; its shipped `COPYING` is BSD-style.
  `mbedtls_2_2_1/LICENSE` and `mbedtls_2_26_0/LICENSE` state Apache 2.0.
- SDK syscfg IDs are `VM_XOSC_INDEX=106`, `VM_WIFI_PA_DATA=107`,
  `VM_WIFI_PA_MCS_DGAIN=108`, and `VM_WIFI_RF_INIT_INFO=187`; getters use
  2-byte, 7-byte, 20-byte and caller-sized trim records respectively.
  `apps/demo/demo_wifi/board/wl82/board.c` has XOSC `{0x0b,0x0b}` and PA
  trim `{1,7,4,7,11,1,7}`. Board MCS-gain values vary; the report is right to
  say they come from the configuration source rather than claim one universal
  per-device value.
- Archive bitcode source paths identify wpa_supplicant 2.9; the upstream
  project states the BSD license (advertising clause removed) on its
  [project page](https://w1.fi/wpa_supplicant/). The exact archive is binary
  only in this SDK inventory.
- fm1-base `tools/build.py` at `aa2b3bc` lists
  `wl_wifi_sta_sfc.a`, `wpasupplicant.a`, `hsm.a`, `crypto_toolbox_Osize.a`,
  `lib_ccm_aes.a`, and `wl_rf_common.a`; it builds lwIP from source and
  compiles the shim. `examples/wifi/fm1.toml` sets `wifi=true`.
- `hsm.a` debug metadata points into a JieLi HSM path, but this does not prove
  authorship or license for each member; those attributions remain UNCHECKED.

The fresh fm1-base checkout could not be built because the per-user `/tmp`
quota prevented fetching missing clone blobs. The exact commit's existing
ELFs under `/tmp/wifimap.ix6m/fm1-base` were therefore inspected and counted
directly; the build itself was not rerun. `llvm-readelf -S wifi.elf` reports
`.text=408,760`, `.ram_text=7,344`, `.data=520`, `.bss=121,536`, and
`.pool=305,988` bytes. The data-to-BSS alignment gap is 24 bytes, so
`.data + gap + .bss = 122,080`. `wifi.bin` is 416,624 bytes. The template
artifacts are `text.bin=78,896`, `ramtext.bin=3,132`, `data.bin=124`,
`template.bin=82,152`, `.data + .bss=27,844`, and `.pool=69,384` bytes.
Subtracting gives 334,472 bytes image, 94,236 bytes `.data`/`.bss`, and
236,604 bytes pool. The report's “about 330 KB code and RAM” is a fair
estimate; the apps differ, so it is not a controlled feature-only delta.

Against the same existing `wifi.elf`, matching all local and global symbols
reproduced the linked-member counts and named symbol bytes:

| Archive | Retained code members | Named symbol bytes |
| --- | ---: | ---: |
| `wl_rf_common.a` | 13/13 | 16,017 |
| `wl_wifi_sta_sfc.a` | 58/87 | 207,679 |
| `wpasupplicant.a` | 35/42 | 79,127 |
| `hsm.a` | 1/2 | 220 |

The `wl_rf_common.a` total is 16,017 B, one byte below `WIFI.md`'s 16,018 B.
The other archive totals in that table reproduce.

The 163 external OS-service-symbol number did not reproduce from archive
undefineds: my pre-LTO union leaves 203 unresolved references after removing
cross-archive definitions, because it still includes references from
functions later discarded by LTO. I did not rerun the fresh link or recover
the original post-LTO symbol-selection script. The claimed 163 and its
provider counts are therefore UNCHECKED, not evidence of an error.

### Register map and initialization

Local `WL82.h` defines `lsfr_base=0x10000`, `psfr_base=0x50000`, and
`map_adr(grp,adr)=(64*grp+adr)*4`. This gives `JL_ANA` at
`0x10000 + map_adr(0x19,0) = 0x11900`, and `JL_WL` at
`0x10000 + map_adr(0x40,0) = 0x14000`. `JL_PORTA` is `0x50000`; `JL_IOMAP`
is `0x5101C`. The SDK's `hwi.h` defines `IRQ_WF_IDX 52`. The bitcode's
`wl_sdio.c.o` contains direct MMIO pointers `0x30F00`, `0x30F04`,
`0x30F10`, `0x30F14`, `0x30F18`, and `0x30F1C`; `wl_usb_to_sdio.c.o`
also references `0x30F04`. The range is real in the archived code, but the
public header does not name it as a block, so its SDIO-host role is inferred
from those functions/debug names.

For register names/fields I checked Linux v6.6's primary
[`rt2800.h`](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/net/wireless/ralink/rt2x00/rt2800.h).
It defines `MAC_SYS_CTRL=0x1004`, `BBP_CSR_CFG=0x101C`, `PBF_CFG=0x0408`,
`PBF_MAX_PCNT=0x040C`, `TXRXQ_PCNT=0x0438`, `PBF_DBG=0x043C`,
`RX_FILTER_CFG=0x1400`, `TX_RTY_CFG=0x134C`, and the encryption bases
`0x4000/0x6000/0x6800/0x6C00/0x7000`. `BBP_CSR_CFG` field masks include
READ_CONTROL `0x00010000`, BUSY `0x00020000`, and RW_MODE `0x00080000`,
matching the values described in the archived accessors. This verifies the
address/name and bitfield correspondence, not that the device is literally a
Ralink chipset.

`llvm-dis rtmp_init.c.o` reproduces this `MACRegTable` (address, value):

```text
042C F8F0E8E0   0430 6F77D0C8   1408 0000013F   140C 00008003
1400 00017F97   1350 00001020   1018 00001F00   040C 1F3FBF9F
134C 47D01F0F   1404 00000013   1364 05740003   1368 05740003
0408 00F40006   1370 03F44084   1374 01744004   1378 03F44084
136C 01744004   1340 0000583F   1344 00092B20   1380 002400CA
1608 00000002   1204 00000003
```

The table is present and all 22 pairs reproduce from JieLi IR. The original
report's equivalence of those *values* to Linux/Ralink defaults is not
confirmed because the relevant historical driver source did not materialize.

`wl_sdio.c.o`'s `wl30_sdio_enum` IR contains CMD5 with `0x1F000000` twice,
then CMD3 and CMD7 with `0x00010000`, followed by CMD52 writes to CCCR
0x02/0x07/0x04 and function-1 0x10–0x13. This confirms the sequence as
encoded in the archived implementation, not successful enumeration on a
physical FM-1.

The four BBP indirect windows (C8/C9/CA, CC/CD/CE, D0/D1/D2, D4/D5/D6) and
the encryption table bases are present in SDK code / the v6.6 header. The
claim that these windows are custom to JieLi is consistent with the local
accessors, but the complete historical register comparison was not run.

No firmware image is an archive member (`ar t` lists only `.o` files and the
small `version.z.o`). `llvm-nm -A wl_wifi_sta_sfc.a` shows
`RTUSBFirmwareWrite` defined only in `rtusb_io.c.o`, with no undefined
reference to it anywhere in the archive. `rtmp_init_inf.c.o`'s
`rt28xx_init` has no call to it. `cmm_asic.c.o` defines
`AsicSendCommandToMcu` as a body that immediately `ret i8 1`; `rt28xx_init`
does call that stub, so “no MCU command calls” would be false, but “this stub
does not load firmware” is supported. No `NICLoadFirmware` symbol appears.

### Calibration, notes, and product constraints

SDK source confirms the tuning data locations: `wifi_conf.c` defines the
STF/DS/DSSS thresholds and 256-entry `wl_rfd_ram_lut`; `syscfg_id.h` gives
IDs 106/107/108/187; `user_cfg.c` provides weak calibration getters and
lengths. `wifi_conf.c` stores/loads the RF trim by the caller-provided size.
The 68-byte record layout and CRC behavior were not independently decoded
from its original RF source in this review; the archived sequence calls the
trim helpers, but the physical calibration result and timing remain
unchecked. I also did not reproduce the report's 3.5 KiB RAM-code total or
the claim that fm1-base operates from absent-value defaults on stock hardware.

The fresh fm1-radio clone is exactly `c9113b5`. Its `notes/01`–`06` concern
BLE MIDI/controller bring-up; `notes/06-controller-handoff.md` contains the
RAM-trim/high-current warning, and `notes/aac-crash/` covers AAC. This
confirms the contents of those notes, not their experiments. The exact
fm1-base source checkout at `aa2b3bc` has the cited `device/app_wifi.ld`,
`ble/shim/misc.c`, and `ble/wifi/wifi_shim.c` comments; those record the
reported 500 ms flash issue, 4.6 KiB pool sensitivity, fresh-trim choice,
PA/XOSC defaults, and `udelay` behavior. I did not reproduce those hardware
effects.

The repository's `TODO.md` also contains a prior “stage 0 (the map)” summary,
so the “first documentation” claim cannot be confirmed from the repository
history and documents inspected here; no exhaustive search of all project
history or external documentation was performed.

I ran `./build.sh` in this assigned OPLBOX worktree. It exited 0 and reported:

```text
image 342792 B of 581564
RAM .data+.bss 36776 B of 98304
pool 255888 B of 344064
RAM code .ram_text+.ram_code 6840 B of 24576
```

Those numbers confirm the OPLBOX side of the resource table. `README.md` and
`firmware/src/oplbox.c` corroborate the main-loop app and audio budget;
`docs/BASE.md` supplies the flash map. The report's free-space arithmetic
checks: 238,772 B image, 61,528 B `.data`/`.bss`, 88,176 B pool, and 17,736 B
RAM code. The 9 × 32 KiB song slots and free WiFi settings window are the
documented layout, not hardware-probed flash writes. Stages 1–9 are proposals;
no stage's host model, radio traffic, open-network join, WPA2 join, or audio
coexistence was tested by this review.

## Reproduction limits

The fresh `fm1-base` and Linux source checkouts hit the user's `/tmp` quota
while materializing blobs. I used the exact-commit fm1-base ELF/source already
present in `/tmp/wifimap.ix6m` only for read-only `nm`/section inspection.
`fm1-base examples/wifi` was not rebuilt. Linux v6.6 `rt2800.h` was checked
from the exact upstream file; historical Linux v2.6.32/v2.6.37 Ralink and
WEXT bodies were not checked. These limits are why the code-copy and
license-provenance verdicts remain UNCHECKED rather than inferred from names.

## Follow-up (2026-10-25)

`WIFI.md` was corrected for the five WRONG rows: the `wext-core.c.o` count (7), the `wl_rf_common.a` provenance
(new section "What `wl_rf_common.a` carries": 8 of 13 members clean, 5 with Ralink and Linux header lineage), the
GPL source wording (GPLv3 section 6), the `MACRegTable` attribution, and the `wl_rf_common.a` total (16,017 B,
recounted from the same `wifi.elf`). Three UNCHECKED rows moved:

- Every one of the 22 table addresses is named in `rt2800.h`: confirmed (v6.6).
- The 22 `MACRegTable` values: all 22 equal Ralink's `MACRegTable` in Linux v2.6.32 staging `rtmp_init.c`
  (RT2870 build). Whether the MAC is literally an RT2800-family chip stays unchecked.
- The WEXT lineage: all 7 function names are in Linux `wext-core.c` v2.6.33 and v2.6.37; the bodies were not
  compared, so copying stays unchecked.

The licence-compatibility verdicts stay evidence, not a legal opinion. `WIFI.md` now uses `wl_rf_common.a` as a
source of facts only; "What stages 1 and 3 may take from `wl_rf_common.a`" lists the limits.

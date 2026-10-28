#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/oplbox.c on this computer (tests/host.c): build/host/oplbox.wav and the screens as PNG.
# Then check them, so a drawing or sound regression fails here (exit 1) instead of passing silently.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
rm -f build/host/oplbox-*.ppm build/host/oplbox-*.png build/host/oplbox.wav
# lwIP 2.1.3 (firmware/wifi/lwip/, stage 7) for the simulator's WiFi page (stage 9) and the IP tests below: its own files
# without -Werror (upstream code, a newer compiler may warn), C11 (glibc's SSIZE_MAX in GNU mode would make lwIP's arch.h
# want unistd.h), with its assertions on (FM1WIFI_NET_DEBUG); fm1wifi_net.c the same way, with -Werror
mkdir -p build/host/lwip
LWIP_INC="-Ifirmware/wifi/lwip/port -Ifirmware/wifi/lwip/src/include"
for f in firmware/wifi/lwip/src/core/*.c firmware/wifi/lwip/src/core/ipv4/*.c firmware/wifi/lwip/src/netif/*.c; do
  cc -std=c11 -O2 -Wall -Wextra -DFM1WIFI_NET_DEBUG $LWIP_INC -c "$f" -o "build/host/lwip/$(basename "$f" .c).o"
done
cc -std=c11 -O2 -Wall -Wextra -Werror -DFM1WIFI_NET_DEBUG -Ifirmware/wifi $LWIP_INC -c -o build/host/fm1wifi_net.o firmware/wifi/fm1wifi_net.c
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -DDBOPL2_SCOPE_N=512 -Ifirmware/src -Ifirmware/gen \
   -Ifirmware/wifi -Itests/wifi -o build/host/oplbox firmware/src/oplbox.c firmware/src/dbopl2.c firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c \
   firmware/wifi/fm1wifi_wpa.c firmware/wifi/wpa_eapol.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c firmware/wifi/fm1wifi_ip.c \
   tests/wifi/sdio_model.c tests/wifi/wpa_ap.c build/host/fm1wifi_net.o build/host/lwip/*.o tests/ble/fm1ble_test_host.c
build/host/oplbox build/host/oplbox
# the editor's undo and live recording, checked against the song in memory: each script exits 1 when a check fails
build/host/oplbox build/host/undo tests/script_undo.txt
build/host/oplbox build/host/rec tests/script_rec.txt
# the arpeggiator: steps, tempo, gate, latch and the channel it takes, checked against its state and the chip
build/host/oplbox build/host/arp tests/script_arp.txt
# the arp written into the tracker (TODO 4): the cells, their playback against the live arp's key-ons, undo
build/host/oplbox build/host/arp_write tests/script_arp_write.txt
# SYNC: the arp on the song's row clock at every speed (steps on the rows' first samples, 64 rows and more, no drift;
# 1/8T and 1/32 lengths; free-running with the song stopped; swing), written into the tracker as it played; then the
# settings sector it saved loads as SYNC in a second run, holds 0 BPM (an older OPLBOX reads that as out of range:
# 120), and a 10- and a 12-byte sector an older OPLBOX wrote still load
rm -f build/host/arp_sync.set
HOST_SETTINGS=build/host/arp_sync.set build/host/oplbox build/host/arp_sync tests/script_arp_sync.txt
build/host/oplbox build/host/arp_sync_write tests/script_arp_sync_write.txt
HOST_SETTINGS=build/host/arp_sync.set build/host/oplbox build/host/arp_sync_load tests/script_arp_sync_load.txt
python3 - <<'PY'
import sys
from pathlib import Path
st = Path("build/host/arp_sync.set").read_bytes()
want = bytes([1, 12, 0, 0])
if st[:4] != want or st[12:14] != b"\0\0": sys.exit(f"FAIL: SYNC sector {st[:16].hex()}: want header {want.hex()}, arp_bpm 0")
for n in (10, 12):                      # song, loop, mode, octaves, rate 1/16, gate 50, swing 50, latch, 140 BPM (, kb_oct, pad)
    body = bytes([0, 0, 0, 1, 3, 50, 50, 0, 140, 0, 0, 0])[:n]
    Path(f"build/host/arp_old{n}.set").write_bytes(bytes([1, n, 0, 0]) + body + bytes(256 - n))
print("SYNC sector: arp_bpm 0; older 10- and 12-byte sectors written")
PY
HOST_SETTINGS=build/host/arp_old10.set build/host/oplbox build/host/arp_old10 tests/script_arp_sync_old.txt
HOST_SETTINGS=build/host/arp_old12.set build/host/oplbox build/host/arp_old12 tests/script_arp_sync_old.txt
# the keys' instrument outside EDIT (TODO 5): PRESETS, ALGO's copy against the chip's C0, the song's own left alone
build/host/oplbox build/host/live_instr tests/script_live_instr.txt
# OCT- / OCT+: the keys' octave outside EDIT, not the song; a playing arp follows it, a released key takes its note
build/host/oplbox build/host/octave tests/script_octave.txt
# the song slots over SysEx (firmware/src/slot_sysex.c, TODO 2a): pull, push and every refusal through ota.c's framing
# against a fake computer, macsop_check against macsop_parse; then tools/fm1_slot.py against the same code on a pipe
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function -Ifirmware/src -Ifirmware/bsp/chip -o build/host/slot_sysex_test tests/slot_sysex_test.c
build/host/slot_sysex_test
python3 tests/slot_sysex_tool.py build/host/slot_sysex_test build/host
# the fm1wifi library (firmware/wifi/, docs/WIFI.md "Standalone library") stands alone: compiled with only its own
# directory and a port (no firmware/src, hal or bsp on the include path), warning-free, and none of its files
# includes anything from outside it: its own headers, BearSSL's, and lwIP's (firmware/wifi/lwip/: lwip/*.h with its
# priv/ and prot/, netif/*.h, arch/cc.h and lwipopts.h, the port's two). The vendored lwIP files are held to the
# upstream release by their checksums instead (firmware/wifi/lwip/SHA256SUMS: lwIP 2.1.3, unchanged)
for f in firmware/wifi/*.c firmware/wifi/*.h firmware/wifi/lwip/port/*.h firmware/wifi/lwip/port/arch/*.h; do
  if grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$f" | grep -v '"\(fm1wifi[a-z_]*\|wpa_crypto\|wpa_eapol\|bearssl/[a-z_]*\|lwip/\(priv/\|prot/\)\{0,1\}[a-z0-9_]*\|netif/[a-z0-9_]*\|arch/cc\|lwipopts\)\.h"'; then
    echo "FAIL: $f includes a file from outside firmware/wifi/" >&2; exit 1; fi
done
python3 - <<'PY'
import hashlib, sys
from pathlib import Path
d = Path("firmware/wifi/lwip")
want = dict(reversed(l.split(None, 1)) for l in (d / "SHA256SUMS").read_text().splitlines())
have = {str(p.relative_to(d)) for p in (d / "src").rglob("*") if p.is_file()} | {"COPYING"}
bad = [f for f in sorted(want) if not (d / f).is_file() or hashlib.sha256((d / f).read_bytes()).hexdigest() != want[f]]
bad += [f"{f} (not in SHA256SUMS)" for f in sorted(have - set(want))]
if bad: sys.exit("FAIL: firmware/wifi/lwip differs from lwIP 2.1.3: " + ", ".join(bad))
print(f"lwIP: {len(want)} files as in the 2.1.3 release")
PY
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -c -o build/host/fm1wifi.o firmware/wifi/fm1wifi.c
# the register header: the accessors hit their addresses, and every address matches docs/WIFI.md
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_regs tests/wifi_regs.c firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c
build/host/wifi_regs > build/host/wifi_regs.txt
python3 tests/wifi_regs.py build/host/wifi_regs.txt
# fm1wifi against the register model (tests/wifi/sdio_model.c, the library's port on the host): the power-up and
# the first reads must match tests/wifi/bringup_expect.txt and diag_expect.txt (written by hand from
# docs/WIFI.md), the BBP waits are bounded, power-down puts every register back; the fixture format and diff.
# Stage 3 (docs/WIFI_STAGE3.md): the radio's bring-up must write what JieLi's wl_rf_common.a writes against the same
# model, record for record (tests/wifi/radio_trace.txt.gz, from tools/wifi_ir.py --trace), its four kinds of wait
# must end in TIMEOUT when the model never answers, and power-down must put every register back
gzip -dc tests/wifi/radio_trace.txt.gz > build/host/radio_trace.txt
WIFI_AP="tests/wifi/wpa_ap.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c"   # the model's WPA2 AP (tests/wifi/sdio_model.h "ap")
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -Itests/wifi -o build/host/wifi_model tests/wifi_model.c tests/wifi/sdio_model.c firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c $WIFI_AP
build/host/wifi_model tests/wifi build/host
# ... and that test catches a swapped write, a dropped delay and a wait without its bound (8 deliberate bugs)
python3 tests/wifi_mutants.py build/host
# stages 4-6 (docs/WIFI_STAGE4.md): the frame code alone (the RX parser on buffers laid out as the FIFO gives them,
# the scan list, the TX image, the management frames byte for byte, the station's state machine and timeouts), then
# the driver against the model's FIFOs and scripted access points (the DMA register by register, passive and active
# scans, a join and what ends it, every wait bounded, the crash record's step)
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_frame tests/wifi_frame.c firmware/wifi/fm1wifi_frame.c
build/host/wifi_frame
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -Itests/wifi -o build/host/wifi_link tests/wifi_link.c tests/wifi/sdio_model.c firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c $WIFI_AP
build/host/wifi_link tests/wifi
# the generated tables and the trace are what the SDK's archive gives (only where the SDK is: tools/wifi_ir.py)
if [ -f "${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}/cpu/wl82/liba/wl_rf_common.a" ]; then
  python3 tools/wifi_ir.py --check
else
  echo "wifi_ir: no JieLi SDK here, the generated tables and trace not re-checked"
fi
# the WiFi page in the simulator (tests/script_wifi.txt): its text against the made-up core (stage 2's reads, the
# radio's bring-up, page 2's trim and false CCA counts, page 4's channel survey), then against a core whose baseband never answers and one that
# answers nothing: an error on the page, the radio not reached, no hang
HOST_WIFI_REPORT=build/host/wifi_report.bin build/host/oplbox build/host/wifi tests/script_wifi.txt > build/host/wifi_page.txt
HOST_WIFI_MODEL=tests/wifi/core_bbp_stuck.txt build/host/oplbox build/host/wifi_stuck tests/script_wifi.txt > build/host/wifi_page_stuck.txt
HOST_WIFI_MODEL=tests/wifi/core_silent.txt build/host/oplbox build/host/wifi_silent tests/script_wifi.txt > build/host/wifi_page_silent.txt
HOST_WIFI_MODEL=tests/wifi/core_trim_hang.txt build/host/oplbox build/host/wifi_trim tests/script_wifi.txt > build/host/wifi_page_trim.txt
for want in "wifi page 0: WIFI TEST (STAGE 9)" "wifi page 1: PLAY: POWER UP, TRIM, READ" "wifi page 2: SAVE: LEAVE" \
            "wifi page 0: WIFI TEST (STAGE 9)   1/14" "wifi page 1: RESULT: OK" "wifi page 2: MAC_CSR0     35920200" \
            "wifi page 3: MAC_SYS_CTRL 0000000C" "wifi page 4: BBP 0        21" "wifi page 5: PLL BANK  64  MID  95" \
            "wifi page 6: FILTER  141  80  44" "wifi page 7: RAM CHECK: 0 CHANGED" "wifi page 9: PLAY: PAGE 2   SAVE: LEAVE" \
            "wifi page 0: TRIM  CH  6  PWR 6   2/14" "wifi page 1: TX LO  57  41  DC   -1   0" \
            "wifi page 2: TX GAIN   +5  PHASE   +2 Q" "wifi page 3: RX GAIN  -20  PHASE   +7 I" \
            "wifi page 4: RX DC I +123 +103  +76  +22" "wifi page 5: RX DC Q  -77  -66  -51  -21" \
            "wifi page 6: CCA  1- 5   2K  2K  1K  2K  4K" "wifi page 8: CCA 11-13   2K  1K  1K" \
            "wifi page 9: PLAY: PAGE 3   SAVE: LEAVE" "wifi page 0: RX CHECK  CH  6       3/14" \
            "wifi page 1: VGA GAIN    7   7   7" "wifi page 2: VGA PEAK   50  50  50" "wifi page 3: PEAK AT VGA  0    0" \
            "wifi page 4: PEAK AT VGA 41    0" "wifi page 5: RXFE 80 00  23 82" "wifi page 6: RXFE 0  3A  19 00  20 00" \
            "wifi page 7: BBP 25  00" "wifi page 9: PLAY: PAGE 4   SAVE: LEAVE" "wifi page 0: SURVEY  200 MS EACH   4/14" \
            "wifi page 1:     CCA   BUSY     CCA   BUSY" "wifi page 2: 1  2800  28000  8  2400  24000" \
            "wifi page 7: 6  6400  64000 13  1000  10000" "wifi page 8: 7  4400  44000" \
            "wifi page 9: PLAY: PAGE 5   SAVE: LEAVE" \
            "wifi page 0: STEPS                 5/14" "wifi page 1: RADIO   OK" "wifi page 2: SURVEY  OK 13 OF 13 CH" \
            "wifi page 3: LINK    OK" "wifi page 4: PASSIVE OK 4 NETWORKS" "wifi page 5: ACTIVE  OK 4 ANSWERS" \
            "wifi page 6: JOIN    OK AID 1" "wifi page 7: 4-WAY   NOT NEEDED (OPEN)" "wifi page 8: GROUP KEY NOT NEEDED (OPEN)" \
            "wifi page 9: STAY 2S OK 20 BEACONS" "wifi page 10: REJOIN  OK" \
            "wifi page 0: SCAN LIST   4 NETS   6/14" "wifi page 1: HOME        1  -61 WPA2 P" \
            "wifi page 2: OPLBOX      6  -40 OPEN P" "wifi page 3: OLDROUTER  11  -75 WPA P" "wifi page 4: (HIDDEN)   11  -55 OPEN" \
            "wifi page 0: RAW FRAMES 1-2       7/14" "wifi page 1: 1 CH  1  BYTES 104" "wifi page 4: FFFFFFFF FFFF0200 00000001" \
            "wifi page 0: RAW FRAMES 3-4       8/14" "wifi page 0: RX/TX COUNTS          9/14" "wifi page 3: FRAMES 33 BAD 0 TO ME 8" \
            "wifi page 6: SENT 32 ERR 0" "wifi page 7: TX OK 32 FAIL 0 RETRY 0" "wifi page 8: MAC 16:C6:87:70:CF:3D UID" \
            "wifi page 0: JOIN OPLBOX          10/14" "wifi page 1: BSSID 02:00:00:00:00:06" "wifi page 2: CH  6  RSSI  -40  OPEN" \
            "wifi page 3: AUTH  OK" "wifi page 4: ASSOC OK  AID 1" "wifi page 5: 2 S: UP, 20 BEACONS" "wifi page 6: LEFT, REJOIN UP" \
            "wifi page 9: PLAY: PAGE 1   SAVE: LEAVE"; do
  grep -qF "$want" build/host/wifi_page.txt || { echo "FAIL: WiFi page: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: TIMEOUT" "wifi page 2: MAC_CSR0     35920200" "wifi page 4: BBP 0        TIMEOUT" \
            "wifi page 5: PLL BANK  -" "wifi page 1: NOT REACHED" "wifi page 0: RX CHECK              3/14" \
            "wifi page 0: SURVEY                4/14" "wifi page 3: LINK    NOT REACHED" "wifi page 6: JOIN    NOT REACHED"; do
  grep -qF "$want" build/host/wifi_page_stuck.txt || { echo "FAIL: WiFi page, BBP stuck: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: NO ANSWER" "wifi page 2: MAC_CSR0     00000000" "wifi page 6: FILTER  -"; do
  grep -qF "$want" build/host/wifi_page_silent.txt || { echo "FAIL: WiFi page, silent core: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: TIMEOUT AT TRIM" "wifi page 0: TRIM FAILED  CH --  PWR --" \
            "wifi page 1: TX LO  --  --  DC   --  --" "wifi page 6: FILTER   --  --  --" "wifi page 6: CCA  1- 5   --" \
            "wifi page 1: VGA GAIN   --  --  --" "wifi page 3: PEAK AT VGA  0   --" "wifi page 7: BBP 25  --" "wifi page 2: 1    --     --  8    --     --"; do
  grep -qF "$want" build/host/wifi_page_trim.txt || { echo "FAIL: WiFi page, trim timeout: no line '$want'" >&2; exit 1; }
done
HOST_WIFI_MODEL=tests/wifi/core_trim_rxdc_hang.txt build/host/oplbox build/host/wifi_trim_rxdc tests/script_wifi.txt > build/host/wifi_page_trim_rxdc.txt
for want in "wifi page 1: RESULT: TIMEOUT AT TRIM" "wifi page 4: RX DC I  +96   --   --   --" "wifi page 5: RX DC Q  -96   --   --   --"; do
  grep -qF "$want" build/host/wifi_page_trim_rxdc.txt || { echo "FAIL: WiFi page, timeout inside an RX DC range: no line '$want'" >&2; exit 1; }
done
HOST_WIFI_MODEL=tests/wifi/core_trim_rxdc_first_hang.txt build/host/oplbox build/host/wifi_trim_rxdc tests/script_wifi.txt > build/host/wifi_page_trim_rxdc_first.txt
for want in "wifi page 1: RESULT: TIMEOUT AT TRIM" "wifi page 4: RX DC I   --   --   --   --" "wifi page 5: RX DC Q   --   --   --   --"; do
  grep -qF "$want" build/host/wifi_page_trim_rxdc_first.txt || { echo "FAIL: WiFi page, timeout at the first RX DC measurement: no line '$want'" >&2; exit 1; }
done
HOST_WIFI_MODEL=tests/wifi/core_trim_rxdc_q_hang.txt build/host/oplbox build/host/wifi_trim_rxdc tests/script_wifi.txt > build/host/wifi_page_trim_rxdc_q.txt
for want in "wifi page 1: RESULT: TIMEOUT AT TRIM" "wifi page 4: RX DC I +256   --   --   --" "wifi page 5: RX DC Q   --   --   --   --"; do
  grep -qF "$want" build/host/wifi_page_trim_rxdc_q.txt || { echo "FAIL: WiFi page, timeout at the first RX DC Q measurement: no line '$want'" >&2; exit 1; }
done
if grep -q "wifi page 1: NOT REACHED$" build/host/wifi_page_trim.txt; then echo "FAIL: WiFi page: a trim timeout reads NOT REACHED" >&2; exit 1; fi
# stages 4-6 when a step fails: the RX FIFO's DMA never finishes (the passive scan says so, the rest is not run, the
# first frame is not kept), and OPLBOX refuses the association (the join says so, with the status code)
HOST_WIFI_MODEL=tests/wifi/core_model.txt:tests/wifi/aps.txt:tests/wifi/hang_rxdma.txt build/host/oplbox build/host/wifi_rxhang tests/script_wifi.txt > build/host/wifi_page_rxhang.txt
for want in "wifi page 4: PASSIVE FAIL RX TIMEOUT" "wifi page 5: ACTIVE  NOT REACHED" "wifi page 6: JOIN    NOT REACHED" \
            "wifi page 1: NO FRAME RECEIVED" "wifi page 1: NOT REACHED: SEE PAGE 5"; do
  grep -qF "$want" build/host/wifi_page_rxhang.txt || { echo "FAIL: WiFi page, RX DMA hang: no line '$want'" >&2; exit 1; }
done
HOST_WIFI_MODEL=tests/wifi/core_model.txt:tests/wifi/aps_refuse.txt build/host/oplbox build/host/wifi_refuse tests/script_wifi.txt > build/host/wifi_page_refuse.txt
for want in "wifi page 4: PASSIVE OK 1 NETWORK" "wifi page 6: JOIN    FAIL ASSOC REFUSED" "wifi page 9: STAY 2S NOT REACHED" \
            "wifi page 3: AUTH  OK" "wifi page 4: ASSOC REFUSED, STATUS 17" "wifi page 6: LEFT, REJOIN --"; do
  grep -qF "$want" build/host/wifi_page_refuse.txt || { echo "FAIL: WiFi page, association refused: no line '$want'" >&2; exit 1; }
done
echo "WiFi page: text, a stuck baseband (TIMEOUT) and a silent core (NO ANSWER) as expected; stages 4-6 failures say where"
# the page over USB SysEx (firmware/src/wifi_sysex.c, docs/WIFI_STAGE4.md "Reading the results over USB"): with no
# button pressed for it, the report says no run yet, the run command starts one as SAVE then PLAY would (both commands
# say running meanwhile), the report comes whole; a second run from the open page leaves it and runs again; SAVE
# leaves and the report stays; EDIT refuses; bad requests are refused
HOST_WIFI_REPORT=build/host/wifi_usb_report.bin build/host/oplbox build/host/wifi_usb tests/script_wifi_usb.txt > build/host/wifi_usb.txt
for want in "   200 ms  USB reply 62: 02 00 00 00 00 00 00 00 00 | 0 bytes" "   250 ms  USB reply 63: 00 01 00 | 0 bytes" \
            "   250 ms  wifi page 0: WIFI TEST (STAGE 9)" "   251 ms  USB reply 62: 03 00 00 00" "   252 ms  USB reply 63: 03 01 00 |" \
            "  2000 ms  USB reply 62: 00 30 10 00 00 00 00 01 00 | 512 bytes" "  2001 ms  USB reply 62: 00 30 10 00 00 10 00 01 00 | 48 bytes" \
            "  2100 ms  USB reply 63: 00 02 00 |" "  2101 ms  USB reply 62: 03 " "  4000 ms  USB reply 62: 00 34 10 00 00 00 00 02 00 | 512 bytes" \
            "  4300 ms  USB reply 62: 00 34 10 00 00 00 00 02 00 | 512 bytes" "  4500 ms  USB reply 63: 04 00 00 |" \
            "  4800 ms  USB reply 62: 01 " "  4900 ms  USB reply 62: 01 "; do
  grep -qF -- "$want" build/host/wifi_usb.txt || { echo "FAIL: WiFi page over USB: no line '$want'" >&2; exit 1; }
done
[ "$(grep -c 'wifi page 0: WIFI TEST (STAGE 9)   1/14' build/host/wifi_usb.txt)" = 2 ] || { echo "FAIL: WiFi page over USB: not two runs" >&2; exit 1; }
# stage 8 (docs/WIFI_STAGE8.md): the WPA2 network set over USB (0x64; a bad one refused, none taken while a run goes
# on), joined against the model's WPA2 AP (tests/wifi/aps_wpa.txt): the steps, page 10 and 11, the PMK cached for the
# second run, a wrong passphrase (no msg 3, the AP's deauthentication), the network forgotten; the passphrase, the PMK
# and the GTKs in none of the output, the replies or the reports
HOST_WIFI_MODEL=tests/wifi/core_model.txt:tests/wifi/aps_wpa.txt HOST_WIFI_REPORT=build/host/wifi_wpa_report-%u.bin \
  build/host/oplbox build/host/wifi_wpa tests/script_wifi_wpa.txt > build/host/wifi_wpa_page.txt
for want in "   100 ms  USB reply 64: 01 00 | 0 bytes" "   200 ms  USB reply 64: 00 08 | 0 bytes" "   251 ms  USB reply 64: 03 00 | 0 bytes" \
            "  3400 ms  wifi page 6: JOIN    OK AID 1" "  3400 ms  wifi page 7: 4-WAY   OK 1 MS" "  3400 ms  wifi page 8: GROUP KEY OK KEY ID 2" \
            "  3400 ms  wifi page 9: STAY 2S OK 20 BEACONS" "  3400 ms  wifi page 10: REJOIN  OK" "  3900 ms  wifi page 0: JOIN TESTWPA2        10/14" \
            "  3900 ms  wifi page 2: CH  6  RSSI  -57  WPA2" "  4000 ms  wifi page 0: WPA2                 11/14" \
            "  4000 ms  wifi page 1: NETWORK TESTWPA2" "  4000 ms  wifi page 2: PMK  127 MS" "  4000 ms  wifi page 3: EAPOL IN 3 OUT 3 REFUSED 0" \
            "  4000 ms  wifi page 4: SEEN M1 M2 M3 M4 G1 G2" "  4000 ms  wifi page 5: PTK ATTR 09 IV 20000001 OK" \
            "  4000 ms  wifi page 6: GTK ID 2 MODE 0440 IV A0 OK" "  4000 ms  wifi page 7: DECRYPTED 41/41 GROUP 20/20" \
            "  4000 ms  wifi page 8: REKEYS 1  RNG REG CHANGES" "  8000 ms  wifi page 2: PMK  CACHED" "  7400 ms  wifi page 10: REJOIN  OK" \
            " 13400 ms  wifi page 7: 4-WAY   FAIL DEAUTH 15" " 13400 ms  wifi page 8: GROUP KEY NOT REACHED" \
            " 14000 ms  wifi page 4: SEEN M1 M2" " 14000 ms  wifi page 5: PTK  NOT LOADED" " 14000 ms  wifi page 8: NO MSG 3: WRONG PASSPHRASE?" \
            " 14100 ms  USB reply 64: 00 00 | 0 bytes" \
            "  4030 ms  wifi page 0: IP STEPS             12/14" "  4030 ms  wifi page 1: DHCP    OK 1 MS" \
            "  4030 ms  wifi page 2: ARP GW  OK 1 MS" "  4030 ms  wifi page 3: PING GW OK 10/10 0/0/0 MS" "  4030 ms  wifi page 4: PING IN OK 5 IN 2 S" \
            "  4030 ms  wifi page 5: ADDR 192.168.7.23/24" "  4030 ms  wifi page 6: GW 192.168.7.1" "  4030 ms  wifi page 7: GW MAC 02:47:57:00:00:01" \
            "  4030 ms  wifi page 8: DNS 192.168.7.1 +1" "  4060 ms  wifi page 0: DHCP, DATA           13/14" \
            "  4060 ms  wifi page 1: SENT DISC 1 REQ 1 REL 1" "  4060 ms  wifi page 2: GOT OFFER 1 ACK 1 NAK 0" \
            "  4060 ms  wifi page 3: SERVER 192.168.7.1" "  4060 ms  wifi page 4: LEASE 86400 S" "  4060 ms  wifi page 5: OFFER BCAST  ACK UNICAST" \
            "  4060 ms  wifi page 6: TX 24 ERR 0  RX 34 LWIP 34" "  4060 ms  wifi page 7: DECR 7>41 ERR 0>0 GRP 6>20" \
            "  4060 ms  wifi page 8: ICV 0 MIC 0 KEY 0 CLR 0 IV 0" "  4090 ms  wifi page 0: IP FRAMES 1-2        14/14" \
            "  4090 ms  wifi page 1: 1 OUT 368 B DHCP DISCOVER" "  4090 ms  wifi page 2: 08410000 02000000 006616C6" \
            "  4090 ms  wifi page 5: 2 IN 338 B DHCP OFFER" "  4090 ms  wifi page 6: 08020000 FFFFFFFF FFFF0200" \
            "  8030 ms  wifi page 1: DHCP    OK 1 MS" " 14030 ms  wifi page 1: DHCP    NOT REACHED" " 14060 ms  wifi page 1: NOT REACHED: SEE PAGE 12" \
            "USB reply 65: 00 05 C0 A8 07 17 " "USB reply 65: 00 04 C0 A8 07 17 00 00 " "   260 ms  USB reply 65: 00 01 00 00 00 00 00 00 " \
            "   980 ms  USB reply 65: 00 00 00 00 00 00 00 00 00 00 | 0 bytes"; do
  grep -qF -- "$want" build/host/wifi_wpa_page.txt || { echo "FAIL: WiFi page, WPA2: no line '$want'" >&2; exit 1; }
done
python3 - <<'PY'
import hashlib, sys
sys.path.insert(0, "tools")
from pathlib import Path
import fm1_wifi_report as wr
bad = []
secrets = [b"oplbox-host-passphrase", b"wrong-passphrase", b"OPLBOX-HOST-PASSPHRASE", b"WRONG-PASSPHRASE",
           hashlib.pbkdf2_hmac("sha1", b"oplbox-host-passphrase", b"TESTWPA2", 4096, 32),
           hashlib.pbkdf2_hmac("sha1", b"wrong-passphrase", b"TESTWPA2", 4096, 32),
           bytes(range(0x60, 0x70)), bytes((0x60 + i + 0x11) & 0xFF for i in range(16))]   # the model's GTKs
text = Path("build/host/wifi_wpa_page.txt").read_bytes()
for sec in secrets:
    for where, data in [("the page output", text)] + [(f"report {k}", Path(f"build/host/wifi_wpa_report-{k}.bin").read_bytes()) for k in (1, 2, 3, 4)]:
        for form in (sec, sec.hex().encode(), sec.hex().upper().encode()):
            if form in data:
                bad.append(f"{where} holds {sec[:8]!r}...")
r1 = wr.decode(Path("build/host/wifi_wpa_report-1.bin").read_bytes())
c = r1["counters"]
want = {"wpa_state": 4, "wpa_why": 8, "keys_loaded": 3,   # DOWN, LEFT: the run left at the end
         "eapol_rx": 3, "eapol_refused": 0, "group_handshakes": 1,
        "ptk_wcid_lo": 0x00000002, "ptk_wcid_hi": 0xFFFF6600, "ptk_attr": 9, "ptk_iv": 0x20000001, "ptk_eiv": 0,
        "ptk_key_same": 0xFF, "gtk_mode": 0x440, "gtk_attr": 8, "gtk_iv": 0xA0000001, "gtk_key_same": 0xFF, "pmk_cached": 0}
for k, v in want.items():
    if c.get(k) != v:
        bad.append(f"report 1: {k} {c.get(k)!r}, want {v:#x}")
steps = [(f["step"], f["ch"]) for f in r1["frames"] if f["step"].startswith(("eapol", "rejoin"))]
if steps != [("eapol_from_ap", 1), ("eapol_to_ap", 2), ("eapol_from_ap", 3), ("eapol_to_ap", 4), ("eapol_from_ap", 5),
             ("eapol_to_ap", 6), ("rejoin_from_ap", 1), ("rejoin_to_ap", 2), ("rejoin_from_ap", 3), ("rejoin_to_ap", 4)]:
    bad.append(f"report 1: EAPOL frames {steps}")
if "PTK MAC_WCID_ENTRY 00000002 FFFF6600 ATTR 00000009 IV/EIV 20000001 00000000 KEY WORDS AS WRITTEN 8/8" not in r1["text"]:
    bad.append("report 1: no PTK read-back line")
# stage 9: the IP steps' counters, the DHCP messages in the text, the IP frames (DISCOVER first, Protected; the OFFER as
# decrypted), the listening window
want = {"steps_reached": 0x3FF, "ip_start_err": 0, "dhcp_err": 0, "arp_err": 0, "ping_err": 0, "listen_err": 0,
        "dhcp_discover_tx": 1, "dhcp_request_tx": 1, "dhcp_release_tx": 1, "dhcp_offer_rx": 1, "dhcp_ack_rx": 1, "dhcp_nak_rx": 0,
        "ip_addr": 0xC0A80717, "ip_mask": 0xFFFFFF00, "ip_gateway": 0xC0A80701, "dhcp_server": 0xC0A80701,
        "dns_1": 0xC0A80701, "dns_2": 0x01010101, "lease_s": 86400, "gateway_mac_hi": 0x02475700, "gateway_mac_lo": 0x0001,
        "pings_sent": 10, "pings_answered": 10, "pings_late": 0, "listen_out": 5, "data_rx_clear": 0,
        "data_rx_not_decrypted": 0, "data_rx_key_err": 0, "data_rx_mic_err": 0, "data_tx_err": 0}
for k, v in want.items():
    if c.get(k) != v:
        bad.append(f"report 1: {k} {c.get(k)!r}, want {v:#x}")
if not (c.get("listen_in", 0) >= 5 and c.get("echo_out") == c.get("echo_in") and c.get("data_tx", 0) >= 20):
    bad.append(f"report 1: echo / data counts {c.get('listen_in')} {c.get('echo_in')} {c.get('echo_out')} {c.get('data_tx')}")
for line in ("DHCP OFFER YIADDR 192.168.7.23 SERVER 192.168.7.1 MASK 255.255.255.0 ROUTER 192.168.7.1 DNS (2) 192.168.7.1 1.1.1.1 LEASE 86400",
             "DHCP ACK YIADDR 192.168.7.23 ", "ADDRESS 192.168.7.23 MASK 255.255.255.0 GATEWAY 192.168.7.1 GATEWAY MAC 02:47:57:00:00:01",
             "PINGS (MS, - NO REPLY): 0 0 0 0 0 0 0 0 0 0  LATE 0", "IP FRAMES 1-2        14/14"):
    if line not in r1["text"]:
        bad.append(f"report 1: no line {line!r}")
ipf = [f for f in r1["frames"] if f["step"].startswith("ip_")]
if len(ipf) != 8 or (ipf[0]["step"], ipf[0]["ch"]) != ("ip_to_ap", 9) or (ipf[1]["step"], ipf[1]["ch"]) != ("ip_from_ap", 10) \
        or not ipf[0]["data"][1] & 0x40 or ipf[1]["data"][1] & 0x40 or ipf[0]["had"] != len(ipf[0]["data"]):
    bad.append(f"report 1: IP frames {[(f['step'], f['ch'], f['had']) for f in ipf]}")
r3c = wr.decode(Path("build/host/wifi_wpa_report-3.bin").read_bytes())["counters"]
if r3c.get("steps_reached", 0) & 0x3C0 or r3c.get("pings_sent") != 0:
    bad.append(f"report 3 (wrong passphrase): IP steps reached {r3c.get('steps_reached')}")
if "PMK  CACHED" not in wr.decode(Path("build/host/wifi_wpa_report-2.bin").read_bytes())["text"]:
    bad.append("report 2: the PMK not from the cache")
r3 = wr.decode(Path("build/host/wifi_wpa_report-3.bin").read_bytes())
if [f["ch"] for f in r3["frames"] if f["step"] == "eapol_from_ap"] != [1, 1, 1, 1]:
    bad.append("report 3 (wrong passphrase): not msg 1 four times")
if "NO NETWORK SET OVER USB:" not in wr.decode(Path("build/host/wifi_wpa_report-4.bin").read_bytes())["text"]:
    bad.append("report 4: the network not forgotten")
for b in bad: print("FAIL: WiFi page, WPA2:", b, file=sys.stderr)
if bad: sys.exit(1)
print("WiFi page, WPA2: joined, keys read back, rekeyed, PMK cached, a wrong passphrase said so; IP (stage 9): DHCP, "
      "ARP, 10 pings, pinged, the live command; no secret in the output")
PY
# ... and tools/fm1_wifi_report.py reads the button run's report through wifi_sysex.c: its text is the ten pages' text
python3 tests/wifi_report_tool.py build/host/slot_sysex_test build/host/wifi_report.bin build/host/wifi_page.txt build/host
# the WPA2 key primitives (firmware/wifi/wpa_crypto.c on BearSSL): PSK and PRF against IEEE 802.11 J.4.2 / J.3,
# AES key unwrap against RFC 3394 4.1-4.6, the PMK in main-loop steps, and the refusals
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_crypto tests/wifi_crypto.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c
build/host/wifi_crypto
# the station side of the EAPOL-Key 4-way and group key handshakes (firmware/wifi/wpa_eapol.c): Wireshark's recorded
# wpa-Induction handshake replayed byte for byte, a made-up AP for the group key handshake and rekeys, the refusals
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -Itests/wifi -o build/host/wifi_eapol tests/wifi_eapol.c tests/wifi/wpa_ap.c firmware/wifi/wpa_eapol.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c
build/host/wifi_eapol
# the WPA2 station (firmware/wifi/fm1wifi_wpa.c, docs/WIFI_STAGE8.md): the AP's RSN element, the station's join and
# handshakes against the made-up AP (tests/wifi/wpa_ap.c) with no register, then the driver against the model's WPA2
# AP, which plays the key tables as the MAC would use them (the writes register by register, msg 4 before the PTK, the
# read-back, a group key handshake, protected broadcasts, leaving, a wrong passphrase, an AP with no msg 1)
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -Itests/wifi -o build/host/wifi_wpa tests/wifi_wpa.c tests/wifi/sdio_model.c \
   firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c firmware/wifi/fm1wifi_wpa.c firmware/wifi/wpa_eapol.c $WIFI_AP
build/host/wifi_wpa tests/wifi
# IP over WiFi (firmware/wifi/fm1wifi_net.c on lwIP 2.1.3, docs/WIFI.md stage 7): a test that plays the AP, a DHCP
# server and two pinging hosts as 802.11 data frames (DHCP with a silent server, ARP, ICMP echo, the drop rules,
# EAPOL to its callback, our own ARP requests and pings), against the lwIP objects built at the top
cc -std=c11 -O2 -Wall -Wextra -Werror -DFM1WIFI_NET_DEBUG -Ifirmware/wifi $LWIP_INC -o build/host/wifi_net \
   tests/wifi_net.c firmware/wifi/fm1wifi_net.c build/host/lwip/*.o
build/host/wifi_net
# IP on the WPA2 station (firmware/wifi/fm1wifi_ip.c, docs/WIFI_STAGE9.md): the DHCP, ICMP and data frame readers, then
# against the model's WPA2 AP with a router behind it (tests/wifi/sdio_model.h "ap ... net="): DHCP (OFFER under the GTK,
# ACK under the PTK), ARP, ten pings, a host that pings us, a rekey meanwhile, the DHCPRELEASE; a silent server, a NAK,
# no replies, late replies, a wrong GTK, frames in the clear, a CCMP header left in, a deauthentication during DHCP.
# Then the test must catch deliberate bugs in fm1wifi_ip.c (tests/wifi_ip_mutants.py)
cc -std=c11 -O2 -Wall -Wextra -Werror -DFM1WIFI_NET_DEBUG -Ifirmware/wifi -Itests/wifi $LWIP_INC -o build/host/wifi_ip \
   tests/wifi_ip.c tests/wifi/sdio_model.c firmware/wifi/fm1wifi.c firmware/wifi/fm1wifi_frame.c firmware/wifi/fm1wifi_wpa.c \
   firmware/wifi/wpa_eapol.c firmware/wifi/fm1wifi_ip.c build/host/fm1wifi_net.o $WIFI_AP build/host/lwip/*.o
build/host/wifi_ip tests/wifi
python3 tests/wifi_ip_mutants.py build/host
# the BLE HCI layer (firmware/ble/hci.c, docs/BLUETOOTH.md stage 1) stands alone like firmware/wifi/: warning-free with
# only its own directory, and it includes nothing from outside it. Then against the fake controller
# (tests/ble/hci_model.c): the packets byte for byte, H4, a whole session diffed against tests/ble/session_expect.txt
# (reset, event masks, buffer size, advertising, LE Connection Complete, ACL in and out with flow control, a
# connection update, a disconnection either way), the same with answers inside the send call, and how a start fails;
# and the test must catch 17 deliberate bugs in hci.c
for f in firmware/ble/*.c firmware/ble/*.h; do
  if grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$f" | grep -v -e '"hci\.h"' -e '"ble_midi\.h"' -e '"ble_nimble\.h"' \
       -e '"ble_nimble_npl\.h"' -e '"fm1ble_test\.h"' -e '"fm1ble\.h"' -e '"fm1ble_port\.h"' -e '"fm1ble_regs\.h"' -e '"fm1ble_agc\.h"' \
       -e '"fm1ble_stack\.h"' -e '"cordio_ctrl\.h"' -e '"pal_fm1\.h"'; then
    echo "FAIL: $f includes a file from outside firmware/ble/" >&2; exit 1; fi
done
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/ble -c -o build/host/ble_hci.o firmware/ble/hci.c
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Ifirmware/ble -Itests/ble -o build/host/ble_hci \
   tests/ble_hci.c tests/ble/hci_model.c firmware/ble/hci.c
build/host/ble_hci tests/ble
python3 tests/ble_mutants.py build/host
# the BLE-MIDI profile (firmware/ble/ble_midi.c, docs/BLUETOOTH.md stage 2): warning-free alone, then its test: the
# UUIDs and advertising data, packets byte for byte (header, 13-bit timestamps and their wrap, running status, SysEx
# split over packets) worked out from the specification's rules, round trips through the encoder and decoder at ten
# packet sizes, what the decoder drops, the bridge to the app's midi_get(), and random packets; it must catch 32
# deliberate bugs in ble_midi.c
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/ble -c -o build/host/ble_midi.o firmware/ble/ble_midi.c
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Ifirmware/ble -o build/host/ble_midi \
   tests/ble_midi.c firmware/ble/ble_midi.c
build/host/ble_midi
python3 tests/ble_midi_mutants.py build/host
# the NimBLE host (firmware/ble/nimble/, Apache-2.0, unchanged: held to the upstream commit by its checksums) on our port
# (ble_nimble.c, ble_nimble_npl.c, nimble_port/; docs/BLUETOOTH.md "Cordio path as built" step 1), against the fake
# controller: start-up, advertising, a central that walks the GATT database, subscribes, writes MIDI and is sent MIDI,
# the connection interval, disconnection either way, and the ways start-up fails (one process each: NimBLE is a
# singleton); it must catch the deliberate bugs in the port (tests/ble_nimble_mutants.py)
python3 - <<'PY'
import hashlib, sys
from pathlib import Path
d = Path("firmware/ble/nimble")
want = dict(reversed(l.split(None, 1)) for l in (d / "SHA256SUMS").read_text().splitlines())
have = {str(p.relative_to(d)) for p in d.rglob("*") if p.is_file() and p.name not in ("README.md", "SHA256SUMS")}
bad = [f for f in sorted(want) if not (d / f).is_file() or hashlib.sha256((d / f).read_bytes()).hexdigest() != want[f]]
bad += [f"{f} (not in SHA256SUMS)" for f in sorted(have - set(want))]
if bad: sys.exit("FAIL: firmware/ble/nimble differs from NimBLE e00b85cc: " + ", ".join(bad))
print(f"NimBLE: {len(want)} files as at e00b85cc")
PY
NB_DIR=firmware/ble/nimble
NB_INC="-Ifirmware/ble/nimble_port -Ifirmware/ble -Itests/ble -I$NB_DIR/nimble/include -I$NB_DIR/nimble/host/include -I$NB_DIR/nimble/transport/include -I$NB_DIR/porting/nimble/include -I$NB_DIR/nimble/host/services/gap/include -I$NB_DIR/nimble/host/services/gatt/include -I$NB_DIR/nimble/host/util/include"
mkdir -p build/host/nimble
ls $NB_DIR/nimble/host/src/*.c $NB_DIR/nimble/host/services/gap/src/*.c $NB_DIR/nimble/host/services/gatt/src/*.c \
   $NB_DIR/nimble/host/util/src/*.c $NB_DIR/nimble/transport/src/transport.c $NB_DIR/porting/nimble/src/*.c |
  xargs -P 4 -I{} sh -c 'cc -std=gnu11 -O1 -w '"$NB_INC"' -c -o build/host/nimble/$(basename {} .c).o {}'
cc -std=gnu11 -Wall -Wextra -Werror $NB_INC -c -o build/host/nimble/ble_nimble.o firmware/ble/ble_nimble.c
cc -std=gnu11 -Wall -Wextra -Werror $NB_INC -c -o build/host/nimble/ble_nimble_npl.o firmware/ble/ble_nimble_npl.c
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror $NB_INC -o build/host/ble_nimble tests/ble_nimble.c \
   tests/ble/hci_model.c firmware/ble/hci.c firmware/ble/ble_midi.c build/host/nimble/*.o
for w in session silent refused refusing never npl; do build/host/ble_nimble tests/ble build/host $w; done
python3 tests/ble_nimble_mutants.py build/host
# the BLE baseband driver (firmware/ble/fm1ble.c, docs/BLUETOOTH_RADIO.md) compiles warning-free with only its directory;
# its test against the register model (tests/ble_radio.c) needs the stack and runs after Cordio and NimBLE below
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/ble -c -o build/host/fm1ble.o firmware/ble/fm1ble.c
# the C library functions the BLE stacks need beyond the BSP's (firmware/ble/ble_libc.c: a small heap, memmove, strlen)
cc -std=c11 -Wall -Wextra -Werror -Dmalloc=ble_malloc -Dfree=ble_free -Drealloc=ble_realloc -Dmemmove=ble_memmove \
   -Dstrlen=ble_strlen -D__assert_func=ble_assert_func -fno-builtin -c -o build/host/ble_libc.o firmware/ble/ble_libc.c
cc -std=c11 -Wall -Wextra -Werror -fno-builtin -o build/host/ble_libc_test tests/ble_libc.c build/host/ble_libc.o
build/host/ble_libc_test
# Cordio's LE controller (firmware/ble/cordio/, Apache-2.0, held to the upstream commit by its checksums) in our configuration
# and glue (firmware/ble/cordio_port/) on host stubs for the chip's PAL: the HCI start-up and advertising commands, the
# refusals, and the first advertising transmit it asks the baseband for (docs/BLUETOOTH_CORDIO.md). Never run on a chip.
python3 - <<'PY'
import hashlib, sys
from pathlib import Path
d = Path("firmware/ble/cordio")
want = dict(reversed(l.split(None, 1)) for l in (d / "SHA256SUMS").read_text().splitlines())
have = {str(p.relative_to(d)) for p in d.rglob("*") if p.is_file() and p.name not in ("SHA256SUMS", "README.md")}
bad = [f for f in sorted(want) if not (d / f).is_file() or hashlib.sha256((d / f).read_bytes()).hexdigest() != want[f]]
bad += [f"{f} (not in SHA256SUMS)" for f in sorted(have - set(want))]
if bad: sys.exit("FAIL: firmware/ble/cordio differs from Cordio 3656312: " + ", ".join(bad))
print(f"Cordio: {len(want)} files as at 3656312")
PY
CR=firmware/ble/cordio; CP=firmware/ble/cordio_port
CR_INC=$(find $CR -name '*.h' -printf '-I%h\n' | sort -u | tr '\n' ' ')
CR_DEF=$(sed 's/^/-D/' $CP/DEFINES.txt | tr '\n' ' ')
mkdir -p build/host/cordio
sed "s|^|$CR/|" $CP/SOURCES.txt > build/host/cordio/sources.txt
echo $CP/cordio_ctrl.c >> build/host/cordio/sources.txt; echo $CP/cordio_wsf.c >> build/host/cordio/sources.txt
echo $CP/pal_stubs_host.c >> build/host/cordio/sources.txt
xargs -P 4 -I{} sh -c "cc -std=gnu11 -O1 -w -fshort-enums -ffunction-sections -fdata-sections $CR_DEF $CR_INC -Ifirmware/ble -I$CP -c -o build/host/cordio/\$(echo {} | tr / _).o {}" < build/host/cordio/sources.txt
cc -std=gnu11 -Wall -Wextra -Werror -fshort-enums -Ifirmware/ble -I$CP -Wl,--gc-sections -o build/host/ble_cordio tests/ble_cordio.c \
   firmware/ble/ble_midi.c build/host/cordio/*.o
build/host/ble_cordio
# Cordio's PAL on the FM-1's baseband (firmware/ble/cordio_port/pal_fm1.c) on the radio driver's one-packet operations,
# against the register model's air and a central written from the specification: advertising on 37-39, a SCAN_REQ
# answered, a CONNECT_IND, connection events on CSA #1's channels with the anchor from the first packet, data both ways
# with SN/NESN, LL_TERMINATE_IND, advertising again (docs/BLUETOOTH_CORDIO.md). The receive interrupt runs within T_IFS
# in the model: the FM-1 has to show it does. Never run on a chip.
cc -std=gnu11 -Wall -Wextra -Werror -fshort-enums $CR_DEF $CR_INC -Ifirmware/ble -I$CP -c -o build/host/pal_fm1.o $CP/pal_fm1.c
cc -std=gnu11 -Wall -Wextra -Werror -fshort-enums -Ifirmware/ble -I$CP -Itests/ble -Wl,--gc-sections -o build/host/ble_pal tests/ble_pal.c \
   tests/ble/bb_model.c tests/ble/air_central.c firmware/ble/fm1ble.c firmware/ble/ble_midi.c build/host/pal_fm1.o \
   $(ls build/host/cordio/*.o | grep -v pal_stubs_host)
build/host/ble_pal
python3 tests/ble_pal_mutants.py build/host
# the whole stack minus the chip: NimBLE and the BLE-MIDI service over HCI to Cordio, on the PAL and the driver, against the
# model's air and the central (tests/ble/air_central.c) doing L2CAP and ATT: it finds the characteristic, subscribes, writes
# a Note On that comes out of ble_midi_get, gets the app's notification, leaves; the peripheral advertises again
cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -fshort-enums -Ifirmware/ble -I$CP -Itests/ble -Wl,--gc-sections -o build/host/ble_stack \
   tests/ble_stack.c tests/ble/bb_model.c tests/ble/air_central.c firmware/ble/fm1ble.c firmware/ble/fm1ble_test.c \
   firmware/ble/fm1ble_stack.c firmware/ble/ble_midi.c firmware/ble/hci.c \
   build/host/pal_fm1.o $(ls build/host/cordio/*.o | grep -v pal_stubs_host) build/host/nimble/*.o
build/host/ble_stack
# the test run (fm1ble_test.c) as the FM-1 runs it, on the model's air: the driver's own advertising, then the stack;
# the central connects, plays a Note On (midi_in) and is sent one (midi_out); the run closes the connection
build/host/ble_stack run
# the test run's driver steps against the register model (no air: the baseband that repeats events, the one that needs
# re-arming, the one that needs the shared radio's init, a silent one, a CONNECT_IND of made-up bytes Cordio refuses), the
# stack (hci) on the same model
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fshort-enums -Ifirmware/ble -I$CP -Itests/ble -Wl,--gc-sections \
   -o build/host/ble_radio tests/ble_radio.c tests/ble/bb_model.c firmware/ble/fm1ble.c firmware/ble/fm1ble_test.c \
   firmware/ble/fm1ble_stack.c firmware/ble/ble_midi.c firmware/ble/hci.c build/host/pal_fm1.o \
   $(ls build/host/cordio/*.o | grep -v pal_stubs_host) build/host/nimble/*.o
build/host/ble_radio
# the update path from felucca-bsp (ota.c, the update loader) with a package made in the test: the identity, the
# firmware's own frames, an install that keeps the song slots and the settings, refusals before any erase
cc -O2 -Wall -Wno-unused-function -o build/host/loader_test tests/loader_test.c
build/host/loader_test
python3 - <<'PY'
import array, sys, wave
from pathlib import Path
from PIL import Image

bad = []
def check(ok, what):
    if not ok: bad.append(what)

def rgb(r, g, b):                       # RGB() in gfx.c, then RGB565 -> 8 bit as host.c writes the PPM
    c = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    return ((c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
BLACK, MUTE, TXT = (0, 0, 0), rgb(110, 110, 110), rgb(220, 220, 220)   # C_BLACK, C_MUTE, C_TXT

# the screen at 3000 ms (tests/host.c SHOT): something drawn, and the battery icon (oplbox.c batt_draw at
# x 225, y 5) with the 2 bars of 3 that host.c's stand-in count, HOST_BATT 575, gives
p = Path("build/host/oplbox-3000.ppm")
check(p.exists(), "no screen at 3000 ms")
if p.exists():
    im = Image.open(p).convert("RGB")
    check(len(im.getcolors(240 * 240)) > 2, "screen at 3000 ms is blank")
    for xy, want, what in [((225, 5), MUTE, "icon body"), ((236, 8), MUTE, "icon terminal"),
                           ((227, 8), TXT, "bar 1"), ((230, 8), TXT, "bar 2"), ((233, 8), BLACK, "bar 3 off")]:
        got = im.getpixel(xy)
        check(got == want, f"battery {what} at {xy}: {got}, want {want}")

# the sound: 6.5 s of the default script (tests/host.c end_ms), and notes in it
w = wave.open("build/host/oplbox.wav")
check((w.getnchannels(), w.getsampwidth(), w.getframerate()) == (2, 2, 44100), "WAV is not 16-bit stereo 44.1 kHz")
check(w.getnframes() >= 44100 * 6, f"WAV is {w.getnframes()} frames, want 6.5 s")
s = array.array("h", w.readframes(w.getnframes()))
peak = max(map(abs, s), default=0)
check(peak > 1000, f"WAV is silent (peak {peak})")

# the arp (tests/script_arp.txt): heard while the chord is held with the song paused (1.0-3.3 s), quiet once
# every key is up (3.4-3.49 s), quiet again when latch off stops it (4.6-4.9 s)
a = wave.open("build/host/arp.wav")
s2 = array.array("h", a.readframes(a.getnframes()))
def apeak(t0, t1): return max(map(abs, s2[int(t0 * 44100) * 2:int(t1 * 44100) * 2]), default=0)
check(apeak(1.0, 3.3) > 1000, f"arp not heard while the chord is held (peak {apeak(1.0, 3.3)})")
check(apeak(3.4, 3.49) < 100, f"arp still sounding after the keys are up (peak {apeak(3.4, 3.49)})")
check(apeak(4.6, 4.9) < 100, f"arp still sounding after latch off (peak {apeak(4.6, 4.9)})")

for p in sorted(Path("build/host").glob("oplbox-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
for b in bad: print("FAIL:", b, file=sys.stderr)
if bad: sys.exit(1)
print("checks passed: screen, battery icon, sound (peak %d), arp sound" % peak)
PY
# the Bluetooth test page (docs/BLUETOOTH_PAGE.md): firmware/ble/fm1ble_test.c, the run the firmware links, is warning-free
# alone; the simulator links tests/ble/fm1ble_test_host.c instead (a made-up run), so these test the page, the SysEx
# report (firmware/src/ble_sysex.c, commands 0x66 and 0x67) and tools/fm1_ble_report.py, not the radio
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/ble -Ifirmware/ble/cordio_port -c -o build/host/fm1ble_test.o firmware/ble/fm1ble_test.c
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/ble -Ifirmware/ble/cordio_port -c -o build/host/fm1ble_stack.o firmware/ble/fm1ble_stack.c
# over USB SysEx, no button pressed: no run yet (2), the run command with a 2 s wait for a central (none comes: connect
# fails, the MIDI steps are skipped), both commands say running meanwhile (3), the report whole, a second run asked from
# the open page (no wait: the connect steps are skipped) leaves it and runs again, SAVE leaves, EDIT refuses (4), bad
# requests are refused (1)
HOST_BLE_REPORT=build/host/ble_usb_report-%u.bin build/host/oplbox build/host/ble_usb tests/script_ble_usb.txt > build/host/ble_usb.txt
for want in "   200 ms  USB reply 66: 02 00 00 00 00 00 00 00 00 | 0 bytes" "   250 ms  USB reply 67: 00 01 00 | 0 bytes" \
            "   250 ms  ble page 0: BLUETOOTH TEST (STAGE 5)" "   251 ms  USB reply 66: 03 00 00 00" "   252 ms  USB reply 67: 03 01 00 |" \
            "  3652 ms  ble page 6: CONNECT  FAIL NO CENTRAL CONNECTED" "  3652 ms  ble page 7: MIDI_IN  SKIP SKIPPED: AN EARLIER" \
            "  5000 ms  USB reply 66: 00 C4 0C 00 00 00 00 01 00 | 512 bytes" "  5001 ms  USB reply 66: 00 C4 0C 00 00 0C 00 01 00 | 196 bytes" \
            "  5100 ms  USB reply 67: 00 02 00 |" "  5101 ms  USB reply 66: 03 " "  6506 ms  ble page 6: CONNECT  SKIP SKIPPED: NO WAIT FOR" \
            "  7000 ms  USB reply 66: 00 C0 0C 00 00 00 00 02 00 | 512 bytes" "  7300 ms  USB reply 66: 00 C0 0C 00 00 00 00 02 00 | 512 bytes" \
            "  7500 ms  USB reply 67: 04 00 00 |" "  7800 ms  USB reply 66: 01 " "  7900 ms  USB reply 66: 01 " \
            "  8000 ms  USB reply 67: 01 " "  8100 ms  USB reply 67: 01 "; do
  grep -qF -- "$want" build/host/ble_usb.txt || { echo "FAIL: Bluetooth page over USB: no line '$want'" >&2; exit 1; }
done
[ "$(grep -c 'ble page 0: BLUETOOTH TEST (STAGE 5)   1/14' build/host/ble_usb.txt)" = 2 ] || { echo "FAIL: Bluetooth page over USB: not two runs" >&2; exit 1; }
# by the buttons: SAVE (the WiFi page), HOME there (the Bluetooth page), PLAY runs it (a central connects), PLAY turns its fourteen
# pages, SAVE leaves it (the WiFi page's first screen again), SAVE leaves that; then the same with the hci step failing
HOST_BLE_CONNECT=1 HOST_BLE_REPORT=build/host/ble_page_report-%u.bin build/host/oplbox build/host/ble_page tests/script_ble_page.txt > build/host/ble_page.txt
HOST_BLE_FAIL=hci HOST_BLE_REPORT=build/host/ble_fail_report-%u.bin build/host/oplbox build/host/ble_fail tests/script_ble_page.txt > build/host/ble_fail.txt
for want in "   200 ms  wifi page 0: WIFI TEST (STAGE 9)" "   300 ms  ble page 0: BLUETOOTH TEST (STAGE 5)" "   400 ms  ble page 1: WORKING" \
            "ble page 6: CONNECT  PASS CONNECTED, INTERVAL" "ble page 0: STEP 3 HCI   4/14" "ble page 4: RAW: 41 BYTES (40 SHOWN)" \
            "ble page 0: COUNTERS 4/5   13/14" "ble page 8: RAM_END                01C16940" \
            "ble page 0: COUNTERS 5/5   14/14" "ble page 1: PAL_ADV_EVENTS         0000000C" "ble page 3: PAL_TIMER_LATE_US" \
            "  4300 ms  wifi page 0: WIFI TEST (STAGE 9)"; do
  grep -qF -- "$want" build/host/ble_page.txt || { echo "FAIL: Bluetooth page: no line '$want'" >&2; exit 1; }
done
for want in "ble page 3: HCI      FAIL MADE-UP FAILURE (HOS" "ble page 4: ADV_SET  SKIP SKIPPED: AN EARLIER" "ble page 8: MIDI_OUT SKIP SKIPPED: AN EARLIER"; do
  grep -qF -- "$want" build/host/ble_fail.txt || { echo "FAIL: Bluetooth page, failed step: no line '$want'" >&2; exit 1; }
done
# ... and tools/fm1_ble_report.py reads the button run's report through ble_sysex.c: its text is the pages' text
python3 tests/ble_report_tool.py build/host/slot_sysex_test build/host/ble_page_report-1.bin build/host/ble_page.txt build/host/ble_fail_report-1.bin build/host
# tools/ble_central.py, the BlueZ central a bench run uses (fm1_ble_report.py --central), on stand-ins for bluetoothctl and
# alsa-utils (tests/ble/fake_bluez/): the trials' addresses, connect, the sequencer port, the Note On both ways, failures
python3 tests/ble_central_tool.py build/host

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/oplbox.c on this computer (tests/host.c): build/host/oplbox.wav and the screens as PNG.
# Then check them, so a drawing or sound regression fails here (exit 1) instead of passing silently.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
rm -f build/host/oplbox-*.ppm build/host/oplbox-*.png build/host/oplbox.wav
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -DDBOPL2_SCOPE_N=512 -Ifirmware/src -Ifirmware/gen \
   -Ifirmware/wifi -Itests/wifi -o build/host/oplbox firmware/src/oplbox.c firmware/src/dbopl2.c firmware/wifi/fm1wifi.c tests/wifi/sdio_model.c
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
# includes anything from outside it
for f in firmware/wifi/*.c firmware/wifi/*.h; do
  if grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$f" | grep -v '"\(fm1wifi[a-z_]*\|wpa_crypto\|bearssl/[a-z_]*\)\.h"'; then
    echo "FAIL: $f includes a file from outside firmware/wifi/" >&2; exit 1; fi
done
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -c -o build/host/fm1wifi.o firmware/wifi/fm1wifi.c
# the register header: the accessors hit their addresses, and every address matches docs/WIFI.md
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_regs tests/wifi_regs.c firmware/wifi/fm1wifi.c
build/host/wifi_regs > build/host/wifi_regs.txt
python3 tests/wifi_regs.py build/host/wifi_regs.txt
# fm1wifi against the register model (tests/wifi/sdio_model.c, the library's port on the host): the power-up and
# the first reads must match tests/wifi/bringup_expect.txt and diag_expect.txt (written by hand from
# docs/WIFI.md), the BBP waits are bounded, power-down puts every register back; the fixture format and diff.
# Stage 3 (docs/WIFI_STAGE3.md): the radio's bring-up must write what JieLi's wl_rf_common.a writes against the same
# model, record for record (tests/wifi/radio_trace.txt.gz, from tools/wifi_ir.py --trace), its four kinds of wait
# must end in TIMEOUT when the model never answers, and power-down must put every register back
gzip -dc tests/wifi/radio_trace.txt.gz > build/host/radio_trace.txt
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -Itests/wifi -o build/host/wifi_model tests/wifi_model.c tests/wifi/sdio_model.c firmware/wifi/fm1wifi.c
build/host/wifi_model tests/wifi build/host
# ... and that test catches a swapped write, a dropped delay and a wait without its bound (8 deliberate bugs)
python3 tests/wifi_mutants.py build/host
# the generated tables and the trace are what the SDK's archive gives (only where the SDK is: tools/wifi_ir.py)
if [ -f "${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}/cpu/wl82/liba/wl_rf_common.a" ]; then
  python3 tools/wifi_ir.py --check
else
  echo "wifi_ir: no JieLi SDK here, the generated tables and trace not re-checked"
fi
# the WiFi page in the simulator (tests/script_wifi.txt): its text against the made-up core (stage 2's reads, the
# radio's bring-up, page 2's trim and channels), then against a core whose baseband never answers and one that
# answers nothing: an error on the page, the radio not reached, no hang
build/host/oplbox build/host/wifi tests/script_wifi.txt > build/host/wifi_page.txt
HOST_WIFI_MODEL=tests/wifi/core_bbp_stuck.txt build/host/oplbox build/host/wifi_stuck tests/script_wifi.txt > build/host/wifi_page_stuck.txt
HOST_WIFI_MODEL=tests/wifi/core_silent.txt build/host/oplbox build/host/wifi_silent tests/script_wifi.txt > build/host/wifi_page_silent.txt
HOST_WIFI_MODEL=tests/wifi/core_trim_hang.txt build/host/oplbox build/host/wifi_trim tests/script_wifi.txt > build/host/wifi_page_trim.txt
for want in "wifi page 0: WIFI TEST (STAGE 3)" "wifi page 1: PLAY: POWER UP, TRIM, READ" "wifi page 2: SAVE: LEAVE" \
            "wifi page 0: WIFI TEST (STAGE 3)    1/2" "wifi page 1: RESULT: OK" "wifi page 2: MAC_CSR0     35920200" \
            "wifi page 3: MAC_SYS_CTRL 0000000C" "wifi page 4: BBP 0        21" "wifi page 5: PLL BANK  64  MID  95" \
            "wifi page 6: FILTER  141  80  44" "wifi page 7: RAM CHECK: 0 CHANGED" "wifi page 9: PLAY: PAGE 2   SAVE: LEAVE" \
            "wifi page 0: TRIM  CH  6  PWR 6    2/2" "wifi page 1: TX LO  57  41  DC   -1   0" \
            "wifi page 2: TX GAIN   +5  PHASE   +2 Q" "wifi page 3: RX GAIN  -20  PHASE   +7 I" \
            "wifi page 4: RX DC I +123 +103  +76  +22" "wifi page 5: RX DC Q  -77  -66  -51  -21" \
            "wifi page 6: ADC  1- 5   25  21  17  24  20" "wifi page 8: ADC 11-13   18  25  21" \
            "wifi page 9: PLAY: PAGE 1   SAVE: LEAVE"; do
  grep -qF "$want" build/host/wifi_page.txt || { echo "FAIL: WiFi page: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: TIMEOUT" "wifi page 2: MAC_CSR0     35920200" "wifi page 4: BBP 0        TIMEOUT" \
            "wifi page 5: PLL BANK  -" "wifi page 1: NOT REACHED"; do
  grep -qF "$want" build/host/wifi_page_stuck.txt || { echo "FAIL: WiFi page, BBP stuck: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: NO ANSWER" "wifi page 2: MAC_CSR0     00000000" "wifi page 6: FILTER  -"; do
  grep -qF "$want" build/host/wifi_page_silent.txt || { echo "FAIL: WiFi page, silent core: no line '$want'" >&2; exit 1; }
done
for want in "wifi page 1: RESULT: TIMEOUT AT TRIM" "wifi page 0: TRIM FAILED  CH"; do
  grep -qF "$want" build/host/wifi_page_trim.txt || { echo "FAIL: WiFi page, trim timeout: no line '$want'" >&2; exit 1; }
done
if grep -qF "NOT REACHED" build/host/wifi_page_trim.txt; then echo "FAIL: WiFi page: a trim timeout reads NOT REACHED" >&2; exit 1; fi
echo "WiFi page: text, a stuck baseband (TIMEOUT) and a silent core (NO ANSWER) as expected"
# the WPA2 key primitives (firmware/wifi/wpa_crypto.c on BearSSL): PSK and PRF against IEEE 802.11 J.4.2 / J.3,
# AES key unwrap against RFC 3394 4.1-4.6, the PMK in main-loop steps, and the refusals
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_crypto tests/wifi_crypto.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c
build/host/wifi_crypto
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

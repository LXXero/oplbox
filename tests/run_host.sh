#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/oplbox.c on this computer (tests/host.c): build/host/oplbox.wav and the screens as PNG.
# Then check them, so a drawing or sound regression fails here (exit 1) instead of passing silently.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
rm -f build/host/oplbox-*.ppm build/host/oplbox-*.png build/host/oplbox.wav
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -DDBOPL2_SCOPE_N=512 -Ifirmware/src -Ifirmware/gen -o build/host/oplbox firmware/src/oplbox.c firmware/src/dbopl2.c
build/host/oplbox build/host/oplbox
# the editor's undo and live recording, checked against the song in memory: each script exits 1 when a check fails
build/host/oplbox build/host/undo tests/script_undo.txt
build/host/oplbox build/host/rec tests/script_rec.txt
# the arpeggiator: steps, tempo, gate, latch and the channel it takes, checked against its state and the chip
build/host/oplbox build/host/arp tests/script_arp.txt
# the arp written into the tracker (TODO 4): the cells, their playback against the live arp's key-ons, undo
build/host/oplbox build/host/arp_write tests/script_arp_write.txt
# the keys' instrument outside EDIT (TODO 5): PRESETS, ALGO's copy against the chip's C0, the song's own left alone
build/host/oplbox build/host/live_instr tests/script_live_instr.txt
# the song slots over SysEx (firmware/src/slot_sysex.c, TODO 2a): pull, push and every refusal through ota.c's framing
# against a fake computer, macsop_check against macsop_parse; then tools/fm1_slot.py against the same code on a pipe
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function -Ifirmware/src -o build/host/slot_sysex_test tests/slot_sysex_test.c
build/host/slot_sysex_test
python3 tests/slot_sysex_tool.py build/host/slot_sysex_test build/host
# the WiFi register header (firmware/hal/fm1_wifi_regs.h): it and the HAL compile warning-free (the device's MMIO
# path too), the accessors hit their addresses, and every address matches docs/WIFI.md
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/hal -c -o build/host/fm1_wifi_hal.o firmware/hal/fm1_wifi_hal.c
cc -std=c11 -Wall -Wextra -Werror -DFM1_WIFI_HOST -Ifirmware/hal -o build/host/wifi_regs tests/wifi_regs.c firmware/hal/fm1_wifi_hal.c
build/host/wifi_regs > build/host/wifi_regs.txt
python3 tests/wifi_regs.py build/host/wifi_regs.txt
# the WiFi register model (tests/wifi/sdio_model.c), its fixture format and diff: the HAL's enumeration runs
# against a fake card and must match tests/wifi/sdio_enum_expect.txt (wl30_sdio_enum, docs/WIFI.md)
cc -std=c11 -Wall -Wextra -Werror -DFM1_WIFI_HOST -DFM1_WIFI_SDIO_MODEL -Ifirmware/hal -Itests/wifi -o build/host/wifi_model tests/wifi_model.c tests/wifi/sdio_model.c firmware/hal/fm1_wifi_hal.c
build/host/wifi_model tests/wifi build/host
# the WPA2 key primitives (firmware/wifi/wpa_crypto.c on BearSSL): PSK and PRF against IEEE 802.11 J.4.2 / J.3,
# AES key unwrap against RFC 3394 4.1-4.6, the PMK in main-loop steps, and the refusals
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/wifi -o build/host/wifi_crypto tests/wifi_crypto.c firmware/wifi/wpa_crypto.c firmware/wifi/bearssl/*.c
build/host/wifi_crypto
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

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

for p in sorted(Path("build/host").glob("oplbox-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
for b in bad: print("FAIL:", b, file=sys.stderr)
if bad: sys.exit(1)
print("checks passed: screen, battery icon, sound (peak %d)" % peak)
PY

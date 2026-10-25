#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_slot.py against the device's own code: build/host/slot_sysex_test --serve runs
firmware/src/slot_sysex.c with ota.c behind a pipe (one SysEx message a line, hex), so the tool and the FM-1's side
exchange the same bytes they would over USB-MIDI. Exit 1 when a check fails.

  tests/slot_sysex_tool.py build/host/slot_sysex_test OUTDIR
"""
import io
import os
import queue
import subprocess
import sys
import threading
from contextlib import redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import fm1_slot  # noqa: E402

fm1_slot.DELAY.update(reply=1.0, idle=3.0)
BIN, OUT = sys.argv[1], sys.argv[2]
bad = []


class PipeLink:
    """fm1_install's link interface on the fake FM-1's stdin / stdout"""

    def __init__(self, proc):
        self.p, self.q = proc, queue.Queue()
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        for line in self.p.stdout:
            self.q.put(bytes.fromhex(line.strip()))

    lost = False

    def send(self, pkt):
        self.p.stdin.write(pkt.hex() + "\n")
        self.p.stdin.flush()
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        pass


class PipeBackend:
    """one fake FM-1 for every run of the tool, so a push is still there for the pull"""

    def __init__(self, *args):
        self.p = subprocess.Popen([BIN, "--serve", *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.link = PipeLink(self.p)

    def input_names(self):
        return ["FM-1 (pipe)"]

    output_names = input_names

    def open(self, i, o):
        return self.link

    def stop(self):
        self.p.stdin.close()
        self.p.wait(5)


def tool(backend, *argv):
    err = io.StringIO()
    with redirect_stderr(err):
        rc = fm1_slot.main(list(argv), backend=backend, out=io.StringIO())
    return rc, err.getvalue()


def check(ok, what):
    if not ok:
        bad.append(what)


def song(i):
    path = os.path.join(OUT, f"song{i}.cmf")
    subprocess.run([BIN, "--dump", str(i), path], check=True)
    return path, open(path, "rb").read()


kaiser, kaiser_b = song(13)                                   # 2041 bytes: one chunk
traders, traders_b = song(8)                                  # 28510 bytes: 56 chunks either way
back = os.path.join(OUT, "back.cmf")

dev = PipeBackend("2")                                        # SLOT 2 holds the song in the player
rc, err = tool(dev, "push", "3", traders)
check(rc == 0, f"push TRADERS INTRO to slot 3: exit {rc} {err}")
rc, err = tool(dev, "pull", "3", back)
check(rc == 0 and open(back, "rb").read() == traders_b, f"pull slot 3: exit {rc} {err}, not the file pushed")
rc, err = tool(dev, "push", "9", kaiser)
rc2, _ = tool(dev, "pull", "9", back)
check(rc == 0 and rc2 == 0 and open(back, "rb").read() == kaiser_b, "KAISER through slot 9")

rc, err = tool(dev, "push", "2", kaiser)
check(rc == 1 and "in the player" in err, f"push to the slot in the player: exit {rc} {err}")
rc, err = tool(dev, "pull", "5", back)
check(rc == 1 and "empty" in err, f"pull an empty slot: exit {rc} {err}")
junk = os.path.join(OUT, "junk.cmf")
open(junk, "wb").write(bytes(1000))
rc, err = tool(dev, "push", "4", junk)
check(rc == 1 and "not a .CMF" in err, f"push a file that is no .CMF: exit {rc} {err}")
open(junk, "wb").write(b"A.H." + bytes(0x8000 - 16 - 3))
rc, err = tool(dev, "push", "4", junk)
check(rc == 2 and "bigger than a slot" in err, f"push 32 KiB - 15: exit {rc} {err}")
rc, err = tool(dev, "pull", "3", back)
check(rc == 0 and open(back, "rb").read() == traders_b, "slot 3 changed after the refusals")
dev.stop()

old = PipeBackend("--old")                                    # a firmware that only answers the handshake
rc, err = tool(old, "pull", "1", back)
check(rc == 4 and "no reply" in err, f"pull from a firmware without slot transfer: exit {rc} {err}")
old.stop()

for b in bad:
    print("FAIL:", b, file=sys.stderr)
if bad:
    sys.exit(1)
print("slot_sysex_tool: checks passed (fm1_slot.py push / pull / refusals against slot_sysex.c)")

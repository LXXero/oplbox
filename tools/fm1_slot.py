#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pull or push an OPLBOX song slot (.CMF) over USB-MIDI.

  fm1_slot.py pull SLOT FILE [--port NAME]    slot SLOT (1-9) of the FM-1 -> FILE
  fm1_slot.py push SLOT FILE [--port NAME]    FILE -> slot SLOT

SysEx in the updater's framing (firmware/src/slot_sysex.c, docs/API.md): pull asks
for 512 bytes at a time; push announces the file and the FM-1 reads it with the
updater's read requests, checks it as a .CMF and writes the slot. The FM-1 refuses
the slot whose song is in the player, files bigger than a slot (32 KiB - 16) and
files that are not a .CMF it can play. Finds the FM-1 the way fm1_install.py does.
Needs mido with python-rtmidi.

Exit codes: 0 done, 1 the FM-1 refused, 2 bad arguments or file, 3 FM-1 not
found, 4 no reply (connection lost, or a firmware without slot transfer).
"""
import argparse
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm1_install as fi  # noqa: E402

PULL, PUSH = 0x60, 0x61
SLOTS, SLOT_MAX = 9, 0x8000 - 16
STATUS = {1: "bad request", 2: "the slot is empty", 3: "bigger than a slot (32 KiB - 16)",
          4: "that slot's song is in the player: pick another song on the FM-1 first",
          5: "the transfer failed (no answer, or the file changed on the way)",
          6: "not a .CMF the FM-1 can play", 7: "the flash write failed"}
DELAY = {"reply": 3.0, "idle": 20.0}   # seconds; the tests shorten them


class SlotError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def message(cmd, body):
    m = bytearray([0x00, 0x59, cmd, len(body) & 0xFF, (len(body) >> 8) & 0xFF, len(body) >> 16]) + bytes(body)
    m.append(~sum(body) & 0xFF)
    return b"\xF0" + fi.pack7(m) + b"\xF7"


def parse_reply(pkt, cmd):
    """the body of a reply to cmd, or None"""
    if len(pkt) < 2 or pkt[0] != 0xF0 or pkt[-1] != 0xF7:
        return None
    d = fi.unpack7(pkt[1:-1])
    if len(d) < 7 or d[:3] != bytes([0x00, 0x59, cmd]) or int.from_bytes(d[3:6], "little") + 7 != len(d):
        return None
    if ~sum(d[6:-1]) & 0xFF != d[-1]:
        return None
    return d[6:-1]


def wait_reply(link, cmd, secs, serve=None):
    """the next reply to cmd; read requests meanwhile go to serve(addr, n)"""
    end = time.monotonic() + secs
    while (left := end - time.monotonic()) > 0:
        pkt = link.read(left)
        if pkt is None:
            break
        r = fi.parse_request(pkt)
        if r and serve:
            serve(r[1], r[2])
            end = time.monotonic() + secs        # the FM-1 is busy with us: wait on
            continue
        body = parse_reply(pkt, cmd)
        if body is not None:
            return body
    raise SlotError("noreply", "no reply from the FM-1 (unplugged, or a firmware without slot transfer?)")


def refused(slot, st):
    return SlotError("refused", f"slot {slot + 1}: {STATUS.get(st, f'status {st}')}")


def pull(link, slot):
    data, off, total = bytearray(), 0, None
    while total is None or off < total:
        for _ in range(3):
            if not link.send(message(PULL, bytes([slot]) + off.to_bytes(3, "little"))):
                raise SlotError("noreply", "the MIDI port is gone")
            try:
                b = wait_reply(link, PULL, DELAY["reply"])
            except SlotError:
                continue
            if len(b) >= 8 and int.from_bytes(b[5:8], "little") == off:
                break
        else:
            raise SlotError("noreply", f"slot {slot + 1}: no reply for offset {off}")
        if b[0] != slot or b[1]:
            raise refused(slot, b[1])
        total = int.from_bytes(b[2:5], "little")
        if len(b) == 8 and off < total:
            raise SlotError("noreply", f"slot {slot + 1}: an empty chunk at {off} of {total}")
        data += b[8:]
        off += len(b) - 8
    return bytes(data)


def push(link, slot, data):
    if len(data) > SLOT_MAX:
        raise SlotError("usage", f"{len(data)} bytes: bigger than a slot ({SLOT_MAX})")

    def serve(addr, n):
        if n > fi.MAXDATA or addr + n > len(data):
            raise SlotError("noreply", f"the FM-1 asked for {addr}+{n}, outside the file")
        link.send(fi.response(addr, data[addr:addr + n]))

    crc = zlib.crc32(data) & 0xFFFFFFFF
    if not link.send(message(PUSH, bytes([slot]) + len(data).to_bytes(3, "little") + crc.to_bytes(4, "little"))):
        raise SlotError("noreply", "the MIDI port is gone")
    b = wait_reply(link, PUSH, DELAY["idle"], serve)
    if len(b) < 2 or b[0] != slot or b[1]:
        raise refused(slot, b[1] if len(b) > 1 else -1)


EXIT = {"refused": 1, "usage": 2, "notfound": 3, "noreply": 4}


def run(a, backend, out):
    slot = a.slot - 1
    if a.cmd == "push":
        try:
            data = open(a.file, "rb").read()
        except OSError as e:
            raise SlotError("usage", str(e))
    up = fi.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        raise SlotError("notfound", str(fi.not_found(up)))
    try:
        if a.cmd == "pull":
            data = pull(dev.link, slot)
            with open(a.file, "wb") as f:
                f.write(data)
            print(f"slot {a.slot} -> {a.file} ({len(data)} bytes)", file=out)
        else:
            push(dev.link, slot, data)
            print(f"{a.file} -> slot {a.slot} ({len(data)} bytes)", file=out)
    finally:
        dev.link.close()
    return 0


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("pull", "push"))
    ap.add_argument("slot", type=int, choices=range(1, SLOTS + 1), metavar="SLOT", help="1-9, as the jukebox shows them")
    ap.add_argument("file", help=".CMF file")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    a = ap.parse_args(argv)
    try:
        if backend is None:
            try:
                backend = fi.MidoBackend()
            except ImportError:
                raise SlotError("usage", "needs mido and python-rtmidi (pip install mido python-rtmidi)")
        return run(a, backend, out)
    except SlotError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT.get(e.code, 1)


if __name__ == "__main__":
    sys.exit(main())

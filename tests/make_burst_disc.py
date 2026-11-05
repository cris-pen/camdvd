#!/usr/bin/env python3
"""
Build disc images that mimic how a camcorder writes: each recording lands on
disc in bursts with filler sectors between them.

  burst.img  scene A (30 s) then scene B (20 s). No scene numbers in the
             navigation packs; the stream clock restarts at scene B.
             Expected: 2 scenes, 30 s and 20 s.
  nav.img    scene C (24 s), one continuous encode, where the navigation
             packs switch from VOB 1 to VOB 2 halfway (clock never breaks).
             Then scene D, made of two separate 6 s encodes (clock restarts
             between them) whose navigation packs say VOB 3 throughout with
             cell time carrying on. Expected: 3 scenes, 12 s, 12 s, 12 s.

Usage: make_burst_disc.py OUTDIR FIXTURE_DIR
"""
import os
import subprocess
import sys

S = 2048
out, fx = sys.argv[1], sys.argv[2]
os.makedirs(fx, exist_ok=True)


def encode(name, seconds, freq):
    path = os.path.join(fx, name)
    if not os.path.exists(path):
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=720x480:rate=30000/1001:duration=%d" % seconds,
                        "-f", "lavfi", "-i", "sine=frequency=%d:duration=%d" % (freq, seconds),
                        "-vf", "setfield=tff", "-target", "ntsc-dvd", "-flags", "+ilme+ildct", path], check=True)
    return open(path, "rb").read()


def scr(sec):
    b4, b5, b6, b7, b8 = sec[4], sec[5], sec[6], sec[7], sec[8]
    return (((b4 >> 3) & 7) << 30 | (b4 & 3) << 28 | b5 << 20 |
            ((b6 >> 3) & 0x1F) << 15 | (b6 & 3) << 13 | b7 << 5 | (b8 >> 3))


def to_bcd(v):
    return ((v // 10) << 4) | (v % 10)


def patch_nav(data, vob_for, time_offset=0.0):
    """Write VOB number, cell 1 and cell elapsed time into each nav pack."""
    b = bytearray(data)
    n = len(b) // S
    scr0 = scr(b[0:S])
    for i in range(n):
        sec = b[i * S:(i + 1) * S]
        if sec[14:18] != b"\x00\x00\x01\xbb" or sec[0x400:0x404] != b"\x00\x00\x01\xbf":
            continue
        t = (scr(sec) - scr0) / 90000.0 + time_offset
        dsi = i * S + 0x407
        vob = vob_for(i, n)
        b[dsi + 24] = vob >> 8
        b[dsi + 25] = vob & 0xFF
        b[dsi + 27] = 1
        whole = int(t)
        b[dsi + 28] = to_bcd(whole // 3600)
        b[dsi + 29] = to_bcd(whole // 60 % 60)
        b[dsi + 30] = to_bcd(whole % 60)
        b[dsi + 31] = 0xC0 | to_bcd(int((t - whole) * 30) % 30)
    return bytes(b)


def bursts(data, size=1500, filler=16):
    """Interleave runs of filler sectors the way burst writing leaves them."""
    outb = bytearray()
    for i in range(0, len(data), size * S):
        outb += data[i:i + size * S]
        if i + size * S < len(data):
            outb += b"\0" * (filler * S)
    return bytes(outb)


lead = b"\0" * (2048 * S)   # reserved track, never written

A = encode("sceneA_30s.mpg", 30, 300)
B = encode("sceneB_20s.mpg", 20, 500)
with open(os.path.join(out, "burst.img"), "wb") as f:
    f.write(lead + bursts(A) + b"\0" * (32 * S) + bursts(B))

C = encode("sceneC_24s.mpg", 24, 700)
D1 = encode("sceneD1_6s.mpg", 6, 900)
D2 = encode("sceneD2_6s.mpg", 6, 1100)
C = patch_nav(C, lambda i, n: 1 if i < n // 2 else 2)
D1 = patch_nav(D1, lambda i, n: 3, 0.0)
D2 = patch_nav(D2, lambda i, n: 3, 6.0)
with open(os.path.join(out, "nav.img"), "wb") as f:
    f.write(lead + bursts(C) + bursts(D1) + bursts(D2))
print("ok")

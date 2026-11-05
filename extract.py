#!/usr/bin/env python3
"""
Rebuild the recordings (scenes) on a raw camcorder DVD image.

Works on images from unfinalized DVD-R (Video mode VOB data) and DVD-RAM
(VR mode VRO data). Both store video as 2048-byte MPEG-2 program stream
packs, so this scans the sectors directly instead of relying on a file
system that an unfinalized disc does not have.

Camcorders write the disc in bursts with filler between them, so the video
on disc is cut into many short pieces. Pieces are joined back into one scene
unless the camera's own markers say a new scene began:
  * the scene (VOB) number in the DVD navigation packs changes, or
  * the stream clock restarts or jumps, and nothing says the scene continues.

Usage:
  extract.py IMAGE [--map IMAGE.map] [-o OUTDIR] [--format mpg,prores,mp4] [--join]
"""
import argparse
import os
import shutil
import subprocess
import sys

SECTOR = 2048
CLOCK = 90000  # SCR base ticks per second


# ------------------------------------------------------------------ parsing

def parse_map(path):
    runs = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 4 and p[0] == "run":
                runs.append((int(p[1]), int(p[2]), p[3]))
    return runs


def scr_of(sec):
    """SCR base of an MPEG-2 pack header, or None if not an MPEG-2 pack."""
    if sec[0:4] != b"\x00\x00\x01\xba" or (sec[4] & 0xC4) != 0x44:
        return None
    b4, b5, b6, b7, b8 = sec[4], sec[5], sec[6], sec[7], sec[8]
    return (((b4 >> 3) & 7) << 30 | (b4 & 3) << 28 | b5 << 20 |
            ((b6 >> 3) & 0x1F) << 15 | (b6 & 3) << 13 | b7 << 5 | (b8 >> 3))


def has_video(sec):
    """True if the pack carries an MPEG video PES (stream 0xE0-0xEF)."""
    pos = 14 + (sec[13] & 7)
    while pos + 6 <= SECTOR:
        if sec[pos:pos + 3] != b"\x00\x00\x01":
            return False
        sid = sec[pos + 3]
        if 0xE0 <= sid <= 0xEF:
            return True
        ln = sec[pos + 4] << 8 | sec[pos + 5]
        pos += 6 + ln
    return False


def bcd(b):
    return (b >> 4) * 10 + (b & 0x0F)


def nav_of(sec):
    """DVD-Video navigation pack: (vob_id, cell_id, cell_elapsed_seconds), or None.

    Layout: pack header, system header, PCI packet at 0x26, DSI packet at 0x400.
    DSI general info holds the VOB number, the cell number and the time elapsed
    in the cell. Zeroed fields (some encoders leave them blank) count as unknown.
    """
    if (sec[14:18] != b"\x00\x00\x01\xbb" or sec[0x26:0x2A] != b"\x00\x00\x01\xbf"
            or sec[0x2C] != 0x00 or sec[0x400:0x404] != b"\x00\x00\x01\xbf" or sec[0x406] != 0x01):
        return None
    dsi = sec[0x407:]
    vob = dsi[24] << 8 | dsi[25]
    cell = dsi[27]
    h, m, s, fr = dsi[28], dsi[29], dsi[30], dsi[31]
    eltm = bcd(h) * 3600 + bcd(m) * 60 + bcd(s) + bcd(fr & 0x3F) / 30.0
    if vob == 0:
        return None
    return vob, cell, eltm


# ------------------------------------------------------------------ pieces

class Piece:
    """A stretch of packs written in one go, with no gap or clock break inside."""
    def __init__(self, index, workdir):
        self.index = index
        self.path = os.path.join(workdir, "piece_%05d.mpg" % index)
        self.f = open(self.path, "wb")
        self.first_lba = self.last_lba = None
        self.packs = self.video_packs = self.bad = 0
        self.scr_first = self.scr_last = None
        self.nav_first = self.nav_last = None
        self.gap_before = 0        # non-video sectors between this piece and the previous one
        self.unread_before = 0     # unread sectors in that gap
        self.cut = ""              # why the previous piece ended

    def add(self, lba, sec, scr, nav):
        if self.first_lba is None:
            self.first_lba, self.scr_first = lba, scr
        self.last_lba, self.scr_last = lba, scr
        if nav:
            if self.nav_first is None:
                self.nav_first = nav
            self.nav_last = nav
        self.f.write(sec)
        self.packs += 1

    def close(self):
        self.f.close()


def read_pieces(image, mapfile, workdir, gap):
    total = os.path.getsize(image) // SECTOR
    if mapfile:
        runs = parse_map(mapfile)
        marks = sorted((s, s + c, st == "+") for s, c, st in runs)
    else:
        marks = [(0, total, True)]

    def sectors():
        with open(image, "rb") as f:
            for a, b, good in marks:
                if not good:
                    for lba in range(a, b):
                        yield lba, None
                    continue
                f.seek(a * SECTOR)
                lba = a
                while lba < b:
                    n = min(512, b - lba)
                    chunk = f.read(n * SECTOR)
                    got = len(chunk) // SECTOR
                    for i in range(got):
                        yield lba + i, chunk[i * SECTOR:(i + 1) * SECTOR]
                    if got < n:
                        for l in range(lba + got, b):
                            yield l, None
                        break
                    lba += n

    pieces, cur = [], None
    other_run = unread_run = 0
    stats = {"packs": 0, "other": 0, "unread": 0, "nav": 0}

    for lba, sec in sectors():
        if sec is None:
            stats["unread"] += 1
            unread_run += 1
            continue
        scr = scr_of(sec)
        if scr is None:
            stats["other"] += 1
            other_run += 1
            continue
        stats["packs"] += 1
        nav = nav_of(sec)
        if nav:
            stats["nav"] += 1

        cut = ""
        if cur is None:
            cut = "start"
        elif other_run >= gap:
            cut = "gap"
        elif scr + CLOCK // 2 < cur.scr_last:
            cut = "clock restart"
        else:
            allowance = CLOCK + (lba - cur.last_lba) * CLOCK // 50
            if scr - cur.scr_last > allowance:
                cut = "clock jump"
            elif nav and cur.nav_last and nav[0] != cur.nav_last[0]:
                cut = "scene number"
        if cut:
            if cur:
                cur.close()
            cur = Piece(len(pieces) + 1, workdir)
            cur.cut = cut
            cur.gap_before = other_run
            cur.unread_before = unread_run
            pieces.append(cur)
        else:
            cur.bad += unread_run
        cur.add(lba, sec, scr, nav)
        if has_video(sec):
            cur.video_packs += 1
        other_run = unread_run = 0
    if cur:
        cur.close()
    return pieces, stats


# ------------------------------------------------------------- grouping

def boundary(prev, nxt, mode):
    """Decide whether nxt starts a new scene. Returns (new_scene, reason)."""
    pn, nn = prev.nav_last, nxt.nav_first
    if pn and nn and pn[0] != nn[0]:
        return True, "scene number %d -> %d" % (pn[0], nn[0])
    if mode == "pieces":
        return True, nxt.cut
    if mode == "cells" and pn and nn and pn[1] != nn[1]:
        return True, "cell %d -> %d" % (pn[1], nn[1])

    delta = nxt.scr_first - prev.scr_last
    span = nxt.first_lba - prev.last_lba
    clock_continues = 0 <= delta <= CLOCK + span * CLOCK // 50
    if clock_continues:
        return False, "clock continues"
    # Clock broke. Navigation data may still show the same scene carrying on.
    if pn and nn and pn[0] == nn[0] and 0 <= nn[2] - pn[2] <= 2.0:
        return False, "scene time continues"
    if delta < 0:
        return True, "clock restarted"
    return True, "clock jumped %.0fs" % (delta / CLOCK)


def group(pieces, mode):
    scenes, reasons = [], []
    merged = {}
    for p in pieces:
        if mode != "pieces" and p.video_packs == 0 and p.packs < 16:
            # a stray pack or two between bursts: attach to the scene it sits in
            if scenes:
                scenes[-1].append(p)
            continue
        if not scenes:
            scenes.append([p]); reasons.append("start")
            continue
        new, why = boundary(scenes[-1][-1], p, mode)
        if new:
            scenes.append([p]); reasons.append(why)
        else:
            scenes[-1].append(p)
            merged[why] = merged.get(why, 0) + 1
    return scenes, reasons, merged


# ------------------------------------------------------------- output

def run(cmd):
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stderr


def probe_duration(path):
    rc = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                         "-of", "default=nw=1:nk=1", path], stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        return float(rc.stdout.strip())
    except ValueError:
        return 0.0


_encoders = None
def has_encoder(name):
    global _encoders
    if _encoders is None:
        p = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        _encoders = p.stdout
    return (" " + name + " ") in _encoders


FF = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y"]
FF_IN = ["-fflags", "+genpts+discardcorrupt+igndts", "-err_detect", "ignore_err",
         "-analyzeduration", "100M", "-probesize", "100M"]


def build_master(pieces, out_mpg, workdir, tag):
    """Lossless .mpg for a run of pieces.

    Pieces whose clock carries on are joined byte for byte, which is exact.
    Where the clock restarts, the runs are remuxed separately and joined with
    ffmpeg's concat demuxer, which re-bases the timestamps.
    """
    runs = [[pieces[0]]]
    for p in pieces[1:]:
        prev = runs[-1][-1]
        delta = p.scr_first - prev.scr_last
        if 0 <= delta <= CLOCK + (p.first_lba - prev.last_lba) * CLOCK // 50:
            runs[-1].append(p)
        else:
            runs.append([p])

    parts = []
    for i, r in enumerate(runs):
        raw = os.path.join(workdir, "%s_run%03d.raw" % (tag, i))
        with open(raw, "wb") as o:
            for p in r:
                with open(p.path, "rb") as src:
                    shutil.copyfileobj(src, o, 1 << 20)
        part = os.path.join(workdir, "%s_run%03d.mpg" % (tag, i))
        rc, err = run(FF + FF_IN + ["-i", raw, "-map", "0:v:0", "-map", "0:a?",
                                    "-c", "copy", "-f", "vob", part])
        os.remove(raw)
        if rc != 0:
            return rc, err
        parts.append(part)

    if len(parts) == 1:
        shutil.move(parts[0], out_mpg)
        return 0, ""
    lst = os.path.join(workdir, "%s_list.txt" % tag)
    with open(lst, "w") as f:
        for p in parts:
            f.write("file '%s'\n" % p.replace("'", "'\\''"))
    rc, err = run(FF + ["-f", "concat", "-safe", "0", "-i", lst, "-map", "0:v:0", "-map", "0:a?",
                        "-c", "copy", "-f", "vob", out_mpg])
    for p in parts:
        os.remove(p)
    return rc, err


def transcode(master, outbase, fmt):
    src = FF + FF_IN + ["-i", master, "-map", "0:v:0", "-map", "0:a?"]
    if fmt == "prores":
        out = outbase + ".mov"
        cmd = src + ["-c:v", "prores_ks", "-profile:v", "3", "-vendor", "apl0",
                     "-pix_fmt", "yuv422p10le", "-flags", "+ilme+ildct", "-c:a", "pcm_s16le", out]
    else:
        out = outbase + ".mp4"
        v = (["-c:v", "libx264", "-crf", "18", "-preset", "medium"] if has_encoder("libx264")
             else ["-c:v", "h264_videotoolbox", "-b:v", "8M"])
        cmd = src + ["-vf", "bwdif=mode=send_frame"] + v + [
            "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", out]
    rc, err = run(cmd)
    return out, rc, err


def fmt_time(sec):
    sec = int(round(sec))
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return "%d:%02d:%02d" % (h, m, s) if h else "%d:%02d" % (m, s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--map", help="map file from camdvd rip (default IMAGE.map)")
    ap.add_argument("-o", "--out", help="output folder (default 'scenes' next to the image)")
    ap.add_argument("--format", default="mpg",
                    help="comma list of mpg (lossless), prores (ProRes 422 HQ, interlaced kept), "
                         "mp4 (H.264, deinterlaced). Default mpg")
    ap.add_argument("--split", choices=["scenes", "cells", "pieces"], default="scenes",
                    help="scenes: one file per recording (default). cells: also split where the "
                         "camera starts a new DVD cell. pieces: every burst on its own, for diagnosis")
    ap.add_argument("--join", action="store_true", help="also write the whole disc as one file")
    ap.add_argument("--gap", type=int, default=8,
                    help="non-video sectors that mark a break between bursts (default 8)")
    a = ap.parse_args()

    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        sys.exit("ffmpeg is not installed. Install it with:  brew install ffmpeg")

    image = a.image
    mapfile = a.map or (image + ".map")
    if not os.path.exists(mapfile):
        mapfile = None
    outdir = a.out or os.path.join(os.path.dirname(os.path.abspath(image)), "scenes")
    workdir = os.path.join(outdir, ".work")
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    formats = [f.strip() for f in a.format.split(",") if f.strip()]
    for f in formats:
        if f not in ("mpg", "prores", "mp4"):
            sys.exit("unknown format %r (use mpg, prores, mp4)" % f)

    report = []
    def log(s=""):
        print(s, flush=True)
        report.append(s)

    log("Scanning %s%s" % (image, "" if mapfile else " (no map file, assuming all sectors were read)"))
    pieces, stats = read_pieces(image, mapfile, workdir, a.gap)
    log("Video packs: %d (navigation packs with scene numbers: %d), other sectors: %d, unread: %d"
        % (stats["packs"], stats["nav"], stats["other"], stats["unread"]))
    if not pieces:
        log("No MPEG-2 video found in the image.")
        sys.exit(1)

    cuts = {}
    for p in pieces[1:]:
        cuts[p.cut] = cuts.get(p.cut, 0) + 1
    log("Pieces on disc: %d  (breaks: %s)" % (len(pieces),
        ", ".join("%s %d" % kv for kv in sorted(cuts.items())) or "none"))

    scenes, reasons, merged = group(pieces, a.split)
    kept = [(sc, why) for sc, why in zip(scenes, reasons) if sum(p.video_packs for p in sc)]
    if len(kept) < len(scenes):
        log("Skipped %d piece(s) with no video in them" % (len(scenes) - len(kept)))
    scenes = [sc for sc, _ in kept]
    reasons = [why for _, why in kept]
    if merged:
        log("Joined back together: %s" % ", ".join("%d where %s" % (n, why) for why, n in sorted(merged.items())))
    log()

    failures, rows, masters = 0, [], []
    for i, sc in enumerate(scenes, 1):
        base = os.path.join(outdir, "scene_%03d" % i)
        master = base + ".mpg"
        rc, err = build_master(sc, master, workdir, "s%03d" % i)
        if rc != 0 or not os.path.exists(master):
            failures += 1
            log("scene %d: could not build: %s" % (i, err.strip()[-300:]))
            continue
        masters.append(master)
        outs = [master] if "mpg" in formats else []
        for fmt in formats:
            if fmt == "mpg":
                continue
            out, rc, err = transcode(master, base, fmt)
            if rc != 0 or not os.path.exists(out) or os.path.getsize(out) == 0:
                failures += 1
                log("scene %d: %s conversion failed: %s" % (i, fmt, err.strip()[-300:]))
            else:
                outs.append(out)
        rows.append((i, sc, reasons[i - 1], probe_duration(master), outs))

    if a.join and len(masters) > 1:
        lst = os.path.join(workdir, "all.txt")
        with open(lst, "w") as f:
            for m in masters:
                f.write("file '%s'\n" % m.replace("'", "'\\''"))
        whole = os.path.join(outdir, "whole_disc.mpg")
        rc, err = run(FF + ["-f", "concat", "-safe", "0", "-i", lst, "-map", "0:v:0", "-map", "0:a?",
                            "-c", "copy", "-f", "vob", whole])
        if rc != 0:
            failures += 1
            log("whole disc file failed: %s" % err.strip()[-300:])

    if "mpg" not in formats:
        for m in masters:
            os.remove(m)

    total = sum(r[3] for r in rows)
    log("Scenes: %d, total %s" % (len(rows), fmt_time(total)))
    for i, sc, why, dur, outs in rows:
        bad = sum(p.bad + p.unread_before for p in sc[1:]) + sc[0].bad
        note = "  (%d unreadable sectors inside)" % bad if bad else ""
        nav = ""
        if sc[0].nav_first:
            nav = "  VOB %d" % sc[0].nav_first[0]
        log("  scene %03d  %8s  %4d piece%s  starts LBA %-8d  %s%s%s" % (
            i, fmt_time(dur), len(sc), " " if len(sc) == 1 else "s", sc[0].first_lba,
            "" if i == 1 else "new scene: " + why, nav, note))
        for o in outs:
            log("             %s" % os.path.basename(o))
    if a.join and len(masters) > 1:
        log("  whole_disc.mpg")

    with open(os.path.join(outdir, "scenes.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    shutil.rmtree(workdir, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

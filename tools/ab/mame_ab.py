#!/usr/bin/env python3
"""mame_ab.py -- the core against MAME, frame for frame.

Side A is MAME's own Master System with the FujiNet cartridge graft
(fujinet-firmware pico/sms/emu/apply.sh) and no BIOS; side B is this core's
sms_headless. Both start at VBLANK at power-on, take the same input script
and snapshot the same frames; every pair must be pixel-identical.

    mame_ab.py [--model sms1] [--frames 60,120] [--input script] [--fm]
               [--build build] [--keep DIR] [--ram]
               [--bios NAME:FILE] [--inst FILE] IMAGE [IMAGE...]

IMAGE "config" means the baked CONFIG client (no cartridge image). MAME is
$MAME (default ~/Workspace/mame), run from its own tree (-autoboot_script is
ignored otherwise). FUJINET_TCP points both sides at a closed port, so
neither takes a running fujinet-pc's single BoIP slot.

--bios boots a real BIOS on both sides: NAME is MAME's ROM_SYSTEM_BIOS name
for the model (bios13, alexkidd, jbios21, ...) and FILE the same image for
the core. --inst gives the core the YM2413 patch ROM MAME loads from its
own ym2413.zip (FM comparisons need it). The smsj has no "none" BIOS in
MAME, so it always needs --bios.

--ram also compares the 8K of work RAM after the last frame (the timing
probes in tools/ab/probes record their measurements there) and prints the
first differing offsets.

Exit status 0 when every frame (and the RAM) matches.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAME = os.environ.get("MAME", os.path.expanduser("~/Workspace/mame"))
DEAD_PORT = "127.0.0.1:1"


def config_image():
    """The baked CONFIG client, for MAME's -cart (its slot needs an image)."""
    path = os.path.join(ROOT, "core", "sms", "fuji-generated", "fujiconfigrom.h")
    data = bytearray()
    body = open(path).read().split("{", 1)[1].split("}", 1)[0]
    for tok in body.replace("\n", " ").split(","):
        tok = tok.strip()
        if tok:
            data.append(int(tok, 16))
    return bytes(data)


def side_a(model, image, frames, inp, fm, work, ram=None, bios=None):
    snap = os.path.join(work, "a")
    os.makedirs(snap, exist_ok=True)
    args = ["./mame", model, "-bios", "none", "-skip_gameinfo",
            "-slot", "fujinet", "-cart", image,
            "-snapshot_directory", snap,
            "-nvram_directory", os.path.join(work, "nvram"),
            "-cfg_directory", os.path.join(work, "cfg"),
            "-autoboot_script", os.path.join(HERE, "abshot.lua"),
            "-video", "none", "-sound", "none", "-nothrottle",
            "-seconds_to_run", "600"]
    if bios:
        args[3] = bios
    if model == "sg1000m3":
        args[2:4] = []                      # no BIOS socket at all
        if fm:
            args += ["-sgexp", "fm"]
    env = dict(os.environ, AB_FRAMES=",".join(str(f) for f in frames),
               AB_INPUT=inp or "", AB_RAMDUMP=ram or "", FUJINET_TCP=DEAD_PORT)
    p = subprocess.run(args, cwd=MAME, env=env, capture_output=True, text=True, timeout=900)
    shots = []
    for root, _, files in os.walk(snap):
        shots += [os.path.join(root, f) for f in files if f.endswith(".png")]
    shots.sort()
    if len(shots) != len(frames):
        sys.stderr.write("MAME: %d snapshots for %d frames\n%s\n"
                         % (len(shots), len(frames), (p.stdout + p.stderr)[-2000:]))
    return dict(zip(sorted(frames), shots))


def side_b(build, model, image, frames, inp, fm, work, ram=None, bios=None, inst=None):
    out = os.path.join(work, "b", "f")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    exe = os.path.join(build, "core", "tests", "sms_headless")
    args = [exe, "--model", model, "--frames", str(max(frames)), "--out", out,
            "--every", "1", "--boip", DEAD_PORT]
    if image:
        args += ["--cart", image]
    if inp:
        args += ["--input", inp]
    if fm:
        args += ["--fm"]
    if ram:
        args += ["--dumpram", ram]
    if bios:
        args += ["--bios", bios]
    if inst:
        args += ["--inst", inst]
    subprocess.run(args, check=True, capture_output=True)
    return {f: "%s%05d.ppm" % (out, f) for f in frames}


def compare(a, b):
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return "size %s vs %s" % (ia.size, ib.size)
    diff = ImageChops.difference(ia, ib)
    box = diff.getbbox()
    if box is None:
        return None
    n = sum(1 for px in (diff.get_flattened_data() if hasattr(diff, "get_flattened_data") else diff.getdata()) if px != (0, 0, 0))
    return "%d pixels differ in %s" % (n, box)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="sms1")
    ap.add_argument("--frames", default="60,120")
    ap.add_argument("--input")
    ap.add_argument("--fm", action="store_true")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--keep")
    ap.add_argument("--ram", action="store_true")
    ap.add_argument("--bios")
    ap.add_argument("--inst")
    ap.add_argument("images", nargs="+")
    o = ap.parse_args()
    frames = sorted(int(f) for f in o.frames.split(","))

    bad = 0
    for img in o.images:
        work = tempfile.mkdtemp(prefix="mame_ab.")
        try:
            if img == "config":
                cart = os.path.join(work, "config.sms")
                open(cart, "wb").write(config_image())
                b_cart = None
            else:
                cart = os.path.abspath(img)
                b_cart = cart
            ra = os.path.join(work, "a.ram") if o.ram else None
            rb = os.path.join(work, "b.ram") if o.ram else None
            bname, bfile = (o.bios.split(":", 1) if o.bios else (None, None))
            sa = side_a(o.model, cart, frames, o.input, o.fm, work, ra, bname)
            sb = side_b(o.build, o.model, b_cart, frames, o.input, o.fm, work, rb,
                        bfile and os.path.abspath(bfile), o.inst and os.path.abspath(o.inst))
            if o.ram:
                da, db = open(ra, "rb").read(), open(rb, "rb").read()
                diffs = [i for i in range(min(len(da), len(db))) if da[i] != db[i]]
                if diffs:
                    bad += 1
                    print("%s RAM: %d bytes differ; first at $%04X (MAME %02X, core %02X)"
                          % (os.path.basename(img), len(diffs), 0xc000 + diffs[0],
                             da[diffs[0]], db[diffs[0]]))
                else:
                    print("%s RAM: match" % os.path.basename(img))
            for f in frames:
                if f not in sa:
                    print("%s frame %d: no MAME snapshot" % (img, f))
                    bad += 1
                    continue
                why = compare(sa[f], sb[f])
                if why:
                    bad += 1
                print("%s frame %d: %s" % (os.path.basename(img), f, why or "match"))
            if o.keep:
                dst = os.path.join(o.keep, os.path.basename(img))
                shutil.rmtree(dst, ignore_errors=True)
                shutil.copytree(work, dst)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

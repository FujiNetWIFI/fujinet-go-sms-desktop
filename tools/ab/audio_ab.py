#!/usr/bin/env python3
"""audio_ab.py -- the core's sound against MAME's, for one image.

Side A is MAME (-wavwrite) with the FujiNet cartridge graft, side B the
core's sms_headless (--wav), both from power-on for the same number of
frames with the same image, BIOS and YM2413 patch ROM. The two mixers
resample differently (MAME's stream resampler, the core's box decimator), so
the waveforms are not compared sample for sample. What must agree:

  - the alignment: the best cross-correlation lag of the loudness envelopes
    is within a frame;
  - the loudness envelope (RMS per 10 ms), as a correlation;
  - the pitch: the dominant frequency of every loud 85 ms window, which
    must match to within one FFT bin in most windows.

    audio_ab.py [--model smsj] [--seconds 20] [--bios NAME:FILE]
                [--inst FILE] [--fm] [--build build] [--keep DIR] IMAGE

As mame_ab.py: MAME is $MAME (default ~/Workspace/mame), FUJINET_TCP points
both sides at a closed port. Exit status 0 when every measure passes.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAME = os.environ.get("MAME", os.path.expanduser("~/Workspace/mame"))
DEAD_PORT = "127.0.0.1:1"
RATE = 48000


def read_mame_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, "MAME writes 16-bit PCM"
        n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
        data = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64) / 32768.0
    return data.reshape(-1, ch).mean(axis=1), rate


def read_core_wav(path):
    # IEEE float stereo, the header sms_headless writes
    raw = open(path, "rb").read()
    data = np.frombuffer(raw[44:], dtype="<f4").astype(np.float64)
    return data.reshape(-1, 2).mean(axis=1), RATE


def envelope(x, rate, ms=10):
    n = int(rate * ms / 1000)
    k = len(x) // n
    return np.sqrt((x[:k * n].reshape(k, n) ** 2).mean(axis=1))


def pitches(x, rate, win=4096):
    out = []
    hann = np.hanning(win)
    for i in range(0, len(x) - win, win):
        seg = x[i:i + win]
        if np.sqrt((seg ** 2).mean()) < 1e-3:
            out.append(None)
            continue
        spec = np.abs(np.fft.rfft(seg * hann))
        spec[:3] = 0                       # DC
        out.append(int(np.argmax(spec)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="smsj")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--bios")
    ap.add_argument("--inst")
    ap.add_argument("--fm", action="store_true")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--keep")
    ap.add_argument("image")
    a = ap.parse_args()

    work = a.keep or tempfile.mkdtemp(prefix="audio_ab.")
    os.makedirs(work, exist_ok=True)
    hz = 49.7015 if a.model in ("sms1pal", "smspal") else 59.9227
    frames = int(a.seconds * hz)
    image = os.path.abspath(a.image)

    # side A
    wav_a = os.path.join(work, "mame.wav")
    args = ["./mame", a.model, "-bios", "none", "-skip_gameinfo",
            "-slot", "fujinet", "-cart", image, "-wavwrite", wav_a,
            "-samplerate", str(RATE),
            "-nvram_directory", os.path.join(work, "nvram"),
            "-cfg_directory", os.path.join(work, "cfg"),
            "-video", "none", "-sound", "none", "-nothrottle",
            "-seconds_to_run", str(int(a.seconds + 1))]
    if a.bios:
        args[3] = a.bios.split(":", 1)[0]
    if a.model == "sg1000m3":
        args[2:4] = []
        if a.fm:
            args += ["-sgexp", "fm"]
    env = dict(os.environ, FUJINET_TCP=DEAD_PORT)
    p = subprocess.run(args, cwd=MAME, env=env, capture_output=True, text=True, timeout=900)
    if not os.path.exists(wav_a):
        sys.exit("MAME wrote no wav:\n" + (p.stdout + p.stderr)[-2000:])

    # side B
    wav_b = os.path.join(work, "core.wav")
    exe = os.path.join(a.build, "core", "tests", "sms_headless")
    bargs = [exe, "--model", a.model, "--frames", str(frames), "--every", str(frames + 1),
             "--out", os.path.join(work, "f"), "--boip", DEAD_PORT, "--cart", image,
             "--wav", wav_b]
    if a.bios:
        bargs += ["--bios", a.bios.split(":", 1)[1]]
    if a.inst:
        bargs += ["--inst", a.inst]
    if a.fm:
        bargs += ["--fm"]
    subprocess.run(bargs, check=True, capture_output=True)

    xa, ra = read_mame_wav(wav_a)
    xb, rb = read_core_wav(wav_b)
    assert ra == rb == RATE, (ra, rb)
    n = min(len(xa), len(xb))
    xa, xb = xa[:n], xb[:n]
    ea, eb = envelope(xa, RATE), envelope(xb, RATE)

    # alignment, on the envelopes, within +-0.5 s
    best, lag = -2.0, 0
    za = (ea - ea.mean()) / (ea.std() or 1)
    zb = (eb - eb.mean()) / (eb.std() or 1)
    for l in range(-50, 51):
        if l >= 0:
            c = np.corrcoef(za[l:], zb[:len(zb) - l])[0, 1] if l < len(za) else -1
        else:
            c = np.corrcoef(za[:l], zb[-l:])[0, 1]
        if c > best:
            best, lag = c, l
    print("envelope: correlation %.3f at lag %+d ms (MAME rms %.4f, core rms %.4f)"
          % (best, lag * 10, np.sqrt((xa ** 2).mean()), np.sqrt((xb ** 2).mean())))

    pa, pb = pitches(xa, RATE), pitches(xb, RATE)
    both = [(x, y) for x, y in zip(pa, pb) if x is not None and y is not None]
    agree = sum(1 for x, y in both if abs(x - y) <= 1)
    frac = agree / len(both) if both else 0.0
    print("pitch: %d of %d loud windows agree (%.0f%%)" % (agree, len(both), 100 * frac))

    ok = abs(lag) <= 2 and best >= 0.9 and frac >= 0.8 and len(both) > 0
    print("PASS" if ok else "FAIL")
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

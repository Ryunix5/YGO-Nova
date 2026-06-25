#!/usr/bin/env python3
"""
generate_sfx.py - synthesise the placeholder SFX bank that AudioManager loads.

Everything here is procedural: simple oscillators, envelopes and noise via
the Python standard library only (wave / math / struct / random). No external
samples, no copyrighted audio, no downloads. Output is 44.1 kHz 16-bit PCM
mono WAV files into assets/sfx/, where AudioManager picks them up by name.

Usage:
    python tools/generate_sfx.py [--out assets/sfx]

The script lists every generated file with its duration, peak level and
output path so it's easy to confirm the bank is complete.
"""

import argparse
import math
import os
import random
import struct
import sys
import wave

SR = 44100   # sample rate (Hz)


# ─── DSP primitives ─────────────────────────────────────────────────────────

def silence(seconds):
    return [0.0] * int(seconds * SR)


def sine(freq, seconds, phase=0.0):
    n = int(seconds * SR)
    out = [0.0] * n
    omega = 2.0 * math.pi * freq / SR
    for i in range(n):
        out[i] = math.sin(omega * i + phase)
    return out


def sweep(f0, f1, seconds, shape="lin"):
    """Sine sweep f0 -> f1 over `seconds`. shape: 'lin' or 'exp'."""
    n = int(seconds * SR)
    out = [0.0] * n
    phase = 0.0
    for i in range(n):
        t = i / max(1, n - 1)
        if shape == "exp" and f0 > 0 and f1 > 0:
            f = f0 * ((f1 / f0) ** t)
        else:
            f = f0 + (f1 - f0) * t
        phase += 2.0 * math.pi * f / SR
        out[i] = math.sin(phase)
    return out


def noise(seconds, lowpass=None):
    n = int(seconds * SR)
    out = [random.uniform(-1.0, 1.0) for _ in range(n)]
    if lowpass:                                # simple one-pole IIR LP
        alpha = math.exp(-2.0 * math.pi * lowpass / SR)
        y = 0.0
        for i in range(n):
            y = alpha * y + (1.0 - alpha) * out[i]
            out[i] = y
    return out


def env_ar(buf, attack_s, release_s):
    """Apply an attack-release envelope in place."""
    n = len(buf)
    a = max(1, int(attack_s * SR))
    r = max(1, int(release_s * SR))
    for i in range(n):
        if i < a:
            buf[i] *= i / a
        elif i > n - r:
            buf[i] *= max(0.0, (n - i) / r)
    return buf


def add(buf, other, gain=1.0):
    """Mix `other` into `buf` (in place), padding/truncating to len(buf)."""
    for i in range(min(len(buf), len(other))):
        buf[i] += gain * other[i]
    return buf


def normalize(buf, peak_db=-3.0):
    """Normalise so the peak hits the requested dBFS (default -3 dB)."""
    pk = max(abs(s) for s in buf) if buf else 0.0
    if pk < 1e-9:
        return buf, 0.0
    target = 10 ** (peak_db / 20.0)
    g = target / pk
    return [s * g for s in buf], pk * g


def declick(buf, fade_in=0.005, fade_out=0.018):
    """Short fade in/out so the waveform starts and ends at zero — removes the
    click/pop that otherwise makes a synthesized one-shot sound cheap."""
    n = len(buf)
    if n == 0:
        return buf
    fi = min(int(fade_in * SR), n // 2)
    fo = min(int(fade_out * SR), n // 2)
    out = list(buf)
    for i in range(fi):
        out[i] *= i / fi
    for i in range(fo):
        out[n - 1 - i] *= i / fo
    return out


def write_wav(path, buf):
    buf = declick(buf)             # de-pop every one-shot before writing
    n = len(buf)
    # clamp to [-1, 1) then convert to 16-bit PCM.
    pcm = bytearray(n * 2)
    for i, s in enumerate(buf):
        if s >  1.0: s =  1.0
        if s < -1.0: s = -1.0
        v = int(s * 32767.0)
        struct.pack_into("<h", pcm, i * 2, v)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(pcm))


# ─── Sound definitions ──────────────────────────────────────────────────────
# Each generator returns a list of float samples (mono). The dispatcher
# below normalises to a comfortable peak and writes the WAV.

def sfx_click():
    buf = sine(1100, 0.030)
    add(buf, sine(2200, 0.030), 0.4)
    return env_ar(buf, 0.001, 0.025)

def sfx_hover():
    buf = sine(1500, 0.040)
    return env_ar(buf, 0.002, 0.035)

def sfx_confirm():
    a = sine(660, 0.060)
    a = env_ar(a, 0.001, 0.050)
    b = sine(990, 0.090)
    b = env_ar(b, 0.001, 0.080)
    out = a + [0.0] * int(0.020 * SR) + b
    return out

def sfx_cancel():
    return env_ar(sweep(900, 480, 0.150, "exp"), 0.001, 0.120)

def sfx_error():
    # Two short raspy hits.
    buf = []
    for _ in range(2):
        t = sine(220, 0.080, phase=random.random())
        n = noise(0.080, lowpass=2000)
        mixed = [0.7 * t[i] + 0.3 * n[i] for i in range(len(t))]
        env_ar(mixed, 0.001, 0.070)
        buf += mixed + [0.0] * int(0.030 * SR)
    return buf

def sfx_draw():
    # Quick noise-burst "swipe" with a high resonance.
    buf = noise(0.150, lowpass=4000)
    add(buf, sweep(2500, 700, 0.150, "exp"), 0.5)
    return env_ar(buf, 0.003, 0.130)

def sfx_shuffle():
    out = []
    for k in range(4):
        chunk = noise(0.070, lowpass=3500)
        env_ar(chunk, 0.002, 0.060)
        out += chunk + [0.0] * int(0.025 * SR)
        random.seed(1000 + k * 7)
    return out

def sfx_summon():
    base = sweep(180, 660, 0.350, "exp")
    add(base, sweep(360, 1320, 0.350, "exp"), 0.45)
    add(base, sine(110, 0.350),                0.30)
    return env_ar(base, 0.010, 0.140)

def sfx_special_summon():
    base = sweep(200, 880, 0.450, "exp")
    add(base, sweep(400, 1760, 0.450, "exp"), 0.55)
    add(base, sine(140, 0.450),               0.35)
    add(base, noise(0.450, lowpass=1500),     0.15)
    return env_ar(base, 0.015, 0.180)

def sfx_set():
    # Soft low thump + brief click.
    a = sine(140, 0.120)
    add(a, sine(70, 0.120), 0.6)
    env_ar(a, 0.003, 0.110)
    b = noise(0.020, lowpass=4000)
    env_ar(b, 0.001, 0.018)
    return a + b

def sfx_activate():
    base = sweep(880, 1760, 0.180, "exp")
    add(base, sweep(440, 880,  0.180, "exp"), 0.5)
    return env_ar(base, 0.005, 0.140)

def sfx_chain():
    # Two-tone alert chime.
    a = sine(880, 0.090); env_ar(a, 0.002, 0.080)
    b = sine(1320, 0.110); env_ar(b, 0.002, 0.100)
    return a + [0.0] * int(0.030 * SR) + b

def sfx_send_gy():
    return env_ar(sweep(700, 180, 0.220, "exp"), 0.003, 0.180)

def sfx_banish():
    base = sweep(1200, 4000, 0.150, "exp")
    add(base, noise(0.150, lowpass=4500), 0.40)
    return env_ar(base, 0.001, 0.130)

def sfx_attack():
    base = noise(0.140, lowpass=2800)
    add(base, sweep(400, 100, 0.140, "exp"), 0.6)
    return env_ar(base, 0.003, 0.120)

def sfx_damage():
    base = sine(95, 0.220)
    add(base, sine(48, 0.220), 0.5)
    add(base, noise(0.220, lowpass=1200), 0.5)
    return env_ar(base, 0.003, 0.180)

def sfx_victory():
    # Major triad arpeggio.
    out = []
    for f, dur in [(523.25, 0.140), (659.25, 0.140), (783.99, 0.220),
                   (1046.5, 0.260)]:
        seg = sine(f, dur); add(seg, sine(f * 2, dur), 0.35)
        env_ar(seg, 0.005, dur * 0.7)
        out += seg + [0.0] * int(0.020 * SR)
    return out

def sfx_defeat():
    out = []
    for f, dur in [(523.25, 0.180), (415.30, 0.180), (329.63, 0.260),
                   (261.63, 0.350)]:
        seg = sine(f, dur); add(seg, sine(f * 0.5, dur), 0.5)
        env_ar(seg, 0.005, dur * 0.7)
        out += seg + [0.0] * int(0.025 * SR)
    return out

def sfx_duel_start():
    # Tonic chord + sweep up.
    chord = sine(196, 0.500)
    add(chord, sine(294, 0.500), 0.7)
    add(chord, sine(392, 0.500), 0.7)
    env_ar(chord, 0.020, 0.250)
    sweep_layer = sweep(80, 1500, 0.500, "exp")
    add(chord, sweep_layer, 0.40)
    add(chord, noise(0.500, lowpass=1800), 0.20)
    return chord


SOUNDS = [
    ("click",          sfx_click,          "short soft tick"),
    ("hover",          sfx_hover,          "subtle blip"),
    ("confirm",        sfx_confirm,        "positive chime"),
    ("cancel",         sfx_cancel,         "downward blip"),
    ("error",          sfx_error,          "harsh warning"),
    ("draw",           sfx_draw,           "card swipe"),
    ("shuffle",        sfx_shuffle,        "card flutter"),
    ("summon",         sfx_summon,         "rising pulse"),
    ("special_summon", sfx_special_summon, "stronger magical pulse"),
    ("set",            sfx_set,            "soft thud"),
    ("activate",       sfx_activate,       "bright effect pulse"),
    ("chain",          sfx_chain,          "alert tone"),
    ("send_gy",        sfx_send_gy,        "downward whoosh"),
    ("banish",         sfx_banish,         "phase out"),
    ("attack",         sfx_attack,         "slash impact"),
    ("damage",         sfx_damage,         "hit + low pulse"),
    ("victory",        sfx_victory,        "triumphant chime"),
    ("defeat",         sfx_defeat,         "descending tone"),
    ("duel_start",     sfx_duel_start,     "dramatic start sting"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="assets/sfx",
                    help="output folder (default: assets/sfx)")
    ap.add_argument("--seed", type=int, default=1337,
                    help="RNG seed for reproducible noise (default: 1337)")
    args = ap.parse_args()

    random.seed(args.seed)
    os.makedirs(args.out, exist_ok=True)
    print(f"output:  {os.path.abspath(args.out)}")
    print(f"format:  44100 Hz / 16-bit PCM / mono")
    print(f"sounds:  {len(SOUNDS)}")
    print()

    print(f"{'name':<16} {'dur':>6}  {'peak':>6}   description")
    print("-" * 64)
    written = 0
    for name, gen, descr in SOUNDS:
        buf = gen()
        buf, pk = normalize(buf, peak_db=-3.0)
        path = os.path.join(args.out, f"{name}.wav")
        try:
            write_wav(path, buf)
            written += 1
        except Exception as e:
            print(f"  [skip] {name}: {e}", file=sys.stderr)
            continue
        dur = len(buf) / SR
        print(f"{name:<16} {dur:>5.2f}s  {pk:>6.3f}   {descr}")
    print()
    print(f"wrote {written}/{len(SOUNDS)} files into {args.out}")
    if written == len(SOUNDS):
        print("ok — AudioManager will load these on the next app launch.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

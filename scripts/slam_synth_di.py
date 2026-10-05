"""Synth SLAM-study DI: sparse hits + chugs + sustains, stdlib only.

Writes build/slam_di.wav (48 kHz float32 mono) and build/slam_di_map.txt
(section/event table consumed by tdm_slam_study). Deterministic (seeded RNG).

Karplus-Strong plucks (labpitch precedent): real pick attack, harmonic decay,
palm-mute damping variants. Sections:

  S1 sparse mutes : 6x E2 palm mutes, 1.2 s apart (t=0.5..6.5)
  S2 breakdown    : 8x B1-root power stabs, 0.85 s apart (t=9..15)
  S3 med chugs    : E2 8ths @130 bpm, 4 s (t=16..20)
  S4 fast chugs   : E2 16ths @150 bpm, 4 s (t=21..25)
  S5 sustains     : E2 2.2 s @26, B1 2.5 s @28.5
  S6 power chords : E5/G5/A5 @32/33/34
  S7 dense chord  : Em strum @36 + short arp
  S8 high riff    : E3/G3/A3 8ths @37.5, 3 s

Map format (seconds, one entry per line):
  section <name> <start> <end>
  event <name> <time>
"""

import math
import random
import struct

SR = 48000


def ks_note(freq, dur, damp=0.996, brightness=0.5, seed=1, pick=0.5, choke=None):
    """One KS pluck. choke: if set, force-exponential mute after choke sec
    (palm-mute staccato: damp harder once the pick attack has passed)."""
    period = max(2, int(SR / freq + 0.5))
    rng = random.Random(seed)
    line = [rng.random() * 2 - 1 for _ in range(period)]
    smooth = [0.5 * (line[i] + line[i - 1]) for i in range(period)]
    line = [(1 - brightness) * s + brightness * n
            for s, n in zip(smooth, line)]
    n = int(SR * dur)
    out = [0.0] * n
    idx = 0
    prev = 0.0
    choke_n = int(SR * choke) if choke else n + 1
    for i in range(n):
        cur = line[idx]
        d = damp if i < choke_n else damp * 0.90
        v = d * 0.5 * (cur + prev)
        prev = cur
        line[idx] = v
        idx = (idx + 1) % period
        out[i] = cur
    cn = int(SR * 0.0012)
    for i in range(min(cn, n)):
        w = 1 - i / cn
        out[i] += (rng.random() * 2 - 1) * pick * w * 0.5
        if i > 0:
            out[i] = 0.5 * (out[i] + out[i - 1])
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak for v in out]


def power(freq, dur, seed, damp=0.9965, brightness=0.55, pick=0.6, choke=None):
    """Root+fifth+octave struck together."""
    a = ks_note(freq, dur, damp, brightness, seed, pick, choke)
    b = ks_note(freq * 1.5, dur, damp, brightness, seed + 101, pick * 0.8, choke)
    c = ks_note(freq * 2.0, dur, damp, brightness, seed + 202, pick * 0.6, choke)
    return [(x + 0.8 * y + 0.6 * z) / 2.4 for x, y, z in zip(a, b, c)]


def main():
    total = SR * 42
    di = [0.0] * total
    sections = []
    events = []

    def place(note, at, gain=0.5):
        s = int(SR * at)
        for i, v in enumerate(note):
            if s + i < total:
                di[s + i] += v * gain

    def section(name, start, end):
        sections.append((name, start, end))

    def event(name, t):
        events.append((name, t))

    # S1: sparse isolated palm mutes (E2), 1.2 s apart.
    section("sparse", 0.0, 8.0)
    for k in range(6):
        t = 0.5 + 1.2 * k
        n = ks_note(82.41, 1.1, damp=0.994, brightness=0.35,
                    seed=1000 + k, pick=0.7, choke=0.28)
        place(n, t, 0.55)
        event("sparse", t)

    # S2: breakdown stabs (B1 power), 0.85 s apart.
    section("breakdown", 8.6, 16.0)
    for k in range(8):
        t = 9.0 + 0.85 * k
        n = power(61.74, 0.8, seed=2000 + k, damp=0.995,
                  brightness=0.45, pick=0.8, choke=0.4)
        place(n, t, 0.6)
        event("breakdown", t)

    # S3: medium chugs, E2 8ths @130.
    section("medchug", 16.0, 20.5)
    step = 60.0 / 130.0 / 2.0
    k = 0
    t = 16.2
    while t < 20.0:
        n = ks_note(82.41, step * 0.95, damp=0.993, brightness=0.35,
                    seed=3000 + k, pick=0.7, choke=step * 0.7)
        place(n, t, 0.55)
        event("medchug", t)
        t += step
        k += 1

    # S4: fast chugs, E2 16ths @150.
    section("fastchug", 20.5, 25.5)
    step = 60.0 / 150.0 / 4.0
    k = 0
    t = 20.7
    while t < 25.0:
        n = ks_note(82.41, step * 0.95, damp=0.992, brightness=0.38,
                    seed=4000 + k, pick=0.65, choke=step * 0.7)
        place(n, t, 0.55)
        event("fastchug", t)
        t += step
        k += 1

    # S5: sustains.
    section("sustain", 25.5, 31.5)
    place(ks_note(82.41, 2.2, seed=5001, pick=0.5), 26.0, 0.5)
    event("sustain", 26.0)
    place(ks_note(61.74, 2.5, seed=5002, pick=0.5), 28.5, 0.5)
    event("sustain", 28.5)

    # S6: power chords E5/G5/A5.
    section("powerchords", 31.5, 35.5)
    for k, f in enumerate([82.41, 98.0, 110.0]):
        t = 32.0 + k
        place(power(f, 0.9, seed=6000 + k), t, 0.6)
        event("powerchords", t)

    # S7: dense Em strum + arp.
    section("dense", 35.5, 37.5)
    em = [82.41, 123.47, 164.81, 196.0, 246.94, 329.63]
    for k, f in enumerate(em):
        place(ks_note(f, 1.6, seed=7000 + k, brightness=0.6, pick=0.4),
              36.0 + 0.018 * k, 0.4)
    event("dense", 36.0)

    # S8: higher riff, 8ths.
    section("highriff", 37.5, 41.5)
    riff = [164.81, 196.0, 164.81, 220.0, 196.0, 164.81, 146.83, 164.81]
    step = 60.0 / 132.0 / 2.0
    for rep in range(2):
        for k, f in enumerate(riff):
            t = 37.7 + rep * len(riff) * step + k * step
            if t > 41.0:
                break
            place(ks_note(f, step * 0.95, seed=8000 + rep * 100 + k,
                          brightness=0.6, pick=0.5), t, 0.45)
            event("highriff", t)

    with open("build/slam_di.wav", "wb") as f:
        n = len(di)
        f.write(b"RIFF" + struct.pack("<I", 36 + n * 4) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 3, 1, SR, SR * 4, 4, 32))
        f.write(b"data" + struct.pack("<I", n * 4))
        f.write(struct.pack("<%df" % n, *di))
    with open("build/slam_di_map.txt", "w") as f:
        for name, a, b in sections:
            f.write("section %s %.4f %.4f\n" % (name, a, b))
        for name, t in events:
            f.write("event %s %.4f\n" % (name, t))
    print("wrote build/slam_di.wav + build/slam_di_map.txt")


main()

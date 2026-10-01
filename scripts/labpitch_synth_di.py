"""Synth a Karplus-Strong guitar-DI audition file with stdlib only.

Writes build/labpitch_di.wav: 48 kHz float32 mono, ~14 s. Sections:
  1. low E single notes (sustained, decaying)
  2. palm-muted chugs (damped, staccato 8ths)
  3. power chords (E5/G5/A5, struck together)
  4. full E-minor chord (strummed) + arpeggio
  5. fast alternate-picked 16th riff (E/F/G on low E string)
  6. sustained low B power chord (7-string proxy)

Karplus-Strong gives real pluck physics: pick-noise excitation, harmonic
decay, palm-mute damping variants - a far better pitch-shifter probe
than sines. Deterministic (seeded RNG).
"""
import math
import random
import struct

SR = 48000


def ks_note(freq, dur, damp=0.996, brightness=0.5, seed=1, pick=0.5):
    """One KS pluck. damp: loop decay (lower = deader). brightness: blend
    of raw noise burst vs lowpassed excitation. pick: extra click gain."""
    period = max(2, int(SR / freq + 0.5))
    rng = random.Random(seed)
    line = [rng.random() * 2 - 1 for _ in range(period)]
    smooth = [0.0] * period
    for i in range(period):
        smooth[i] = 0.5 * (line[i] + line[i - 1])
    line = [(1 - brightness) * s + brightness * n for s, n in zip(smooth, line)]
    n = int(SR * dur)
    out = [0.0] * n
    idx = 0
    prev = 0.0
    for i in range(n):
        cur = line[idx]
        v = damp * 0.5 * (cur + prev)
        prev = cur
        line[idx] = v
        idx = (idx + 1) % period
        out[i] = cur
    # Pick click: short noise burst, lowpassed, ~1 ms.
    cn = int(SR * 0.0012)
    for i in range(min(cn, n)):
        w = 1 - i / cn
        out[i] += (rng.random() * 2 - 1) * pick * w * 0.5
        if i > 0:
            out[i] = 0.5 * (out[i] + out[i - 1])
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak for v in out]


def main():
    total = SR * 17
    di = [0.0] * total

    def place(note, at, gain=0.5):
        s = int(SR * at)
        for i, v in enumerate(note):
            if s + i < total:
                di[s + i] += gain * v

    # 1. Low E single notes (E2), open sustain, 1.1 s apart.
    for k, t in enumerate([0.3, 1.4, 2.5]):
        place(ks_note(82.41, 1.2, seed=101 + k), t, 0.55)

    # 2. Palm-muted chugs: E2 8ths @ 140 bpm, damped + dark.
    step = 60.0 / 140.0 / 2.0
    for k in range(8):
        place(ks_note(82.41, 0.30, damp=0.94, brightness=0.25, seed=201 + k, pick=0.7),
              4.0 + k * step, 0.6)

    # 3. Power chords: E5, G5, A5 (root+fifth+octave struck together).
    for k, (t, root) in enumerate([(6.2, 82.41), (7.2, 98.0), (8.2, 110.0)]):
        dur = 0.9
        a = ks_note(root, dur, seed=301 + 3 * k)
        b = ks_note(root * 1.4983, dur, seed=302 + 3 * k)
        c = ks_note(root * 2.0, dur, seed=303 + 3 * k)
        chord = [(x + 0.8 * y + 0.6 * z) / 2.4 for x, y, z in zip(a, b, c)]
        place(chord, t, 0.62)

    # 4. Open E-minor strum (E2 B2 E3 G3 B3 E4, ~7 ms stagger) + arpeggio.
    em = [82.41, 123.47, 164.81, 196.0, 246.94, 329.63]
    for k, f in enumerate(em):
        place(ks_note(f, 1.6, seed=401 + k), 9.4 + k * 0.007, 0.34)
    for k, f in enumerate(em):
        place(ks_note(f, 0.5, seed=501 + k), 11.3 + k * 0.11, 0.4)

    # 5. Fast 16th riff @ 150 bpm: E2 E2 F2 E2 G2 E2 F2 E2 ...
    riff = [82.41, 82.41, 87.31, 82.41, 98.0, 82.41, 87.31, 82.41,
            82.41, 87.31, 98.0, 103.83, 98.0, 87.31, 82.41, 82.41]
    step16 = 60.0 / 150.0 / 4.0
    for k, f in enumerate(riff):
        place(ks_note(f, 0.22, damp=0.985, brightness=0.6, seed=601 + k, pick=0.8),
              12.4 + k * step16, 0.55)

    # 6. Sustained low-B power chord (7-string proxy: B1+F#2+B2).
    for k, f in enumerate([61.74, 92.50, 123.47]):
        place(ks_note(f, 2.2, seed=701 + k), 14.4, 0.34)

    # Gentle normalize + fade the last 50 ms (clean file end).
    peak = max(abs(v) for v in di) or 1.0
    g = 0.89 / peak
    fade = int(SR * 0.05)
    for i in range(total):
        v = di[i] * g
        if i > total - fade:
            v *= (total - i) / fade
        di[i] = v

    data = struct.pack("<%df" % len(di), *di)
    with open("build/labpitch_di.wav", "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 3, 1, SR, SR * 4, 4, 32))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)
    print("wrote build/labpitch_di.wav (%.1f s)" % (total / SR))


if __name__ == "__main__":
    main()

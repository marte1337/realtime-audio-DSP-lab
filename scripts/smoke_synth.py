"""Synth deterministic offline-smoke assets with stdlib only (no binaries in git).

Writes build/smoke_di.wav (plucky DI-ish phrases + silence) and
build/smoke_ir.wav (decaying noise-burst cab-ish IR), both 48 kHz float32
with correct format headers (python's wave module cannot write float32).
"""
import math
import random
import struct

SR = 48000


def write_float32_mono(path, samples):
    data = struct.pack("<%df" % len(samples), *samples)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 3, 1, SR, SR * 4, 4, 32))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


def main():
    di = [0.0] * (SR * 2)
    # Plucks at E2/A2-ish fundamentals with a pick transient, every 0.28 s.
    for n in range(6):
        start = int(SR * (0.25 + 0.28 * n))
        for i in range(int(SR * 0.26)):
            t = i / SR
            env = math.exp(-9.0 * t)
            v = (math.sin(2 * math.pi * 82.41 * t) * 0.55
                 + math.sin(2 * math.pi * 123.47 * t) * 0.3
                 + math.sin(2 * math.pi * 1244.0 * t) * 0.12) * env
            if i < 24:  # pick click
                v += (random.Random(n * 100 + i).random() - 0.5) * 0.8 * (1 - i / 24)
            if start + i < len(di):
                di[start + i] += 0.5 * v
    write_float32_mono("build/smoke_di.wav", di)

    rng = random.Random(7)
    ir = [0.0] * int(SR * 0.04)  # cab-like: 40 ms, fast early decay
    for i in range(len(ir)):
        t = i / SR
        ir[i] = (rng.random() * 2 - 1) * math.exp(-60.0 * t)
    ir[0] += 1.0  # direct arrival dominates, like a real speaker impulse
    peak = max(abs(v) for v in ir) or 1.0
    ir = [v / peak * 0.9 for v in ir]
    write_float32_mono("build/smoke_ir.wav", ir)
    print("wrote build/smoke_di.wav + build/smoke_ir.wav")


if __name__ == "__main__":
    main()

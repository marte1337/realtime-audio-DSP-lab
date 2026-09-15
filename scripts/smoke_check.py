"""Verify an offline-smoke render: finite, bounded, and non-silent."""
import math
import struct
import sys

with open(sys.argv[1], "rb") as f:
    blob = f.read()
assert blob[0:4] == b"RIFF" and blob[8:12] == b"WAVE", "not a WAV"
# Minimal parse: assume canonical 44-byte header written by tdm_render.
assert blob[12:16] == b"fmt " and struct.unpack("<H", blob[20:22])[0] == 3, "expect float32"
nch = struct.unpack("<H", blob[22:24])[0]
n = (len(blob) - 44) // (4 * nch)
vals = struct.unpack("<%df" % (n * nch), blob[44:44 + n * nch * 4])
assert all(math.isfinite(v) for v in vals), "non-finite output!"
peak = max(abs(v) for v in vals)
print("frames=%d ch=%d peak=%.4f" % (n, nch, peak))
# Raw M0 path has no output trim (v0.1 scope): a cranked test model can peak
# far above 0 dBFS. This bound only catches explosions, not hot amps.
assert peak < 100.0, "output implausibly hot"
assert peak > 0.01, "output silent: NAM+IR did nothing?"
print("smoke OK")

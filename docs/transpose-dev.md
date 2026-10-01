# Transpose DEV environment (GT2 vs TONE3000 reference)

DEV-only A/B integration. Not production, not final UI. The accepted
`GuitarTransposeV2` baseline (hardware-auditioned vs TONE3000 t3k30 at
48 kHz / 128 frames, found almost indistinguishable) is frozen; the
TONE3000 engine is a frozen external oracle.

## Architecture

```
input -> InputTrim -> Gate -> DEV Transpose -> TightDrive -> NAM -> IR
      -> ToneShape -> Space -> OutputTrim -> output
                             |
                +------------+------------+
                |                         |
             OUR GT2              TONE3000 reference
          (30 ms baseline)         (30 ms, Tonality OFF)
```

Exactly one engine feeds the rig at a time. The selector never touches
unrelated rig state; each engine keeps its own configuration; downstream
(TightDrive onward) never changes on a switch.

Files:

- `dsp/TransposeInsert.h` — dependency-free rig seam (null in production).
- `dsp/TechDeathRig.h/.cpp` — seam calls only (bit-exact when null).
- `dsp/lab/Pitch/DevTranspose.h/.cpp` — DEV stage (both engines, live
  selector, latency-matched bypass, GT2 DEV config, T3K live controls).
- `app/TdmTransposeDev.cpp` — `tdm_transpose_dev` live host (CLI + stdin).
- `tests/TestDevTranspose.cpp` — baseline protection regression suite.
- `dsp/lab/Pitch/GuitarTransposeStudy.cpp` — sweepable harness
  (`--shifts`, `--cells`, NCC min/max, tap range, fade occupancy).

Isolation: `TechDeathRig` includes only the seam header (no JUCE, no
TONE3000, no bench, no lab). `DevTranspose.cpp` compiles twice — plain
(GT2-only) for `tdm_tests`, and with `-DTDM_HAVE_TONE3000` (+ JUCE /
TONE3000 includes) only for `tdm_transpose_dev`. `tdm_live`,
`tdm_render`, `tdm_dev` link no TONE3000/JUCE/bench/GT2/DevTranspose
symbols (verified with `nm`; see the task report).

## Controls

Shared (live, both engines follow):

- `shift` — semitones, -12.0 … +12.0, float internally (integers normal).
- `on` / `off` — enabled / latency-matched bypass (LabWsolaLive pattern).
- engine — `ours` (GT2) / `ref` (TONE3000), crossfaded, settings kept.

GT2 DEV (start-time CLI only — GT2 derives geometry at reset, so changes
need an audio restart; validated as a whole, rejected loudly):

`--gt2-window` 10–120 ms · `--gt2-floor` 0.5–10 · `--gt2-corr` 5–60 ·
`--gt2-fademin` 4–120 · `--gt2-fademax` fademin–250 ·
`--gt2-fadencc-hi` (lo,1] · `--gt2-fadencc-lo` [0,hi) ·
`--gt2-onsetfade` 0.5–10 · `--gt2-onsetspan` 1–12 · `--gt2-lead` 1–16 ·
`--gt2-refr` 5–200 · `--gt2-hpf` 100–4000 Hz · `--gt2-smooth` 0.5–10 ·
`--gt2-overmin` 3–24 dB · `--gt2-overmax` 1–18 dB ·
`--gt2-cells` 10–200 · `--gt2-skip` 1–20 · `--gt2-resync` on|off

Known-good baseline = GT2 defaults (30 / 2 / 25 / 30 / 120 / 0.95 /
0.60 / 2 / 4 / 4 / 40 / 600 / 2 / 9 / 6 / 50 / 5 / on). Omitting all
`--gt2-*` flags restores it; the host prints a warning whenever a
custom config is active. `devtranspose` unit tests pin every field.

TONE3000 reference (live, frozen algorithm, documented params only):

- `--t3k-window` / `t3k-window` — 20|30|40|60 (default 30, auditioned).
- `--t3k-tonality` / `t3k-tonality` — off|1000–20000 Hz (default off).
- shift/bypass shared (see above).

Caveats:

- GT2 reset at exact `0.0` st is a zero-latency wire until restart; live
  shifts away from a 0-start need a restart on the GT2 side (the host
  warns; TONE3000 follows through 0 at full latency). Use `off` for dry.
- `--no-standby` renders only the active engine (production-like CPU,
  cold standby after a switch). Default warms both for instant A/B.

## Live A/B workflow

Build once: `make transpose-dev` (needs the TONE3000/JUCE checkouts for
`check-t3k-deps`; not part of `all`).

Start (example rig; substitute your NAM/IR):

```
./build/tdm_transpose_dev --nam <amp.nam> --ir <cab.wav> --buffer 128 \
  --tight-drive --transpose gt2:-7
```

Then compare without rebuilding (stdin, Enter after each):

```
ref      # TONE3000 at the same shift, same downstream
ours     # back to GT2
shift -2 # both engines follow live
off      # latency-matched bypass (dry at the same feel)
on       # back to wet
status   # engine / shift / T3K / latencies
q        # quit
```

Reference commands (A–F), each with the same downstream:

```
A. ./build/tdm_transpose_dev --nam <amp.nam> --ir <cab.wav> --buffer 128 --tight-drive --transpose gt2:-2
B. ./build/tdm_transpose_dev --nam <amp.nam> --ir <cab.wav> --buffer 128 --tight-drive --transpose t3k:-2
C. ... --transpose gt2:-7
D. ... --transpose t3k:-7
E. ... --transpose gt2:-12
F. ... --transpose t3k:-12
```

Or start once and drive everything live: `ours`/`ref` switch engines,
`shift X` moves both, `t3k-window`/`t3k-tonality` retune the reference,
`off`/`on` bypasses. Restart with different `--transpose`/`--gt2-*` to
change GT2 advanced config (start-time only).

## Offline characterization

```
make build/tdm_gt2_study
./build/tdm_gt2_study --shifts -1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11,-12 --cells gt2
./build/tdm_gt2_study --shifts -2,-7,-9,-12 --cells t3k30
./build/tdm_gt2_study --shifts 1,2,4,7,12 --cells gt2
```

Default (no args) reproduces the promotion study. WAVs land in
`build/labpitch/gt2_<id>/shift_<st>_*`. See the task report for the
-1…-12 tables and findings (first meaningful degradation around -5/-6;
-12 low-string pitch loss is GT2-specific; all other deep-shift
degradation is shared with the reference).

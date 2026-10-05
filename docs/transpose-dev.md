# Transpose: production engine + DEV A/B environment

## Production promotion

OUR accepted transpose baseline is now the production engine
(`dsp/Pitch/GuitarTranspose.h`, class `tdm::GuitarTranspose`), promoted
verbatim from the hardware-auditioned lab baseline (found almost
indistinguishable from TONE3000 t3k30 at 48 kHz / 128 frames). The
promotion is sample-equivalent by construction and pinned by a golden
regression (`guitartranspose` suite: note/chord/riff at -1/-2/-7,
bit-exact hashes).

Production chain (real product path):

```
input -> InputTrim -> Gate -> GuitarTranspose -> TightDrive -> NAM -> IR
      -> ToneShape -> Space -> OutputTrim -> output
```

Production controls (`RigParams`, live-safe): `transposeEnabled` and
`transposeSemitones` (-12.0 … +12.0, float internally, integers are the
normal use). The engine runs at its default baseline config (30 ms
window, 2 ms floor, 25 ms correlation, 30–120 ms fades, re-sync on);
advanced Config fields are NOT exposed to production. Nominal reported
latency is 768 samples @ 48 kHz (16 ms); the disengaged tap is an exact
wire, the engaged path runs wet (nonzero shift) or latency-matched dry
(shift 0) with 128-sample ramps. `tests/TestTranspose.cpp` covers the
production controls, bypass, reset/determinism, chain order, and
downstream behavior.

Validated primary use: fixed detune **-1 / -2**. The -12 … +12 range is
available but NOT equally validated: the deep-shift (-5 and below) and
positive-shift (+4 and up) research notes below remain backlog.

## DEV A/B environment (ours vs TONE3000 reference)

DEV-only A/B integration. Not final UI. The TONE3000 engine is a frozen
external oracle and stays DEV-only: it never enters production builds.

## Architecture

```
input -> InputTrim -> Gate -> DEV Transpose -> TightDrive -> NAM -> IR
      -> ToneShape -> Space -> OutputTrim -> output
                             |
                +------------+------------+
                |                         |
      OUR production GT        TONE3000 reference
     (rig engine + DEV A/B)     (30 ms, Tonality OFF)
```

The DEV stage substitutes for the production transpose at the same rig
position (via the seam, off-RT install). Its "ours" side IS the
production engine class — DEV adds only the A/B wrapper, the reference
engine, and off-RT advanced Config overrides. Exactly one engine feeds
the rig at a time. The selector never touches unrelated rig state; each
engine keeps its own configuration; downstream (TightDrive onward) never
changes on a switch.

Files:

- `dsp/Pitch/GuitarTranspose.h/.cpp` — OUR production engine (baseline).
- `dsp/TransposeInsert.h` — dependency-free rig seam (DEV substitute when
  installed, production transpose otherwise).
- `dsp/TechDeathRig.h/.cpp` — production transpose + seam substitution.
- `dsp/lab/Pitch/DevTranspose.h/.cpp` — DEV stage (both engines, live
  selector, latency-matched bypass, GT2 DEV config, T3K live controls).
- `app/TdmTransposeDev.cpp` — `tdm_transpose_dev` live host (CLI + stdin).
- `tests/TestDevTranspose.cpp` — baseline protection regression suite.
- `dsp/lab/Pitch/GuitarTransposeStudy.cpp` — sweepable harness
  (`--shifts`, `--cells`, NCC min/max, tap range, fade occupancy).

Isolation: `DevTranspose.cpp` compiles twice — plain (ours-only, no
JUCE) for `tdm_tests`, and with `-DTDM_HAVE_TONE3000` (+ JUCE /
TONE3000 includes) only for `tdm_transpose_dev` and `tdm_dev`.
Production targets (`tdm_live`, `tdm_render`) link the production
engine (our own code) but no TONE3000/JUCE/DevTranspose/bench symbols
(verified with `nm`; see the task report).

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

- DEV-stage GT2 reset at exact `0.0` st is a zero-latency wire until
  restart; live shifts away from a 0-start need a restart on the GT2
  side (the host warns; TONE3000 follows through 0 at full latency).
  Use `off` for dry. (The production rig path avoids this caveat: it
  primes the engine at -2 st when the requested shift is 0, inaudibly,
  so live 0 → N shifts work without a restart.)
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

## AppKit developer UI

The same stage is hosted in the existing developer app (`tdm_dev`,
`host/dev/TdmDevApp.mm` — no second app): transpose section between Gate
and TightDrive with enable checkbox, Our GT2 / T3K Ref segmented control,
one-click A/B button, shift slider + integer stepper, and a compact status
line. GT2 Advanced and TONE3000 Reference are in-place disclosures; the
17 GT2 rows are built from the shared adapter table
(`dsp/lab/Pitch/DevTransposeUi.h`), validate eagerly, and apply on the
next Start (Stop → Start, like NAM/IR). See `docs/dev-control-app.md`.

Known research note (not fixed here): the offline −12 GT2 low-B
discrepancy vs TONE3000 is backlog — hardware A/B found the engines
subjectively almost indistinguishable on tested guitar material even at
−12. Positive-shift architecture limits (+4 and up) are likewise backlog.

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

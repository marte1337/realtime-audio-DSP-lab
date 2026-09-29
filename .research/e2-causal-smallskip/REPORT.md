# E2 causal / small-skip WSOLA experiment — report

Date: 2026-09-29. Lab-only fork, no TechDeathRig integration, no commit.
Design input: `.research/pitch-architecture-study/REPORT.md` sections 8
(lessons 1-2) and 11 (E2); no external implementation code opened while
writing. Accepted W20 (`LabWsolaShift`) untouched, available as reference
(bit-identity proof in tests).

Goal: test whether a causal, small-skip-biased WSOLA materially reduces
audible polyphonic rephasing while lowering latency. Bounded experiment:
one fork, five cells, existing deterministic stress set.

## Cells (W20 fixed; `LabWsolaV2` = line-for-line fork + tie-break modes)

- **A** = Drift + symmetric (480,480): accepted W20 reference (proven
  bit-identical to `LabWsolaShift`, full-DI and unit level).
- **B** = Drift + causal (960,0): Q1 (latency 963 = 20.06 ms @-1).
- **C** = SmallSkip + symmetric: Q2 isolation.
- **D** = SmallSkip + causal: the E2 candidate.
- **E** = Drift + (840,120): Q1 boundary probe, added after traces showed
  B's failure mechanism (latency 1083 = 22.56 ms @-1). E is Drift-only, so
  it runs on the ACCEPTED class (`setSearch(840,120)` admits it) — study
  data transfers bit-exactly (proof test).

SmallSkip rule (the whole Q2 A/B): pegged non-transient frames land by
minimal time-map jump among valid lags (score within `skipBand_`, default
0.05, of the frame max; d != dPrev anti-stick). Drift runs, transient
outright-max + floor, HP detector, NCC, resampler: shared untouched.
Deferred by design: candidate-energy score (not in Q1/Q2; keeps NCC
shared for attribution).

## Q1: did causal processing work without regressions? NO at Dp=0.

B holds clean sustains (sines/KS pitch exact, lowBen 1.0, voicing AM ~A)
but regresses real DI material:

- @-1: chord wantMin 0.875->0.731 (min moves to E5 ROOT), leakMax
  0.216->0.392 (~2x dry-through), riff min-onset 1.000->0.681; E5 root
  per-fund 0.972/0.99 -> 0.731/0.41, octave 1.017/0.98 -> 0.794/0.33.
- @-2: wantMin 0.821->0.607, chug min 1.000->0.844, riff 0.661 (five
  onsets at 0.66-0.80, systematic).
- @-7 (secondary): wantMin 0.476->0.098 (G5 fifth eliminated), chugs
  0.642, riff 0.555.

Mechanism (trace-proven): 86% of DI frames flag transient (decaying KS +
dense onsets vs trailing average — "transient" is really
"non-stationary"). Transient landings take the outright max; the valid
(period-peak) landing sits at d~+70..+100, which B's transient floor
[-(Lov-1), 0] EXCLUDES. B's transient landings score 0.26-0.35 (vs A's
0.74-0.81) at pitch-wrong jumps (J=20-137, phase steps ~100 deg on the
root). The +Dp side is load-bearing for transient landings, not luxury.

Cell E ((840,120), Dp admits the landing peak):

- @-1: ~= A everywhere (wantMin 0.863, leak 0.226, chugs 1.000, riff
  0.856 with only 2/16 onsets soft, sines/KS exact).
- @-2: partial (wantMin 0.607 = B on A5 fifth, 0.607/0.05 mush vs A's
  0.977/0.99; leak/chugs/riff much better than B).
- @-7: identical to B (collapse). Required Dp grows with shift depth;
  chasing it further is tuning, out of bounds.

Latency saving: B 963 smp (20.06 ms) = -480 smp / -10.0 ms vs W20
(1443), but broken; E 1083 smp (22.56 ms) = -360 smp / -7.5 ms vs W20
(-200 smp / -4.2 ms vs latency-study config-a at 1283), clean @-1 only.
CPU bonus (incidental): B 0.53x, E 0.62x of A on DI — transient frames
search fewer lags and 86% of DI frames are transient.
Realtime stability: margin proof reads Dp (covers 0/120); DC no-starve
pinned for both; block-determinism pinned.

## Q2: did small-skip bias reduce rephasing? NO — decisively falsified.

Predicted residuals (from measured jumps x interval ratios) and measured
AM depth are IDENTICAL across all cells (r4 AM @-1: 0.114-0.119; res5th
0.30-0.33, res4th 0.19-0.21). SmallSkip changes neither. Worse, it
degenerates three ways by content class (all trace/spectrum-proven):

1. Low pure tones: near-peg lags score ~0.95+ (drift step << period), so
   minimal-jump landings peg-climb at J~=1 every frame (348/348 pegged,
   sel~=0.96) = slow-pin = DRY OUTPUT. Measured +6.20%, predicted
   +6.22% exactly; output dry-bin 0.998/wander 0.99. The J>=1 anti-stick
   exclusion is defeated: J=1 IS the pin. Pitch mechanism destroyed.
2. High content (E4): shoulder-trap — takes the first peak's near
   shoulder (J~=112-115, sel~=0.95, every ~4-5 frames) instead of A's
   peak-top J=266 (sel 1.0, every ~11 frames). 22 inexact skips/sec churn
   harmonics: KS highE fund energy 1.057 -> 0.466 (@-1), 1.358 -> 0.269
   (@-2).
3. Low mixtures: ~= drift (same period peaks, -3 samples, -0.05 join
   score): medPegJ 552 vs 555, AM unchanged. No benefit, marginally
   worse joins (sacMax = full band).

Band sweep (r4 KS) shows the Pareto front is a cliff, no knee: band
<=0.2 no jump change; 0.3 jumps barely shrink (552->406) with joins at
selMin 0.24; 0.5 jumps tiny (med 6) with joins ~0, 56% frames pegged
(degenerate map), AM WORSE. Landscape proof: near-peg scores 0.06-0.28
vs max 0.67 — "small valid skips" don't exist; validity and smallness
are mutually exclusive. The drift rule's global-max landing is ALREADY
the minimal-valid skip in practice (one root period; smaller = invalid).

## Q3: what replaced the wobble? Nothing — same wobble everywhere.

No cell changes skip sizes, skip rates (4.6/s @-1, 8.9/s @-2, 99.7/s
@-7, all cells), residuals, or AM depth. C/D add no chorus/drift/flam on
DI (C row == A row bit-near-identical on every DI aggregate: transient
frames bypass SmallSkip by design and dominate DI). SmallSkip's damage
modes (dry-climb, shoulder churn) appear only on clean sustains — a new
failure class, not a wobble replacement.

## Verdicts

- Causal Dp=0 (B): REJECTED (transient-landing exclusion breaks DI
  chords/attacks). D (SmallSkip+causal): REJECTED (both diseases).
  C (SmallSkip): REJECTED (Q2 falsified with mechanism).
- E ((840,120)) is the ONE gate-surviving candidate: passes all
  replicated accepted bars (pitch 3%, chord 1/3, leak 1/2, sine 1%,
  no-starve, chug 0.7 — all shifts incl. -7) AND is ~= W20 @-1 on DI.
  Exposed for audition SCOPED to -1 only (see below); -2 has one
  fifth-mush wart, -7 collapses like B.
- IMPORTANT gate finding: B also passes all replicated unit bars despite
  real DI regressions — the isolated-note/toy-chug stimuli are blind to
  transient-storm damage. DI-table bars (riff/chord/leak) must gate any
  WSOLA geometry promotion, not unit bars alone.

## Sustained-polyphonic problem: REMAINS STRUCTURAL AND AUDIBLE.

Recommend ending the single-alignment WSOLA sustain quest (Q2's
falsification target "WSOLA mitigations can reach acceptable sustain"
FAILED): jump sizes are period-quantized by validity (smaller = invalid
or dry), and per-skip rephasing frac(q*J/P) is unchanged by any tested
mitigation. What survives: E as a latency reference (@-1), WSOLA as the
transient specialist (H1-style hybrid premise intact — attacks are its
proven strength), and per-bin/hybrid architectures (E4/H1) as the only
paths that exclude the wobble class (per D3).

## Audition: exactly one configuration (scoped)

`./build/tdm_live --lab-wsola -1 --lab-wsola-cfg 20:840:120
[--nam amp.nam] [--ir cab.wav]` — NO code changes needed (accepted class
admits (840,120); study data transfers bit-exactly by proof test).
SCOPE: -1 feel/latency check ONLY (22.6 ms vs 30.1 ms). Sustain quality
is unchanged by design (same wobble — do not audition for it); -2/-7
explicitly excluded by measurement. Offline A/B WAVs:
build/labpitch/e2_{A,E}/shift_-1_di.wav (+ r4/r5/voicings).

## Files / commands

- Fork: `dsp/lab/Pitch/LabWsolaV2.h/.cpp` (Drift/SmallSkip + Dp=0;
  `LabWsolaShift` untouched).
- Harness: `dsp/lab/Pitch/LabWsolaE2Study.cpp` ->
  `make build/tdm_e2_study && ./build/tdm_e2_study` (WAVs
  `build/labpitch/e2_<A|B|C|D|E>/`).
- Singles: `tdm_labpitch --in X --out Y --shift -1 --e2 A|B|C|D|E`.
- Tests: `tests/TestLabPitchV2.cpp`-sibling `tests/TestLabWsolaV2.cpp`
  (`labwsolav2` suite: Drift bit-identity incl. (840,120)
  audition-transfer; (960,0)/(840,120) validation + latency pins
  963/963/964 and 1083/1083/1084; no-starve; SmallSkip dry-climb
  pinned as the rejection record; determinism; bypasses).
- Scratch (not shipped): /tmp/e2_probe.cpp, /tmp/e2_sweep.cpp,
  /tmp/e2_trace.cpp, /tmp/e2_ditrace.cpp, /tmp/e2_gate.cpp,
  /tmp/edry.py; full tables /tmp/e2_study2.log.

## Test result

`make build/tdm_tests && ./build/tdm_tests`: all 19 suites PASS,
checks=1474 failures=0, including new `labwsolav2`. Accepted `labwsola`,
`labwsolalatency`, `labwsolalive` suites still green (W20 untouched).

## Working-tree state (uncommitted, intentionally)

M Makefile (E2 object + TEST_SRCS + e2-study target), M
dsp/lab/Pitch/LabPitchRender.cpp (additive --e2 flag), M
tests/TestMain.cpp (suite registration); new LabWsolaV2.h/.cpp,
LabWsolaE2Study.cpp, tests/TestLabWsolaV2.cpp. Rejected production
pitch-v1 stash untouched. No commit per instructions.

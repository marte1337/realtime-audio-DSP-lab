# PV-D v2 transient/reset experiment (E1) — report

Date: 2026-09-28. Lab-only fork, no TechDeathRig integration, no commit.
Design input: `.research/pitch-architecture-study/REPORT.md` section 11 (E1);
no external implementation code opened while writing. Geometry fixed at
PV-D 2048/256 @ 48 kHz for all variants.

Research question: does improved transient/reset handling materially rescue
PV-D's chord and low-frequency quality WITHOUT increasing its 2048-sample
analysis window?

Answer: **no.** Chord and low-B failures are fundamentally resolution-limited
at 2048. Handling fixes cannot rescue them. Details below.

## Variants (all in `dsp/lab/Pitch/LabPitchV2.h/.cpp`)

- **A (baseline):** `LabPitchShift` algorithm verbatim. Proven bit-identical
  to PV-D at equal config (test `pv2 A bit-identical to PV-D`, shifts
  -1/-2/-7 on pluck stimulus).
- **B (freq-delimited reset):** on flux-transient frames, reset only bins at
  or above 1000 Hz; lower bins propagate + lock as on normal frames.
- **C (transient dominance):** on transient frames, scale the frame's OLA
  accumulator add by 2.0 WITHOUT scaling its window-sum (reconstruct the
  attack at full strength at its true time position).
- **D:** B + C.
- Shared (attribution-clean): geometry, full-spectrum flux detector,
  per-bin IF propagation + nearest-peak identity locking, same-bin mapping,
  Hann/Hann WOLA, cubic slow read, latency formula.

Key implementation fact for the mechanism analysis: nearest-peak assignment
is by **bin distance**, and non-peak bins always inherit their peak's
synthesis phase. A weak partial that forms no local maximum (shoulder on a
stronger neighbor's skirt) is captured by the neighbor's peak; a weak
partial whose peak flickers in/out across frames gets a skirt-corrupted IF
when present and the neighbor's IF when absent. Both paths produce
energy-without-coherence at the weak partial's translated frequency.

## Measurement method

- Harness `build/tdm_pv2_study` (from `dsp/lab/Pitch/LabPV2Study.cpp`)
  renders A/B/C/D at -1/-2 (primary) and -7 (secondary stress) through
  `build/labpitch_di.wav` (deterministic `scripts/labpitch_synth_di.py`)
  plus deterministic in-harness KS voicings; latency-compensated WAVs in
  `build/labpitch/pv2_<A|B|C|D>/`.
- Forensics (this session, first-hand): per-fund translated-energy ratio
  `want/dry` (incoherent Goertzel at the translated fund, 200 ms
  subwindows) and coherence index `wander = coherent/incoherent` (1.0 =
  stable tone, ~0.1 = incoherent smear), plus attack-slope inventories and
  PV-A (4096/1024) / 8192-window ladder renders via `tdm_labpitch`.
- Dry-wander baselines measured for every chord case (dense dry 0.88-1.00,
  DI fifths dry 0.93-1.00; two KS instances have unsteady dry fifth funds
  at 0.67 — outputs matching that are dry-faithful, not damaged).

## 1. A/B/C/D results (first-hand)

Low strings, KS voicings @ -1, window 1-3 s, `want/dry` / wander:

| case | A | B | C | D |
|---|---|---|---|---|
| lowB fund | 0.529 / 0.12 | 0.420 / 0.11 | 0.529 / 0.12 | 0.420 / 0.11 |
| lowE fund | 1.001 / 0.92 | 1.001 / 0.92 | 1.001 / 0.92 | 1.001 / 0.92 |

DI power chords @ -1: root + octave essentially perfect in ALL variants
(0.98-1.01, wander 0.98-1.00); ONLY the fifth degrades:

| chord fifth | A | B | C | D |
|---|---|---|---|---|
| E5 (B2) | 0.709 / 0.20 | 0.659 / 0.21 | 0.709 / 0.20 | 0.659 / 0.21 |
| G5 (D3) | 0.725 / 0.23 | 0.696 / 0.22 | 0.725 / 0.23 | 0.696 / 0.22 |
| A5 (E3) | 0.813 / 0.11 | 0.817 / 0.11 | 0.813 / 0.11 | 0.817 / 0.11 |

Isolated voicings @ -1 (weakest fund adjacent to stronger neighbor fails):

- r5: root 0.640/0.09 (A) vs 0.722/0.04 (B) — B adds INCOHERENT energy;
  fifth 0.995/0.56 (A, dry 0.67 — nearly faithful).
- r4: root 0.906/0.13, fourth 0.969/0.38 (A ~= B).
- rmaj3: root 1.005/0.96, third 0.892/0.08.
- maj: root 0.996/0.71, third 0.956/0.12, fifth 0.721/0.11.
- dense: strongest fund (G2) 1.005/0.92; all three weaker funds retain
  energy (0.84-1.04) but wander 0.10-0.17.

@ -2 E5: same fifth-only pattern (A fifth 0.642/0.09; B 0.661/0.07).
@ -7 DI: root+octave STILL near-perfect (0.93-1.01, wander 0.7-1.0);
fifths collapse (E5 0.364, G5 0.095) and B is worse than A on every fifth
(E5 0.280, G5 0.083; lowBen 0.152 -> 0.112).

Attacks: C restores PV-dulled chug slopes to dry level on all 8 chugs
(A 12-20% below dry; C 0.54-0.69 vs dry 0.51-0.60, overshoot 0-23%),
timing preserved (+/-1 ms). C sustain funds are bit-identical to A
(C == A to 4 decimals on every fund window; test `pv2 C steady-state
middle equals A`).

## 2. Which feature helped what

- B: helped nothing coherently. Its one headline gain (r5 wantMin
  0.640 -> 0.722) is louder mush (wander 0.09 -> 0.04). Hurt lowB
  (-21% energy), DI fifths, and every @ -7 metric.
- C: genuinely restores attack slopes (WOLA-dilution hypothesis
  confirmed for transients); zero effect on sustain/chord/LF quality
  by construction and measurement.
- Neither B nor C moves any sustain/chord/LF metric in the coherent
  direction.

## 3. Additive or conflicting

Neither additive nor synergistic: D == B on every sustain/chord/LF fund
(to 3 decimals) while inheriting C's attack gain. The two mechanisms are
orthogonal (B touches reset phases, C touches OLA weights) with no
interaction — but since B's contribution is net-negative, D is strictly
worse than C alone.

## 4. Is low-B fundamentally resolution-limited? YES — proven by ladder

Same-file lowB @ -1/-2, PV-D (2048) vs PV-A (4096/1024):

| | PV-D 2048 | PV-A 4096 |
|---|---|---|
| lowB @ -1 | 0.529 / 0.12 | 1.006 / 0.99 |
| lowB @ -2 | 0.217 / 0.15 | 1.007 / 1.00 |
| lowE @ -1 | 1.001 / 0.92 | 1.001 / 0.99 |

Window doubling alone fully rescues lowB; no handling variant moves it.
Chords follow a spacing-tier ladder (wander @ -1):

| case (spacing) | 2048 | 4096 | 8192 |
|---|---|---|---|
| lowB single (61.7 Hz self) | 0.12 | 0.99 FIXED | — |
| maj third (21/20 Hz 2-sided) | 0.12 | 0.14 | 0.98 FIXED |
| maj fifth | 0.11 | 0.27 | 0.69 = dry (0.67) FIXED |
| dense semitones (4.9/5.8 Hz) | 0.1-0.2 | 0.1-0.2 | 0.19-0.55 partial |

4096 does NOT fix thirds (21 Hz = 1.8 bins, still inside the Hann
mainlobe; third energy even drops 0.956 -> 0.688). Each doubling rescues
exactly one spacing tier; semitone clusters need impractically large
windows. Universal pathology at 2048: strongest fund survives coherently,
weaker adjacent funds keep energy but lose coherence — one clear note
plus beating mush. This is an IF-estimation/peak-formation failure, which
is why reset/OLA handling (B/C/D) cannot touch it.

## 5. Exact latency (identical all variants, tested)

- -1 st: 2153 smp = 44.9 ms (Hs=242)
- -2 st: 2269 smp = 47.3 ms (Hs=228)
- -7 st: 2940 smp = 61.2 ms (Hs=171)
Formula `(N+1)+(N-Ha)*(1/r-1)`, shared with PV-D; test pins all three.

## 6. Audition verdict

**C only, narrowly scoped to transient feel** (chugs/riff attack A/B) —
it is the sole non-harmful delta and its 0-23% slope overshoot needs
ears (could read tight or clicky). Do NOT audition any variant for chord
or low-string quality (C == A; B/D worse). If the audition question is
"did handling rescue chords," no listening is needed: it did not.

## 7. Generated paths / launch commands

- Fork: `dsp/lab/Pitch/LabPitchV2.h/.cpp` (lab-only; not in rig/tests
  of rig/dev app; linked into `tdm_tests` for its own suite only)
- Harness: `dsp/lab/Pitch/LabPV2Study.cpp` ->
  `make build/tdm_pv2_study && ./build/tdm_pv2_study`
- Renders: `build/labpitch/pv2_<A|B|C|D>/shift_<m1|m2|m7>_<di|voicing>.wav`
- Single renders: `tdm_labpitch --in X.wav --out Y.wav --shift -1 --pv2 A|B|C|D`
- Ladder renders: `tdm_labpitch --shift -1 --fft 4096 --hop 1024`
  (PV-A), `--fft 8192 --hop 2048`
- Tests: `tests/TestLabPitchV2.cpp` (`labpv2` suite in `tdm_tests`)
- Scratch forensics (not shipped): `/tmp/pv2_probe.py`, `/tmp/kdry.py`

## 8. Test result

`make build/tdm_tests && ./build/tdm_tests`: all 18 suites PASS,
checks=1432 failures=0, including new `labpv2` (A bit-identity vs PV-D
@ -1/-2/-7; cross-mode + PV-D latency equality; exact latencies 2153 /
2269 / 2940; B/C/D engage + finite + bounded on pluck; C steady-state
middle == A; reset determinism; bypass-0 and disabled bypass bit-exact).

## 9. Working-tree state

Uncommitted, intentionally (research phase, no commit): `M Makefile`
(LABPITCH_LIB + TEST_SRCS wiring), `M dsp/lab/Pitch/LabPitchRender.cpp`
(`--pv2` flag), `?? dsp/lab/Pitch/LabPitchV2.h/.cpp`,
`?? dsp/lab/Pitch/LabPV2Study.cpp`, `?? tests/TestLabPitchV2.cpp`,
`M tests/TestMain.cpp`, `?? .research/`. Rejected production pitch-v1
stash untouched.

## NEXT RESEARCH STEP (exactly one)

Run **E2 — the causal small-skip WSOLA probe** (report section 11), as
pre-registered for an E1-failure outcome: short-PV chords are proven
resolution-limited, so the remaining live-latency question is whether
WSOLA mitigations can reach acceptable sustain. Do NOT run E5 (vertical
twist graft): its precondition was "sustains close-but-phasey," and this
experiment falsified it — weak-partial sustains are incoherent
(wander ~0.1), not slightly phasey; neighbor-twist smoothing cannot
resurrect partials whose peaks never form.

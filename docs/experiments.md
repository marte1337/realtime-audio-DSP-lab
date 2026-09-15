# TechDeathMachine DSP Experiment Log

This document records DSP experiments, listening results, failed ideas, and potentially reusable findings.

The purpose is not polished documentation. It is an engineering notebook.

---

## Experiment template

### Date

YYYY-MM-DD

### Module

Example: Gate

### Hypothesis

What are we attempting to achieve?

### Implementation

Briefly describe the DSP approach being tested.

### Audition setup

Document relevant variables such as:

- guitar
- pickup
- tuning
- NAM model
- IR
- sample rate
- important parameter settings

### Expected behavior

What should change?

What should remain unchanged?

### Listening result

What actually happened?

### Problems

Anything unexpected, undesirable, unstable, noisy, or difficult to control.

### Next step

The next concrete experiment.

### Transferable learning

Could this knowledge or code be useful in:

- HoldsworthEngine
- Thall Suite
- another future DSP project

---

# Experiments

Add new experiments below this line.

---

## Date

2026-09-15

### Module

TechDeathGate (v1, first original DSP feature)

### Hypothesis

A peak-detector gate with instantaneous attack, fixed 6 dB hysteresis, a
short fixed hold, and one-pole smoothed gain can deliver machine-tight
stops for high-gain tech-death rhythm without clicks, pumping, or chopped
attacks — with only Threshold + Release exposed.

### Implementation

`dsp/TechDeathGate.h/.cpp`, inserted pre-NAM in `TechDeathRig`
(Input → mono → Gate → NAM → IR → Output). Detector: peak follower,
instant attack, 1 ms exponential release. Comparator: open above Threshold
(-60..-20 dBFS peak, default -40), close below Threshold - 6 dB (fixed).
Hold: fixed 8 ms below the close level before the release ramp starts
(bridges ~6.1 ms ripple valleys of open low E). Gain: one-pole toward 1
(0.15 ms attack) or toward a -80 dB floor (Release = 60 dB fall time,
10..500 ms, default 50). Reset state is closed (no startup hiss). Bypass
(default) copies bit-exactly; M0 behavior unchanged until explicitly
enabled. Envelope snaps to zero below 1e-12 (denormal guard).

Offline audition proxy (synth DI + -50 dBFS hum/noise, gate only):
gap RMS 0.002306 bypassed vs 0.000000 gated, pluck RMS identical
(0.0871 vs 0.0871). Full automated suite: 104 checks, 0 failures,
including release timing at 44.1/48/96 kHz.

### Audition setup

- guitar: (fill in)
- pickup / tuning: (fill in)
- NAM model / IR / sample rate: (fill in)
- gate: start Threshold -40 dB, Release 50 ms (`tdm_live --gate-thresh -40 --gate-rel 50`)
- reference takes: same riff with gate off (no flags)

### Expected behavior

What should change: level between intentional notes drops according to
threshold/release; stops feel immediate and silent.

What should remain unchanged: with gate fully open, NAM tone, cab tone,
and pick attack are untouched (verified bit-near-exact offline); no
deliberate EQ or distortion from the gate.

### Listening result

NOT YET AUDITIONED with a real guitar. Do not call v1 finished until this
section is filled in from playing: rapid palm-muted riffs, alternate-picked
lines, tremolo picking, legato, sustained notes, inter-note muting, abrupt
chord stops, low-level pickup noise.

### Problems

None observed offline. Known tradeoffs: fixed 8 ms hold delays every stop;
no ratio/range control (hard -80 dB floor); slow-decaying loud notes can
duck under threshold and fade early at extreme settings; no lookahead;
threshold is pre-NAM DI level, so interface gain staging matters a lot.

### Next step

Real-guitar audition (procedure in final task report); adjust defaults
(threshold/release/hold) only from listening evidence. Candidates if
listening demands: dual-rate release (fast stop + gentle tail), exposed
hold, sidechain HPF for low-string chug control.

### Transferable learning

- HoldsworthEngine: same detector/hold/hysteresis topology fits any
  high-gain gate need; `currentGain()` accessor pattern is handy for
  metering/tests.
- Thall Suite: low-tuned extended-range ripple needs the same hold-vs-stop
  tradeoff analysis (measure ripple period of the lowest string first).

---

## Date

2026-09-15

### Module

InputTrim (listening-driven addition, no gate retune)

### Hypothesis

Increasing input trim should simultaneously:

1. drive the NAM amp harder
2. move the guitar signal farther above the gate threshold
3. potentially make gate closing feel less aggressive at identical
   threshold/release settings

This entry does NOT claim to solve the gate cutoff artifacts. Those must be
evaluated after the input level is corrected.

### Implementation

`dsp/InputTrim.h/.cpp`, inserted first in `TechDeathRig`
(Input → Trim → Gate → NAM → IR → Output). Pure linear gain, dB in /
linear internally, range -12..+18 dB, default 0 dB, clamped. Smoothing:
one-pole lowpass on the applied linear gain, tau ~10 ms, coefficient
recomputed in `reset()` from the sample rate (rate-independent timing).
`reset()` snaps applied gain exactly to target, so 0 dB passes bit-exactly.
No EQ, distortion, latency, or polarity change by construction (single
multiply per sample). Exposed in both dev tools as `--input-trim <dB>`.

### Audition setup

- guitar: (fill in — same guitar / Scarlett gain / NAM / IR / gate
  threshold / gate release as the gate v1 audition)
- take 1 (baseline): no `--input-trim` flag (0 dB)
- take 2 (moderate): `--input-trim 6`
- take 3 (stronger): `--input-trim 9` or `--input-trim 12`
- change NOTHING else between takes

### Expected behavior

What should change: amp saturation/drive, perceived loudness, gate opening
easier, gate closing later/softer at the same threshold/release.

What should remain unchanged: tone character apart from drive level; gate
detector/hysteresis/hold/release law untouched (verified: gate suite green,
no gate files modified); M0 path bit-exact at 0 dB (verified: trim-0
render byte-identical to no-trim render).

### Listening result

NOT YET AUDITIONED. Evaluate per take: amp saturation / drive, pick
response, perceived loudness, gate opening, gate closing, sustain, and
remaining crackle/fragment behavior. Record which trim value makes the amp
feel correctly driven; that value becomes the reference level for the
repeat gate audition.

### Problems

None observed offline. Automated suite: 133 checks, 0 failures (29 new
trim checks). Known non-issues: settled gain stalls ~1e-4 absolute from
target in float (one-pole rounding, inaudible); trim changes take ~70 ms
to fully settle (by design, zipper-free).

### Next step

Run the three-take audition above, then repeat the gate v1 audition at the
chosen trim and re-evaluate the closing crackle/fragments.

### Transferable learning

- The one-pole-on-linear-gain pattern (with snap-on-reset) is a reusable
  primitive for any future gain-like parameter (output trim, mix blends),
  but per AGENTS.md it stays a per-module idiom until a third use case
  proves a shared helper worthwhile.

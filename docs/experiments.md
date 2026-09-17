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

2026-09-16

### Module

Space v1 (Delay + Reverb, `dsp/Space/`; first stereo stage in `TechDeathRig`)

### Hypothesis

A small post-cab time/space stage can give leads and ambience room to
breathe while the dry brutal rhythm tone stays mathematically untouched:
series routing (room hears the echoes), additive mixes (dry never scaled),
true ping-pong delay plus a damped Schroeder-Moorer room — with only
Time/Feedback/Mix + Decay/Mix exposed.

### Implementation

`dsp/Space/Delay.h/.cpp` — two cross-coupled lines, dry enters the L line
only, each line recirculates the OTHER line's damped tap: echoes strictly
alternate L/R/L (true ping-pong) with one Time control. Fractional taps
(linear interp); Time glides like a tape knob (~30 ms smoothing);
feedback (0..0.85, default 0.35) and a fixed ~4.5 kHz loop-damping
lowpass live inside the loop, so repeats darken and loop gain stays < 1
by construction. Ranges: Time 20..2000 ms (default 375), wet-only out.

`dsp/Space/Reverb.h/.cpp` — stereo Schroeder-Moorer: 4 parallel damped
combs (T60 0.25..5 s exponential from Decay 0..1, default 0.4; per-rate
retuned comb gains for the same T60) into 2 decorrelated series
allpasses per channel. Fixed 180 Hz send highpass (mud never enters the
tank), fixed ~5.5 kHz comb damping (highs die faster than the T60 lows
by design). Wet-only out.

`dsp/Space/SpaceProcessor.h/.cpp` — mono in, stereo out. Send for the
room is dry + audible delay (series); output is dry + delayMix*delayWet
+ reverbMix*reverbWet (additive, mixes smoothed ~10 ms). Everything
through ToneShape stays mono; Space is the first stereo stage.
`TechDeathRig` carries L/R from Space through Output Trim; N=1 output is
the exact (L+R)/2 fold-down, extra channels cycle the pair. Disabled (or
never reset) is bit-exact dry on L+R.

Offline numbers: full suite 563 checks / 0 failures, including strict
L/R alternation (opposite side exactly silent), fb=0 single echo on L,
DC-step tail ratios == feedback, decay energy slopes agreeing across
44.1/48/96 kHz, exact dry bypass, and rig integration (fold-down,
cycling, trim-after-space). Render proof: first 400 samples of a
space render are bit-identical to the dry render; the tail differs by
> 6.0 peak (space clearly audible, onset untouched).

### Audition setup

- guitar / pickup / tuning: (fill in)
- NAM model / IR / sample rate: (fill in)
- dev app: Space section, both units OFF at audition default (dry first)
- CLI: `tdm_render --delay --delay-time 375 --delay-fb 0.4 --delay-mix 0.3
  --reverb --reverb-decay 0.5 --reverb-mix 0.25 ...` (same flags on `tdm_live`)
- reference takes: same riff dry (no space flags) for A/B

First-audition starting takes (exact settings — set by ear from here):

1. subtle rhythm ambience: Delay OFF; Reverb ON, Decay 0.15 (~0.5 s),
   Mix 0.10. Room glue only; mutes must still feel instant.
2. technical lead: Delay ON, Time 375 ms, Feedback 0.35, Mix 0.22;
   Reverb ON, Decay 0.40 (~1.5 s), Mix 0.18. Echoes sit under fast runs
   without smearing 16th-note separation.
3. large atmospheric lead: Delay ON, Time 500 ms, Feedback 0.50,
   Mix 0.30; Reverb ON, Decay 0.75 (~3 s), Mix 0.28. Wash allowed, dry
   attack must still lead every note.

### Expected behavior

What should change: leads gain depth/width; single-note lines feel
supported; the ping-pong reads as left/right movement on headphones.

What should remain unchanged: palm-mute tightness and pick attack (dry
is never scaled — disabling either unit returns exact dry immediately);
no low-end buildup (send HPF + darkening repeats); no harshness stacking
on fast repeats (loop damping); stereo image collapses to mono cleanly
(exact fold-down, decorrelated but energy-matched L/R).

Bug tells: any audible difference between dry and mix-0 (not bit-exact
= bug); echoes that do NOT alternate sides (ping-pong broken);
metallic ringing / fixed pitch in the tail (comb gains wrong);
mushy attacks with Reverb on (send HPF too high or mix too hot —
setting issue, not DSP, unless Mix 0.1 already smears).

### Listening result

NOT YET AUDITIONED with a real guitar. Offline behavior is fully
pinned by tests, but passing tests never replace listening validation:
play rapid technical rhythm AND legato leads through the three takes
above before calling v1 finished.

### Problems

One real DSP bug found by the tests during development and fixed: both
delay lines were fed the dry input, so L/R wet were bit-identical
(dual-mono wearing a "ping-pong" label — symmetric cross-coupling can
never alternate). Fix: dry enters the L line only; R recirculates L's
damped tap. One-line change, now pinned by strict alternation tests
(third echo included). Also fixed: Space mix getters returned the
smoothed gain instead of the target (siblings return targets); two test
oracles that misread the design (peak-based reverb assertions vs damped
HF, absolute-energy rate comparison vs per-rate echo density) were
reworked to energy/slope oracles — no DSP change.

### Next step

Real-guitar audition of the three takes above; adjust defaults/mixes
only from listening evidence. Candidates if listening demands: delay
low-cut / high-cut exposure, reverb pre-delay, ducking hooks for rhythm
clarity (all deliberately out of v1 scope).

### Transferable learning

- Additive wet + never-scale-dry is a 4-for-4 proven house pattern now
  (Bite, ToneShape sections, Space mixes): mix-0-bit-exact falls out
  for free and makes A/B testing honest.
- Series delay→reverb send at audible level (not unity-tap) is what
  makes the room "hear the echoes" — worth reusing anywhere a room
  follows a delay.
- Per-rate T60 comb retuning works (slopes agree 44.1/48/96); test decay
  with energy slopes, never peaks, when the design damps highs.
- Symmetric cross-coupled feedback CANNOT ping-pong — the dry feed must
  be asymmetric. Obvious in hindsight; the strict-alternation test is
  what caught it.

### Senior DSP review requested

- Ping-pong fix (dry into L only): confirm no wanted topology was lost
  (e.g. dual-mono first echo for mono-compatibility purists) — the
  fold-down stays exact either way.
- Comb/allpass tunings (prime-ish lengths, 0.7 allpass gain, 180 Hz send
  HP, 5.5 kHz comb damping) against real tech-death lead targets —
  chosen from standard practice, not from listening yet.
- T60 mapping (Decay 0..1 → 0.25..5 s exponential) and wet gains at the
  extremes — max Decay Mix 1.0 is a lot of 5 s wash; confirm useful.
- Delay damping (fixed 4.5 kHz) vs exposing a tone control — v1 says no,
  revisit if leads sound dull or harsh.

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

`dsp/Gate/TechDeathGate.h/.cpp`, inserted pre-NAM in `TechDeathRig`
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

---

## Date

2026-09-15

### Module

TightDrive (v1, pre-NAM conditioning / overdrive)

### Hypothesis

A tight pre-drive highpass into moderate tanh saturation with post
presence shaping can firm up palm mutes, clarify pick attack, and push the
NAM in a useful way — without fizz, thinning, or oversampling complexity.

### Implementation

`dsp/TightDrive/TightDrive.h/.cpp`, inserted post-gate in `TechDeathRig`
(Input → Trim → Gate → Drive → NAM → IR → Output). Mono float, in-place
safe, ~15 ops/sample, 3 filter states, RT-safe (no alloc/lock/IO).

- Tight 0..1 (default 0.5): one-pole HPF, exponential map 40 Hz → 320 Hz.
  40 Hz is transparent for 7-string low B; 320 Hz cuts low-E fundamental
  ~12 dB (surgical). First-order: gentle, stable, no resonance.
- Drive 0..1 (default 0.3): preGain 1..8 linear (0..+18 dB) into tanh.
  C-infinity, odd-symmetric (no DC), bounded +/-1 (overdrive, never fuzz).
- Bite 0..1 (default 0.5): post-shaper parallel high-shelf @ 3 kHz,
  -5..+5 dB (0.5 = exactly 0 dB, neutral). Post placement keeps fizz out
  of the clipper. Measured end-to-end sweep at 5 kHz: 1.998x (~6 dB);
  220 Hz moves 0.6% (spectral control, not global gain).
- Fixed one-pole lowpass @ 12 kHz after the shelf: anti-fizz/alias
  insurance above the presence range, not a tone control.
- Smoothing: applied preGain/HPF-coeff/shelf-mix each follow targets via
  one-pole tau ~10 ms (rate-independent). Fixed-LPF coeff needs none.
- Gain staging: NO output compensation (the push is the product). Level
  rises with Drive by design; judge focus/drive in audition, not loudness.
- reset(): validates rate, zeroes states, snaps smoothed values to
  targets. Disabled (default) or unreset: bit-exact memcpy bypass.
- Denormals: all three filter states snap to 0 below 1e-12 (gate pattern).

### Audition setup

- FIXED reference (do not change while evaluating TightDrive): Scarlett
  48 kHz, same guitar/pickup, Input Trim +9 dB, Gate -40 dB / 50 ms, same
  ENGL Powerball-style NAM, same Mesa Traditional V30 IR.
- A (Subtle): `--tight-drive --tight 0.25 --drive 0.15 --bite 0.45`
  (HPF ~67 Hz, pre +6.2 dB, shelf -0.5 dB). Expect: slightly firmer lows,
  minimal saturation, natural feel.
- B (Tight): `--tight-drive --tight 0.55 --drive 0.35 --bite 0.6`
  (HPF ~121 Hz, pre +10.8 dB, shelf +1 dB). Expect: tighter palm mutes,
  better pick definition and fast-note separation, no severe thinning.
- C (Surgical): `--tight-drive --tight 0.85 --drive 0.5 --bite 0.7`
  (HPF ~234 Hz, pre +13.1 dB, shelf +2 dB). Expect: extremely controlled
  low end, hard attack; may be too extreme for general use.

### Expected behavior

What should change: low-end tightness, pick clarity, NAM saturation feel
across A → B → C; palm-mute weight vs control tradeoff.

What should remain unchanged: bypassed drive = previous rig bit-exactly
(verified); gate/trim/NAM/IR algorithms untouched (suites green);
noiseless switching (smoothed params); no DC, no denormal issues.

### Listening result

NOT YET AUDITIONED. Listen per preset for: palm-mute tightness vs weight,
pick attack clarity without scratch/fizz, fast single-note separation,
chord intelligibility, low-string mud vs thinness, NAM focus vs mere
loudness, gate-interaction changes, and artifacts (fizz, metallic aliasing
highs, clicks, unstable lows, over-compression, dynamics loss).

### Problems

None observed offline. Automated suite: 208 checks, 0 failures (75 new
drive checks), incl. cutoff tracking at 44.1/48/96 kHz and a full-chain
drive+NAM render. Known limitations: first-order HPF only (steeper slope
later if listening demands); no lookahead; loudness rises with Drive
(confound noted above); aliasing strategy is argue-not-oversample (below).

Aliasing position: tanh is smooth, drive capped moderate, no HF
pre-emphasis into the clipper, fixed 12 kHz post-LPF, Mesa IR lowpasses
hard above ~5-6 kHz. If audition reports metallic highs: add oversampling
or a lower post-LPF corner. Flagged for senior DSP review either way.

### Next step

Run A/B/C audition against the fixed reference chain; promote one preset
toward defaults only from listening evidence. Candidates if listening
demands: steeper Tight slope, dual-rate release-style saturation feel,
sidechain-aware low-string handling.

### Transferable learning

- Parallel-form shelf (y = x + k*HP(x)) is unconditionally stable,
  interpolation-safe, and neutral-bit-exact at k = 0: good default shape
  for any future tone control (ToneShape).
- Quadrature-correlation fundamental measurement is the right test tool
  whenever a waveshaper sits in the measured path (peak/RMS lie).

---

## Date

2026-09-16

### Module

OutputTrim (gain-staging correction, no gate retune)

### Hypothesis

Amp comparison needs post-chain loudness matching that leaves NAM drive,
saturation, compression, and gain structure untouched. A manually
controlled scalar gain stage after Cabinet IR separates the two jobs that
Input Trim was incorrectly doing double duty for:

- Input Trim (pre Gate/TightDrive/NAM): interface/guitar calibration;
  intentionally changes the level feeding nonlinear stages and therefore
  the NAM response.
- Output Trim (post NAM + IR): loudness matching / preset and model
  compensation; changes listening level only.

### Implementation

`dsp/OutputTrim.h/.cpp`, inserted after Cabinet IR in `TechDeathRig`
(... → NAM → IR → Output Trim → broadcast). Pure linear gain, dB in /
linear internally, range -24..+24 dB, default 0 dB, clamped. Smoothing:
one-pole lowpass on the applied linear gain, tau ~10 ms, coefficient
recomputed in `reset()` from the sample rate (same idiom as InputTrim;
per the InputTrim log entry this stays a per-module pattern — two uses do
not justify a shared gain framework). `reset()` snaps applied gain exactly
to target, so 0 dB passes bit-exactly from the first sample. No limiter,
compressor, clipping, normalization, auto-gain, or analysis of any kind.
No output protection by design: over-unity float results are preserved and
the user (or later host handling) owns safe audition levels.

### Audition setup

Reference measurements (same synthetic DI, same IR, Gate/Drive off,
Input Trim 0 dB):

- Powerball test capture: approximately -34.83 dBFS RMS
- 5150 II Crunch: approximately -16.03 dBFS RMS
- Mesa Red: approximately -20.38 dBFS RMS

Powerball sits ~18.8 dB below the 5150 and ~14.4 dB below the Mesa, inside
the +/-24 dB range with margin. Mesa Red is the temporary loudness
reference. Starting compensations (RMS-derived, verify by ear, do NOT
hard-code as defaults): Powerball +14.45 dB, Mesa 0 dB, 5150 -4.36 dB.
These may later become per-preset/per-model metadata; this stage needs no
changes to support that (a preset layer would just call setOutputTrimDb).

### Expected behavior

What should change: relative loudness of captures at fixed drive feel.

What should remain unchanged: NAM saturation/compression character per
model at matched loudness; gate and drive behavior (verified: gate/trim/
drive suites green, no files modified there); M0-and-later path bit-exact
at 0 dB (verified: trim-0 full-chain render byte-identical to previous).

### Listening result

NOT YET AUDITIONED. Confirm the three captures feel equally loud at the
starting compensations without feeling like different amps than at
reference (i.e. drive character preserved, only level moved).

### Problems

None observed offline. Automated suite: 252 checks, 0 failures (34 new
outtrim checks), incl. a gate-transition ratio test proving the trim is
pure post scaling, and an unclipped >1.0 Rails test. Known non-issues:
settled gain stalls ~5e-4 absolute from target in float (inaudible);
trim moves take ~100 ms to fully settle (by design, zipper-free);
`tdm_live --list` throws on headless machines (no CoreAudio devices;
untouched code path, environmental).

### Next step

Run the level-matched three-amp comparison; record the by-ear trims next
to the RMS-derived starting points. Then repeat the gate audition at
corrected level. Later: per-NAM compensation metadata in curated presets.

### Transferable learning

- Upstream/downstream test-ratio technique (out(+X)/out(0) == linear(X)
  at every audible sample, incl. through gate transitions) is a reusable
  pattern for proving any future post stage is side-effect free.
- Second data point for the InputTrim log note: two copies of the trivial
  one-pole gain smoother still do not justify a framework. Revisit at
  three.

## Date

2026-09-16

### Module

TechDeathGate v1.1 closing refinement (hard stop kept, crackle removed)

### Hypothesis

The real-guitar audition found the gate fundamentally useful and aggressive
at ~52 ms Release, but closing felt excessively hard and sometimes produced
small crackle/fragment artifacts. Two distinct mechanisms, both in the gate
only:

1. Hardness: the hold-to-release boundary is a slope discontinuity (a
   single-pole fade falls fastest at its first sample, ~1.15 dB/ms at
   52 ms), heard as an abrupt grab at the stop.
2. Crackle: NOT a gain discontinuity (gain is continuous everywhere).
   Faint tail residue, hum, and sympathetic-string blips hovering just above
   Threshold re-cross the open level after a close, punching the ~0.15 ms
   attack back open for isolated 10-40 ms bursts. Offline probes measured a
   train of 5 such open/close episodes (15-39 ms each, first-sample jumps up
   to 62 dB) on decaying guitar-like material. Hold only delays the first
   close; nothing guarded reopening, so each blip became one fragment.

Downstream (TightDrive+NAM) was investigated as a suspect and cleared as a
reshaper: probes show the gate tail is always sub -44 dBFS when the fade
starts (release requires 8 ms below the close level first), so TightDrive
stays in its linear zone there and its output tracks the gate fade. The
amp's small-signal gain is the audibility multiplier (why a -37 dBFS
fragment is clearly heard), not a second cliff.

### Implementation

`dsp/Gate/TechDeathGate.h/.cpp` only. No new controls, no latency, no attack
change, no Trim/Drive/NAM/IR touch. Two internal mechanisms:

- Cascaded release: closing runs through a two-stage follower cascade
  (intermediate stage feeds the applied gain, both sharing one
  coefficient). Fade follows (1+t/tau)*e^(-t/tau): starts with zero slope
  (soft knee), steepens mid-release. tau = Release/9.23 keeps Release
  defined as the total 60 dB fall time, so the ~52 ms stop timing is
  preserved and the existing release-timing tests (3 rates x 10/50/500 ms)
  pass unmodified.
- Reopen reluctance: for 60 ms after each close, reopening needs
  Threshold + 6 dB instead of Threshold. Faint residue cannot retrigger the
  fast attack; real pick attacks (tens of dB hotter) clear the bar with zero
  extra delay, and after the window the normal Threshold applies again, so
  steady-state threshold behavior is unchanged.

Release semantics: unchanged (60 dB fall time). Release contour: changed
from exponential to S-curve (gentler first ~10 ms, identical silence point).
A sustained above-threshold resonance after a stop still ghosts exactly
once per event; rapid cycling is what was removed. That single-ghost
behavior is pinned by tests as correct threshold operation, not chatter.

### Expected behavior

What should change: stops land slightly softer at the very first
milliseconds, then stop just as fast; the intermittent crackle/fragments
during closing are gone.

What should remain unchanged: ~52 ms stop/start feel, attack immediacy,
Threshold behavior (a -38 dB sustained tone still opens the gate),
palm-mute tightness (5-chug proxy: attacks survive, gaps go fully silent),
bypass, all Trims/Drive/NAM/IR.

### Listening guide (for the audition below)

1. Physically: the fade now eases in over ~10 ms instead of grabbing
   instantly, then falls at the same overall rate; faint post-stop
   resonances can no longer machine-gun the attack stage.
2. Changing: gate close contour + post-close reopen guard only.
3. Constant: Input Trim 0 dB, TightDrive 0.85/0.50/0.70, 6505 unboosted
   NAM, Mesa V30 IR, Output Trim +2.6 dB, Threshold, Release 52 ms.
4. Should feel like: the same locked-in brutal stop, but the initial edge
   is clean rather than hard, and choked stops decay into silence with no
   fizzing/crackling tail.
5. Bug indicators: any rapid stuttering after a stop (guard failing);
   sluggish or softened pick attacks (bar too high — should be impossible,
   attacks clear it by 20+ dB); audibly later silence point (timing drift);
   low sustained notes cutting out (hold/cascade regression).

Audition command (flag syntax verified against app/TdmLive.cpp; substitute
your local 6505_unboost.nam and Mesa V30 IR paths — no curated models/IRs
are vendored in the repo):

./build/tdm_live --nam <path-to>/6505_unboost.nam --ir <path-to>/MesaV30.wav \
  --input-trim 0 --gate-thresh -40 --gate-rel 52 \
  --tight-drive --tight 0.85 --drive 0.50 --bite 0.70 \
  --output-trim 2.6

### Listening result

NOT YET AUDITIONED. The offline evidence is strong (probe-measured
fragment train eliminated in simulation; attended criteria all green), but
per project rules passing tests never replace the ear: confirm the stop
still feels brutal at 52 ms and the crackle is gone on real chugs and
single-note stops.

### Problems

None observed offline. Automated suite: 281 checks, 0 failures (29 new
gate checks: S-knee slope, blip-flurry pair counting, attack trio,
palm-mute proxy). Old-vs-new verification: the committed scenarios were
run against pristine HEAD gate sources — old code fails the knee slope
(early drop 0.129 vs mid 0.043, i.e. steepest at start), reopens once per
flurry blip (3 fragment pairs: 35/43/45 ms), and reopens on faint
in-window residue; new code passes all. `make smoke` green (gate/drive
renders finite and bounded).

Build hazard found (not fixed, out of scope): the Makefile has no header
dependencies, so touching a header without `make clean` links stale
objects against a new class layout (observed as 15 phantom gate failures
that vanished on clean rebuild). Workaround until fixed: always
`make clean && make test` after header edits.

### Next step

Live audition with the command above; record whether 52 ms still feels
like the musical stopping point and whether any fragment character
remains. If a post-NAM cleanup stage is ever considered for sub-threshold
tails, note it is a separate proposal, not part of this fix.

### Transferable learning

- A gate's crackle is usually the *reopen* path, not the release ramp:
  instrument open/close *events with timestamps* on decaying material
  before touching any coefficient. The 5-episode train made the fix
  obvious (guard the bar) where a waveform alone suggested the wrong fix
  (lengthen Release).
- Threshold-boundary tests are cliffs: a -38 dB blip against a -40 dB
  threshold plus hum/hiss either always or never fires depending on
  sub-dB phase accidents. Design margins symmetrically (here: -37 dB
  stimulus sits 3 dB above Threshold and 3 dB below the reopen bar) and
  verify the test fails pre-fix rather than trusting hand arithmetic.
- Single-frequency test blips phase-lock with a same-frequency tail and
  cancel deterministically; use an incommensurate blip frequency
  (110 Hz vs 82.41 Hz main) or cosine-phase onsets so the first peak is
  phase-proof.
- Poll gate *state during* decaying stimuli, not after: end-of-buffer
  isOpen() checks are vacuous when the stimulus re-closes the gate by
  itself.

### Senior DSP review requested

- Release cascade shares one coefficient across two one-pole stages
  (critically-damped form); confirm no preferable normalization and that
  float stall behavior near the -80 dB floor stays benign (tests show
  settle within 1e-6 of floor; denormal risk unchanged from v1 design).
- 60 ms / +6 dB reluctance constants are empirically sized from probe
  trains (gaps 24-41 ms); confirm the sizing argument holds for
  drop-tuned extended-range guitars with longer string ring.
- Pre-NAM gate placement remains: downstream investigation cleared
  TightDrive of reshaping the tail, but a post-NAM cleanup stage could
  suppress sub-threshold tails further. Flagged as future proposal only.

## Date

2026-09-16

### Module

TechDeathGate v1.2 state-aware retrigger (sustained-note stutter fix)

### Real-guitar reference update

The -40 dB threshold reference was too high for the actual guitar DI:
normal initial picks could be gated. The usable reference is now
approximately **-50 dB** (threshold range unchanged, still -60..-20 dB;
no auto-calibration added). Release 52 ms still feels musically
appropriate; the v1.1 two-stage release contour is kept unchanged.

### Hypothesis (confirmed by probes before any code change)

v1.1 fixed close-edge hardness but real sustained notes exposed repeated
retrigger/stutter: OPEN -> CLOSE -> OPEN -> CLOSE ... on one unre-picked
note. Suspect: the fixed 60 ms reopen-protection window is the wrong
abstraction — a sustained note keeps decaying and beating long after any
fixed window expires, and each re-crossing of the normal open threshold
then stutters the gate again.

Probe evidence (temporary offline probes, threshold -50 dB, release
52 ms, current v1.1 code): a 4 s beating low-E sustain (harmonics, +/-3 dB
beating, hum, hiss, ~2 s dwelling near threshold) produced **11
open/close cycles over 2.1 s** (e.g. CLOSE 854.7 ms, OPEN 918.7 ms,
CLOSE 945.3 ms, ... last event 2946.7 ms). A 3 s low-B proxy produced
12 opens / 11 closes. Hypothesis confirmed: expiry of the fixed window
while the tail still beats is exactly the stutter mechanism. The release
contour itself is not implicated (gain only follows the cycling
comparator), so it was left alone.

Probe-measured sizing data: worst residual beat peak after the close was
-44.1 dBFS (+5.9 dB above the -50 dB threshold); longest sub-close
valley in beating was 20.7 ms.

### Implementation

`dsp/Gate/TechDeathGate.h/.cpp` only. The boolean open flag plus 60 ms timer
became an explicit CLOSED -> OPEN -> CLOSING -> CLOSED state machine:

- CLOSED: normal Threshold opens with zero delay (no permanent penalty).
- OPEN: unchanged, incl. 8 ms hold bridging ripple valleys.
- CLOSING: release fade keeps running (v1.1 cascade untouched), but
  reopening needs Threshold + 12 dB (retrigger bar) for as long as the
  tail takes — no expiry. Back to CLOSED after 250 ms continuously below
  the close level; any louder peak restarts the confirmation.

Why +12 dB / 250 ms: +12 holds ~6 dB headroom over the worst measured
beat peak (+5.9 dB); 250 ms is ~12x the longest measured beat valley
(20.7 ms), so beating cannot fake "tail gone" but real silence restores
normal threshold semantics promptly. A transient/rising-slope criterion
was investigated and rejected: faint blips with sharp onsets (the v1.1
flurry) would retrigger through it and regress the crackle fix —
amplitude alone separates residue from repicks.

The fixed 60 ms window was therefore REPLACED, not retained. Public
controls still Threshold + Release only; no latency added; RT contract
unchanged (two scalar ints + enum replace two scalar ints; still no
allocation/locks/IO, deterministic).

### Expected behavior

What should change: sustained notes close exactly once and stay shut
through arbitrarily long beating tails; no stutter.

What should remain unchanged: brutal 52 ms stop/start feel, immediate
opening from fully closed, v1.1 release contour, attack immediacy for
real picks, bypass, all Trims/Drive/NAM/IR.

### Listening guide (for the audition below)

1. Physically: once the gate commits to closing, only a new attack 12 dB
   above Threshold reopens it; tail beating below that rides the fade to
   silence. After 250 ms of true quiet the normal threshold resumes.
2. Changing: gate retrigger logic only (state machine + bar + confirm).
3. Constant: Input Trim 0 dB, TightDrive 0.85/0.50/0.70, 6505 unboosted
   NAM, Mesa V30 IR, Output Trim +2.6 dB, Threshold -50 dB, Release 52 ms.
4. Should feel like: stops and sustained notes end cleanly with a single
   decisive close; fast repicks and chugs respond exactly as before.
5. Bug indicators: any stuttering tail (bar too low); missed soft repicks
   just above -38 dBFS during a ringing tail (bar too high — but see the
   documented tradeoff); sluggish attacks (must not happen: attacks clear
   the bar by 20+ dB with zero added delay).

Audition command (flag syntax verified against app/TdmLive.cpp; substitute
your local 6505_unboost.nam and Mesa V30 IR paths):

./build/tdm_live --nam <path-to>/6505_unboost.nam --ir <path-to>/MesaV30.wav \
  --input-trim 0 --gate-thresh -50 --gate-rel 52 \
  --tight-drive --tight 0.85 --drive 0.50 --bite 0.70 \
  --output-trim 2.6

### Listening result

NOT YET AUDITIONED. Offline evidence: the probe-measured 11-cycle
stutter collapses to exactly 1 open + 1 close; repicks at -12/-24/-32/-36
dB reopen within ~0 ms during CLOSING; chugs unaffected. Ear confirmation
still required per project rules.

### Problems

None observed offline. Automated suite: 363 checks, 0 failures (82 new
v1.2 gate checks across 44.1/48/96 kHz). Pre-fix failure observed on the
live v1.1 tree via probes (11-cycle stutter on the same signal the new
sustained-note test uses); the new tradeoff/flurry tests contradict
v1.1's window mechanism by design (documented below). `make smoke`
green. Note: neither v1.1 nor v1.2 was committed to git, so no
pre-fix/post-fix binary diff exists beyond the probe logs; the v1.1
60 ms/+6 dB mechanism is fully described in the preceding log entry.

### Compromise involving soft intentional repicks (pinned tradeoff)

Repicks at -36 dBFS and hotter reopen within ~0 ms during CLOSING
(tested -12/-24/-32/-36). A repick BELOW the retrigger bar (-38 dBFS at
the -50 reference) that lands while a previous tail is still alive stays
shut until the tail dies plus the 250 ms confirmation — committed test
pins a -44 dB repick blocked during CLOSING yet opening normally from
CLOSED. Once fully CLOSED, picks just above Threshold (-48 dB tested)
engage with zero delay: no permanent penalty.

### Test changes (why window-era expectations moved)

- Flurry test (default -40 dB threshold): v1.1 expected one clean pair
  (post-window reopen); v1.2's bar (-28 dB here) has no expiry, so the
  whole -37 dB flurry stays shut: counts updated 2/1/4 -> 1/0/3. Blip2
  still opens once fully CLOSED.
- Attack trio: checks unchanged in substance (hot opens, faint blocked,
  late faint opens); comments updated from "window" to "CLOSING/CLOSED".
- Added: sustained low-E single-close (3 rates), low-B no-burst tail
  (3 rates), repick strengths -12..-36 within 2 ms (3 rates), sub-bar
  tradeoff pin, from-CLOSED near-threshold opening, -50 dB chug proxy
  (3 rates). Untouched and passing: bypass, release timing, attenuation,
  reset, knee contour, all Trim/Drive/NAM/IR/rig tests.

### Next step

Live audition with the command above at Threshold -50 dB: confirm single
decisive closes on sustained notes, intact fast repicks/chugs, and the
unchanged brutal 52 ms feel.

### Transferable learning

- A fixed-duration guard against a non-fixed-duration phenomenon
  (decaying/beating note) fails by construction once the timer expires
  mid-phenomenon. Make the guard a function of STATE (CLOSING until the
  tail is observably gone), not of elapsed time.
- Size state thresholds from measured tail statistics (worst beat peak,
  longest valley), not from musical time constants: +12 dB bar and
  250 ms confirmation came straight off probe maxima with ~6-12x margin.
- In CLOSING, "envelope above close level" must RESTART (not freeze) the
  tail-gone confirmation, or cumulative ripple valleys across beat cycles
  fake a dead tail and the stutter returns through the back door.
- A sharp-onset (transient) criterion cannot separate faint flurry blips
  from soft repicks — both have abrupt onsets. Amplitude is the only
  separator; name the resulting soft-repick tradeoff explicitly and pin it
  in tests.

### Senior DSP review requested

- +12 dB / 250 ms constants are sized from synthetic beating tails
  (worst beat +5.9 dB, longest valley 20.7 ms); confirm headroom for
  drop-tuned extended-range guitars with deeper/longer beating and
  hotter DIs (residue levels are absolute, threshold-relative margins
  shift with player level — the -50 dB reference assumes the auditioned
  DI).
- CLOSING band behavior (close..retrigger restarts confirmation
  indefinitely): a slow volume swell from a live tail that never exceeds
  the bar stays shut; assessed as contrived (real swells start from
  CLOSED and cross Threshold normally) but flagged.
- Pre-NAM placement question from v1.1 stands unchanged.

---

## Date

2026-09-16

### Module

ToneShape v1 (post-cab Weight / Contour / Presence)

### Hypothesis

Three linear post-IR controls can pull different amp captures into one
TechDeathMachine family (tight mass, separable mids, forward attack)
without erasing their character and without touching saturation (which
belongs to TightDrive/NAM, not EQ). Post-cab placement means the
controls shape what the speaker already filtered; linear means they can
only re-balance, never create fizz — so no anti-fizz stage is needed.

### Implementation

`dsp/ToneShape/ToneShape.h/.cpp`, inserted post-IR in `TechDeathRig`
(... → NAM → IR → ToneShape → Output Trim → broadcast). All sections
parallel form y = x + k*section(x), one-pole-smoothed k (tau ~10 ms),
scalar states only, RT-safe, opt-in bypass bit-exact, neutral bit-exact
from reset:

- Weight: parallel low shelf, corner 140 Hz, +/-6 dB (exact at DC).
- Contour: parallel bell, center 600 Hz, Q 1, +/-6 dB (exact at fc via
  numeric normalization of the realized biquad response). The bell is one
  fixed RBJ bandpass (constant 0 dB peak gain form, TDF2); only k moves.
- Presence: parallel high shelf, corner 3.5 kHz, +/-5 dB nominal
  (~+4 dB realized at 15 kHz/48 kHz — one-pole curve, Bite precedent).

Topology evidence (offline probes, stepped sines through both vendored
NAMs + test IR, fundamental tracking):

- 5150 vs 6505 diverge most at 150 Hz (-9.5 dB), 60 Hz (-7.9), 220 Hz
  (-7.1), 1200 Hz (-6.5), 620 Hz (-5.7), 2.2-5 kHz (-4..-5) — one axis
  per control, corners 140 / 600 / 3500 Hz confirmed, not fitted.
- A subtractive one-pole bell (LP1000-LP350) was measured and REJECTED:
  +2.7 dB interaction at 100 Hz at full body. The biquad bell measures
  <1.6 dB at 100 Hz / 3 kHz at full extremes (suite pins <2 dB).
- Fixed anti-fizz LPF considered and REJECTED: linear + post-IR means
  ToneShape cannot create fizz or alias products; a fixed LPF would only
  dull existing content. Harshness unresponsive to Presence-cut is an
  amp/IR choice by definition.
- Test-oracle lesson (cost one debug round): the parallel gain is
  |1 + k*H| with COMPLEX H — matching on magnitudes alone underpredicts
  by ~0.1 (weight LP lags ~78 deg at 1 kHz). The suite evaluates the
  digital-exact complex closed forms. Same trap likely lurks in any
  future parallel-topology test.

Automated suite: 485 checks, 0 failures (76 new: defaults/clamps,
bypass incl. enabled-neutral bit-exact, per-control spot gains at
44.1/48/96 kHz, skirts/locality, stability at both extreme corners,
mid-stream jumps reconverge, rig composition shape-x-trim). `make smoke`
green incl. a new `--tone-shape` render leg.

Direction check on real captures (linear post-EQ, same amp cancels):
6505 at 0.55/0.38/0.60 moves 620 Hz -1.3 dB and 4-5 kHz +0.4 dB as
specified; 5150 at 0.38/0.40/0.42 trims lows/mids/presence toward the
6505 with correct signs. Residual level gaps are saturation character —
correctly outside ToneShape's job (OutputTrim + TightDrive).

### Audition setup

Same guitar/DI for every take. Match loudness with Output Trim BY EAR
first (captures differ ~15 dB), then judge shape. Reference chain:
Gate -55 dB / 52 ms, TightDrive 0.85/0.50/0.70, 6505 unboosted + Mesa
V30 unless noted. Starting points (offline-derived, NOT ear-verified):

- 6505_unboost — (a) Neutral 0.50/0.50/0.50; (b) Stage scoop
  0.55/0.38/0.60.
- 5150II_crunch — (a) Family match 0.38/0.40/0.42; (b) Body kept
  0.50/0.45/0.45. Missing saturation is a TightDrive question, not this.
- Powerball OD1 (description-derived, unmeasured — no local capture) —
  (a) Tame bite 0.55/0.50/0.35; (b) Mass+ 0.62/0.55/0.40. Level-match
  with OutputTrim first (quiet capture).

Example: `./build/tdm_live --nam <6505> --ir <V30> --input-trim 0
--gate-thresh -55 --gate-rel 52 --tight-drive --tight 0.85 --drive 0.50
--bite 0.70 --tone-shape --weight 0.55 --contour 0.38 --presence 0.60
--output-trim 2.6` (or dial the same in `tdm_dev`, ToneShape ON).

### Expected behavior

What should change: per-amp low mass, mid boxiness vs separation, and
pick-attack forwardness, converging toward one family without the amps
becoming interchangeable.

What should remain unchanged: saturation/drive character per amp at
matched loudness (verify: same take at neutral vs shaped must differ
only in balance, never in gain feel); gate/trim/drive/NAM/IR behavior
(suites green, no files modified there); bypassed rig bit-exact
(verified); parameter moves zipper-free (smoothed k).

### Listening result

NOT YET AUDITIONED. Confirm per starting point: palm-mute weight vs
boom, single-note separation vs hollowness (Contour cut too far hollows
chords), attack clarity without scratch, and that neutral sounds
identical to shape-off (it is bit-exact — any audible difference there
is a bug).

### Problems

None observed offline. Known non-issues: Weight boosts DC up to +6 dB
(no DC blocker in v1, rig-wide stance); enabled-neutral is bit-exact
only from reset (after moves it converges to float noise); top-octave
Presence realizes ~4 dB not 5 dB (documented one-pole curve).

### Next step

Run the per-amp starting takes above; promote winners toward curated
defaults only from listening evidence. Candidates if listening demands:
Contour Q exposure, Weight corner shift for 8-strings, per-NAM
compensation metadata (preset layer calls the same setters).

### Transferable learning

- Parallel y = x + k*section(x) with smoothed k is now a 3-for-3 proven
  house pattern (Bite, Weight/Presence, Contour): neutral-bit-exact,
  zipper-free without coefficient interpolation, trivially RT-safe.
- A fixed-coefficient biquad inside a smoothed parallel mix gives
  parametric-grade skirts with zero coefficient-zipper risk — the right
  shape whenever a bell is needed; still needs senior review as the
  first biquad in the codebase.
- Always test parallel topologies against complex |1 + k*H|, never
  against magnitudes.

### Senior DSP review requested

- RBJ bandpass coefficient derivation + numeric normalization approach
  (first biquad in the repo; suite pins center/skirts at 3 rates).
- Corner/width choices (140 Hz shelf, 600 Hz Q1 bell, 3.5 kHz shelf)
  against real-cabinet tech-death targets — broad by design, confirm no
  howler.
- Weight DC gain (+6 dB on NAM DC offset) and the standing no-DC-blocker
  stance for a post-IR linear stage.

---

## Date

2026-09-17

### Module

Space v1.1 (pair-equalized ping-pong + 8-line FDN reverb; first
real-guitar audition results for v1 + redesign)

### Hypothesis

The v1 audition confirmed the architecture (dry intact, series routing,
LF control) but indicted two structural properties: the ping-pong's
per-echo decay leans every decaying alternating train toward its lead
side, and 4 sparse comb modes ring metallic no matter the tuning. v1.1
fixes both structurally: equal-gain L/R echo pairs (the only monotonic
envelope with exact cumulative balance) and an 8-line FDN tank whose
density comes from Householder mixing, not tuning luck — with no new
public controls.

### Implementation

`dsp/Space/Delay.h/.cpp` — pair-equalized ping-pong. The L->R seed is
unity (gated near fb=0); the R->L return carries the pair decay fb^2.
Echoes arrive 1,1,F,F,... with strict L/R alternation and unchanged
Time meaning. All gains <= 1 (round trip <= fb^2 = 0.72: stability
trivially preserved), unity DC (sustained content balances exactly),
echo 1 stays a perfect dry copy. R-seed gate is a linear ramp 0..0.1
on the smoothed fb, so fb=0 is still a single L echo with R exactly
silent. Default Time 375 -> 220 ms (connected technical-lead range;
semantics untouched). Rejected alternatives: fixed R boost (unbalances
sustained notes — damping is transparent to them — and risks loop gain
> 1), input damping (dulls the first echo's attack), alternating lead
side per transient (stateful, lab territory).

`dsp/Space/Reverb.h/.cpp` — 8-line FDN replacing Schroeder/Moorer.
Send HP 180 Hz kept (audition-approved); 2 series input-diffusion
allpasses (~8/12 ms, g=0.6); 8 incommensurate tank lines (~31-49 ms)
with per-line 5.5 kHz damping + T60 gains (same formula: Decay 0..1 ->
0.25..5 s, rate-independent, 50 ms morph); Householder feedback matrix
(orthogonal: unconditionally stable with per-line gains < 1); stereo
from disjoint even/odd line sets (decorrelated, energy-matched). No
internal modulation (moving-pitch risk on hi-gain guitar; revisit only
if long decays still ring). Public API unchanged (Decay/Mix only).
~60 flops/sample, ~80 KB @48 kHz (~160 KB @96 kHz). Rejected: retune
(topology-limited, measured), Dattorro plate (too much machinery for
v1.1), nested allpass (decay hard to control).

Offline numbers: full suite 584 checks / 0 failures (21 new). Delay:
guitar-content wet L/R = 1.002 (was 28:1), sustain 1.09, impulse 3.5
(HF-only worst case, documented); per-side DC-step pins pair decay +
one-generation R lag exactly; fb=0 R bit-silent; near-zero fb graceful.
Reverb: late crest ~3.9 (near-Gaussian; v1 measured 10+), spectral
flatness ~-2 dB (white noise ~-2.5; v1 peak/avg 6-9x), slopes agree
44.1/48/96 within ~4%, DC leak ~1e-8, 80/440 Hz wet ratio 0.17,
extremes finite/bounded. Render proof: first 400 samples bit-identical
dry vs space; tail differs 8.9 peak.

### Audition setup

Same guitar/DI A/B procedure as v1. Reference chain unchanged (Gate
-55 dB / 52 ms, TightDrive 0.85/0.50/0.70, 6505 unboosted + Mesa V30).
New starting takes (exact settings — set by ear from here):

1. subtle rhythm space: Delay OFF; Reverb ON, Decay 0.15, Mix 0.10.
   Room glue only; mutes must still feel instant.
2. technical lead: Delay ON, Time 220 ms, Feedback 0.40, Mix 0.22;
   Reverb ON, Decay 0.40, Mix 0.18. Pairs sit under fast runs; 16th
   separation must survive.
3. large atmospheric lead: Delay ON, Time 300 ms, Feedback 0.50,
   Mix 0.28; Reverb ON, Decay 0.70, Mix 0.26. Wash allowed, dry
   attack must still lead every note.

CLI: same flags (`--delay-time 220 ...` in the smoke leg). Dev app:
sliders follow the new default automatically (constant-sourced).

### Expected behavior

What should change vs v1: wet image centered (no left lean);
repeats feel connected rather than separated (pairs + shorter Time);
reverb reads as room/depth, not metal/phase; atmospheric take lush,
not cheap.

What should remain unchanged: everything the v1 audition approved —
dry attack, LF control, depth-without-wash on fast playing, mute
tightness, mono fold-down.

Bug tells: left lean on held chords (pair balance broken); R answer
missing at low fb (seed gate stuck); sudden full R echo when nudging
fb off 0 (gate discontinuity); metallic tail at Decay 0.7 (FDN still
too sparse -> modulation revisit); dull first repeats (damping wrong).

### Listening result (v1 real-guitar audition — the input to v1.1)

Delay: alternation clearly audible and correct; dry attack intact;
technical-lead delay adds depth without washing fast playing. BUT:
375 ms feels too separated, and the wet image leans noticeably left.
Reverb: low-end control good; dry attack intact. BUT: metallic/phasey,
no convincing room; atmospheric setting cheap rather than lush.
=> v1.1 NOT YET AUDITIONED. Play the three takes above before calling
it finished; the open question is perceptual, not numerical (numbers
are all green): does the FDN actually sound like a room on hi-gain
guitar, and do pairs feel connected at 220 ms?

### Problems

Two test-setup traps during v1.1 (both DSP-correct, both instructive):
params set AFTER reset exercise the intentional smoothing paths, so
landing/pinning tests must set params BEFORE reset (documented snap);
and the R side structurally lags one generation after input stops, so
pair assertions must be per-side, not mono-sum (mono ratios 0.625/0.4
looked like a DSP bug; per-side they are exactly 0.25/1.0). Also: an
initial FDN build shipped a mean-removal matrix mislabeled Householder
(stable but thinner); probes caught it before tests were written.

### Next step

Real-guitar audition of the three takes above. Candidates if listening
demands: per-line modulation depth exposure (only if ringing
persists), delay tone control (only if repeats dull/harsh), reverb
pre-delay (only if articulation needs more room), ducking hooks
(still out of scope).

### Transferable learning

- With monotonic decay + fixed lead side + strict alternation, EXACT
  cumulative balance forces equal-gain pairs — a proof, not a tweak.
  Any future bouncing effect inherits this.
- Balance must be probed with representative spectra, not impulses:
  an impulse overstates one extra damping stage 8x (28:1 vs 1.002 on
  guitar content). A fixed compensation gain would have "fixed" the
  impulse while unbalancing sustained notes.
- Objective metallic metrics that work: short-window late crest
  (~Gaussian = dense) + spectral flatness in dB (white noise ~-2.5).
  Full-window crest is envelope-inflated; raw peak/avg sits near the
  DFT noise floor (~7) and cannot discriminate.
- Householder factor is (2/N), not (1/N) — the mislabeled version is
  still stable (eigenvalues 0/-1) so nothing caught it but a probe.

### Senior DSP review requested

- Pair-decay Feedback semantics (per-pair fb^2 vs per-echo fb):
  confirm the knob still feels right across 0..0.85 — pairs at 0.35
  decay fast (1,1,.12,.12), which suits leads but changes the knob's
  learned feel from v1.
- FDN line lengths (31-49 ms) + diffusion (8/12 ms, g=0.6) + no
  modulation: chosen from standard practice + probes, not ears yet.
  Flag any howler risk for hi-gain (e.g. diffusion AP ringing on
  palm mutes).
- Seed-gate knee (0.1, linear): inaudible in probes, but confirm no
  better idiom (e.g. gate on target instead of smoothed value).
- Spectral-flatness bound (-4 dB) as a permanent metallic guard: is
  it the right test, or does it risk blessing a flat-but-dead room?

---

## Date

2026-09-17 (later)

### Module

Space v1.2 (modulated FDN reverb; v1.1 real-guitar audition results +
redesign. Delay untouched — auditioned good.)

### Hypothesis

The v1.1 audition kept the architecture verdict (dry intact, LF
controlled, Delay now centered and connected) but convicted the static
8-line FDN heard alone: metallic/phasey, cramped, opens up far less
than Decay promises. Diagnosis: 31-49 ms tank lines ring a coarse
modal grid (small-room signature); 2 short diffusers leave a sparse
early response; zero modulation pins every mode at a fixed frequency
so the tail can never evolve. v1.2 adopts Dattorro's three signature
ideas into the validated FDN tank — fixed predelay, deep 4-stage
diffusion, slowly modulated tank delays — instead of transcribing the
plate verbatim (lower risk, same perceptual family: modulated FDNs
are the modern plate/room standard).

### Implementation

`dsp/Space/Reverb.h/.cpp` only — public API identical (Decay/Mix,
same T60 0.25..5 s mapping, same 180 Hz send HP, same 5.5 kHz tank
damping, same Householder mixing, same even/odd stereo pickups):
mono in -> fixed 8 ms predelay -> send HP -> 4 series diffusion
allpasses (~4/6/9/14 ms, g=0.7) -> 8-line tank, 2 short (~28/36 ms)
seeding early energy + 6 long (60-86 ms) giving size -> stereo out.
Each tank line reads through its own slow wobble (+/-0.125 ms at
0.11-0.43 Hz per line, linear-interp reads, phases parked in reset):
max pitch deviation well under a cent — modes walked, never chorus.
One modulated tap per line feeds damping, mixing, and output alike.
Interpolation is a convex combination and only READ positions move,
so loop gains — and the energy-contractive stability proof — are
untouched by modulation. ~60 flops + 8 sinf per sample, ~80 KB @48
kHz (~200 KB @96 kHz). Rejected: verbatim Dattorro transcription
(fuzzy-memory constant risk), nested allpass (decay control), static
sign-pattern stereo widening (per-ear comb-coloration risk).

Offline numbers: full suite 589 checks / 0 failures (5 new). Sine-wet
wander 1.52 dB (v1.1: 0.001 — lower test bound is the modulation
regression guard; upper bound the anti-chorus guard); late-tail L/R
rho ~-0.44 with energy match ~1.0 (width without Delay); crest
~3.1-3.3 (was 3.9); flatness ~-1.1 dB; decay slopes agree 44.1/48/96
within ~2%; wet onset exactly 36.0 ms at all rates (8 ms predelay +
28 ms shortest line — rate-independent in TIME); DC leak ~1e-8;
80/440 Hz wet ratio 0.05 (was 0.17); 10 s max-decay run finite, peak
0.054, still decaying. Render proof (reverb ALONE, Decay 0.7 Mix
0.3): first 400 samples bit-identical to dry; tail differs 0.25 peak.

### Audition setup

Same guitar/DI A/B as v1/v1.1. Reference chain unchanged. New takes
— the key change: judge the Reverb takes with Delay OFF first
(acceptance: convincing alone; acceptable-only-with-delay is a fail):

1. subtle room/space: Delay OFF; Reverb ON, Decay 0.18, Mix 0.12.
   Glue, not wash; mutes instant; must sound like a room, not a pedal.
2. technical lead: Delay ON, Time 220 ms, Feedback 0.40, Mix 0.22
   (unchanged — auditioned good); Reverb ON, Decay 0.40, Mix 0.18.
   First pass with Delay OFF: lead must still sit in a space.
3. lush atmospheric lead: Delay ON, Time 300 ms, Feedback 0.50,
   Mix 0.28; Reverb ON, Decay 0.70, Mix 0.26. Delay OFF must still
   feel large/open/wide — this is the v1.1-failure case, re-test it.

CLI unchanged (`--reverb --reverb-decay 0.7 --reverb-mix 0.3 ...`).
Dev app unchanged (constant-sourced).

### Expected behavior

What should change vs v1.1: reverb alone reads as space/depth, not
metal/phase; long Decay genuinely feels large; stereo width obvious
on headphones with no delay; no fixed pitches emerging over seconds.

What should remain unchanged: everything v1.1 earned — dry attack,
LF control, Delay behavior (untouched, still centered/connected),
mono fold-down, mute tightness.

Bug tells: slow audible pulsing/beating (modulation too deep/fast);
fixed whistling pitch in long tails (wobble not smearing — deepen?);
room arriving audibly late/detached (predelay too long for the take —
setting issue unless 8 ms already disconnects); dull wash (damping);
narrow image with reverb alone (stereo still weak — next lever is
pickup diversity, not wider constants).

### Listening result (v1.1 real-guitar audition — input to v1.2)

Delay: good, centered, 220 ms connected, dry intact — DO NOT TOUCH.
Reverb: improved over v1, lows controlled, dry intact; but alone
still metallic/phasey, cramped at high Decay, opens up less than
expected; atmospheric gets wider but not lush; with Delay it becomes
acceptable (delay masks the reverb character). Verdict: reverb must
stand alone.
=> v1.2 NOT YET AUDITIONED. Same honest status as every prior round:
numbers green, ears pending. The perceptual risk is stated plainly —
no probe hears "lush".

### Problems

None observed offline. One near-miss: an early DC probe over an
unsettled window read -1.1e-6 and looked like HP leakage; the
committed-shape window ([2.5,3.0] s) reads ~1e-8 at all rates —
transient, not leak. Lesson logged: settle-time-aware windows for
leak tests.

### Next step

Real-guitar audition of the three takes above, Delay-OFF-first.
Candidates if listening demands: modulation depth exposure (only if
ringing persists AND wander tests stay green), pickup-diversity
stereo (only if width still weak), predelay exposure (only if 8 ms
disconnects rhythm takes), damping corner (only if wash dull).

### Transferable learning

- Dattorro's ideas port as principles (predelay gap, diffusion depth,
  modulated tank, pickup diversity) without transcribing his network:
  keep validated machinery (T60 mapping, orthogonal mixing), graft
  the perceptual levers.
- A "modulation alive" test needs BOTH bounds: lower (guards
  frozen-tank regression — v1.1 measures 0.001 dB) and upper
  (anti-chorus). One-sided bounds would bless either failure.
- Wet-onset time (first nonzero sample) is a free rate-independence
  check when lengths scale: 36.0 ms at all three rates.
- Early-response holes are structural: tank output starts at the
  shortest line, so short seed lines (not louder diffusion) are the
  fix that preserves the pure-dry window.

### Senior DSP review requested

- Modulation depth/rate choice (+/-0.125 ms, 0.11-0.43 Hz): probe-set,
  not ear-set. Too subtle to matter, or enough? Flag chorus/pulsing
  risk on sustained hi-gain notes especially.
- Mixed tank lengths (28/36 ms + 60-86 ms): short lines at high Decay
  carry near-unity gains on a coarse grid — flutter risk the metrics
  may miss; ears must confirm.
- Width strategy (disjoint sets + diverging phases, no sign tricks):
  rho ~-0.44 measured; if audition says narrow, is pickup diversity
  (at static-coloration risk) the right next lever?
- Wander bounds (0.2 / 4.0 dB): does the lower bound pin "enough
  movement to matter" or merely "nonzero"?

---

## Date

2026-09-18

### Module

Space v1.3 (dual-tank cross-coupled plate; v1.2 real-guitar audition
failure + redesign. Delay accepted — untouched.)

### Hypothesis

The v1.2 audition's spatial complaints (center-heavy, spreads-then-
collapses, "inverted" vs Delay, never lush alone) share one structural
root: a single fully-mixed tank is ONE resonant field, so both ears
ring the same modes at similar levels — and sustained tonal guitar
correlates across ears no matter how decorrelated the impulse tail
measures. Probes confirmed the mechanism precisely: a 1760 Hz band sat
at side/mid 0.32 while neighbors read 1-5 (a centered metallic band in
the harshness zone). v1.3 splits the field in two: two tanks with
different modal grids, differential drive, slow rotation cross-feed
with no favored common mode, and a sign-designed output stage. Width
becomes structural (different pitches per ear) instead of statistical.

### Implementation

`dsp/Space/Reverb.h/.cpp` only — public API identical (Decay/Mix,
same T60 0.25..5 s mapping, same 180 Hz send HP, same 5.5 kHz damping,
same 8 ms predelay, same 4-stage diffusion, same subtle wobble):
two tanks of 4 modulated lines (A ~269 ms total, B ~277 ms, all eight
lengths distinct), per-tank Householder-4 mixing, per-pair rotation
cross-feed (full A<->B exchange over ~0.35 s, coefficients derived in
reset), differential drive (+dif to A, -dif to B), outputs L =
own-tank sum with NEGATIVE cross bleed (mirrored for R). Mixing is
exactly energy-preserving (Householder rows + rotation pairs, both
orthogonal), so with loop gains < 1 the tank is strictly contractive
whatever the modulation does. The bleed sign is a deliberate tradeoff
knob: +0.25 measured center-heavy (S/M 0.36); -0.20 wide (S/M 2.3)
but spiky bins; -0.15 splits it (S/M ~2.1, mono ~0.32). Mono
fold-down keeps ~1/3 energy by construction (shared common content,
same sign). ~75 flops + 8 sinf per sample, ~80 KB @48 kHz (~160 KB
@96 kHz). Rejected along the way: static sign-pattern widening
(per-ear comb risk), deeper wobble as the width lever (wrong axis —
width is structural), pinning single-bin spread (modulation luck, not
health).

Offline numbers: full suite 599 checks / 0 failures (10 new). S/M arc
flat ~2.1-2.5 across the whole tail (was 0.36 center-heavy, no
collapse); triplet-band min 0.99, spread 1.7x; tonal floor 0.67+ at
every driven pitch; fold-down 0.32; L/R balance 0.96-1.21 over time
(no rotation seasickness); wander 1.31 dB (inside the 0.2/4.0 guards
unchanged); crest ~3.9; flatness ~-1.6 dB; slopes agree 44.1/48/96
within ~1%; onset exactly 37.9 ms at all rates; DC ~1e-8; 80/440 Hz
wet ratio 0.04; extremes finite/bounded. Render proof (reverb ALONE,
Decay 0.7 Mix 0.3): first 400 samples bit-identical to dry; tail
differs 0.25 peak.

### Audition setup

Same guitar/DI A/B as ever. Reference chain unchanged. Takes below —
judge Delay-OFF first as before (convincing alone or fail):

1. subtle ambience: Delay OFF; Reverb ON, Decay 0.18, Mix 0.12.
   Glue, not wash; mutes instant; must sound like air, not a pedal.
2. technical lead: Delay ON, Time 220 ms, Feedback 0.40, Mix 0.22
   (unchanged); Reverb ON, Decay 0.40, Mix 0.18. Delay-OFF pass:
   lead must sit inside a believable space.
3. lush atmospheric plate: Delay ON, Time 300 ms, Feedback 0.50,
   Mix 0.28; Reverb ON, Decay 0.70, Mix 0.26. Delay OFF must feel
   large, open, EDGED — width with left/right extremities, not a
   bigger center. This is the twice-failed case; A/B it against v1.2
   memory explicitly.

CLI unchanged. Dev app unchanged (constant-sourced).

### Expected behavior

What should change vs v1.2: reverb alone reads wide with extremities
from the first second; no frequency region sits centered while the
rest blooms; long Decay feels large and keeps evolving rather than
settling into a centered hum; high sustained notes bloom wide
instead of whistling down the middle.

What should remain unchanged: everything earned so far — dry attack
(onset bit-exact 38 ms), LF control, Delay behavior (untouched),
mute tightness, mono usability (fold-down keeps ~1/3 energy;
isolated bins may thin in mono — known plate tradeoff, dry carries).

Bug tells: image leaning/pumping slowly L-R-L (rotation slosh too
deep — shorten cross-time?); a pitch beaming down the middle on some
note (a band collapsed — which band?); phasiness on palm mutes
(diffusion ringing?); wash arriving detached (predelay — setting
unless 8 ms already disconnects); dullness (damping).

### Listening result (v1.2 real-guitar audition — input to v1.3)

Delay accepted, do not touch. Reverb: better than v1.1 but not
convincing — still metallic, still cramped, energy center-heavy with
only a smaller portion reaching the field; tail spreads slightly then
collapses; "inverted" vs the naturally wide Delay; long Decay never
lush or open; acceptable only WITH delay (masking, not blending).
Dry intact throughout.
=> v1.3 NOT YET AUDITIONED. Numbers green, ears pending — as always,
no probe hears "lush", and this round's probes were explicitly tuned
not to be gamed (behavioral bands, floors, arcs — see Problems).

### Problems

Two near-misses, both caught by probes before tests were written.
First: the initial build used positive output bleed and measured
S/M 0.36 — the SAME center-heaviness as v1.2 wearing a new tank.
Derivation (not tuning) located it: mutually-uncorrelated tank sums
make the bleed sign the whole field ((1-k)/(1+k))^2, so the fix was
one sign, honestly probed at three settings. Second: deeper wobble
(0.125 -> 0.25 ms) spiked single-bin side/mid ratios (bins near
anti-phase read 6-9) while regions stayed smooth — single-bin spread
is modulation luck, so the committed test pins energy-averaged
triplets instead; the isolated-bin mono thinning is logged as accepted
plate behavior (broadband fold-down unaffected, dry carries).

### Next step

Real-guitar audition of the three takes above, Delay-OFF-first, with
explicit v1.2-memory A/B on take 3. Candidates if listening demands:
bleed constant (one-line width/mono lever, tradeoffs documented),
cross-time (evolution speed), wobble depth (only if ringing persists
AND wander tests stay green), pickup tap points (only if width still
weak). No new UI controls until ears settle the constants.

### Transferable learning

- One fully-mixed tank = one modal field = centered tonal ringing no
  matter the impulse-tail decorrelation. Steady-state tonal probes
  (Goertzel per bin under chord drive), not impulse rho, track what
  guitarists hear. Any future stereo space effect gets the tonal
  probe before any topology debate.
- With mutually-uncorrelated sub-signals, an output MIXING SIGN is a
  whole-field lever ((1-k)/(1+k))^2 — derive it, probe three points,
  pick the middle. Signs are architecture, not tuning.
- Rotation (no real eigenvectors) is the collapse-proof coupler:
  difference norm preserved by construction, common mode never
  favored — the property Householder-8 lacked. Prefer it wherever two
  fields must stay different forever.
- Test the disease, not the metric: single-bin spread measured
  modulation luck; triplet regions measure collapse. When a metric
  wiggles under a mechanism you intend to keep, widen the metric,
  not the design.

### Senior DSP review requested

- Bleed constant -0.15: the width/mono tradeoff is derived and
  probed, but "wide enough to satisfy ears" is unproven — is 2.1 the
  right neighborhood, or should the default stance be wider still?
- Isolated-bin mono thinning (bins near anti-phase lose room content
  in fold-down): accepted as plate behavior with dry carrying —
  confirm acceptability, especially at 440 Hz (measured 8.2 side/mid
  single-bin; regions unaffected).
- Cross-time 0.35 s: evolution speed is probe-silent (balance flat
  0.96-1.21 either way) — only ears can say whether the field
  evolves too fast/slow/still.
- Tonal floor bound (0.5, measured 0.67): thinnest margin in the
  suite — tripwire or false-alarm risk on future retunes?

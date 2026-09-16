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

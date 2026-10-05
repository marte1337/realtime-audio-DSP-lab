# SLAM architectural study — living notes

Branch: `research/slam`. Production untouched; all SLAM DSP under `dsp/lab/Slam/`.

## 1. What SLAM means measurably (§2)

Perceptual goal: huge physical impact around palm mutes / breakdown accents,
main guitar stays intelligible. Metrics explain behavior; ears decide.

Per render (whole-file + per-event windows from `build/slam_di_map.txt`):

- peak dBFS, RMS dBFS, crest factor dB (peak − RMS over event window)
- band RMS (2nd-order Linkwitz-ish-ish biquad bank, dBFS):
  sub 20–65 Hz / low 65–160 / lowmid 160–400 / mid 400–2k / pres 2k+
- HF preservation: pres-band RMS wet vs dry (dB delta, want ≈ 0)
- low-mass added: low+lomid RMS wet vs dry during event windows
- recovery: RMS in [80, 160] ms after each sparse-hit onset, wet vs dry
  (want ≈ dry: no mud tail between hits)
- gap floor: RMS in the silence/gap just before each sparse hit (mud
  accumulation check for fast chugs: compare early vs late in S4)
- clipping: samples with |x| ≥ 0.99
- spectral centroid + 85% rolloff (FFT, event windows only)
- trigger log (candidate D/E): fire times, strengths, false fires in gaps
- added latency: samples of deterministic delay (all candidates causal,
  minimum-phase → expect 0 + group-delay note)
- CPU: µs per 1024-frame block, candidate only, release build

Comparisons always raw AND level-matched: matched set scales each render so
its event-window RMS equals dry's (single scalar per render, reported).

## 2. Candidates

- A SlamPre: parallel low-band comp+sat, inserted pre-NAM (two tap points:
  A1 before TightDrive, A2 after TightDrive / before NAM).
- B SlamPostNam: parallel low-band comp+sat on NAM output, pre-IR.
- C SlamPostIr: parallel low-band comp+sat on finished cab tone.
- D SlamImpact: transient-triggered decaying low burst + brief low-band
  bloom; silent otherwise.
- E (hybrid, only if A–D justify it): D's detector gating C's branch.

Frequency sweep per parallel candidate: LP corner {70, 120, 200, 320} Hz.
Amount sweep: {0.5, 1.0, 1.5} on the default context, then finalists only.

## 3. Contexts

- Amps: 5150II_crunch (tight modern) vs 6505_unboost (looser).
- IR: test_cab (single available cab; second cab N/A — documented gap).
- Drive: TightDrive defaults ON vs OFF.
- Transpose: OFF vs −2 vs −7 (production GuitarTranspose in harness).
- Gate: bypassed (study inputs are clean by construction).
- ToneShape: enabled at defaults (matches a real session; neutral-ish).
- Space: bypassed (mono study; Space is linear stereo, orthogonal).

Dry-chain sanity: harness dry render must be BIT-EXACT vs `tdm_render`
with equivalent flags before any candidate result is trusted.

## 4. Material

`scripts/slam_synth_di.py` → `build/slam_di.wav` + `build/slam_di_map.txt`.
Deterministic KS plucks (labpitch precedent). No real recorded DI exists in
the repo — documented limitation; sections below.

Also reuse `build/labpitch_di.wav` (regenerated from committed script) as a
second opinion for finalists.

## 5. Results log

### 5.0 Harness validation

Study dry render is BIT-EXACT vs `tdm_render` with equivalent flags
(`cmp build/slam_ref.wav build/slam/dry.wav` identical, peak −4.49 dB on
`build/slam_di.wav` through 5150II_crunch + test_cab + drive@defaults).
All candidate results below share this verified chain. Transpose-ON runs
are self-consistent within the harness (steady-wet from sample 0; rig
engage ramp deliberately not replayed — see SlamStudy.cpp header).

### 5.1 Default-context sweep v1 (5150II + drive@defaults, transpose off)

All candidates add low/lowmid mass with presence intact (pres delta ≈
−0.1..−0.4 dB, HF preserved). CPU ≈ 13–15 µs/1024-block for every
candidate (≈0.07% of a core — negligible). Added latency: 0 samples
(all causal minimum-phase; D's burst starts 8 ms after the crossing by
design, masked by the attack).

- A1 (pre, before drive) @140: lowmid +5.5, evtCrest +0.1, recov +0.2.
  A1_70 ≈ +0.0: anything below ~100 Hz added pre-drive dies in the
  TightDrive HPF (113 Hz @ tight 0.5) + cab. A1_320 ducks presence
  (−1.4 dB): wide pre mass pushes the amp into compression that eats
  pick attack. A1 vs A2 (post-drive tap) nearly identical (+5.5 vs
  +4.8) — tap point before/after TightDrive hardly matters.
- B (post-NAM, pre-IR) @200: lowmid +8.7, evtCrest +1.2, recov +0.4.
  Stronger than A (no drive-HPF loss; cab shapes the branch).
  No clips anywhere in the sweep.
- C (post-IR) @220: lowmid +9.2, evtCrest +1.0, recov +0.4. Strongest
  raw mass, but NOTHING downstream absorbs peaks: x1.5 clips 48
  samples. Headroom is C's weakness.
- D v1 failed twice, instructively: (1) dpre fired 17x but measured
  +0.01 — a 70→48 Hz burst is erased by drive HPF + cab; (2) the
  post-IR detector missed 7/8 breakdown repeats (compressed sustain
  masks flux) and alternate hits (floor release 900 ms too slow).

### 5.2 Detector + burst rework (D v2)

- Floor 30/150 ms (was 120/900): repeated equal 8ths retrigger, one
  hit's decay never refires.
- 8 ms confirm window: fire only if flux still ≥45% of window peak
  (real attacks sustain; lone spikes rejected — unit-tested).
- Velocity from measured attack peak (was crossing-time sample).
- Burst band follows tap: pre = 160→110 Hz (above drive HPF),
  post = 70→48 Hz (true sub/low); bloom LP = 1.5x sweep start.
- dpre: 23 fires (6/6 sparse, 8/8 breakdown, no doubles, velocities
  0.73–0.92) but fire-window RMS only +0.16 dB — the AMP compresses
  the burst away. Pre-amp transient impact is weak by physics.
- dpost: 13 fires (post detector still misses repeats; sensitivity a
  no-op 9–15 dB) but where it fires: +5.6 dB peak, recov +0.00.
- => dhyb (split tap: pre-drive detector + scheduled post-IR burst):
  23/23 attacks incl. all chords, +5.6 dB fire peaks, recovery +0.00.
  Burst gain trimmed 0.9→0.75 + bloom 2.2→2.0 to kill 15 clip samples.

### 5.3 E (gated branch) = honest negative

Same 23 split-tap fires, but fire-window RMS +0.04 dB, peak −0.10 dB.
Root cause: the 8 ms confirm delay opens the gate AFTER the transient
peak, so the saturator multiplies only decayed signal. D works despite
the same delay because the burst BRINGS its own energy (decay spans the
window). Lesson: impact needs added energy AT the attack, not delayed
multiplication. (E could be revived with zero-lookahead eager firing —
spike false-fires cost little without an oscillator — but that is a new
thread; D answers the transient question for now.)

### 5.4 Amp context: 6505_unboost (looser, ~6 dB quieter dry)

- A collapses: a1_140 +5.49 → +1.81. Pre-NAM mass NEEDS amp
  compression to read as mass — it works by driving the amp, so it is
  inherently amp-dependent. (Product risk for A.)
- B/C hold: b_200 +8.65 → +9.63, c_220 +9.21 → +9.95. Post-amp mass is
  amp-independent (by construction).
- dhyb fire peaks +4.67 → +9.02 dB: FIXED burst gain does not track
  rig level. Product D needs level-adaptive burst gain (scale to
  trailing dry RMS or similar). No clips (dry is quiet), recov +0.01.
- Refire gap: breakdown 8ths @850 ms refire 8/8; groove 8ths @230 ms
  do NOT refire (first hit only); 16ths @100 ms correctly skipped.
  Matches the sparse-accent use case; faster grooves get first-hit-only.
- ehyb marginally less dead on 6505 (fire peak +3.8 dB — softer amp
  attacks peak later, gate catches more) but still negligible vs D.
- Mud: fastchug halves equal (+2.3/+2.4 B, +3.6/+3.7 C) — no
  accumulation over 4 s of 16ths at any amount. Continuous branches
  raise the floor (recov +0.4..+1.1) but do not run away.

### 5.5 TightDrive interaction: orthogonal

Drive OFF reproduces every candidate within ±0.2 dB (a1_140 +5.49 =
+5.49, b +8.43, c +9.33, dhyb 23 fires +2.9 crest). SLAM neither needs
nor fights TightDrive at these settings; the drive HPF only threatens
sub-100 Hz pre-mass that the cab kills anyway. A1-vs-A2 tap ≈ same.

### 5.6 Transpose interaction: orthogonal (−2 and −7)

−2 reproduces everything within ±0.3 dB (a1 +5.92, b +8.97, c +9.24,
dhyb +3.17 crest, 23 fires). −7: c +8.95, dhyb +3.12, 23 fires — deep
transpose does NOT pile up with SLAM into excess (same deltas, no
runaway). Detector works on transposed attacks (pass-1 stamps stay in
the transposed timeline; live needs --slam-delay-ms 16 with transpose).

### 5.7 Second material (labpitch_di, no map): consistent

b +8.71, c +9.27, a1 +4.17, dhyb 11 fires (fewer sparse events in that
material — expected). No material overfit.

## 6. Study answers (§15)

- Pre-NAM, post-NAM, or post-IR? POST-AMP (B/C/D-burst). Pre-NAM mass
  is amp-dependent (collapses +5.5→+1.8 on the looser amp) and
  sub-100 Hz pre-mass is physically erased (drive HPF + cab). Post-NAM
  (B) gets free cab shaping + downstream glue; post-IR (C/D) is most
  controllable. A is not dead — it is the "integrated amp" flavor —
  but it cannot be the only SLAM.
- Continuous or transient-triggered? TRANSIENT (D) for the stated
  goal. Continuous branches (B/C) deliver big mass numbers but raise
  the inter-hit floor (recov +0.4..+1.1); D adds +5.6 dB peaks with
  recov +0.00. Sparse breakdown accents are D's home ground.
- Sub, bass, or low-mid? POSITION-DEPENDENT: pre-amp must live
  120–220 Hz (low-mid thump; sub is erased); post-IR can live 50–90 Hz
  (true sub/low). The "mass" that survives the whole chain concentrates
  65–400 Hz. 320 Hz corners start ducking presence (−1~−1.4 dB) and
  sounding broadband-loud rather than heavy.
- Does saturation help or mud? Helps INSIDE a gated/parallel branch
  (B's asymmetric sat: even-harmonic density, no clips); continuous
  wideband sat accumulation was NOT observed (fastchug halves equal).
  Saturation's risk is headroom (C x1.5 clips), not mud.
- Does it need compression? The branch comp (fast attack, 90–160 ms
  release, ratio 5–10) is what turns "more lows" into "sustained
  weight"; uncompressed parallel lows were not separately tested —
  backlog if B/C productize.
- Attack/release for impact without smear? Detector: 0.3/8 ms flux vs
  30/150 ms floor + 90 ms refractory + 8 ms confirm. Burst: 1.5 ms
  attack, 130 ms decay, 70 ms bloom. Recovery is complete by ~200 ms.
- TightDrive: orthogonal (better AND redundant are both wrong — they
  stack linearly). Transpose: orthogonal at −2/−7.
- Genuinely useful or just louder? D: genuinely different behavior
  (recov +0.00 with +5.6 dB peaks cannot be loudness). B/C: MUST be
  judged on the _m matched renders — raw they are +1~2 dB louder and
  the loudness WILL fool ears.

## 7. Finalists + audition

1. dhyb (split-tap transient burst) — the SLAM concept winner.
2. c_220 (post-IR parallel) — most controllable continuous mass.
3. b_200 (post-NAM parallel) — integrated amp/cab violence.
   (a1_140 live-auditionable as the pre-NAM flavor; E documented dead.)

Live: `tdm_live --nam ... --ir ... --tight-drive --tone-shape
--slam d|c|a [--slam-band Hz] [--slam-amount 0..2]
[--slam-delay-ms 16]` (delay only with transpose on).
Renders: build/slam/{dry,a1_140,a2_140,b_200,c_220,dpre,dpost,dhyb,
ehyb}[_m].wav + build/slam6505 + nodrive/m2/m7/lp metrics.

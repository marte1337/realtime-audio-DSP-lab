# Special FX Reference Study — Laser & Glitch

Branch: `research/special-fx-reference` (from Alien checkpoint `8cd1c57`).
Date: 2026-10-11. Status: Laser prototype built + offline-verified, NOT yet
hardware-auditioned. Glitch is proposal-only.

Reference: Neural DSP Archetype: Misha Mansoor X, Special FX section.
Sources: official manual (`neuraldsp.com/manual/archetype-misha-mansoor-x`),
product page (`neuraldsp.com/plugins/archetype-misha-mansoor`), "Laser &
Glitch Deep Dive" video (`youtu.be/UTHPDyet7Jk`, title confirmed; description
carries no technical content), press reviews (MusicRadar, Guitar World,
Sound on Sound — single-source claims marked as such).

We build INDEPENDENT implementations with comparable musical behaviour.
No proprietary code, assets, presets, branding, or artwork is copied.

---

## 1. CONFIRMED (manual + product page)

### Laser
- Dual-algorithm pedal: **ring modulator** + **pitch shifter** (one or two
  voices); serves as "a pitch shifter, a wild ring modulator, or a lush
  chorus".
- Ring Modulator: "dissonant overtones that blend with your original
  signal" (dry blend exists). Pitch Shifter: "one or two pitch shifters
  ... warped harmonies to stacked intervals".
- **19 Laser Presets**, each "a different sound character" (preset =
  parameter bundle, not exposable DSP).
- **Pedal mode**: manual expression-pedal control. **Envelope mode**:
  envelope follower tracks amplitude, "like a virtual foot moving the
  expression pedal" — i.e. both modes drive ONE expression position.
- **Speed**: envelope response speed. **Sensitivity**: reaction strength;
  hint says low-output guitars should raise it (input-level calibration).
- **Trigger button** (Pedal mode only): forces expression to maximum
  while pressed (momentary).
- **Pedal** control: expression position, "blending smoothly between heel
  and toe settings" — heel/toe are preset-defined parameter endpoints.
- Stomp bypass.

### Glitch
- Granular pedal: chops signal into grains, reshuffles into new patterns.
- **Loop Length**: capture-buffer size. **Read Point**: playhead offset
  behind incoming audio — Pitch Delay & Texturizer only.
- Four modes: **Sequence** (long evolving note sequences), **Texturizer**
  (dense grain clouds, ambient), **Interfere** (repeater + automatic mix;
  high mix blends into or disrupts playing), **Pitch Delay**
  (classic delay-style; chaos control increases feedback).
- Grain: Length (tempo subdivs), Occurrence (tempo subdivs), Pitch Amount
  (random pitch), Pitch Pattern (up / down / both).
- **Chaos**: randomizes multiple internal params at once (low = structured
  rhythmic; high = evolving unpredictable).
- Mix section: **Filter** (tilt — clockwise cuts lows, counter-clockwise
  cuts highs), **Amount** (dry/granulated), **Sync** (host tempo or tap),
  Tempo stomp (internal tempo). Engage stomp bypass.

### Chain
- Manual tab order: Special FX → Pre FX → Amp → Cab → EQ → Post FX
  (Neural's convention: tabs follow signal flow).
- Special FX = Laser + Glitch "in series ... separately or combined".
- Pre FX includes overdrive/distortion AFTER Special FX.

## 2. THIRD-PARTY REPORTED (single sources, useful but unconfirmed)
- 13 pitch-shifting presets + 6 ring-mod presets = 19 (Guitar World).
- "Sweepable parameters", virtual treadle, "Trigger mini-switch ... for
  instantaneous and momentary application" (Guitar World / MusicRadar).
- Texturizer reverses grains; "luscious ... on clean and edge-of-breakup"
  (Guitar World).
- Possible Fractal Axe-Fx pitch-block inspiration (Sound on Sound
  speculation — architecturally suggestive: modifier-mapped parameters).

## 3. INFERRED (implied, not documented)
- Expression is a normalized 0..1 macro; presets map it to DSP params
  (ring frequency, shift amounts, mix); interpolation curves unknown.
- Envelope = peak/RMS follower with attack/release (Speed) + pre-gain or
  threshold (Sensitivity); exact detector and ranges unknown.
- "Lush chorus" from a pitch shifter implies detune voices (±cents) with
  dry blend, possibly LFO-modulated.
- Ring/pitch are likely preset-routed (select, series, or parallel with
  per-preset mix); topology per preset unknown.
- Laser→Glitch series order follows presentation order (weak inference).

## 4. UNKNOWN (needs controlled measurement or proprietary access)
- All 19 preset parameter values, sweep ranges, and curves.
- Pitch algorithm (polyphonic? latency? shift range? formant handling?).
- Envelope detector type, attack/release ranges, sensitivity law.
- Ring/pitch routing topology and per-preset dry/wet.
- Exact Laser↔Glitch order and per-pedal bypass routing.
- Any sonic-equivalence claim: NO controlled A/B performed.

## 5. OUR ARCHITECTURE (SpecLaser — independent, not a copy)

One normalized expression position 0..1 → per-voice heel/toe maps.
Sources: Manual slider/pedal, Envelope follower, momentary Toe (Manual
only, forces 1 while held). Four experimental voices (ours, not copies):

| Voice  | Heel (e=0) | Toe (e=1) | Notes |
|---|---|---|---|
| Whammy | unison (bit-exact dry leg) | +12 st single voice | linear st map |
| Dual | unison pair | V1 −12, V2 +12 (contrary) | 0.5 gains, no clip |
| Ring | 60 Hz carrier (growl AM) | 2000 Hz carrier (metallic) | log sweep |
| Detune | ±6¢ + LFO ±2¢ @0.15 Hz | ±24¢ + LFO ±30¢ @4 Hz | opposite-phase LFO |

Primitives reused, not duplicated: 2× AlienDoppler (pitch legs),
AlienRing (ring), AlienDsp SmoothParam/QuadOsc. All legs process every
sample (primed); voice select crossfades gains — no stale audio, ever.

Pitch tradeoff (documented in `SpecLaser.h`): Doppler variable-delay
gives sweep-continuous, polyphonic, low-latency pitch with static sag
at extremes (~−1.5 st at −12, ~−0.8 st at +12, measured toward unison).
Production GuitarTranspose was rejected for this role: 16 ms latency,
weak upward shifts, resync warble under continuous sweeps.

Envelope: peak follower, Speed → attack 100..1 ms / release 1000..30 ms
(log); Sensitivity → detector pre-gain 0.5..8× (default 0.6 ≈ 2.5×, so
typical DI peaks reach full sweep; post-amp taps turn it down).
NaN/Inf input sanitized (a NaN would otherwise wedge the follower).

Insertion: reference puts Special FX FIRST. Our DEV router (`DevSpecFx`)
runs twin engines at pre-gain (post-transpose, pre-TightDrive) and
post-IR taps with instant click-free A/B (dip + 50 ms history preroll).
Default: Pre. Expectation: pre = intermodulation through amp (harmonics
distorted together, more integrated); post = clean transform of finished
tone (more separable). Hardware decides.

## 6. GLITCH PROPOSAL (not implemented — Laser validates first)

Reusable granular engine for all four modes:

- **Capture**: circular buffer (Loop Length, e.g. 50 ms..4 s, tempo
  subdivisions in Sync), always recording input (pre-gain tap proposed,
  same position as Laser).
- **Scheduler**: sample-accurate grain clock; next-onset = Occurrence
  subdivision + Chaos jitter; tempo from host (VST later) or tap/knob.
- **Voices**: N polyphonic grain heads (N=8..16); each: read offset
  (Read Point + Chaos spread), rate (Pitch Amount × Pattern direction;
  1.0 = repeater), envelope (raised-cosine, 5..50% crossfade by Length),
  pan if stereo later (mono first).
- **Pitch**: grain-rate resampling (varispeed: pitch ∝ 1/rate, chipmunk
  on purpose — NOT transpose-corrected; that IS the glitch sound).
- **Randomness**: deterministic seeded PRNG (reproducible renders/tests).
- **Feedback**: Pitch Delay routes grain bus back to capture (Chaos→fb
  amount per manual); others unity.
- **Mix/Filter**: dry + grain bus through tilt filter (manual's Filter
  law) and Amount blend; Interfere auto-rides Amount with grain density.

Shared: capture, scheduler, voices, envelopes, PRNG, filter, mix.
Per-mode: Onset law (Sequence: long evolving = sparse long grains +
slow rate drift; Texturizer: dense clouds + reverse flag; Interfere:
repeater grid + auto-mix; Pitch Delay: delay-line grid + feedback),
Read Point enable (Pitch Delay + Texturizer only, per manual),
default Chaos/Pitch/Length/Occurrence maps.

Estimated scope: engine ~500 lines + tests + study + UI ≈ one focused
milestone AFTER Laser hardware verdict. No code written yet — deliberate.

## 7. VALIDATION STATUS
- `tdm_tests`: specfx suite 58 checks green (maps, sweeps, envelope,
  trigger, bypass, switching, rig isolation); full suite green.
- Study renders: 9 files (`/tmp/spec_renders`), peaks 0.63–0.71.
- CPU/voice: 15–40 µs/block (0.14–0.38%); twin-instance worst < 0.8%.
- Latency: 1680 samples (35 ms doppler bound; ring leg ~1).
- Synthetic signals prove MATH ONLY. Musical verdict needs hardware:
  Whammy pre sweep → Dual → Ring → Detune → Envelope → post A/B.

## 8. PERFORMANCE GESTURES (research/special-fx-performance, uncommitted)

Hardware accepted Whammy/Ring/Toe (preserved bit-exact, golden-pinned
vs `6113f3b`). Added: Toe Rise/Return (0..1000 ms, default 0 = legacy
stab exactly) and Divebomb (Dive button + Depth/Time/Return; dedicated
V3 leg, monotonic 0 → −depth, voice crossfades toward it; both control
modes). Precedence base < Toe < Dive via continuous functions of
independently-tracked gestures. Depth capped at 12 st: probe showed
−18/−24 smear (+2..+8 st, signal-dependent, mush below). Dive has no
reference counterpart (our design). Gestures are bools + ms through the
atomic architecture (MIDI-ready). Details: `docs/experiments.md`
2026-10-11 entry.

## 9. FILES (milestone 6113f3b + uncommitted gestures above)
New: `dsp/SpecInsert.h`, `dsp/lab/SpecialFx/{SpecLaser,DevSpecFx}.h`,
`dsp/lab/SpecialFx/SpecStudy.cpp`, `tests/TestSpecFx.cpp`,
`docs/special-fx-reference.md`. Modified: `dsp/TechDeathRig.h/.cpp`
(additive null-guarded seams), `host/dev/TdmDevApp.mm` (section+probe),
`tests/TestMain.cpp`, `Makefile`, `docs/experiments.md` (entry below),
`dsp/lab/Alien/DevAlien.h` + `tests/TestAlienFx.cpp` (Rise-glide fix,
see report).

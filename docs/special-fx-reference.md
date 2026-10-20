# Special FX Reference Study — Laser & Glitch

Branch: `feature/pitch-hardware-refinements` (from checkpoint
`49231ac` on `feature/pitch-controls-refactor`; via Toe/Dive
milestone `3b59005` on `research/special-fx-performance`, from
`6113f3b`). Date: 2026-10-19. Status: hardware-audition
refinements built + offline-verified, UNCOMMITTED. Hardware
ACCEPTED Whammy/Ring/Toe (bit-preserved); Detune REMOVED;
unified Whammy + DualWham v1 + gestures await audition.
Glitch is proposal-only.

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

One normalized expression position 0..1 → per-voice heel/toe maps,
driven by the manual slider/pedal (future MIDI) and performance
gestures. Envelope-follower control was REMOVED per hardware feedback
(envelope/Speed/Sensitivity deleted from DSP, router, UI, and tests).
Four creative voices (ours, not copies):

| Voice  | Heel (e=0) | Toe (e=1) | Notes |
|---|---|---|---|
| Whammy | unison (bit-exact dry leg) | ±12 st single voice | Direction Up/Down selects the leg |
| Dual | unison pair | V1 −12, V2 +12 (contrary) | 0.5 gains, no clip |
| DualWham | unison pair | A +12, B +24 targets (meas. +11.2/+22.7) | V2→V3 cascade, no silent clamp |
| Ring | 60 Hz carrier (growl AM) | 2000 Hz carrier (metallic) | log sweep |

Detune was REMOVED per hardware feedback (unmusical next to the other
voices; its heel was measurably detuned ±6¢ — the idle-colour culprit).
WhamUp/WhamDown merged into one Whammy + Direction (single source of
truth; no contradictory Up/Down states, no silent cancellation).

Primitives reused, not duplicated: 7× AlienDoppler (V1/V2/V3 normal —
V3 doubles as the DualWham-B cascade stage 2; V4/V5 dedicated gesture
legs; V6/V7 gesture DualWham-B cascade), 2× AlienRing (normal +
gesture), AlienDsp SmoothParam/QuadOsc. All legs process every sample
(primed; parked dopplers cost one ring write); selection only retargets
gains — no stale audio, ever.

THREE INDEPENDENT SECTIONS (TRANSPOSE / CREATIVE-EXPRESSION /
CREATIVE-GESTURES, each with its own enable): one shared engine tap —
either section keeps it live, never stacked. Expression-off parks the
normal legs at heel behind a bit-dry leg; gestures fire with Expression
disabled (press bends, release returns to dry). Per-voice UI gating
(`exprVoiceUsesDir/Depth`, `gestureVoiceUsesDir` in `SpecGesture.h`):
Direction only where it steers, Depth only on pitched voices — no
silent no-op dials.

Pitch tradeoff (documented in `SpecLaser.h`): Doppler variable-delay
gives sweep-continuous, polyphonic, low-latency pitch with static sag
at extremes (~−1.5 st at −12, ~−0.8 st at +12, measured toward unison).
Deeper finding (stacking study, 2026-10-20): the wrap-slip error is
INPUT-FREQUENCY-dependent — printed(+12) = 2f − signed(frac(0.015·f))
× 66.67 Hz (8/8 blind predictions hit to 0.1 Hz). A 220 Hz input
prints +11.2, a 196 Hz input +12.2, so Transpose −2 moves the Whammy
toe only ~1 st, and the DualWham cascade compounds this until voice B
moves only ~0.4 st — the hardware "same range" report, explained.
Shared with the accepted Whammy path; pitch-sync stagger is the known
fix but would change the accepted sound. Production GuitarTranspose
was rejected for this role: 16 ms latency, weak upward shifts, resync
warble under continuous sweeps.

PERFORMANCE GESTURES (unified engine, `SpecGesture.h`): 8 independent
slots × {effect, direction, depth, motion, time, return}, triggered by
stable slot ID (press/release momentary + one-shot; gen-counter
command, MIDI-ready). Same-voice gestures override expression (legacy
Toe shape, bit-exact); cross-voice gestures crossfade to the dedicated
legs (legacy Dive shape). To-Depth: normal → depth over Time, back over
Return. From-Depth: engage AT depth (10 ms masked fade), sweep home
over Time. Newest trigger wins; release returns to live normal — or to
dry when Expression is off (stale gestures never revive). Pitch-voice
gestures always target their pos-1 extreme (Whammy polarity comes from
Direction selecting the leg, NOT the map end). NaN/Inf input sanitized.
Depth capped at 12 st (probe: −18/−24 smear to mush).

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

## 7. VALIDATION STATUS (feature/pitch-hardware-refinements, uncommitted)
- `tdm_tests`: 2764 checks / 0 failures (specfx: Whammy/Dual/Ring
  goldens vs `6113f3b` bit-identical + toe-press MATCH, neutral suite,
  unified direction, DualWham holds, independence, 8-slot gestures,
  interruption, one-shot, MIDI-readiness, transpose+gesture
  simultaneity). Detune golden retired with the voice (never accepted).
- `tdm_dev --smoke-test`: 0 failures (incl. rewritten UI probe:
  3 sections, 4 voices + direction/depth wiring + gating, gestures
  fire with Expression off and return near-dry). `--smoke-ui`: exit 0.
- Study renders (`/tmp/spec_renders3`): whamup/whamdown/dual/dualwham/
  ring pre sweeps + whamup/dualwham/ring post A/B + dive / toestab /
  reverse-dive / ring-sweep / dualwham-stab gesture demos; all peaks
  0.54–0.72, movement confirmed vs bypass halves.
- CPU/voice: whamup 40.8 / whamdown 38.2 / dual 45.5 / dualwham 50.3 /
  ring 29.4 µs/block (0.28–0.47%); dive/ring held ≈ 60 µs (0.57%).
- Latency: 1680 samples (35 ms doppler bound; ring leg ~1). Unchanged.
- DualWham measured (220 Hz sine): toe A=420 (+11.2), B=820 (+22.7);
  expr 0.25 → A=258/B=308; expr 0.75 → A=356/B=584; bounded, finite.
  Mid-sweep smears (doppler character — audition decides).
- Synthetic signals prove MATH ONLY. Hardware accepted Whammy/Ring/Toe
  (preserved bit-exact above); unified Whammy/Direction + DualWham v1
  + independent gestures await audition (open questions in §9 and
  the experiments log).

## 8. PITCH SHIFT CONSOLIDATION (this branch, uncommitted)

The Toe Rise/Return + Divebomb milestone (`3b59005`) was refactored into
the unified gesture engine (§5) without changing the accepted sound:
goldens prove Whammy/Ring/Dual/Detune/toe-press are bit-identical.
Envelope control removed throughout. The DEV app now has one PITCH
SHIFT group (TRANSPOSE + CREATIVE PITCH + PERFORMANCE GESTURES, 8
staged slots, trigger-by-ID + one-shot, per-slot factory reset); DSP
routing is untouched. Details: `docs/experiments.md` refactor entry.

## 9. HARDWARE AUDITION REFINEMENTS (this branch, uncommitted)

Checkpoint `49231ac`, then on `feature/pitch-hardware-refinements`:

1. Neutral probe FIRST: Whammy/Dual heels bit-dry, Ring heel full AM
   (by design), Detune heel measurably detuned → Detune removed rather
   than repaired.
2. Unified Whammy: one voice + Direction Up/Down (single source of
   truth); Up keeps the accepted V1 path, Down the working V3 path.
3. DualWham v1: A on V1, B via the V2→V3 cascade (+12+12, existing
   Doppler first, skew unaligned); +24 target measures +22.7 — no
   silent clamp, audition decides usability.
4. Independent enables: `setExpressionEnabled` / `setGesturesEnabled`
   share one tap target (either keeps it live, never stacked);
   expression-off parks behind a bit-dry leg; gestures fire dry.
5. Per-voice UI gating from `SpecGesture.h` (no silent no-ops).
6. Tests rewritten (50 specfx groups): goldens hold, neutral suite,
   independence, DualWham stepped-hold oracles (continuous-sweep
   peak-picking was defeated by doppler spread + grid-miss nulls).

MIDI-ready API preserved: `pressSlot`/`releaseSlot`/`oneShotSlot` by
stable ID 0–7, working with Expression disabled.

Remaining hardware-audition questions: DualWham +24 usability (sag /
skew / aliasing under real playing); Whammy-Down continuous feel;
gesture firing with Expression off across NAMs; any residual idle
colour with Expression on at heel; From-Depth fast-print limits.

## 10. FILES (milestones + uncommitted refinements above)
New: `dsp/SpecInsert.h`,
`dsp/lab/SpecialFx/{SpecLaser,DevSpecFx,SpecGesture}.h`,
`dsp/lab/SpecialFx/SpecStudy.cpp`, `tests/TestSpecFx.cpp`,
`docs/special-fx-reference.md`. Modified: `dsp/TechDeathRig.h/.cpp`
(additive null-guarded seams), `host/dev/TdmDevApp.mm` (section+probe),
`tests/TestMain.cpp`, `Makefile`, `docs/experiments.md` (entries),
`dsp/lab/Alien/DevAlien.h` + `tests/TestAlienFx.cpp` (Rise-glide fix,
see report).

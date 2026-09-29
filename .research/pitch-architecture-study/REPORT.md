# Pitch-Shifter Architecture Study: Comparative Report

Study date: 2026-09-28. Status: reading-only technical research (no builds, no listening tests in this phase).
Our repo (`/Users/marte/Code/TechDeathMachine`) was treated as READ-ONLY; external clones under `/tmp/tdm-pitch-study/<project>/` were read, not built.
External algorithms are described in our own words; code excerpts are limited to tiny fragments.
Each section marks INSPECTED (read the body) vs INFERENCE (reasoned conclusion) vs ASSUMPTION (unverified).

Target product context (fixed for all recommendations): electric guitar, polyphonic chords,
fixed -1/-2 detune primary, deeper shifts secondary, future continuous dives, 48 kHz,
callback realtime, ultra-low latency, distortion/NAM AFTER the shifter.

---

## 1. Executive findings

1. **Our WSOLA wobble is structural, not a tuning gap.** One global time-domain skip per frame
   rephases every chord partial at once; partials precess against each other at rates set by
   interval ratios (fifth ~0.5 = benign sign-flip; fourth ~0.335 = audible precession). This is
   INFERENCE from our trace finding, corroborated by INSPECTED fact that every mature
   single-alignment engine (SoundTouch `TDStretch`, example `WsolaStretch`) shares the failure
   class, while every per-bin spectral engine (Signalsmith, Rubber Band R2/R3) structurally
   excludes it. No search-window enlargement can remove it; hardware probes already confirmed this.
2. **Latency ranking at 48 kHz, -1 st (all INSPECTED formulas, computed not measured):**
   our WSOLA sym-W20 1443 smp / 30.1 ms; our WSOLA asym-(640,320) 1283 smp / 26.7 ms;
   our PV-D (2048/256) ~2153 smp / ~44.9 ms; our PV-A (4096/1024) ~4278 smp / ~89 ms;
   our multi-res = PV-A + 396 smp crossover (~8.25 ms) ~ 97 ms; SoundTouch defaults ~92 ms
   initial (~59 ms average, 44.1 kHz); Signalsmith default 120 ms round-trip; Rubber Band
   R2-realtime ~21 ms, R3-single ~27 ms, R3-multi ~43 ms, live shifter ~50-60 ms.
   **Sub-30 ms is proven viable for spectral engines only by Rubber Band R2 (~21 ms)
   and R3-single (~27 ms)** -- and both pay for it in LF resolution and phase-lock range.
3. **Pitch decomposition consensus is near-total: resample + reciprocal stretch.**
   SoundTouch, Rubber Band (both engines + live), our PV, our multi-res, and our WSOLA all
   perform 100% of the pitch change in a resampler and use the stretcher only for duration
   correction (INSPECTED in all five codebases). The sole dissenter is Signalsmith (pure
   spectral frequency remap, no resampler). Our compress-first/slow-read pairing is therefore
   mainstream, not idiosyncratic.
4. **What mature code does that we don't, before any audible failure appears:**
   causal (history-only) correlation search (SoundTouch) vs our future-reaching symmetric
   search; center-biased small-skip preference vs our drift-seeking tie-break; candidate-energy
   score normalisation vs our full NCC; anti-alias-filtered resampling vs our bare cubic;
   frequency-delimited transient resets vs our full-spectrum PV reset; per-band peak-locked
   phase advance vs our nearest-peak lock; harmonic/percussive classification vs our single
   flux/HP detectors. Details in section 7.
5. **Global time-domain rephasing is unavoidable for single-alignment architectures.**
   Proof sketch (INFERENCE, standard result): one shared offset d shifts partial f by phase
   2*pi*f*d/sr; relative rephasing between partials is zero only if d is a common multiple of
   all partial periods, impossible for chord/inharmonic content. Mature single-alignment code
   (SoundTouch) only reduces skip rate and size (long frames, center bias, crossfades).
6. **Best-fit mature architecture for our target: Rubber Band R2-realtime-shaped,
   single-resolution short-window phase vocoder with time-anchored transient resets and
   resample-based pitch** -- i.e. our PV-D direction hardened with R2/R3 techniques
   (INFERENCE, justified in section 10). Rationale: only per-bin architectures escape the
   wobble family; only short windows meet ultra-low latency; distortion-after-shifter
   exposes periodic wobble via intermodulation far more ruthlessly than it exposes PV
   phasiness; R2 proves ~21 ms spectral latency is achievable.
7. **WSOLA stays viable as a transient specialist, not as the sustain engine.**
   Its strengths (attacks, LF preservation, no F0 assumption, ~27 ms asymmetric) are
   complementary to a short PV's weaknesses, which motivates a transient/sustain hybrid
   (section 10) -- but the hybrid's sustain path must be per-bin to escape the wobble.
8. **Licensing (research, not legal advice):** studying all four projects is safe.
   SoundTouch is LGPL-2.1 (dynamic-link/relink discipline if ever integrated);
   Signalsmith Stretch is MIT (permissive); the example-code repo has NO license statement
   (study-only, do not copy a line); Rubber Band is GPL-2-or-later or paid commercial
   (study safe; shipping its code needs GPL compliance or a licence). Any reimplementation
   must be clean-room: own-words design notes plus provenance log, no copied code.

---

## 2. Our current architecture and failure mechanisms

All claims in this section are INSPECTED (file bodies read) unless marked otherwise.

### 2.1 Approach A -- variable-delay/Doppler (rejected): no code remnants

A repo-wide search for Doppler / variable-delay / variableDelay / "approach A" across
`dsp/`, `tests/`, and docs returned **no remnants**: the only pitch-adjacent delay files are
the unrelated product reverb/delay (`dsp/Space/Reverb.h`, `dsp/Space/Delay.h`), and every
`dsp/lab/Pitch/*` file belongs to approaches B/C/D. FINDING: approach A exists only as the
verbal description "fragmentary/unstable" -- there is nothing to inspect, so the matrix
column for Our Doppler is reconstructed from that description (marked accordingly).

### 2.2 Approach B -- conventional phase vocoder (`LabPitchShift.h/.cpp`)

- **Decomposition (INSPECTED):** same-bin PV time compressor (synthesis hop
  `Hs = round(r*Ha) <= Ha`, carriers untouched) + cubic-Lagrange slow fractional read at
  `o*r`. An earlier spectral-remap revision was deleted after probes convicted it (E3 sine
  at -7 st returned at 0.14x level from carrier incoherence). Header documents the proof.
- **Polyphony (INSPECTED):** per-bin instantaneous-frequency propagation over Hs, then
  nearest-peak re-lock each frame (peaks = local magnitude maxima above max/1000 floor,
  regions to midpoints). Single resolution, Hann/Hann WOLA with explicit window-sum
  normalisation.
- **Transients (INSPECTED):** normalised positive spectral flux vs median-of-8 adaptive
  threshold; on trigger, FULL-spectrum phase reset to analysis phases + baseline re-sync.
- **Configs (INSPECTED):** default 4096/1024; study grid in `tests/TestLabPitch.cpp`
  (`cfgs = {{4096,1024},{2048,512},{1024,256},{2048,256}}`).
  **PV-A = 4096/1024 and PV-D = 2048/256 are pinned by `LabMultiPitch.h`** ("keep A's
  (4096/1024) ... D's (2048/256)"). ASSUMPTION: middle grid entries are B/C in listed
  order; only A and D are named in code.
- **Latency (INSPECTED formula):** `(N+1) + (N-Ha)*(1/r-1)`; computed @48 kHz, -1 st:
  PV-A ~4278 smp (~89 ms), PV-D ~2153 smp (~44.9 ms). At -7: PV-A ~5631 (~117 ms).
- **Dynamics (INSPECTED):** shift is reset-gated; no sweeps; exact-0.0f bit-exact bypass.
- **Observed failure (given):** large FFT preserves polyphony but latency/smear excessive;
  small windows lose LF/chord structure.

### 2.3 Approach C -- multi-resolution PV (`LabMultiPitch.h/.cpp` + `LabCrossover.h/.cpp`)

- **Topology (INSPECTED):** split FIRST (complementary linear-phase FIR/subtract crossover,
  -6 dB @ ~300 Hz, ~200 Hz transition, constant ~8.3 ms group delay: 793 taps / 396 smp
  @48 kHz), then PV-A (4096/1024) on low, PV-D (2048/256) on high, then sample-aligned sum
  (high path padded by exactly `L_low - L_high`).
- **Hazards (INSPECTED):** split-phase cancellation confined (not eliminated) to transition
  energy; holes/double-processing structurally impossible (exact complementary sum);
  latency = crossover + L_low, honestly reported (no early-highs trick).
- **Observed failure (given):** kept large latency plus crossover problems.

### 2.4 Approach D -- WSOLA (`LabWsolaShift.h/.cpp`, `LabWsolaLive.h/.cpp`)

- **Decomposition (INSPECTED):** WSOLA time compressor (analysis frames W every Ha=W/2,
  lag refined over [-Dm,+Dp] by full normalised cross-correlation against the synthesised
  tail, overlap-add every Hs=round(r*Ha) with raised-cosine crossfade) + cubic slow read
  at `o*ar` where ar is the TRUE rounded ratio.
- **Continuity strategy (INSPECTED):** drift-seeking tie-break (within 1e-3 of max score,
  take lag nearest exact continuation `prev-(Ha-Hs)`); pegged/transient frames use 1e-6
  (outright max). Deleted ancestor: a per-frame time-map-debt servo that froze all drift
  (output dry everywhere) -- header documents the retraction.
- **Transient model (INSPECTED):** input-side HP-energy rise (>6x trailing average, rate
  0.25, floors) takes outright max; transient search floor clamps to d >= -(Lov-1) when
  Dm > Lov so attacks can't be deleted by sustain-lag jumps (measured on -1 chugs).
- **Span rule (INSPECTED):** Dm+Dp must cover a strong period of the lowest content
  (low-B needs 778+ smp @48 kHz, probed).
- **Latency (INSPECTED formula + proof):** `W + Dp + C`, `C = ceil(3/ar-1)`, pops held
  until tick L. Computed @48 kHz: sym-W20 -1 st = 960+480+3 = 1443 (30.1 ms);
  study geom "a" (640,320) = 1283 (26.7 ms); geom "m" (680,280) = 1243 (25.9 ms).
  Only +Dp costs latency (future reach); -Dm reads history.
- **Trace API (INSPECTED):** `enableTrace` + `FrameTrace` (lag, continuation target,
  top-3 peaks, score, transient/pegged flags) + `landscape()` score buffer; pinned
  read-only w.r.t. DSP state.
- **Live wrapper (INSPECTED):** constant-latency bypass (dry delayed by L), 128-smp
  atomic ramp toggle, fixed shift per run, [-7,0] st audition range, default W20 symmetric.
- **Observed failure (given, trace-confirmed):** sustained polyphonic wobble is periodic
  skip-back rephasing, NOT correlation ambiguity: one global skip preserves the root but
  rephases upper partials by frac(f_upper/f_root) per skip. Larger backward search did not
  structurally fix it on hardware.

### 2.5 Lab infrastructure (INSPECTED)

- `tests/TestLabPitch.cpp`, `TestLabMulti.cpp`, `TestLabWsola.cpp`,
  `TestLabWsolaLatency.cpp`, `TestLabWsolaLive.cpp`: pinned latency formulas, bit-exact
  determinism, no-starve grids, transient-floor regression, geometry table
  (`a=(20ms,640,320)` robust winner, `m=(20ms,680,280)` Dp-floor probe).
- `dsp/lab/Pitch/LabWsolaStudy.cpp` (latency-study measurement harness),
  `LabWsolaTraj.cpp` (wobble trajectory instrument: lag stats, jump counts, peak margins,
  AM depth, CSV/landscape dumps), `LabPitchRender.cpp` (offline renderer).
- `scripts/labpitch_synth_di.py` (INSPECTED): deterministic 48 kHz KS guitar DI, sections:
  low-E sustains, palm-muted chugs @140 bpm, E5/G5/A5 power chords, E-minor strum+arpeggio,
  16th riff, sustained low-B power chord -- with pick-click transients. This is the
  evaluation backbone everything below reuses.

---

## 3. SoundTouch deep dive (v2.4.1, LGPL-2.1)

Source: `/tmp/tdm-pitch-study/soundtouch/DIVE.md` (full body reads) + prior-result-4
evidence. All bullets INSPECTED; file/function refs from the dive.

- **Decomposition:** pitch factor p = resample(rate=p) + reciprocal stretch(tempo=1/p);
  ALL pitch from resampling (`SoundTouch::calcEffectiveRateAndTempo`). Stage order flips
  at rate=1: resample-first when pitching down, stretch-first when pitching up
  (`SoundTouch::putSamples`); an opt-in build flag forces always-stretch-first to avoid
  the crossover click.
- **Stretcher (`TDStretch`):** pure time-domain WSOLA/OLA. One global integer offset per
  frame for the whole mix (`processSamples`); joint all-channel interleaved correlation
  (`calcCrossCorr`); score = candidate-energy-only normalisation (`corr/sqrt(norm)`, NOT
  full NCC); center bias `(corr+0.1)*(1-0.25t^2)` with offset 0 pre-penalised x0.75
  (`seekBestOverlapPositionFull`); optional coarse/fine QuickSeek (default off);
  linear-ramp crossfade, same offset every channel (`overlapStereo`).
- **Search geometry is causal:** offsets range over [0, seek) into buffered input --
  no future reach. Default auto tables: 73 ms sequence / 18 ms seek / 8 ms overlap at
  tempo 1 (90/20 at 0.5x, 40/15 at 2x). Output batch = SEQ-OVL; input per batch =
  nominalSkip.
- **Transients:** none -- no detector, reset, or bypass anywhere in the frame loop.
  Smear bounded structurally by the 8 ms overlap; alignment may drag transients by up to
  the seek length. The header documents the audible cost as "drift" and prescribes
  shrinking the seek window.
- **Resampler (`RateTransposer` + `AAFilter` + `FIRFilter`):** default cubic
  interpolation (integer builds forced linear); Hamming-sinc AA FIR, 64 taps, cutoff
  0.5/rate or 0.5*rate, redesigned (fresh allocs) on every setRate; filter/transpose
  order flips with rate; latency = interp + AA/2, silence-prefilled.
- **Latency:** initial `sampleReq = max(intskip+ovl, seq) + seek` dominates (~91 of ~92 ms
  @44.1 kHz defaults); no separate live mode.
- **CPU/realtime:** correlation-dominated (~seek x overlap x ch per batch); runtime
  SSE/MMX dispatch; OpenMP parallel seek scan; allocation-free steady-state only if FIFOs
  pre-grown; synchronous unbounded per-call work -- mediocre callback fit.
- **Ratio changes:** fully dynamic, unsmoothed; skipFract/midBuf persist; divebombs work
  mechanically with zipper noise + per-call AA redesign cost.
- **Key verdict for us:** SAME global-skip rephasing failure class as our W20 (single
  broadband offset, joint score, no per-partial handling). Severity -- not kind -- is
  reduced by much longer frames (fewer skips), center bias (smaller skips), 8 ms
  crossfades, and (pitch-down) windows covering more periods on the downsampled signal.

## 4. Signalsmith Stretch deep dive (1.4.0, MIT) + example-code progression

Sources: `/tmp/tdm-pitch-study/signalsmith-stretch/DIVE.md`,
`/tmp/tdm-pitch-study/pitch-time-example-code/DIVE.md` + prior-result-1/3 evidence.
All bullets INSPECTED.

### 4.1 Production library (header-only `signalsmith-stretch.h`)

- **Decomposition:** pitch = spectral frequency remap ONLY (peak-anchored smoothstep map
  in `updateOutputMap`, energies interpolated + density-scaled by map gradient); time =
  input-consumption ratio per process call. NO resampler exists anywhere.
- **Representation:** single-resolution modified STFT (half-bin offset), Kaiser window
  with forced perfect reconstruction, 120 ms block / 30 ms hop default (100/40 cheaper
  split mode). No multiresolution, no LF path, no F0 tracker (crude 3-peak guess for
  formant width only).
- **Continuity (the signature mechanism):** per-bin time propagation
  (`input*conj(prevInput)` twist re-applied to output phasors) followed by a vertical
  cross-frequency re-prediction pass blending +/-1 and +/-fft/hop neighbours through
  input-measured twists; max-energy channel leads, others phase-locked to it.
- **Polyphony:** multiple peaks anchor one shared map; no global splice exists, so the
  WSOLA skip-wobble CANNOT occur in the same form. Failure mode is diffuse phasiness /
  blur for unresolved partials -- interval-ratio-selective precession has no mechanism here.
- **Transients:** none -- no detector; silence gate only. 120 ms windows smear attacks;
  vertical predictor blurs clicks into phasiness. Time-aliasing beyond 2x slowdown is
  fought with randomised time-factor spread (diffusion over metallic ringing).
- **Latency:** block-peak/peak halves (+1 hop in split mode); default 60+60 = 120 ms.
- **CPU/realtime:** zero allocation in process (asserted by dev harness); 2 fwd FFTs per
  block when rate != 1 (re-analyse prev); FFT-backend macros, no hand SIMD; split mode
  spreads bursts per sample at +1 hop latency.
- **Ratio changes:** fully dynamic per block, unsmoothed; divebombs glide structurally
  (phasors persist) but smear across the window.

### 4.2 Example-code progression (ADC22, 8 CLI modes in `shift-stretch.h`)

Trajectory (INSPECTED): plain overlap-add (smear baseline) -> WSOLA (single global
bestOffset, normalised-difference score, joint channels, bestOffset/2 schedule feedback)
-> spectral-cut (per-segment single phase rotation, exact intra-segment phases) ->
conventional phase vocoder (default blends raw input phase for fresh energy; `--pure`
disables) -> PaulStretch (random phases; null endpoint) -> vase-phocoder (vertical-only
chaining, no cross-block state) -> **hybrid-phase (final)**: per-band complex blend of
multi-stride vertical {1,2,4,8,16} x timeFactor (weight 2), horizontal vocoder angle
scaled by timeFactor*freqFactor (weight 1), inter-channel vote (weight 1), plus strongest
bonus; block-centre time rotation in/out; energy read at freq-mapped bin / freqFactor.
- Pitch in OLA/WSOLA/PV/Paul modes = stretch + WAV sample-rate retag (no interpolating
  resampler in repo); hybrid/vase/spectral-cut = direct spectral map, no resample.
- LF from block length only (120 ms hybrid/PV/Paul, 80 ms others, overlap 4x); latency
  exactly one block. No peak detection, no transient handling, no vertical-scale cap in
  the repo (all successor-only per the design article). No multiresolution anywhere.
- Quality/latency movers vs plain PV at IDENTICAL window: vertical multi-stride timing
  evidence, centred-time convention, magnitude-weighted averaging -- i.e. per-bin
  independence + cross-frequency timing without peak-picking, phase-lock regions, or a
  global offset.
- Repo quirks noted in dive: no license file (study-only); `dsp/` dependency not vendored
  (build impossible); channel max-track stores horizontal prediction (apparent typo).

## 5. Rubber Band deep dive (live/realtime focus, GPL-2+ or commercial)

Source: `/tmp/tdm-pitch-study/rubberband/DIVE.md` (full body reads) + prior-result-2
evidence. All bullets INSPECTED. Checkout SHA e4296ac (2025-02-27), version 4.0.0.

- **Engine map:** R2 "Faster" (default) vs R3 "Finer" (OptionEngineFiner bit) in
  `RubberBandStretcher::Impl`; `RubberBandLiveShifter` ALWAYS constructs the dedicated
  `R3LiveShifter` (fixed 512-frame blocks, genuinely separate class).
- **Decomposition:** stretch-by-(time*pitch) then resample 1/pitch; 100% of pitch from
  the resampler (`R2Stretcher::getEffectiveRatio`). Before/after order depends on mode:
  R3-offline always after; RT HighSpeed resample-before-up/after-down (cheap side),
  HighQuality the reverse; HighConsistency always-after so sweeps cross unity cleanly.
  Live shifter runs DUAL resamplers (in @1/pitch for up, out @1/pitch for down, one
  always at unity).
- **R3 analysis:** multi-resolution 4096/2048/1024 guided bands (crossovers adapt within
  500-1100 Hz / 4-7 kHz by descending to magnitude valleys) with single-window fallback;
  harmonic/percussive/residual median-filter classification; modal segmentation to 3
  boundary frequencies; one-hop classification readahead; cepstral formant envelope
  (bounded correction, <10 kHz).
- **Continuity:** peak-locked phase advance with beta-weighted offsets (`PhaseAdvance`);
  per-frame kick/preKick/reset/unlock/channelLock ranges from the Guide; fixed inhop +
  servoed outhop with FIFO peek/skip; pre-kick magnitude withholding (`pendingKick`);
  hop-normalised overlap-add into longest-FFT accumulators. R2: laminar top-down phase
  inheritance (vs independent mode), window-area-normalised OLA.
- **Transients:** R3 = frequency-delimited kicks/resets (never full-spectrum except
  silence/unity); R2 = compound onset detector (3 dB-rise fraction + median-filtered HF),
  baseline hard/soft peak time-anchoring with interpolated hops, RT rule 0.35+10% rise
  with amnesty + divergence veto; Crisp/Mixed (spare 150-1000 Hz)/Smooth user modes.
- **Polyphony / skip question:** NO global waveform skip exists in either engine. R3:
  per-bin/per-band phases on a fixed ratio-dictated hop grid -- a shared rephasing event
  is structurally impossible; dense-chord failure is local (diffuse/metallic per band).
  R2: one global increment sequence, but it only sizes hops around exact-time anchors,
  never jumps position -- again no precession mode.
- **Resampler:** builtin rational-polyphase Kaiser-sinc (90 dB, 5% transition, Farey
  denominator <= 96k/192k), `FastestTolerable` everywhere; ~1 ms raised-cosine
  crossfade on ratio change (RT smooth mode; live shifter uses sudden switching);
  effective ratio reported back to the hop servo. Steady-state group delay negligible vs
  window terms (exact figure unmeasured -- unresolved).
- **Latency @48 kHz (reported formulas):** R3-multi = half window-source span 2048
  (~42.7 ms, /pitchScale when pre-resampling); R3-single 1280 (~26.7 ms); live = span +
  measured resampler transients + block correction, ~50-60 ms documented; R2-RT = half
  analysis window 1024 (~21.3 ms, cheapest of all paths).
- **CPU/realtime:** double-precision spectral path, float I/O; R3-multi ~6x 2-4k FFTs
  per ~256 output samples (stereo); FFT preference ipp>vDSP>sleef>fftw>builtin>kiss;
  realtime paths preallocate everything, no alloc/lock/block in steady state; atomics
  for ratios; R3 single-threaded, R2-RT single-threaded.
- **Ratio changes:** fully dynamic in RT; hop servo checkpoint + 50-100 ms divergence
  correction, hops clamped 0.3-2x nominal; divebombs glide (live shifter can zip on
  very fast sweeps due to sudden resampler switching).

---

## 6. Comparative architecture matrix

Conventions: INS = inspected body; GIV = given empirical finding; INF = inference;
ASM = assumption. Latencies @48 kHz, -1 st unless noted; "low-B" = ~62 Hz fund.

| Row | Our Doppler | Our PV-A | Our PV-D | Our multi-res PV | Our W20 WSOLA | SoundTouch | Signalsmith Stretch | Rubber Band live path(s) |
|---|---|---|---|---|---|---|---|---|
| core domain | time delay (GIV desc; no code) | spectral PV (INS) | spectral PV (INS) | 2x spectral PV + FIR xover (INS) | time WSOLA + resample (INS) | time WSOLA/OLA + resample (INS) | spectral hybrid PV (INS) | spectral PV + resample (INS) |
| pitch-shift decomposition | var-delay rate change (GIV desc) | compress Hs=r*Ha, slow cubic read; pitch 100% resample (INS) | same as PV-A (INS) | per-band PV-A/PV-D + cubic read (INS) | compress Hs=r*Ha, slow cubic read; pitch 100% resample (INS) | resample p + stretch 1/p; pitch 100% resample; order flips at rate=1 (INS) | spectral freq remap only; NO resampler (INS) | stretch time*pitch + resample 1/pitch; pitch 100% resample; live = dual resamplers (INS) |
| continuity strategy | none (fragmentary, GIV) | per-bin IF propagation + nearest-peak lock; Hann/Hann WOLA (INS) | same as PV-A (INS) | same per band + aligned sum (INS) | drift-seeking tie-break + raised-cosine OLA (INS) | center-biased argmax + linear xfade; causal search (INS) | time twist + vertical 4-nb re-prediction; max-E ch lock (INS) | peak-locked advance + beta offsets; hop-norm OLA (INS) |
| polyphonic model | none (GIV) | multi-peak lock regions, single resolution (INS) | same, coarser bins (INS) | peaks per band; split-phase risk at xover (INS) | single global lag sees mix (INS) | single global offset, joint ch score (INS) | per-bin phases + upward chain; peak-anchored map (INS) | R3 per-bin/band/scale; R2 laminar inherit (INS) |
| transient model | none (GIV) | flux-median detect, FULL-spectrum reset (INS) | same (INS) | same per band (INS) | HP-rise detect, outright-max + floor clamp (INS) | NONE; 8 ms xfade bound only (INS) | NONE; silence gate only (INS) | R3 freq-delimited kick/reset + pre-kick withholding; R2 anchored resets, Crisp/Mixed/Smooth (INS) |
| LF resolution mechanism | n/a (GIV) | 4096 window (~11.7 Hz bins) (INS) | 2048 window (~23 Hz bins) (INS) | 4096 for <~300 Hz (INS) | 20 ms corr window + 960 span (INS) | 18 ms seek + 8 ms corr aperture; auto tables (INS) | 120 ms window (~8 Hz) (INS) | R3-multi 4096 below adap xover; R3-live/R2 single 2048 (INS) |
| main latency source | n/a | window + (N-Ha)*(1/r-1) (INS) | window + ditto, smaller (INS) | crossover 396 + L_low (INS) | W + Dp + C proof (INS) | sampleReq FIFO threshold (INS) | half-window in + half-window out (+hop split) (INS) | half window-span; live + resampler/block terms (INS) |
| latency @48k -1st | n/a | ~4278 / ~89 ms (computed) | ~2153 / ~44.9 ms (computed) | ~4674 / ~97 ms (computed) | sym 1443/30.1 ms; asym-a 1283/26.7 ms (INS pins) | ~92 ms init / ~59 ms avg @44.1k dflt (INS) | 120 ms r.t. dflt (INS) | R2 ~21 ms; R3-1win ~27 ms; R3-multi ~43 ms; live ~50-60 ms (INS) |
| expected chord stability | poor (GIV) | good (GIV) | degraded: loses fifths/low-B (GIV) | good except xover region (GIV) | wobble via skip rephasing (GIV+trace) | same wobble class, slower rate (INF from INS) | phasiness, no interval-selective wobble (INF from INS) | local diffuse/metallic at worst; no precession (INF from INS) |
| expected transient quality | poor (GIV) | smeared by 85 ms window (GIV) | tighter (GIV) | high band tight, low smeared (GIV) | good: outright-max + short window (GIV) | bounded smear, driftable (INF) | poor: 120 ms smear (INF) | R3 tight highs + withheld pre-smear; R2 anchored (INF) |
| expected low-B behaviour | n/a | resolved (~5 bins/fund) (INF) | marginal (~2-3 bins) (GIV) | resolved in low band (GIV) | span-rule OK at 960 (GIV) | weak: 8 ms aperture < 16 ms period (INF) | coarse centroiding, rigid sub-peak ext (INF) | R3-multi OK; single-window marginal (INF) |
| realtime suitability | no (GIV) | poor (latency) | borderline (45 ms) | poor | YES: bounded, prealloc, RT-safe (INS) | mediocre: unbounded bursts, AA reallocs (INF) | split mode yes, 120 ms+ (INF) | YES: prealloc, atomics, fixed-block live (INS) |
| dynamic-ratio suitability | no (GIV) | no: reset-gated (INS) | no: reset-gated (INS) | no: reset-gated (INS) | no: reset-gated (INS) | yes, unsmoothed + AA churn (INS) | yes per-block, unsmoothed (INS) | yes: servo + 1 ms resampler xfade (INS) |
| implementation complexity | low | medium (~380-line PV) | medium | medium-high (xover+align) | medium (~500-line WSOLA) | medium (TD+resampler+SIMD) | medium-high (1 header, dense) | high (2 engines + guide + classif) |
| licence/integration | ours | ours | ours | ours | ours | LGPL-2.1: dynamic-link discipline (INS) | MIT: permissive (INS) | GPL-2+ or commercial fee (INS) |

---

## 7. What we did differently (7 divergence answers)

### D1. Which of our decisions differ from mature implementations BEFORE our audible failures appear?

WSOLA line (ours vs SoundTouch, both INSPECTED):
(a) Search symmetry: ours spans [-Dm,+Dp] reaching Dp into the future (latency term);
SoundTouch searches [0, seek) over already-buffered input only -- zero lookahead beyond
the FIFO threshold. Our symmetric half alone costs 480 smp @W20 that a causal search
would not.
(b) Tie-break philosophy: ours drift-seeks (nearest-to-continuation within 1e-3);
SoundTouch center-biases (`(1-0.25t^2)`, offset 0 x0.75) -- it prefers SMALL skips where
we prefer DRIFTING skips. Both are heuristics over the same argmax; they minimise skip
size, we maximise drift continuity.
(c) Score normalisation: ours full NCC (`num/sqrt(candE*tailE)`); SoundTouch candidate
energy only (`corr/sqrt(norm)`). Different weak-signal behaviour; neither is validated
against the other on guitar.
(d) Transient handling: ours has an HP-rise guard + search floor; SoundTouch has none.
Here WE are the more elaborate design -- a genuine point of difference in our favour,
though unvalidated against mature alternatives.
(e) Crossfade: ours raised-cosine, SoundTouch linear -- minor.
PV line (ours vs Signalsmith/Rubber Band, all INSPECTED):
(f) Pitch mechanism: ours = same-bin compression + resample (mainstream: matches
SoundTouch/RB, differs from Signalsmith's spectral remap only).
(g) Phase coherence: ours = propagate-all + nearest-peak lock; Signalsmith = time twist +
vertical re-prediction; R3 = peak-locked advance + beta offsets + guided bands; R2 =
laminar inheritance. Ours is the simplest of the four and the only one without any
cross-frequency constraint beyond same-frame lock.
(h) Transient reset scope: ours resets the FULL spectrum; R3 never spans full spectrum
except silence/unity (Crisp-mode warning documents why); R2 offers Mixed (spare
150-1000 Hz). Our reset audibly interrupts coincident stable partials by design.
(i) Resampling: ours bare cubic, no anti-alias filter; SoundTouch adds a 64-tap AA FIR,
RB a polyphase Kaiser-sinc. Our images sit ~40 dB down (header claim, unprobed here);
mature code spends real silicon to do better.
(j) Ratio dynamics: ours reset-gated; ALL FOUR mature projects are per-block dynamic
(unsmoothed except RB's 1 ms resampler crossfade + hop servo). Our fixed-shift scope is
a study choice, but the gap matters for the "future continuous dives" requirement.

### D2. Simplifying assumptions we made that mature code avoids

1. "One global alignment suffices for a chord" -- avoided by all three spectral engines
   (per-bin/per-band decisions); shared only with SoundTouch, which mitigates it
   (long frames, center bias) rather than solving it.
2. "Symmetric future-reaching search is affordable" -- SoundTouch's causal search avoids
   the Dp latency term entirely.
3. "Full-spectrum transient reset is harmless" -- R3/R2 delimit resets in frequency or
   time-anchor instead; R2's docs warn Crisp resets interrupt stable tones.
4. "Window length is the only LF lever" -- R3's guided multi-scale + valley-descending
   crossovers extract more LF stability per millisecond of latency than any single window.
5. "Bare cubic resampling is clean enough post-distortion" -- unproven; both resampling
   engines that feed high-gain use cases filter (SoundTouch AA, RB polyphase).
   INFERENCE: distortion-after-shifter amplifies alias images intermodulation products;
   this assumption is high-risk for our exact signal chain.
6. "Fixed shift forever" -- every mature engine supports dynamic ratios; our reset-gated
   design defers the divebomb requirement to a future rewrite of state handling.

### D3. Is global time-domain rephasing unavoidable for single-alignment architectures?

YES (INFERENCE from inspected mechanisms + standard DSP). One shared integer offset d
rotates partial f by 2*pi*f*d/sr; the relative rephasing between two partials vanishes
only when d is a common multiple of both periods. Chord and inharmonic guitar content
has no small common multiple inside any affordable search span, so every skip rephases
upper partials against the root by ~frac(f_upper/f_root) -- exactly the trace finding.
Evidence pattern across inspected code: SoundTouch documents the residue as "drift" and
fights it with window shrinkage; the example WSOLA damps it with schedule feedback;
NO single-alignment engine eliminates it; NO per-bin engine exhibits it. Larger backward
search cannot help because the skip SIZE is not the disease -- the SHAREDNESS is.

### D4. Which PV techniques did we miss that alter quality/latency?

Ranked by expected leverage for our target (all INSPECTED in mature code):
1. Frequency-delimited transient resets + pre-kick magnitude withholding (R3) -- keeps
   attacks without interrupting sustains; directly fixes our full-reset weakness.
2. Time-anchored hop servo with interpolated hops (R2: `StretchCalculator`) -- exact
   transient placement without phase damage; the mechanism behind R2's 21 ms liveability.
3. Peak-locked bands with beta-weighted offsets / laminar inheritance (R3/R2) -- cheaper
   and more stable than our propagate-all + hard re-lock.
4. Vertical cross-frequency re-prediction (Signalsmith) / multi-stride timing blend
   (example hybrid) -- timing coherence at fixed window; the only technique shown to buy
   transient quality WITHOUT lengthening the window.
5. Kaiser-PR / window-area-normalised OLA (Signalsmith/R2) vs our Hann/Hann -- flatter
   gain, better sidelobes for dense chords.
6. Compound onset detection (R2: 3 dB-rise fraction + median-filtered HF) vs our
   single flux/HP statistics -- fewer false positives driving resets.
7. Split computation (Signalsmith) -- spreads burst cost per sample at +1 hop; relevant
   only if we adopt long windows (we shouldn't for live).
8. Tonality limit + formant preservation (Signalsmith/R3) -- secondary for detune, but
   "brightness survives -7" is an audition criterion worth borrowing later.

### D5. Which mature architecture best fits our target?

**Rubber Band R2-realtime-shaped: single-resolution short-window phase vocoder,
time-anchored transient resets, resample-based pitch** (INFERENCE, architecturally
justified): (i) per-bin phases are the ONLY inspected mechanism family that excludes
the wobble -- required, because distortion-after-shifter turns periodic rephasing into
intermodulation products while merely smearing PV phasiness; (ii) R2 proves ~21 ms
spectral latency is achievable, inside our ultra-low budget where every other spectral
default (89-120 ms) fails; (iii) fixed -1/-2 primary keeps hop-rounding error negligible
and lets a short window (2048-class) resolve E2+ content -- low-B is the documented
marginal case, honestly scoped; (iv) dynamic-ratio machinery (servo + resampler
crossfade) is the proven path to future dives; (v) complexity is the lowest of the
spectral options (no guide/classifier/scales). Runner-up: R3-live-shifter single-window
guided PV (~27-60 ms) if R2's LF/inheritance quality proves inadequate on low-B.
Signalsmith's remap-only design is disfavoured (120 ms, no transient path, remap
machinery mismatched to fixed detune); SoundTouch's time-domain line shares our disease.
Note: this is an ARCHITECTURE fit verdict, not a build/buy decision (licensing: R2/R3
are GPL/commercial -- section 12).

### D6. Architecturally-justified (not ad-hoc) hybrid ideas

Each hybrid below breaks the single global decision along a principled axis:
- H1 transient/sustain split (time axis): WSOLA places attack frames (its proven
  strength: outright-max alignment, short window, LF intact), short-PV renders sustain
  (per-bin stability, no wobble), ~5-10 ms crossfade at the boundary. Justification:
  attacks are broadband-correlated (one lag is near-correct for all partials); sustains
  are narrowband-resolved (per-bin phases required). Mirrors R2's reset philosophy taken
  to topology level. Risk: boundary timbre step; needs the DI chug-vs-sustain sections
  to arbitrate.
- H2 band-split dual WSOLA (frequency axis): independent single-alignment per band
  through our exact-sum crossover. Justification: partials within a narrow band are
  near-harmonically related, so intra-band rephasing per skip shrinks; cross-band
  precession (the audible fourth/fifth against root) disappears. Risk: each band still
  wobbles internally; doubles correlation cost.
- H3 WSOLA-servoed PV hops (control axis): WSOLA lag trajectory drives PV synthesis-hop
  timing (R2-style anchoring from time-domain evidence). Justification: keeps ALL phase
  decisions per-bin while grounding transient placement in waveform truth. Risk: most
  novel, least precedent -- research-grade.
- REJECTED as ad-hoc: per-partial lag voting inside WSOLA (reintroduces F0 tracking
  through the back door); longer asymmetric search (hardware already falsified);
  post-hoc wobble cancellation (fights a structural cause with a cosmetic effect).

### D7. Which parts of our lab infrastructure stay useful regardless?

ALL of it (INSPECTED): the KS DI recipe (`labpitch_synth_di.py` -- deterministic,
sectioned, pick transients included) is backend-agnostic; the pinned test suites
(latency formulas, bit-exact determinism, no-starve grids) encode contracts any
successor must satisfy; the trace/landscape telemetry generalises to any search-based
component; the study/traj harnesses (`LabWsolaStudy`, `LabWsolaTraj`) are measurement
tools, not WSOLA tools; the live wrapper (`LabWsolaLive`) is engine-agnostic audition
scaffolding (constant-latency bypass + ramped toggle); the crossover module is a proven
exact-sum splitter reusable by H2 or any multiresolution successor.

---

## 8. Lessons for our WSOLA line

Each lesson ties inspected code on both sides (ours -> mature):

1. **Go causal.** Our +Dp future reach costs 320-480 smp of latency for search span we
   could take from history (`LabWsolaShift::reset`: `nextFrameAt_ = tolPlus_ + frameLen_`).
   SoundTouch searches [0, seek) over buffered input only (`TDStretch::processSamples`) --
   same span rule, no lookahead term. A causal W20 keeps span 960 at latency 963 (~20 ms).
   Experiment E2 tests the cost in join quality.
2. **Bias toward small skips, not drifting skips.** Our 1e-3 drift-seeking tie-break
   (`placeFrame` pass 2) maximises drift continuity; SoundTouch's parabolic center bias
   minimises skip SIZE. Since rephasing damage scales with skip size (phase = 2*pi*f*d),
   small-skip preference directly reduces per-skip wobble depth. A/B the two tie-breaks
   on sustained fourths (traj harness already measures AM depth).
3. **Try candidate-energy score normalisation.** Our full NCC divides by tail energy too
   (`num/sqrt(candE*tailE)`); SoundTouch divides by candidate energy only. Ours is more
   correct per-pair; theirs may be more stable when the tail is mid-crossfade. Cheap A/B.
4. **Keep the transient guard -- it is ahead of SoundTouch.** Our HP-rise + floor clamp
   (`transLo`, `kFluxRise`) has no counterpart in `TDStretch`; probes show it saves pick
   attacks. Do not "simplify" toward SoundTouch here.
5. **Filter the resampler.** Our bare cubic (`placeFrame` slow read) vs SoundTouch's
   64-tap AA FIR (`AAFilter`) / RB polyphase. With NAM after the shifter, alias images
   become intermodulation -- measure images first (sine sweep), then decide taps.
6. **Accept the ceiling.** Lessons 1-3 reduce wobble rate/depth; D3 proves none removes
   the class. Cap WSOLA-line investment at the transient-specialist role (H1) unless E1
   fails and E2 surprises.

---

## 9. Lessons for our PV line

1. **Delimit transient resets in frequency.** Our full-spectrum reset
   (`LabPitchShift::processFrame`, `synthPhase_ = phase_`) interrupts coincident stable
   partials -- R2's docs warn exactly of this for Crisp mode. Adopt R3's discipline
   (reset percussive bands, spare harmonic lows) or R2's Mixed default (spare 150-1000 Hz).
2. **Anchor transients in time, not just phase.** Our reset preserves attack SHAPE but
   places it on the rigid hop grid; R2's `StretchCalculator` interpolates hop sizes
   around exact-time anchors. This is the highest-leverage PV addition (see E1).
3. **Constrain across frequency, not just within frames.** Our nearest-peak re-lock is
   per-frame only; add vertical neighbour constraints (Signalsmith twist blend) or
   peak-lock bands with beta offsets (R3) so partials can't drift independently.
4. **Shorten the window before adding machinery.** PV-D (2048/256, ~45 ms) is already
   closer to liveable than any guided long-window design; lessons 1-3 applied to PV-D
   test the R2 thesis (short + disciplined > long + clever) at lowest cost.
5. **Upgrade detection before trusting resets.** Our single flux statistic
   (`kFluxFactor`, `kFluxAdd`) should become a compound detector (R2: rise-fraction +
   median-filtered HF) so resets fire on attacks, not on chord-beat flux.
6. **Borrow windows, not just algorithms.** Kaiser-PR (Signalsmith) or window-area
   normalisation (R2) should replace Hann/Hann once lessons 1-2 land -- sidelobe
   leakage between dense chord partials is a second-order wobble contributor.
7. **Go dynamic when the static case wins.** All mature engines modulate per block; our
   reset-gated shift blocks the divebomb roadmap. RB's pattern (servo checkpoint +
   clamped hops + resampler crossfade) is the template -- but only after E1 validates
   the static short-PV core.

---

## 10. Best candidate architectures

Ranked for the section-1 target (INFERENCE from the matrix):

1. **Short disciplined PV ("PV-D hardened", R2-shaped).** 2048-class window, per-bin
   propagation + peak-lock bands, compound onset detection, time-anchored frequency-
   delimited transient resets, resample-based pitch, AA-filtered resampler. Expected:
   ~25-45 ms, no wobble class, R2-precedented liveability. Main risk: low-B marginal
   resolution (honest scope: validate on DI section 6; low-B may need 4096 lows or H2).
2. **Transient/sustain hybrid (H1).** W20-asymmetric for attacks + candidate 1 for
   sustains. Expected: best attack scores of any option + stable sustains. Main risk:
   boundary artefacts; added complexity. Pursue only if candidate 1's attacks fall
   short of WSOLA on the chug sections.
3. **Guided single-window PV (R3-live-shaped).** Fixed-block live topology, 2048 FFT,
   guided reset/unlock ranges, dual resamplers. Expected ~27-60 ms, better LF
   behaviour than candidate 1 via guidance. Main risk: complexity without multires
   payoff; GPL/commercial licence if borrowed rather than reimplemented.
4. **Causal small-skip WSOLA (fallback).** Lessons 8.1-8.3 applied: causal search,
   center bias, candidate-energy score; ~20 ms. Expected: best latency, reduced but
   PRESENT wobble. Fallback if all spectral candidates fail below 30 ms.
5. **Revisit multiresolution (deprioritised).** R3's valley-descending adaptive
   crossovers are the only multires design that addresses our crossover problems -- but
   at R3-multi latency (~43 ms) and high complexity. Revisit only if low-B forces it.

## 11. Recommended next experiments

Ranked by information value (each reuses the DI + pinned harnesses; each falsifies a
load-bearing claim):

- **E1 (highest): PV-D + R3-style frequency-delimited reset + R2-style time anchoring
  probe.** Fork `LabPitchShift` at 2048/256; delimit resets to percussive/high bands
  (spare sub-~1 kHz); interpolate synthesis hops around detected onsets. Measure on DI:
  sustain-fourth AM depth (must drop vs W20-a), chug attack preservation (must approach
  WSOLA), low-B root stability, latency. Falsifies: "short PV can hold chords at live
  latency." Cost: medium (one fork, existing instruments).
- **E2: Causal small-skip WSOLA probe.** Fork `LabWsolaShift` W20: causal search span,
  SoundTouch-style center bias, candidate-energy score. Measure: skip size/rate stats,
  fourth-precession depth, join quality, latency (~20 ms expected). Falsifies: "WSOLA
  mitigations can reach acceptable sustain." Cost: low.
- **E3: Resampler image measurement.** Sine sweeps through our cubic vs AA-filtered
  cubic vs polyphase, pre/post NAM-style waveshaping. Falsifies: "bare cubic is clean
  enough after distortion." Cost: low; blocks H1 boundary work if images dominate.
- **E4: H1 hybrid probe.** Attack/sustain router over W20-a + E1 engine with short
  crossfade. Falsifies: "one engine must do both jobs." Cost: medium-high; do last.
- **E5: Vertical-constraint graft.** Add Signalsmith-style neighbour twist blend to the
  E1 fork at fixed window. Falsifies: "cross-frequency constraints buy quality at fixed
  latency." Cost: medium; do only if E1 sustains are close-but-phasey.

**RECOMMENDED NEXT RESEARCH STEP (exactly one, not a production decision):**
Run **E1** -- the PV-D + frequency-delimited reset + time-anchoring probe -- because it
is the cheapest test of the study's central architectural bet (per-bin short PV escapes
the wobble at live latency), it reuses every piece of lab infrastructure, and both
outcomes are decisive: success promotes candidate 1 and scopes WSOLA to transients;
failure (with E2 as the immediate follow-up) tells us the wobble/latency frontier is
harder than any inspected engine suggests.

---

## 12. Integration/licensing considerations

Technical research, not legal advice. "Safe to study" = reading code to understand
algorithms; all four projects permit that. Anything beyond studying diverges:

- **SoundTouch (LGPL-2.1-or-later, `COPYING.TXT`, per-file headers):** linking (even
  dynamic) into a closed product imposes LGPL duties (licence notice, source offer for
  the library, relinkability with modified library). Static linking is the worst case.
  Study: safe. Integrate: needs LGPL compliance engineering. Copy-from: no.
- **Signalsmith Stretch (MIT, `LICENSE.txt`):** permissive -- study, link, distribute,
  and adapt freely with copyright-notice preservation. Lowest integration friction of
  the four. Its `linear` dependency is likewise MIT (0.6.4 pinned).
- **Example code repo (NO license statement found -- dive-grepped, negative result):**
  study-only. No licence file and no licence grant means no right to copy, link, or
  distribute its code can be assumed from the repo itself; successor-MIT claims are
  secondary and do not cover this snapshot. Clean-room discipline mandatory (see below).
- **Rubber Band (GPL-2-or-later + paid commercial alternative, `COPYING` + README):**
  study safe; shipping its code (or derivatives) in a closed product requires the
  commercial licence; shipping under GPL requires full GPL compliance. Of the four,
  the highest integration bar -- which is why section 10 recommends its ARCHITECTURE
  (reimplemented), never its code.
- **Clean-room reimplementation provenance discipline:** (1) this report + DIVE.md files
  are the only design inputs (own-words descriptions, tiny snippets); (2) no external
  source file open while writing new lab code; (3) provenance log per new module (which
  report section + which public concepts, dated); (4) no copied identifiers, comments,
  or magic constants -- re-derive (e.g. our tie-break constants come from our probes,
  not their bias curves); (5) fresh tests from our DI recipe, not their test vectors.
  This discipline is what keeps an R2-shaped reimplementation ours.

---

## 13. Exact repositories/commits/files/functions inspected

SHAs re-verified against the /tmp clones in this session (INSPECTED via git rev-parse).

| Project | Repo URL | Branch/tag | SHA | Licence | Files/functions inspected |
|---|---|---|---|---|---|
| TechDeathMachine (ours) | local, READ-ONLY | n/a | n/a | ours | `dsp/lab/Pitch/LabWsolaShift.h/.cpp` (setSearch/setConfig/reset/placeFrame/processBlock/trace/landscape), `LabPitchShift.h/.cpp` (setConfig/reset/processFrame/processBlock), `LabMultiPitch.h/.cpp` + `LabCrossover.h/.cpp` (reset/processSample/processBlock), `LabWsolaLive.h` (prepare/processBlock), `LabWsolaStudy.cpp`, `LabWsolaTraj.cpp`, `tests/TestLabPitch.cpp` (study grid), `tests/TestLabWsolaLatency.cpp` (geometry table a/m + pins), `scripts/labpitch_synth_di.py` (DI recipe) |
| SoundTouch | https://codeberg.org/soundtouch/soundtouch (official; studied via release tarball https://www.surina.net/soundtouch/soundtouch-2.4.1.tar.gz) | 2.4.1 (`SOUNDTOUCH_VERSION`, changelog head) | UNRESOLVED (git blocked; see below) | LGPL-2.1 (`COPYING.TXT`) | `include/SoundTouch.h`, `source/SoundTouch/SoundTouch.cpp` (`calcEffectiveRateAndTempo`, `putSamples`, `getSetting`), `TDStretch.h/.cpp` (`processSamples`, `calcCrossCorr`, `seekBestOverlapPositionFull/Quick`, `overlapStereo`, `setTempo`, `calcSeqParameters`, `newInstance`), `RateTransposer.h/.cpp` (`newInstance`, `processSamples`, `setRate`, `getLatency`), `AAFilter/FIRFilter/Interpolate*`, `FIFOSampleBuffer.cpp`, `sse/mmx_optimized.cpp` |
| Signalsmith Stretch | https://github.com/Signalsmith-Audio/signalsmith-stretch | main, tag 1.4.0 | a670068d9aeb64913331d5cc29337b19a457a7df (verified) | MIT (`LICENSE.txt`) | `signalsmith-stretch.h` (full: `configure`, `process`, `processSpectrum`, `findPeaks`, `updateOutputMap`, `mapFreq`, `updateFormants`, `setTransposeFactor`, `inputLatency`, `outputLatency`, `outputSeek`, `presetDefault`, `seek/flush/exact`), `linear/stft.h` (`DynamicSTFT`, `addWindowProduct`), `cmd/main.cpp`, `cmd/main-dev.cpp`, `CMakeLists.txt` (FetchContent linear 0.6.4), `web/*` |
| Example code | https://github.com/Signalsmith-Audio/pitch-time-example-code | main, no tags | 3ab4e4457af6c314b347a8e4a4e48756f4ba7bbd (verified) | NONE FOUND (study-only) | `shift-stretch.h` (full 894 lines: all 8 classes incl. `HybridPhaseStretch::processSpectrum/scaleAngle`, `WsolaStretch::processBlock`, `PhaseVocoderStretch::processSpectrum`), `main.cpp` (`WavCmd`), design article https://signalsmith-audio.co.uk/writing/2023/stretch-design/ (successor contrast) |
| Rubber Band | https://github.com/breakfastquay/rubberband | default HEAD | e4296ac80b1170018a110bc326fd0d45a0eb27d6 (verified) | GPL-2+ or commercial (`COPYING`) | `RubberBandStretcher/LiveShifter` API+Impl, `src/finer/*` (`R3Stretcher::consume/analyseChannel/synthesiseChannel/getStartDelay/calculateHop`, `R3LiveShifter::shift/readIn/generate/readOut`, `Guide::updateGuidance`, `PhaseAdvance::advance`, `BinClassifier::classify`, `BinSegmenter::segment`), `src/faster/*` (`R2Stretcher::calculateSizes/getEffectiveRatio`, `StretcherProcess::modifyChunk/analyseChunk`, `CompoundAudioCurve`), `src/common/*` (`StretchCalculator::calculate/calculateSingle`, `BQResampler`, `FFT::pickImplementation`) |
| signalsmith-linear (dep) | https://github.com/Signalsmith-Audio/linear | 0.6.4 pinned (clone HEAD de55e6a5) | de55e6a50ffcf6f8f43f649692d94691c7025151 (clone only) | MIT | `stft.h` full, `fft.h` skimmed (backend/step API) |

Unresolved items carried over + new (see submission object): SoundTouch git SHA;
SoundStretch/BPM/PeakFinder bodies; example `dsp/*` dependency bodies; talk video;
example repo licence; no-build constraint; BQ steady-state group delay; RB
per-backend FFT/Thread/frontends; formula-derived (unmeasured) latencies; PV-B/C
label assumption; Doppler-A description-only status.


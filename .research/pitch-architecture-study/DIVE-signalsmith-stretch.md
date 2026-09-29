# Signalsmith Stretch — architecture dive (for TDM pitch-study contrast)

## Provenance

- Repository URL: https://github.com/Signalsmith-Audio/signalsmith-stretch
- Branch checked out: `main`; tag pointing at HEAD: `1.4.0`
- Commit SHA: `a670068d9aeb64913331d5cc29337b19a457a7df`
  (`git log -1`: 2026-09-25 10:54:33 +0100, "Bump Linear version (0.6.4 for bugfixes) and add block/overlap args to cmd/")
- In-header version constant: 1.3.2 (`signalsmith-stretch.h`, `SignalsmithStretch::version`) — stale vs the 1.4.0 tag.
- License: MIT, file `LICENSE.txt` (Copyright 2022 Geraint Luff / Signalsmith Audio Ltd.)
- Dependency (not vendored, fetched by CMake at pinned tag 0.6.4): https://github.com/Signalsmith-Audio/linear
  (cloned shallow to /tmp/tdm-pitch-study/linear at de55e6a5 for STFT reference; CMakeLists.txt pins 0.6.4).
- Clone location for this study: /tmp/tdm-pitch-study/signalsmith-stretch (repo untouched; no builds run).

## Files actually inspected (opened and read in full)

1. `signalsmith-stretch.h` (1060 lines) — the entire algorithm, header-only, `signalsmith::stretch::SignalsmithStretch<Sample, RandomEngine>`.
2. `include/signalsmith-stretch/signalsmith-stretch.h` — one-line shim including the root header.
3. `README.md` (177 lines) — API/latency/seek/flush/formant docs.
4. `CMakeLists.txt` — INTERFACE lib + FetchContent of signalsmith-linear 0.6.4.
5. `cmd/main.cpp` — file-stretch example (outputSeek/process/flush staging, block-ms/overlap flags).
6. `cmd/main-dev.cpp` — profiling/dev harness; asserts zero allocation inside `process()`.
7. `cmd/CMakeLists.txt` — example build.
8. `web/emscripten/main.cpp` — WASM export wrappers (configure/seek/process/flush passthroughs).
9. `web/web-wrapper.js` — AudioWorklet driver; notable for its seek-every-block time-map strategy.
10. `SUPPORT.txt`, `LICENSE.txt` — metadata.
11. Dependency `linear/stft.h` (`DynamicSTFT`, windows, ring buffers, normalisation) — read fully; `linear/fft.h` skimmed for backend/step API only.

## One-paragraph summary

Signalsmith Stretch is a single-resolution spectral phase-vocoder pitch-shifter/time-stretcher in one header, with no time-domain splicing and no resampler.
Pitch comes from a peak-anchored spectral frequency remap, time-stretch comes from consuming input at a different rate than output, and phase coherence is maintained by a two-pass
prediction: a per-bin time-propagation pass (classic phase-vocoder twist) followed by a cross-frequency ("vertical") re-prediction pass that blends short (+/-1 bin) and long
(+/-fft/interval bins) neighbour constraints measured from the input spectrum, with the strongest channel leading and other channels phase-locked to it.
There is no transient detector, no multiresolution bank, and no global alignment splice, so the WSOLA-style global-skip rephasing wobble cannot occur in the same form;
its failure modes are phasiness/smear instead. Default latency is ~1 window (120 ms at 48 kHz) split into two reported halves.

---

## (1) Input / buffering

- Block model: fixed STFT geometry chosen once in `configure()` (`signalsmith-stretch.h:SignalsmithStretch::configure`): `blockSamples` (window/FFT span)
  and `intervalSamples` (hop). Presets: `presetDefault` = 0.12 s block / 0.03 s hop (4x overlap); `presetCheaper` = 0.10 s block / 0.04 s hop (2.5x overlap).
  The CLI (`cmd/main.cpp`) exposes these as `--block-ms` (default 120) and `--overlap` (default 4).
- Underneath, `configure` calls `stft.configure(channels, channels, blockSamples, intervalSamples + 1)` then `stft.setInterval(intervalSamples, kaiser)`
  (`linear/stft.h:DynamicSTFT::configure/setInterval`). So the input ring holds block + ~1 hop of history; windows are Kaiser with forced perfect reconstruction
  (see facet 4); FFT length is rounded up by `fastSizeAbove`.
- Scheduling lives in `process(inputs, inputSamples, outputs, outputSamples)` (`signalsmith-stretch.h:SignalsmithStretch::process`): the caller passes
  arbitrarily-sized input and output spans (they need not match; their ratio IS the time-stretch factor). A per-output-sample loop tracks
  `blockProcess.samplesSinceLast`; whenever a full hop has elapsed it starts a new spectral block whose input position is
  `inputOffset = round(outputIndex * inputSamples / outputSamples)`, i.e. linear interpolation of the input-consumption path across the call.
  A `copyInput` closure lazily writes caller input into the STFT ring (`writeInput`/`moveInput`) up to each block's position, then once more to the end of the call.
- `prevInputOffset` persists across calls (adjusted by `-inputSamples` at the end) so the input path is continuous between calls. `inputInterval`
  (per-block input advance) drives re-analysis decisions and the time-scaling factor (facet 11).
- Seeking/starting: `seek(inputs, inputSamples, playbackRate)` stuffs up-to (block + hop) samples into the ring without synthesising (`seekLength()` returns
  exactly block + hop); `outputSeek(inputs, inputLength)` does a full `reset()`, seeks to the start, pre-renders `outputLatency()` samples, then negates and
  time-reverses them into the output ring via `addOutput` as anti-pre-roll so the first `process()` output aligns with the sound start. `flush()` drains the
  tail with zero input plus a mirrored `finishOutput` taper to avoid clicks. `exact()` wraps outputSeek/process/flush for one-shot exact-length stretches.
- Fixed vs variable: STFT geometry fixed after `configure()`; the I/O block sizes are fully variable per call (README: "no maximum block size").

## (2) Pitch-shift decomposition (exact stage order)

There are exactly two coupled mechanisms and NO resampling stage anywhere in the codebase:

1. Pitch shift = spectral frequency remap inside one phase-vocoder pass. `setTransposeFactor`/`setTransposeSemitones`/`setFreqMap` define `mapFreq()`;
   per block, `findPeaks()` locates energy-weighted spectral peaks and `updateOutputMap()` builds a per-output-bin `outputMap[b] = {inputBin, freqGrad}`
   via smoothstep interpolation between peak anchors, so partials land on mapped frequencies and valleys stretch smoothly. Output-bin energies are read by
   fractional interpolation of input energies, scaled by the local map gradient (`freqGrad`) to preserve spectral density.
2. Time stretch = input-consumption rate. Nothing in the spectrum is duplicated/dropped by index; instead each output block analyses a different input
   position (facet 1) and the phase predictor is told the rate via `timeFactor = hop / max(1, inputInterval)` (or `seekTimeFactor` after a seek).

Pseudocode of one block (own words):

    inputPos  = round(outIdx * inLen / outLen);  inputStep = inputPos - prevPos
    copy caller input up to inputPos into STFT ring; stash ring state
    analyse current spectrum into Band.input (per channel)
    if |inputStep - hop| > 1 or after seek: re-analyse spectrum hop-ago into Band.prevInput
    if pitch active: smoothEnergies; findPeaks; updateOutputMap else identity map
    if formants: updateFormants (3 sub-steps)
    time-pass: per bin, twist = in * conj(prevIn); outPhase = out * twist / energy
    vertical-pass (8 chunks): per bin, max-energy channel blends +/-1 and +/-long neighbour outs
        through input-measured twists; other channels lock to leader via inter-channel twist
    prevInput = input; copy Band.output to spectra; synthesise + overlap-add; emit hop samples

## (3) Analysis representation

- Single-resolution complex STFT (`linear/stft.h:DynamicSTFT<Sample, false, true>` — note the third template arg selects the MODIFIED spectrum, i.e. bins
  sit at half-integer offsets: `binToFreq(b) = (b + 0.5)/fftSamples`). One window size only; no subbands, no multiresolution, no filterbank.
- Window: Kaiser with bandwidth = block/hop, heuristically optimised beta, then `forcePerfectReconstruction` (per-hop-phase energy normalisation) in the
  symmetric case; analysis and synthesis windows identical, offsets at the window peak (block centre).
- Per-channel working state is `Band {input, prevInput, output, inputEnergy}` (`signalsmith-stretch.h:Band`), one vector of bands x channels in `_channelBands`,
  plus `Prediction {energy, input}` per channel per bin (`Prediction`) holding the mapped (fractionally interpolated) input value and density-scaled energy.
- Classification is minimal: `smoothEnergy` (3 sub-steps: per-bin energy sum across channels into `energy[]`/`inputEnergy`, then two bidirectional
  single-pole smoothing passes with slew from `smoothingBins = fftSamples/hop`) produces `smoothedEnergy[]`; `findPeaks` marks runs where
  `energy > smoothedEnergy` and records each run's energy-weighted centroid bin plus its mapped position. No F0 tracker (except a crude 3-peak estimator
  used only for formant smoothing width — facet 6/12), no voicing detector, no transient classifier.

## (4) Continuity (waveform, phase-over-time, cross-frequency, inter-channel)

- Waveform: STFT overlap-add with self-normalising round-trip window-product tracking (`linear/stft.h:addWindowProduct/readOutput/moveOutput`), so overlapped
  blocks sum to unity gain for the stationary path.
- Phase-over-time (two sub-steps):
  (a) Expected-advance rotation: at each new spectrum, every stored `output` and `prevInput` phasor is advanced by its bin's nominal rotation over one hop
  (`rot = polar(1, binFreq * hop * 2pi)`, `processSpectrum` first `channels` steps). This factors nominal carrier advance out, leaving the twist to carry only deviation.
  (b) Preliminary vocoder prediction (next `channels` steps): `freqTwist = mappedInput * conj(mappedPrevInput)` (fractional interpolation of both),
  then `output = output * freqTwist / (max(prevEnergy, energy) + floor)`. Magnitude is deliberately discarded here; phase accumulates deviation-corrected advance.
- Cross-frequency (the distinctive part, `splitMainPrediction = 8` chunked steps): each output bin is re-synthesised as a weighted blend of up to 4 vertical
  predictions from already-computed neighbours at -1, +1, -long, +long bins, where `longVerticalStep = round(fftSamples / hop)` (i.e. the neighbour distance
  matching one hop's nominal rotation — the classic phase-locked-vocoder vertical-coherence distance). Each contribution multiplies the neighbour's output by
  a twist measured from the INPUT spectra (e.g. `mul<true>(prediction.input, downInput)`), so the output inherits the input's inter-bin phase relations;
  `Prediction::makeOutput` then re-applies the mapped magnitude (`sqrt(energy/phaseNorm)`), falling back to the input phasor when the blend cancels.
  Note the +/- asymmetry: upward steps use `mul(downOut, twist)`, downward steps use `mul<true>(upOut, twist)` (conjugate form), keeping propagation direction consistent.
- Inter-channel: for each bin, the channel with maximum prediction energy is computed first through the full vertical pass; every other channel is then
  phase-locked to it: `channelTwist = chanIn * conj(leaderIn)`, `chanOut = makeOutput(leaderOut * channelTwist)` — preserving the input's inter-channel
  phase differences exactly while sharing one magnitude/phase skeleton. This is a coherence-over-accuracy stereo tradeoff (facet 12).

## (5) Polyphony

- Single global STFT; no per-source/per-region alignment, no independent time-splices, no multi-F0 model. Polyphony is represented implicitly as multiple
  simultaneous peaks in one spectrum, each anchoring the frequency map (`findPeaks` typically yields one anchor per resolved partial; `updateOutputMap`
  smoothstep-interpolates the map between adjacent anchors and flat-extends beyond the extremes).
- Inter-partial rephasing is avoided by construction rather than tolerated: because every partial's phase propagates through input-measured twists
  (time twist + vertical twists), the relative phase evolution of the input is re-imposed on the output each block. There is no moment where all bins are
  jointly re-anchored to a single time offset, so there is no global-skip event.
- Global-skip rephasing failure mode: CANNOT exist in the same form. WSOLA's wobble comes from one splice offset being wrong for all but one periodicity;
  here each bin/partial advances by its own measured deviation, and cross-bin relations are re-asserted every block from the input. The analogous failure is
  diffuse instead of periodic: unresolved/overlapping partials within one peak region share one anchor and one vertical blend, so dense chords can sound
  phasey or blurred (energy smearing across the blend), and fifths-vs-thirds-type stability differences have no reason to appear — stability here depends on
  peak resolvability (window length vs partial spacing), not on interval ratios. For TDM contrast: this design trades WSOLA's periodic wobble for constant
  phasiness + transient smear.

## (6) Transients

- Detection: none. No onset detector, no reset-on-transient, no dual-resolution path. Grep-equivalent full read confirms: the only signal-dependent branch is
  the silence gate.
- Silence path (`process` head): if block energy < `noiseFloor` (1e-15) and `silenceCounter` exceeds 2 blocks, band states are zeroed once and the call
  degrades to input passthrough/wrap (if input given) or zeros — skipping all FFTs. This is a CPU optimisation, not transient handling.
- Resets/alternate paths: manual only (`reset()`, `seek()`, `outputSeek()`); nothing automatic on onsets.
- Smear tradeoff: fully accepted. A 120 ms window smears attacks across ~4 hops; the vertical predictor (which assumes locally sinusoidal inter-bin relations)
  blurs transient clicks into brief phasiness. README's "time-stretch sounds best 0.75x–1.5x" and the ADC-talk lineage (spectral, not WSOLA) confirm transients
  are the known weak axis. Mitigations available to a user: shorter `--block-ms` at the cost of LF resolution (facet 7), or pre/post transient bypass outside the library.

## (7) Low frequencies

- What sets LF resolution: the single window length. Bin spacing = sampleRate/fftSamples with fftSamples ~= block (rounded up to a fast size), so the default
  120 ms block gives ~8 Hz spacing; partials closer than ~1 bin share a peak anchor. `freqToBand`/`bandToFreq` are the only frequency quantisers; there is no
  zoom-FFT, no heterodyne LF path, no longer LF window.
- Why low notes need large windows HERE: with one resolution, the window must span several periods of the lowest target partial for `findPeaks` to resolve it
  and for the vertical predictor's twists to be meaningful (twists measured over +/-1 bin are noise if the partial's main lobe is narrower than a bin...
  conversely too-short windows merge low partials into one anchor). The 120 ms default (≈10 periods at 82 Hz low E) reflects this directly.
- Special LF paths: none. The map extension below the lowest peak is a flat offset (`bottomOffset`), so sub-lowest-peak content shifts rigidly — benign for
  rumble, slightly rigid for a low-B fundamental if it falls below the first detected peak.
- Low-B (~31 Hz) / low-E (~41–82 Hz) consequences: at 48 kHz, 120 ms ≈ 5760 samples, fft ≈ 6144ish → ~7.8 Hz/bin; low-B fundamental sits around bin 4
  (modified spectrum), so only a handful of bins cover the first partials; peak centroiding is coarse and `freqGrad` density scaling does most of the work.
  The `presetCheaper` 100 ms window is marginally worse. Nothing in the code special-cases this; the answer to "low notes" is purely "use a long window and
  pay the latency/smear".

## (8) Resampling

- Location/interpolator/share: NONE. There is no resampler, no interpolator, no anti-alias filter in the library; pitch shift is 100% spectral remap and
  time scaling is 100% input-consumption rate. `setTransposeFactor` only changes `mapFreq()` (plus the tonality-limit knee); `process()` never touches the
  output sample rate. Fractional positions appear only as bin-domain interpolation (`getFractional` linear interpolation of complex spectra/energies) and as
  the integer `round()` of the input-consumption path — both exact, neither a signal resampler.
- Interaction with time scaling: orthogonal by design — map and consumption rate are independent inputs (`outputMap` vs `inputSamples/outputSamples`), so
  pitch and time can be modulated independently (web-wrapper drives both per block).

## (9) Latency

All from `inputLatency()`/`outputLatency()` (`signalsmith-stretch.h`) over `linear/stft.h` primitives (`analysisLatency = block - analysisOffset`,
`synthesisLatency = synthesisOffset`; offsets = window peak ≈ block centre):

- Theoretical minimum: user-configurable; CLI allows arbitrary `--block-ms`/`--overlap`, so the floor is whatever window still resolves the signal
  (plus ≥1 hop of output ring). No hard-coded minimum in the library.
- Default (presetDefault @48 kHz): block 5760, hop 1440 → input ≈ 2880 (60 ms), output ≈ 2880 (60 ms); round-trip ≈ 120 ms. At 44.1 kHz: ≈ 60/60/120 ms likewise.
- Cheaper (presetCheaper, split ON): block 0.10 s, hop 0.04 s → input ≈ 50 ms, output ≈ 50 + 40 = 90 ms; round-trip ≈ 140 ms (cheaper per second, worse latency).
- Split-computation adds exactly one hop to output latency (`+ _splitComputation*defaultInterval()`).
- Overlap/windowing/buffering breakdown: input half = block − peak ≈ half window (lookahead: you must feed input that far ahead of the processing instant);
  output half = peak ≈ half window (overlap-add drain) + optional hop (split). No resampler delay (no resampler). API reports the two halves separately and
  README's automation section defines processing-time = output + outputLatency = input − inputLatency.
- vs TDM W20 (~30 ms): 4–5x higher round-trip by default; the price of 120 ms LF-capable windows.

## (10) CPU / realtime

- Allocations: all vectors sized in `configure()` (`_channelBands`, `energy`, `smoothedEnergy`, `outputMap`, `channelPredictions`, `formantMetric`,
  `tmpProcessBuffer`, `tmpPreRollBuffer`, STFT rings); `process()`/`seek()`/`flush()` only `resize()` within already-reserved capacity or swap ring handles —
  `cmd/main-dev.cpp` tracks allocations and fails the run if `process()` allocates. (`seek()`/`configure()` resize temps; steady-state `process()` is allocation-free.)
- FFT/correlation cost per hop per channel: 1 forward FFT normally, 2 when `reanalysePrev` (i.e. whenever the time-stretch rate ≠ 1, since
  `|inputInterval − hop| > 1`, or after seeks) + 1 inverse FFT; plus O(bands) prediction/vertical/formant passes with small constants (the vertical pass is
  the heaviest scalar loop, chunked 8 ways). README warns Debug builds run up to 10x slower and advises optimising the Stretch TU even in Debug.
- No correlation search at all (no NCC, no candidate lags) — the WSOLA O(window × search) term has no counterpart; cost is FFT-dominated O(N log N).
- SIMD: none hand-written in stretch; speedups come from the linear backend selected by SIGNALSMITH_USE_ACCELERATE / IPP / PFFFT(_DOUBLE) macros, else the
  scalar fallback FFT. `-ffast-math` explicitly supported (with an AppleClang-16 guard in linear).
- Callback-suitability: without split, each hop boundary does a burst (analyse + predict + synthesise) — fine for DAW-size buffering; with
  `splitComputation=true`, the FFT itself is stepped (`analyseStep`/`synthesiseStep` exist but note stretch instantiates `DynamicSTFT<Sample,false,true>`,
  i.e. splitComputation=false at the STFT level, so analyseSteps()==channels and the splitting granularity is per-channel/per-pass/per-chunk, NOT per-FFT-butterfly)
  and `processToStep` advances proportionally to samples emitted, spreading the burst across the hop at +1 hop latency. `process()` is sample-looped with
  per-sample `readOutput`/`moveOutput(1)` (fast path for 1 sample in `moveOutput`), so any host block size works; no threading inside.

## (11) Ratio changes

- Fixed vs dynamic: fully dynamic on both axes. Time ratio is inferred fresh from every `process()` call's `inputSamples/outputSamples` (caller keeps the
  running average; README). Pitch (`freqMultiplier`/`customFreqMap`/`tonalityLimit`) is read live at each new block (`mappedFrequencies` flag,
  `updateOutputMap` per block). Formant factor likewise (`processFormants` per block).
- Smoothing: NONE on pitch/time/formant parameters — a stepped semitone change takes effect at the next block boundary (≤1 hop later). Phase continuity is
  preserved structurally (predictor carries `output` phasors across the change; only the map/energies jump), so steps glide without clicks but without
  portamento either. No zipper-noise filter.
- Divebomb-style modulation: supported naturally — set a new transpose factor per host block and each spectral block re-peaks/re-maps; the web demo sets
  transpose/formant per AudioWorklet quantum. Fast sweeps smear across the window (120 ms of history per analysis) rather than tracking instantaneously.
- State/latency during change: unchanged geometry, so latencies are constant; a rate change only flips `reanalysePrev` on (extra analysis FFT per block while
  rate ≠ 1) and rescales `timeFactor` (with the >2x clean-stretch clamp + randomised spread to decorrelate slowdown artefacts:
  `timeFactorDist(maxCleanStretch*2*timeFactor... )` — uniform jitter around the requested factor). Hard discontinuities (loops/jumps) should go through
  `seek()`/`outputSeek()` (web-wrapper seeks every block for varispeed playback, then `process(0, N)`).

## (12) Deliberate tradeoffs visible in code

1. One long window for everything (LF resolution + frequency precision) vs transient smear and 120 ms latency; no multiresolution escape hatch.
2. Peak-anchored smoothstep map vs naive linear scaling: preserves harmonic relations and avoids partial splitting, at the cost of peak-detection dependence
   (unresolved/noisy spectra get a mushy map; empty-peak fallback is identity).
3. Tonality limit (`mapFreq` knee: linear below, rigid shift above) vs pure shift: preserves high-frequency timbre/brightness at the cost of inharmonic
   top octaves for large shifts.
4. Vertical 4-neighbour re-prediction vs classic per-bin vocoder: much better inter-partial/attack coherence, at the cost of the heaviest scalar loop and
   some phasiness when the sinusoidal assumption breaks (noise/transients).
5. Max-energy-channel phase lock vs per-channel prediction: guarantees stereo image stability, at the cost of per-channel accuracy in wide/diffuse material.
6. Randomised time-factor spread beyond 2x slowdown (`maxCleanStretch`) vs clean-but-metallic extreme stretch: trades metallic ringing for noisier diffusion.
7. Silence bypass vs bit-exactness: passes input through (even wrapping) during long silence — inaudible, saves FFTs, but output ≠ processed zeros.
8. Split-computation mode vs latency: ~even per-sample cost at +1 hop latency; without it, bursty but lower-latency.
9. `Prediction::makeOutput` fallback (weak blend → raw input phasor) vs pure prediction: avoids dropouts/cancellation at the cost of momentary under-coherence.
10. Formant path as energy-ratio reweight (`bins[b].inputEnergy *= energyRatio`) with only a 3-peak rough F0 guess: cheap and unconditionally stable, but
    explicitly "not as sharp as monophonic algorithms such as PSOLA" (README).

## Contrast notes for TDM (mechanisms that would/would-not wobble)

- No variable delay line, no correlation search, no splice offset: the entire WSOLA failure family (periodic skip-back, drift-seeking tie-breaks, search-window
  edge effects) has no counterpart. Closest analogue is the vertical predictor's dependence on `longVerticalStep` neighbours — a wrong long-range lock would
  show as static phasiness, not a periodic wobble, because it is re-measured from the input every block rather than integrated from a splice decision.
- The one periodic structure is the hop grid itself (blocks every 1440 samples); artefacts attach to it as stationary coloration (window modulation),
  not as signal-dependent precession — fourths/thirds vs fifths should behave identically here, differing only by peak resolvability.
- Takeaway for TDM: per-partial phase propagation + input-measured cross-frequency twists is the mechanism that buys polyphonic stability without a global
  alignment; its price (120 ms windows, smear, phasiness) is exactly what WSOLA avoids — the two designs sit at opposite ends of the splice-vs-smear tradeoff.

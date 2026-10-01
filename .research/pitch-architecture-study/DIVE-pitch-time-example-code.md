# DIVE: Signalsmith-Audio pitch-time-example-code (ADC22 "Four Ways To Write A Pitch-Shifter" example code)

## 0. Repository identity

- Exact repository URL: `https://github.com/Signalsmith-Audio/pitch-time-example-code`
- Branch checked out: `main` (also `origin/main`; no tags in repo)
- Commit SHA: `3ab4e4457af6c314b347a8e4a4e48756f4ba7bbd` (`git rev-parse HEAD`)
- Commit date/subject: `2023-04-29 16:02:53 +0100` / `Add link to Signalsmith Stretch`
- Full history is 3 commits: `58c15ed` Initial commit (2022-11-16), `9c7ab72` Edit README (2022-11-16), `3ab4e44` Add link (2023-04-29)
- License: **no license file and no license statement found anywhere in the repo** (searched all tracked files for license/copyright/MIT/GPL/Apache/BSD; only hits are the word "Signalsmith" and DSP-namespace references). License-file path: *none exists*. Note: the successor libraries this code feeds into (Signalsmith Stretch, signalsmith-dsp) are publicly described as MIT-licensed, but that claim is NOT verifiable from this repo itself.
- External dependency (NOT vendored, build breaks without it): the author's DSP library at `https://signalsmith-audio.co.uk/code/dsp/`, included as `dsp/delay.h`, `dsp/windows.h`, `dsp/fft.h`. No `dsp/` directory ships in this repo.
- Companion design article (found via web search, fetched and read): **"The Design of Signalsmith Stretch"**, Geraint Luff, Signalsmith Audio Ltd., 2023-04-29 — `https://signalsmith-audio.co.uk/writing/2023/stretch-design/`. It describes the *successor* library (Signalsmith Stretch), explicitly noting it "takes some ideas in a slightly different direction" from this example code. Treated below as contrast, not as ground truth about this repo.
- Talk video: ADC22 "Four Ways To Write A Pitch-Shifter" — `https://youtu.be/fJUmmcGKZMI` (referenced from article + Stretch README; not watched, no claims drawn from it).
- Successor project page: `https://signalsmith-audio.co.uk/code/stretch/`.
- All DSP logic lives in one file: `shift-stretch.h` (~894 lines). `main.cpp` (~209 lines) is an offline WAV CLI. `util/*.h` are CLI/WAV helpers, no DSP.

## 1. Progression of approaches (what each variant does differently)

The repo contains one base class plus 7 transform variants, exposed as 8 CLI subcommands in `main.cpp:main`. Ordered from time-domain to the final hybrid:

| # | CLI mode | Class (`shift-stretch.h`) | Domain / idea in own words |
|---|----------|---------------------------|----------------------------|
| 0 | `rate` | (none; WAV header retag) | Reference: change of sample-rate tag only, pitch+time move together. Not a stretcher. |
| 1 | `overlap-add` | `OverlapAddStretch` | Plain time-domain overlap-add: windowed blocks taken at input rate, overlapped at output rate. No search, no phase work. Baseline that smears/flanges. |
| 2 | `wsola` | `WsolaStretch` | Time-domain WSOLA: same overlap-add, but each block is slid by a small offset that minimises normalised waveform difference vs the previous block, all channels jointly; next block time is then pulled by half the offset. |
| 3 | `spectral-cut` | `SpectralCutStretch` | Frequency-domain *segment mover*: splits each spectrum into contiguous segments where local energy exceeds a smoothed floor, shifts each segment by an integer bin offset derived from its energy centroid times the pitch factor, and applies one phase rotation per segment so it continues the previous output block. Direct pitch-shift, no resample. |
| 4 | `phase-vocoder` | `PhaseVocoderStretch` | Conventional phase vocoder time-stretch (per-band phase advance measured across input blocks, angle re-scaled to the output step), with an optional blend of raw input phase for fresh energy (default ON; `--pure` disables). Pitch-shift is done by stretch-then-retag. |
| 5 | `paul-stretch` | `PaulStretch` | Extreme/texture endpoint: keeps magnitudes, randomises every phase each block. Shows what full loss of both phase continuities sounds like. |
| 6 | `vase-phocoder` | `VasePhocoderStretch` | "Vertical" counterpart to the vocoder: builds each output spectrum by chaining upward in frequency, measuring each step's phase change over a stretched stride in the *current* input spectrum only (no cross-block state). Direct pitch-shift, no resample. |
| 7 | `hybrid-phase` | `HybridPhaseStretch` | **Final/most advanced variant.** Per output bin, blends several complex phase predictions — multi-stride vertical (timing), horizontal phase-vocoder (pitch), inter-channel — weighted by their own magnitudes plus a bonus for the strongest, then stamps the mapped energy onto the blended phase. Direct time *and* frequency mapping, no resample. |

Trajectory summary: time-domain alignment (1–2) → move spectral chunks with per-chunk phase fix (3) → classic horizontal phase propagation (4) and its randomised null (5) → purely vertical within-block phase chaining (6) → weighted blend of horizontal + vertical + channel evidence (7). The article confirms method 7 is what the successor Stretch library is based on, with later refinements (two-sweep up/down prediction, loudest-channel stereo copy, peak-locked nonlinear frequency map, vertical-scale cap) that are **absent** from this example code.

## 2. Twelve facets (final variant = `HybridPhaseStretch`; contrasts for earlier ones)

### (1) Input/buffering: block model, FIFOs/rings, latency seeds, scheduling

- Fixed-block streaming overlap-add engine shared by ALL variants: `OverlapAddStretch::configure` fixes `blockSamples`, `intervalSamples` at setup; `MultiBuffer` ring histories hold `blockSamples + maxExtraInput` input samples and `blockSamples` of summed output per channel.
- Sample-driven scheduler: `OverlapAddStretch::process` emits output one sample at a time and fires a new block every `intervalSamples` output samples, pulling its input block from fractional time `round(o*invTimeFactor - surplus - block)` clamped to available history. A fractional `surplusInputSamples` accumulator absorbs non-integer rate conversion; `samplesForOutput` reports required input per output chunk so the caller can feed variable-size blocks (CLI uses 256-sample output chunks).
- Initial latency is one full block: `inputLatency()` returns `block/2`, `outputLatency()` returns `block - block/2`; the CLI's total is `round(inLat*stretch + outLat)` and its `--trim` mode cuts exactly that. No extra lookahead buffers exist; "lookahead" is just the second half of the centred window.
- Scheduling hooks: `scheduleNextBlock` lets a variant pull the next block earlier/later (only WSOLA uses it: `interval - bestOffset/2`). Spectral variants fire on the rigid grid; rate changes appear as variable `inputIntervalSamples` passed into `processBlock`/`processSpectrum`.
- Contrasts: identical engine for 1–7; only window choice differs (sine-double-window for time-domain classes, Kaiser for spectral ones, both perfect-reconstruction-normalised in `configure`).

### (2) Pitch-shift decomposition: exact stage order

- **Hybrid (final): single-stage direct spectral mapping — NO resample anywhere.** Each output block: STFT → per-band energy read at frequency-mapped position `freqToBand(bandToFreq(b)/freqFactor)` → phase from blended predictions → inverse STFT → overlap-add. Time factor enters only through (a) input block spacing, (b) vertical stride lengths, (c) horizontal angle scaling. Same single-stage design for `spectral-cut` and `vase-phocoder`.
- Time-stretch+resample decomposition is used ONLY by `overlap-add`, `wsola`, `phase-vocoder`, `paul-stretch`: the CLI stretches by `time*freq` and then retags the WAV sample-rate (`outputWav.sampleRate *= freqFactor`), i.e. 100% of the pitch shift is a *header rewrite* in this offline demo — there is no interpolating resampler in the repo at all.
- Article contrast: the successor Stretch library likewise does time and frequency separately (called "a little unusual" in the article), and discusses why matched 2x-time + octave-down is currently NOT bit-equivalent to a resample (would need reshaped observation windows).

### (3) Analysis representation

- Hybrid: one short-time Fourier spectrum per channel per block (Modified Real FFT with half-bin offset, so `bandToFreq(b) = (b+0.5)/size`), zero-padded 2x by default (`mrfft.setFastSizeAbove(block*zeroPadding)`), Kaiser-windowed. No subband tree, no multiresolution, no autocorrelation/periodicity estimator, no peak list, no voiced/unvoiced classifier in THIS code.
- Derived quantities per output band: interpolated energy (`getEnergy`, linear between bins), interpolated complex bins (`getBin`), both with conjugate-mirrored extension for below-DC lookups (negative-frequency handling).
- Contrasts: WSOLA/OLA are pure time-domain (no spectrum); `spectral-cut` adds an energy-vs-smoothed-floor segmentation (nearest thing to peak handling in-repo); `phase-vocoder` adds prev-input/prev-output spectra + per-band rotation state; `paul-stretch` uses magnitudes only. Peak detection + nonlinear frequency map exist only in the successor (article), not here.

### (4) Continuity: waveform, phase-over-time, cross-frequency, inter-channel

- Waveform: enforced structurally by double-windowing + overlap-add with `forcePerfectReconstruction` normalisation; the only waveform search is WSOLA's.
- Phase-over-time (horizontal): `rotation = bin*conj(prevBin)` between current and previous *input* spectra at the mapped bin, angle scaled by `timeFactor*freqFactor` in `scaleAngle`, then applied to the previous *output* band. Magnitude of the rotation (product of the two bin magnitudes) becomes the prediction's implicit weight. Rotations persist in `horizontalRotations` so repeated input frames reuse them.
- Cross-frequency (vertical): output bands are assembled strictly upward (`b = 0..bands`), each band chaining off already-decided lower neighbours `newSpectrum[b-steps]` via strides `{1,2,4,8,16}` scaled by `timeFactor` (single-stride mode uses stride 1 only). Before any of this, spectra are rotated to block-centre time (`centreTimeRotations` = shift by `-block/2`) so the vertical phase deltas are centred and interpolation-safe; rotated back after.
- Inter-channel: for `c > 0`, rotation measured against channel 0's *current* spectrum and applied to channel 0's *new output* band, i.e. the stereo image is a third voted prediction, not a hard copy (successor instead hard-copies from the loudest channel per the article).
- Blending rule (own-words pseudocode of the per-band core):
  ```
  out_bin = mapped input position for band b, pitch factor F
  E = interpolated input energy at out_bin / F   (× sqrt(T) if slowing)
  v = Σ over strides s in {1,2,4,8,16}:  newOut[b-s] · (bin(p)·conj(bin(p − T·s)))
  h = prevOut[b] · scaleAngle(bin(p)·conj(prevIn(p)), T·F)
  c = newOut_ch0[b] · (bin_chN(p)·conj(bin_ch0(p)))      # channels > 0
  phase = timeW·0.2·v + pitchW·h + chanW·c + maxW·strongest(v,h,c)
  newOut[b] = phase normalised to energy E  (random phase if phase == 0)
  ```
- Contrasts: PV has only horizontal; vase-phocoder only vertical single-chain; spectral-cut preserves *intra*-segment relative phase exactly and fixes only one rotation per segment; PaulStretch enforces none.

### (5) Polyphony: global vs independent alignment, multi-F0, rephasing failure mode

- No global alignment decision exists in the hybrid: there is no single offset, no skip/duplication point, no master phase track. Each output bin receives its own blended phase; coupling between bins happens only through the upward vertical chain and the shared energy map.
- No multi-F0 representation either: no tracks, no peak list, no partial grouping. Polyphony "just" falls out of per-bin locality — each harmonic region independently follows its own strongest evidence, and cross-frequency timing relations are carried by vertical chaining rather than by a harmonic model.
- Can a WSOLA-style global-skip rephasing failure exist in the same form? **No.** The lab's W20 wobble comes from one shared skip point periodically re-anchoring all partials at once; this design has no such event, so there is nothing to precess fourths/thirds/dense chords while sparing fifths. Its characteristic failure textures are different: (a) vertical-chain error propagation *upward* in frequency from a bad low bin; (b) uniform detune/diffusion if horizontal scaling is wrong (all bands share the same `T·F` angle law, so the error is coherent, not interval-selective); (c) generic phasiness when evidence is weak (falls back toward PaulStretch-like randomness via `generateComplex`).
- Caveat: the precomputed `prevOutputRotations` (shift by exactly one output interval) assumes rigid output spacing; WSOLA-style schedule wobble is not fed back into spectral variants, so a late/early block would slightly mistime the horizontal reference — a second-order, non-precessing error.
- Contrasts: `wsola` DOES make one global `bestOffset` per block (all channels, whole spectrum) — the closest analogue in-repo to a global-skip mechanism; its wobble would read as tremolo/pitch wobble, damped by the `bestOffset/2` schedule feedback. `spectral-cut` sits between: independent regions (segments) with exact intra-segment phase preservation, so inter-partial relations survive *within* a segment and are unconstrained *across* segments (boundary/edge artefacts instead of wobble).

### (6) Transients: detection, resets, alternate paths, smear tradeoffs

- **No transient detector, no reset, no alternate path, no transient floor in this example code.** All transient handling is implicit:
  - Hybrid: transient energy automatically up-weights the vertical (timing) predictions because each prediction's magnitude scales with the input bins it was measured from — the design article calls this the "natural weighted average where strong tones or transients count more."
  - Phase-vocoder (default non-pure mode): splits each band's energy into a phase-continued part (`min(|prevOut|², E)`) and a raw-input-phase part (`E − that`), so attacks keep some true timing at the cost of phasiness; `--pure` forces full continuation (maximum smear of attacks, cleanest tones).
  - Vase-phocoder: no cross-block state at all, so it cannot smear across blocks — but also cannot preserve pitch across them; time-stretch emerges purely from vertical stride scaling.
- Time-aliasing (one transient appearing in several overlapping blocks, then stretched into distinct echoes) is NOT treated here: vertical strides scale fully with `timeFactor` with no cap. The article documents this exact failure (8x drum-loop echoes) and says the *successor* caps vertical scaling at 2x plus slight randomisation as a stopgap "hack" — that hack is absent from this repo.
- Net tradeoff: attacks survive in proportion to how much vertical evidence outweighs horizontal (default 2:1 toward timing), with no explicit switching anywhere.

### (7) Low frequencies: resolution, window sizing, special paths, low-B/E consequences

- What sets LF resolution: the analysis window length (`blockMs`, default 120 ms for hybrid/PV/Paul, 80 ms otherwise) plus the Kaiser window's mainlobe (`Kaiser::withBandwidth(block/interval)`); zero-padding (default 2x) interpolates between bins but adds no true resolving power.
- Why low notes need large windows here: nothing else resolves them. There is no multiresolution bank, no LF-special path, no subband-specific block size, no periodicity estimator — one fixed block for all frequencies. Bin spacing at 48 kHz with a 120 ms block and 2x padding is ~4.2 Hz with true resolution roughly 8–16 Hz; low-B (~62 Hz) and low-E (~82 Hz) fundamentals and their first few harmonics are comfortably separated, with ~7–10 pitch periods inside one block. Dropping to the 80 ms default widens the mainlobe and lets adjacent low harmonics leak into each other, which then pollutes both the energy map and the vertical chain in that region.
- Upward-chaining consequence: low bins anchor everything above them (strides reach back down to `b−16`), so poor LF separation propagates upward as smeared harmonic timing; conversely the multi-stride set gives low regions several long-baseline timing observations, which is part of why bass survives time-stretch here.
- Successor-only (not in repo): peak-locked 1:1 frequency map around strong harmonics and a "tonality limit" corner that stops shifting energy above ~4–8 kHz.

### (8) Resampling: location, interpolator, share, interaction

- Hybrid/vase/spectral-cut: **no resampler, zero share of the shift from resampling.** Pitch comes purely from the spectral frequency map, time purely from block spacing + phase laws.
- OLA/WSOLA/PV/Paul: 100% of the pitch shift comes from retagging the output WAV's sample-rate field (`outputWav.sampleRate *= freqFactor`) after stretching by `time*freq`. There is no interpolator, no anti-alias filter, no fractional-delay line in the repo — a realtime product would have to add one. Interaction with time scaling is therefore exact-by-construction offline (stretch ratio and retag ratio are multiplied from the same two CLI numbers) and undefined for realtime use.
- `setRate`/`setTimeFactor`/`invTimeFactor` only steer the overlap-add input-pointer rate; they never touch sample values.

### (9) Latency: terms, minimum, default, live

- Formula (all variants): total = `inputLatency() + outputLatency()` = `block/2 + (block − block/2)` = exactly one `blockSamples`. Terms: window/overlap-add buffering only. There is no separate lookahead, no resampler delay, no API-reported extra (`inputLatency`/`outputLatency` are the whole story; the CLI adds only the `inLat*stretch` scaling for trimmed file edges).
- Default (hybrid): 120 ms block → 120 ms total (60 + 60). Other defaults: 80 ms for OLA/WSOLA/spectral-cut/vase; overlap 4x (interval = block/4), zero-padding 2x — neither affects latency, only CPU density and interpolation smoothness.
- Theoretical minimum: set by whatever block still resolves the lowest content of interest; the code imposes no floor besides the WSOLA search clamp (`search ≤ min((block−interval)/2, interval)`). Halving the block halves latency and roughly halves frequency resolution; nothing else in the algorithm breaks.
- Live use: this CLI is file-based (256-sample streaming chunks through `process()`), so no live figure is measurable from the repo; the architecture is callback-compatible in shape (fixed `process(input,inputN,output,outputN)` with `samplesForOutput` flow control), but the 120 ms default block is far above live-detune territory (~30 ms class) and `double` precision + per-sample loop overhead would need slimming.

### (10) CPU/realtime: allocations, FFT/correlation cost, SIMD, callback-suitability

- Allocations: all vectors/FFTs sized in `configure` (spectra × channels, rotation tables, ring buffers); the per-block path is allocation-free (plain loops over preallocated vectors). No lock, no I/O, no RNG beyond `rand()` in the random-phase fallback paths.
- FFT cost: one forward + one inverse Modified Real FFT per channel per block, size ≈ `block × zeroPadding` rounded up by `setFastSizeAbove`; plus per-band complex multiplies (rotations, predictions, renormalisation) linear in band count. Overlap 4x ⇒ FFT pair every `block/4` samples per channel.
- Correlation cost: only WSOLA, brute-force ±`searchSamples` offsets × block × channels with a normalised-difference score — quadratic-ish in the worst case but bounded by the search clamp above.
- SIMD: none explicit; scalar `double` throughout (`using Sample = double`), `std::complex` arithmetic, `std::arg/cos/sin/sqrt` per band in the angle-scaling paths (the article notes the successor avoids trig by stride-scaling instead — this example still uses trig in `scaleAngle` and the PV rotation update).
- Callback-suitability: structurally yes (bounded, preallocated, stateful object) but tuned for clarity, not realtime: `double` precision, per-output-sample outer loop with a virtual `processBlock` dispatch, and FFT sizes ~11.5k at 48 kHz/120 ms/2x-pad per channel per 30 ms of audio. No denormal guards, no thread-safety provisions.

### (11) Ratio changes: fixed vs dynamic, smoothing, divebomb support, state/latency during change

- Factors are plain per-block state: `setTimeFactor` (base, all variants) and `setFreqFactor` (spectral-cut/vase/hybrid only) are trivial setters; `timeFactor` is re-derived every block from the *actual* `inputIntervalSamples`, and `freqFactor` is re-read every band (`bandToFreq(b)/freqFactor`), so both can change at block granularity with no reconfiguration and no latency change.
- Smoothing: none explicit anywhere — no ramping, no glide filter, no crossfade. The only softening is incidental: the fractional `surplusInputSamples` accumulator absorbs time-rate steps into pointer drift, and the phase predictors low-pass the *phase* (fresh predictions blend against stored `prevInput/prevOutput` spectra and persistent `horizontalRotations`).
- Divebomb-style modulation: structurally admittable (a caller could sweep `freqFactor` per block) but audibly unprotected — fast sweeps would slew the mapped-bin positions faster than the stored spectra stay valid, producing zipper-ish/phase-tearing artefacts; nothing in the code interpolates or resets predictor state on large jumps.
- State during change: predictor history (`prevInput/prevOutput/newOutput` spectra, `horizontalRotations`) is never invalidated on factor change, so continuity is preserved across small changes and stale across violent ones. `reset()` exists on most classes but is never called from the CLI.

### (12) Deliberate tradeoffs visible in code

1. **Clarity over performance** (stated in README: "aims for simplicity rather than performance"): `double`, one giant header, scalar loops, trig per band, brute-force WSOLA search.
2. **Timing evidence outweighs pitch evidence 2:1 by default** (`pitchWeight=1, timeWeight=2` with five 0.2-weighted strides; plus `maxWeight=1` bonus to the single strongest prediction and `channelWeight=1`): transients/timing win ties; all four weights are CLI-tunable, inviting experimentation.
3. **Multi-stride vertical set {1,2,4,8,16}** trades a 5x vertical cost for long-baseline timing coherence without lengthening the window (the key quality/latency move vs a plain vocoder); `--single-vertical` collapses it to stride-1 for comparison.
4. **Energy normalisation choices**: `energy /= freqFactor` (constant total energy under pitch shift) and `× sqrt(timeFactor)` when slowing (compensating sparser overlap density); PaulStretch's `sqrt(fftSize/interval)` gain does the analogous job for random phases.
5. **Window split**: sine-double-window for time-domain classes (cheap, good crossfade) vs Kaiser-with-bandwidth for spectral classes (controlled sidelobes for the STFT), both renormalised to perfect reconstruction.
6. **Zero-padding 2x default**: smoother interpolated reads (`getEnergy`/`getBin`) and finer-mapped shifts at ~2x FFT cost; adjustable 1x+.
7. **Random-phase fallback** in `generateComplex` when blended evidence is exactly zero: silence-regions and nulls degrade to PaulStretch-like diffusion rather than freezing or NaN-ing.
8. **No peak tracking / no transient switching anywhere**: accepted phasiness and time-aliasing in exchange for ~900 lines total, no thresholds to tune, and no mode-switch clicks.
9. **Centred-time convention** (rotate by `-block/2` in, conjugate out): costs two full-spectrum complex multiplies per block per channel but makes vertical deltas symmetric and interpolation well-behaved.
10. **Observed code quirks (read as written, likely unintentional)**: (a) in the channel-prediction max-track, the stored best is the *horizontal* prediction rather than the channel one; (b) the vase-phocoder CLI flag `stretch-phase` is passed positionally into the `stretchStride` constructor parameter, so the flag's effect reads inverted relative to its help text (default build uses trig scaling, flag selects stride scaling); (c) `PhaseVocoderStretch::outputRotations` is sized per-channel but indexed per-band, so channels share slots (benign since each entry is consumed immediately within its channel loop).

## 3. Special attention answers

- **Horizontal vs vertical prediction and weighting.** Horizontal = classic vocoder advance across blocks at the mapped bin, angle × `T·F`, weight 1. Vertical = within-block upward chain across strides `T·{1,2,4,8,16}`, total weight 2 (five taps × 0.2 × 2). Magnitudes multiply through conjugates products so loud evidence self-weights; the single strongest of all predictions earns +1 more. Net: timing dominates ties, pitch dominates steady tones (where vertical deltas are near-zero and horizontal is coherent), stereo + max terms break deadlocks.
- **Spectral peak handling / nonlinear frequency map.** None in this repo. The only peak-adjacent machinery is `spectral-cut`'s energy-vs-smoothed-floor segmentation (a different, earlier variant). The nonlinear 1:1-around-harmonics map and the tonality-limit corner are successor-only per the article.
- **Transient/time-aliasing treatment.** No detection or switching; transients survive only via energy-weighted vertical evidence. Time-aliasing (echoed attacks at large stretches) is untreated here — full `T`-scaled strides, no cap; the article's 2x-cap-plus-dither hack lives in the successor.
- **What alters the quality/latency tradeoff vs a conventional phase vocoder, and why.** Three things, none requiring a longer window: (i) vertical multi-stride predictions recover *timing* (attack placement, amplitude modulation) that a pure vocoder diffuses — quality gain at identical STFT latency; (ii) centred-time rotation makes those vertical measurements symmetric/cheap to interpolate; (iii) magnitude-weighted complex averaging lets each bin follow whichever evidence is locally strongest instead of forcing one global phase law. Polyphonic phase relations survive better because nothing collapses bins onto shared tracks: no peak-picking, no phase-locking regions, no global offset — each bin's cross-frequency ties are preserved through the upward chain while its frequency is preserved through its own horizontal term.
- **Why fifths-vs-fourths-style selective precession cannot occur here:** there is no shared periodic re-anchoring event and no interval-dependent update rate; residual errors are per-bin detune/diffusion (coherent across the spectrum via the common `T·F` law) or upward-propagating chain smear — neither is interval-selective.

## 4. Contrast-spotting vs lab approaches (A–E) — brief, per instructions

- (A) variable-delay/Doppler: no counterpart here; closest is WSOLA's per-block offset, which is bounded, searched, and schedule-compensated rather than free-running — no fragmentary/unstable analogue.
- (B) conventional PV: `PhaseVocoderStretch` IS this baseline (plus the energy-blend refinement); hybrid's horizontal term is literally this per band.
- (C) multiresolution PV + crossover: no counterpart — one fixed resolution for all bands; the hybrid's multi-stride vertical set is the mechanism that most reduces the *need* for multiresolution (timing without short windows), but LF still pays full window cost.
- (D) WSOLA W20: `WsolaStretch` is the direct cousin (symmetric searched offset, joint-channel score — here normalised-difference rather than NCC, no explicit transient guard or tie-break rule, schedule feedback `−bestOffset/2`). The hybrid has no global skip and therefore no skip-back rephasing wobble mode; its failure textures (upward chain smear, coherent detune, diffusion) are disjoint from W20's interval-selective precession.
- (E) asymmetric-search/trajectory research: no counterpart; search here is symmetric and memoryless per block (span rule / transient floor have no analogues).

## 5. Inspected evidence inventory

- Files opened with file-read tools and fully read: `shift-stretch.h` (lines 1–894, all), `main.cpp` (lines 1–209, all), `README.md` + `Makefile` (via shell cat), `util/wav.h` (skimmed header/ChannelReader/sampleRate handling only — WAV container, no DSP claims drawn), design article page body (fetched, ~15k chars, read in full from saved fetch).
- Functions/methods inspected (every material claim above traces to one): `OverlapAddStretch::configure`, `::reset`, `::setRate`, `::setTimeFactor`, `::samplesForOutput`, `::process`, `::inputLatency`, `::outputLatency`, `::channelBlock`, `::processBlock`, `::scheduleNextBlock`; `WsolaStretch::configure`, `::reset`, `::processBlock`, `::previousBlock`; `SpectralStretch::configure`, `::processSpectrum`, `::channelSpectrum`, `::bands`, `::fftSize`, `::bandToFreq`, `::freqToBand`, `::timeShiftPhases`, `::processBlock`, `::generateComplex`; `SpectralCutStretch::configure`, `::reset`, `::setFreqFactor`, `::processSpectrum`, `::newChannelSpectrum`, `::prevChannelSpectrum`, `::copySegmentToNew`; `PhaseVocoderStretch::configure`, `::reset`, `::processSpectrum`, `::prevInputSpectrum`, `::prevOutputSpectrum`; `PaulStretch::processSpectrum`; `VasePhocoderStretch::configure`, `::setFreqFactor`, `::processSpectrum`, `::getEnergy`, `::getBin`; `HybridPhaseStretch::configure`, `::reset`, `::setFreqFactor`, `::processSpectrum`, `::prevInputSpectrum`, `::prevOutputSpectrum`, `::newOutputSpectrum`, `::channelHorizontalRotations`, `::getEnergy`, `::getBin`, `::scaleAngle`; `WavCmd::processBlocks`, `WavCmd::WavCmd`, `main`.
- Git metadata commands: `rev-parse HEAD`, `log --format`, `branch -a`, `status`, `show --stat HEAD`; license search: recursive grep for license/copyright/MIT/GPL/Apache/BSD over all tracked non-git files (negative result).

## 6. Unresolved / not verifiable from available evidence

- `dsp/delay.h`, `dsp/windows.h` (`Kaiser::withBandwidth`, `forcePerfectReconstruction`), `dsp/fft.h` (`ModifiedRealFFT`, `setFastSizeAbove`): not in repo, not fetched — ring-buffer exact semantics, Kaiser parameterisation, FFT normalisation/fast-size rounding, and any extra latency inside them are unverified (only call-shape visible at include/call sites).
- Talk video content (`https://youtu.be/fJUmmcGKZMI`): identified but not watched; no claims depend on it.
- No build or listening test was run (headers + offline CLI only; build would fail on missing `dsp/` includes; reading answers every assigned facet).
- License of THIS repo remains unknown (no statement found); successor-MIT reports are secondary and cited as such, not as repo fact.

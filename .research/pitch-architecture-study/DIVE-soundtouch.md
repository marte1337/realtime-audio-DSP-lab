# SoundTouch 2.4.1 — Architecture Dive (pitch-shift path)

## Provenance
- Official repo: `https://codeberg.org/soundtouch/soundtouch` (author Olli Parviainen; homepage `https://www.surina.net/soundtouch`).
- Studied artifact: official release tarball `https://www.surina.net/soundtouch/soundtouch-2.4.1.tar.gz`, extracted to `/tmp/tdm-pitch-study/soundtouch`.
- Version proof: `SOUNDTOUCH_VERSION "2.4.1"` in `include/SoundTouch.h`; `readme.md` states "latest stable release in Git is 2.4.1"; `README.html` changelog head entry is 2.4.1.
- Git SHA: NOT obtained (codeberg git blocked by proxy CONNECT timeout; github https auth-blocked in sandbox). See Unresolved.
- License: GNU LGPL v2.1 or later. File: `COPYING.TXT` (header: "GNU LESSER GENERAL PUBLIC LICENSE, Version 2.1, February 1999"). Every source file carries the LGPL-2.1-or-later header.

## Files actually inspected (bodies opened with file reads)
- `include/SoundTouch.h` (API, settings IDs, version)
- `source/SoundTouch/SoundTouch.cpp` (orchestration: full body)
- `source/SoundTouch/TDStretch.h`, `TDStretch.cpp` (full body incl. float + integer paths)
- `source/SoundTouch/RateTransposer.h`, `RateTransposer.cpp` (full body)
- `include/FIFOSamplePipe.h`, `include/FIFOSampleBuffer.h`, `source/SoundTouch/FIFOSampleBuffer.cpp` (full)
- `source/SoundTouch/AAFilter.h`, `AAFilter.cpp` (full), `FIRFilter.h`, `FIRFilter.cpp` (full)
- `source/SoundTouch/InterpolateCubic.h/.cpp` (full), `InterpolateLinear.h`, `InterpolateShannon.h`
- `source/SoundTouch/sse_optimized.cpp` (full), `source/SoundTouch/mmx_optimized.cpp` (corr/overlap bodies), `cpu_detect.h`, `include/STTypes.h`
- Not inspected (out of scope): `BPMDetect.cpp`, `PeakFinder.*`, `SoundStretch/*` example app, C# example.

## 0. One-paragraph architecture
Pitch shifting is the classic resample-plus-correct decomposition, entirely time-domain: a `RateTransposer` (cubic-interpolation resampler + Hamming-windowed FIR anti-alias filter) does ALL of the pitch change, and a `TDStretch` WSOLA/OLA time-stretcher applies the reciprocal tempo correction so duration is preserved. There is no STFT, no phase vocoder, no F0 estimation, no transient detector, no subbands. Alignment uses a single global per-frame offset chosen by energy-normalized cross-correlation over an 8 ms overlap window, with a parabolic center bias. Stages run as chained FIFO sample pipes with threshold-triggered batch processing.

---

## (1) Input / buffering
- Block model: threshold-triggered, variable-size. `TDStretch::putSamples` appends to `inputBuffer` then runs `processSamples`, which loops `while (inputBuffer.numSamples() >= sampleReq)` emitting fixed nominal batches. `RateTransposer::processSamples` consumes its whole input FIFO every call (`transpose` drains `src`, FIR `evaluate` drains into `dest`).
- FIFOs: `FIFOSampleBuffer` = flat array + `bufferPos` read cursor + `samplesInBuffer` count; consumed samples advance the cursor, `rewind()` memmoves on next write, `ensureCapacity` grows in 4 KiB steps with 16-byte-aligned storage. No rings, no lock-free structures.
- Chaining: `FIFOProcessor`/`FIFOSamplePipe::moveSamples` COPIES between stages (memcpy), not zero-copy.
- Initial latency: TDStretch emits nothing until `sampleReq` samples accumulate (`sampleReq = max(intskip+overlap, seqLen) + seekLen`, `TDStretch::setTempo`); RateTransposer pre-fills `getLatency()` silents in `clear()`.
- Scheduling: fully synchronous inside `putSamples`; one call can process an unbounded number of frames if fed a huge buffer. No internal threads except `#pragma omp parallel for` data-parallel loops (seek scan, FIR taps).
- Fixed vs variable: input accepts arbitrary chunk sizes; internal TDStretch batch geometry is fixed per parameter set (output batch = `seekWindowLength - overlapLength`, `getOutputBatchSize`).

## (2) Pitch-shift decomposition (exact order)
`SoundTouch::calcEffectiveRateAndTempo` maps user controls to two stage parameters: `tempo = virtualTempo/virtualPitch`, `rate = virtualPitch*virtualRate`. Pitch-only shift of factor p therefore programs rate=p plus tempo=1/p; 100% of the pitch change comes from resampling, 100% of the duration correction from stretching.
Stage order depends on rate (`SoundTouch::putSamples`):
- rate <= 1 (pitch down / slow): input -> RateTransposer (downsample) -> TDStretch (tempo>1, speed back up) -> out.
- rate > 1 (pitch up / fast): input -> TDStretch (tempo<1, slow down) -> RateTransposer (upsample) -> out.
Rationale visible in code: resample in the direction that shrinks data first (downsample-before-stretch when slowing). Order flips live at the rate=1.0 crossover by moving buffered samples between stage FIFOs; an opt-in `SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER` build flag forces always-stretch-first to avoid the crossover click at a quality cost (`STTypes.h`, `SoundTouch::calcEffectiveRateAndTempo`).

## (3) Analysis representation
Pure time-domain. The only "analysis" is broadband cross-correlation between the stored tail of the previous output frame (`pMidBuffer`, length = overlap) and candidate positions in the input buffer. No spectra, peaks, periodicity/F0 estimate, voicing classification, or multiresolution. The `+0.1` correlation floor and center-weighting in the seekers are fixed heuristics, not signal-adaptive analysis.

## (4) Continuity
- Waveform: linear crossfade over `overlapLength` (`overlapMono/Stereo/Multi`: ramp `m1/m2`), overlap-add of the chosen offset against `pMidBuffer`.
- Phase-over-time: preserved only implicitly by picking the max-correlation offset per frame; no phase unwrapping, no phase propagation, no OLA phase locking. Jump discontinuities between frames are absorbed by the 8 ms crossfade.
- Cross-frequency phase: nothing preserves per-partial phase; a single global time offset shifts all partials by the same sample count, i.e. by different phase angles per frequency.
- Inter-channel: correlation sums over ALL channels jointly in one interleaved loop and one shared `bestOffs` is applied to every channel (`overlapStereo/Multi`, `calcCrossCorr`). Channels can never split, so stereo image is stable but alignment is a single compromise across channels.

## (5) Polyphony
- Single global alignment: `seekBestOverlapPosition{Full,Quick}` return one integer offset per frame for the whole mix. No per-band/per-partial/per-channel offsets, no multi-F0 representation.
- Inter-partial rephasing: tolerated, not avoided. The score is dominated by whatever partials carry the most energy in the 8 ms correlation window; weaker partials ride along and get rephased by the same skip. The header documents the audible consequence as a "drifting" artifact and advises shrinking the seek window (`TDStretch.h` DEFAULT_SEEKWINDOW_MS comment).
- Global-skip failure mode verdict: YES, SoundTouch has the same failure class as lab WSOLA W20: one global time-domain skip rephases every chord partial at once. Three mitigations change severity, not kind: (a) much longer frames (default 73 ms sequence + 18 ms seek at tempo 1, vs W20's 20 ms) make skips less frequent; (b) the parabolic center bias +0.75 penalty on offset 0 prefer small, centered skips; (c) the 8 ms linear crossfade smooths each junction. For pitch-down specifically, TDStretch runs on the already-downsampled signal, so fixed-ms windows cover more periods of each partial than in a fixed-window native-rate WSOLA — again a severity shift, same physics.

## (6) Transients
None of: no detector, no reset/discontinuity logic, no transient bypass path, no adaptive window. `processSamples` treats every frame identically. Smear is bounded structurally: any transient is crossfaded over at most `overlapLength` (8 ms default) and alignment may snap the transient's position by up to `seekLength` (18 ms) if correlation prefers it — the documented "drifting"/smearing tradeoff, controlled only by the ms parameters.

## (7) Low frequencies
- What sets LF resolution: the search span `seekLength` (18 ms default @tempo 1) and the correlation aperture `overlapLength` (8 ms). No FFT, so no bin resolution; alignment quality for a partial depends on how many of its periods fit in those windows.
- Low notes vs large windows: a low-E 82 Hz period (12.2 ms) exceeds the 8 ms correlation aperture, so the score sees under one period; the 18 ms seek span offers ~1.5 candidate periods. Longer sequence/seek settings directly improve LF at the cost of pre-echo-ish drift and CPU — this is why the auto tables raise sequence to 90 ms / seek to 20 ms at tempo 0.5 (`calcSeqParameters`).
- Special LF paths: none. Consequences for low B/E: weakest partials with periods near/above the overlap length contribute least reliably to the score; alignment follows higher-energy upper partials instead.

## (8) Resampling
- Location: `RateTransposer`, either before or after `TDStretch` per rate (see facet 2). Within the transposer, filter/transpose order also flips with rate: rate<1 transposes first then AA-filters; rate>1 filters first then transposes (`RateTransposer::processSamples`).
- Interpolator: default CUBIC 4-point (`TransposerBase::algorithm = CUBIC`, `InterpolateCubic::transpose{Mono,Stereo,Multi}` with fractional-phase `fract += rate` stepping); LINEAR float/integer and experimental 8-tap Kaiser-windowed SHANNON variants selectable via `TransposerBase::setAlgorithm`; integer-sample builds force linear.
- Filter: `AAFilter` = Hamming-windowed sinc FIR, default 64 taps (`new AAFilter(64)`; cutoff `0.5/rate` for rate>1 else `0.5*rate`, redesigned on every `setRate`), evaluated through `FIRFilter` (length must be divisible by 8; output = input - filter length).
- Share of pitch shift: all of it. Time scaling contributes none.
- Interaction: resampling ratio and stretch ratio are exact reciprocals for pitch-only operation; `getInputOutputSampleRatio` returns `1/(tempo*rate)`.

## (9) Latency (samples; 44.1 kHz, defaults, tempo=1, rate=1)
Auto params at tempo 1: sequence 73 ms -> `seekWindowLength`=3219; seek 18 ms -> `seekLength`=793; overlap 8 ms -> `overlapLength`=352 (float build; integer build rounds to power-of-2 256).
- TDStretch `sampleReq` = max(2867+352, 3219)+793 = 4012 (~91 ms). Output batch = 3219-352 = 2867 (~65 ms); steady-state input per batch = `nominalSkip` = 2867.
- RateTransposer = interpolator (cubic 1) + AA 64/2 = 33.
- API: `SETTING_INITIAL_LATENCY` = (4012+33)*1 = 4045 (~92 ms); `SETTING_NOMINAL_INPUT_SEQUENCE`=2867, `SETTING_NOMINAL_OUTPUT_SEQUENCE`=2867; average in-stream latency per header docs = initial - output/2 (~59 ms here).
- Theoretical minimum: dominated by `sampleReq`; shrinkable via SETTING_SEQUENCE/SEEK/OVERLAP_MS (e.g. speech preset 40/15/8) at quality cost. No separate lookahead term beyond `seekLength` inside `sampleReq`; no resampler delay beyond 33 samples. "Live" = same numbers; no special live mode.
- Pitch -1 semitone example (rate 0.9439, tempo 1.0595): seq 71 ms, sampleReq 4090, total initial (4090+33)*0.9439 ~= 3892 (~88 ms).

## (10) CPU / realtime
- Correlation dominates: `seekLength` x `overlapLength` x channels MACs per output batch (~793x352x2/2867 ~= ~195 MACs per output sample per channel-equivalent at defaults). FIR AA adds 64 taps x channels per transposed sample when rate != 1 (64-tap default; length settable 8..128 taps).
- SIMD: runtime CPU dispatch via `detectCPUextensions` + `newInstance` factories: SSE `calcCrossCorr` for float (`TDStretchSSE`), MMX corr/overlap/FIR for 16-bit integer (`TDStretchMMX`, `FIRFilterMMX`); SSE FIR stereo path; overlap-add itself is scalar C except MMX stereo. 16-byte-aligned buffers throughout; optional `ST_SIMD_AVOID_UNALIGNED` trades exactness for speed.
- OpenMP `#pragma omp parallel for` on the full-seek scan and FIR loops (needs OpenMP build; the accumulate-norm fast path is disabled under OpenMP/SIMD because offsets evaluate out of order).
- Allocations: steady-state is allocation-free IF input fits grown FIFOs, but `ensureCapacity` (4 KiB steps), `acceptNewOverlapLength`, `FIRFilter::setCoefficients`, and `AAFilter::calculateCoeffs` (fresh `new` on EVERY `setRate`) all allocate — continuous divebomb modulation churns the AA design per call.
- Callback-suitability: mediocre. `putSamples` is synchronous with unbounded per-call work (while-loop over all available frames); feed small blocks. No semaphores (header warns); not lock-free; throws `std::runtime_error` on misuse (or assert with `ST_NO_EXCEPTION_HANDLING`).

## (11) Ratio changes
- Fixed vs dynamic: fully dynamic; `setTempo/setRate/setPitch*` take effect on the next `putSamples`. No smoothing, ramping, or crossfade between settings.
- State during change: `setTempo` recomputes auto seq/seek params, `nominalSkip`, `sampleReq` immediately; the fractional-skip accumulator `skipFract` and `pMidBuffer` persist (skip-phase continuity, possible one-frame geometry mismatch). `setRate` additionally redesigns the AA filter live (allocates). `enableAAFilter` calls `clear()` (buffers dropped); tempo/pitch changes do not clear.
- Stage reorder: crossing rate=1.0 moves buffered audio between stage FIFOs mid-stream; upstream comment + `SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER` flag acknowledge the click risk.
- Divebomb support: works mechanically (continuous `setPitchSemiTones(double)`), with per-call AA redesign cost and no anti-zippering — expect zipper noise on fast sweeps.

## (12) Deliberate tradeoffs visible in code
1. QuickSeek (`seekBestOverlapPositionQuick`, off by default): coarse step-16 scan keeping top-2 + local refine; documented as ~99% match quality at a fraction of the CPU.
2. Center bias + `(corr+0.1)` floor + 0.75 penalty on offset 0: deliberately prefer small centered skips over the raw argmax to fight drift.
3. Auto sequence/seek tables (`calcSeqParameters`): long frames when slowing (90/20 ms @0.5x) vs short when speeding (40/15 ms @2x).
4. `sampleReq` margin: commented-out `seekLength/2` replaced by full `+seekLength` — extra ~18 ms latency bought for robustness.
5. Removed nominal-tempo bypass (`processNominalTempo` commented out): always process, avoiding clicks when tempo crosses 1.0.
6. Integer build: overlap rounded to power-of-2 with shift-divide + adaptive `overlapDividerBitsNorm` (`adaptNormalizer`) — speed over precision.
7. AA filter 64 taps default + always-on (unless crossover build): quality over CPU; length settable.
8. Cubic default interpolator: quality over linear's CPU; Shannon present but header-marked "mostly experimental".
9. OpenMP/SIMD: throughput over determinism (parallel argmax races resolved by critical section; unaligned skipping optional).
10. Global single-offset model: simplicity, stereo coherence, and low CPU over per-partial correctness — the direct cause of the shared rephasing failure mode.

## Special questions
- Processing order for pitch shift: resample (full pitch change) + reciprocal time-stretch (duration correction); resample-first when rate<=1, stretch-first when rate>1.
- sequence vs seek vs overlap: sequence (`seekWindowLength`, 73 ms) = frame body copied per batch; seek (`seekLength`, 18 ms) = candidate-offset search span [0, seek); overlap (`overlapLength`, 8 ms) = correlation aperture AND crossfade length. Output batch = sequence - overlap; input consumed per batch ~= tempo*(sequence-overlap).
- Overlap position choice/score: exhaustive argmax over `seekLength` offsets of `dot(candidate, midBuf)/sqrt(sum(candidate^2))` (candidate-side energy normalization only — NOT full NCC, `calcCrossCorr`), rescored as `(corr+0.1)*(1-0.25*tmp^2)` center weighting with offset 0 pre-penalized x0.75 (`seekBestOverlapPositionFull`); Quick variant does coarse/fine search.
- Latency accounting: see facet 9; TDStretch `sampleReq` dominates (~91 ms of ~92 ms at 44.1 kHz defaults).
- Key verdict: SAME global-skip rephasing failure class as WSOLA W20 (single broadband offset per frame, joint all-channel score, no per-partial handling), with severity reduced by longer frames, center bias, and 8 ms crossfades — plus fixed-ms windows covering more periods on the downsampled pitch-down path.

## Pseudocode (reconstructed in own words)
TDStretch frame step:
```
need = max(round(tempo*(SEQ-OVL)) + OVL, SEQ) + SEEK
while input.used >= need:
    if first frame: skip overlap; adjust fractional skip; mark started
    else:
        best = argmax over o in [0,SEEK) of
               (dot(input[o:o+OVL], mid)/sqrt(dot(input[o:o+OVL],input[o:o+OVL])) + 0.1)
               * (1 - 0.25*((2o-SEEK)/SEEK)^2)     # center bias; o=0 pre-scaled 0.75
        xfade(mid, input[best:best+OVL]) -> output   # linear ramp, all channels same o
        best += OVL
    copy input[best : best+SEQ-2*OVL] -> output
    mid = input[best+SEQ-2*OVL : +OVL]               # store tail for next frame
    frac += tempo*(SEQ-OVL); step = floor(frac); frac -= step; input.drop(step)
```
Pitch p: rate=p, tempo=1/p; if p<=1: resample(p) then stretch(1/p) else stretch(1/p) then resample(p).

## Unresolved
- Git branch/tag/SHA: unobtainable in sandbox (codeberg git CONNECT timeout; github https auth-blocked). Identity pinned by tarball URL + SOUNDTOUCH_VERSION 2.4.1 + changelog head instead.
- `SoundStretch` example app and `BPMDetect`/`PeakFinder` bodies not read (outside assigned focus; no claims depend on them).

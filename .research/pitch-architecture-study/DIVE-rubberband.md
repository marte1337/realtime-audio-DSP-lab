# RubberBand — DSP Architecture Dive (realtime/live focus)

## 0. Repository identity

- URL: https://github.com/breakfastquay/rubberband
- Checkout: shallow clone, `HEAD -> default` (grafted single commit)
- SHA: `e4296ac80b1170018a110bc326fd0d45a0eb27d6`
- Date/subject: 2025-02-27 11:04:40 +0000, "Additional IPP path"
- Library version string in headers: `4.0.0`, API v3.0
- License: GNU General Public License v2-or-later, plus optional commercial
  license. License file: `COPYING` (repo root). Per-file headers confirm
  GPL-2-or-later with commercial alternative; `README.md` restates it.

## 1. Engine map and public-API trace

There are two stretcher engines plus one dedicated live shifter, selected
at construction and immutable afterwards:

| API class | Engine | Selected by | Implemented in |
|---|---|---|---|
| `RubberBandStretcher` | R2 "Faster" (default) | absence of `OptionEngineFiner` | `src/faster/R2Stretcher.*`, `StretcherProcess.cpp` |
| `RubberBandStretcher` | R3 "Finer" | `OptionEngineFiner` | `src/finer/R3Stretcher.*` |
| `RubberBandLiveShifter` | R3-derived live shifter (always) | n/a (only engine) | `src/finer/R3LiveShifter.*` |

Trace details (all verified by reading the bodies):

- `RubberBandStretcher::Impl` (`src/RubberBandStretcher.cpp`) constructs
  either `R2Stretcher` or `R3Stretcher` from the options bit and forwards
  every call (`process`, `retrieve`, `available`, `getSamplesRequired`,
  ratio setters, latency queries). `getEngineVersion()` returns 2 or 3.
- `RubberBandLiveShifter::Impl` (`src/RubberBandLiveShifter.cpp`) always
  constructs `R3LiveShifter`. This is a genuinely separate, current class,
  not a wrapper around the stretcher: fixed-block `shift(in, out)` with
  `getBlockSize() == 512` always.
- Processing mode (offline two-pass `study`/`process` vs realtime single
  streaming pass) is orthogonal to engine choice and is fixed by
  `OptionProcessRealTime` at construction. This dive covers the realtime
  paths; offline-only machinery (key-frame maps, study pass, R2 worker
  threads) is noted only where it contrasts.

Shared infrastructure (`src/common`): `StretchCalculator` (hop servo),
`Resampler` facade over IPP / libsamplerate / Speex / builtin
`BQResampler` backends, `FFT` facade over IPP / vDSP / SLEEF / FFTW /
builtin / KissFFT, `RingBuffer`, `Window` (incl. asymmetric
Niemitalo pair), `SincWindow` (R2 only), `BinClassifier`-supporting
median/histogram filters, `process_t` sample type (`double` by default).

Numerology used below (44.1/48 kHz, derived from `roundUpDiv` = round up
to a power of two): R3 classification FFT 2048, multi-window set
{4096, 2048, 1024}; R3 standard hop limits outhop 128..512, inhop ≤ 2048
(≤ 1024 with readahead); R3 single-window/live hop limits outhop
256..640, inhop ≤ 1536 (≤ 512 with readahead); R2 base FFT 2048, default
increment 256.

## Facet 1 — Input/buffering (block model, FIFOs, latency sources)

R3Stretcher realtime (`R3Stretcher::process`, `R3Stretcher::consume`):

- Input arrives in caller-sized `process()` blocks and is appended to a
  per-channel `RingBuffer<float>` input FIFO sized at 16× the window
  source length. Output accumulates in a symmetric per-channel output
  FIFO drained by `retrieve()`. `available()` reports output FIFO depth
  (-1 when finished); `getSamplesRequired()` reports how much input is
  needed before one more frame can run (window-source length minus input
  FIFO depth, scaled by pitch when pre-resampling).
- The engine consumes the input FIFO in fixed analysis hops (`m_inhop`,
  recomputed only when a ratio changes) and emits variable synthesis
  hops (`outhop`, re-solved every frame by `StretchCalculator`). A frame
  runs only when the FIFO holds a full window-source span; each frame
  peeks that span without consuming it, then skips exactly `inhop`.
- No prefill or start-skip in realtime (offline prefills half a window
  and skips the corresponding start samples). Buffers auto-grow with a
  loud warning if the caller exceeds the declared max process size
  (`ensureInbuf`/`ensureOutbuf`); normal operation never reallocates.

R3LiveShifter (`R3LiveShifter::shift`, `readIn`, `generate`, `readOut`):

- Strictly fixed-block: every `shift()` call takes exactly 512 frames
  per channel and returns exactly 512. Internals still use input/output
  FIFOs (4× window source), but each call does: resample the 512 input
  frames into the input FIFO, run as many stretcher hops as needed to
  cover the block (input FIFO is never drained below one window-source
  span; remaining input/output demand is split evenly over an estimated
  hop count), then resample from the output FIFO back to exactly 512
  frames. First call pre-pads a full window source of silence and
  compensates resampler start-up shortfall explicitly.

R2 realtime (`R2Stretcher::process`, `processOneChunk`, `consumeChannel`):

- Same FIFO shape (per-channel in/out rings) but a single shared hop
  grid: each `process()` call ingests input (optionally through a
  pre-resampler), then `processOneChunk()` advances all channels in
  lockstep by one fixed input increment per chunk, solving one shared
  output increment from the summed-magnitude onset curve. Offline mode
  instead precomputes the whole increment sequence and lets each channel
  (optionally on its own thread) work through it independently.

## Facet 2 — Pitch-shift decomposition (exact stage order)

Both stretcher engines use the same decomposition, stated plainly in
`R2Stretcher::getEffectiveRatio`: the phase vocoder time-stretches by an
effective ratio of `timeRatio * pitchScale`, and a resampler then
corrects the duration by `1/pitchScale`, which is what actually moves the
pitch. All of the pitch shift comes from the resampling ratio; the
stretcher only sets up the duration the resampler needs.

Stage order depends on mode and options (verified in
`R3Stretcher::areWeResampling`, `R3Stretcher::process`/`consume`,
`R2Stretcher::resampleBeforeStretching`, `consumeChannel`/`writeChunk`):

- R3 offline: always stretch-then-resample (except pitch 1.0, where no
  resampler is even created). R3 realtime with `OptionPitchHighConsistency`:
  always stretch-then-resample, even at 1.0, so the ratio can sweep
  through unity without switching topology.
- R3 realtime `HighSpeed` (default): resample-before for upward shifts,
  resample-after for downward shifts (the cheaper direction each way).
  `HighQuality` inverts that (the better-sounding direction each way).
  R3 pitch option is construction-fixed.
- R2: offline always resample-after; realtime `HighConsistency`
  resample-after; `HighQuality` resamples before only for downward
  shifts; `HighSpeed` (default) resamples before only for upward shifts.
  R2 allows changing the pitch option live.
- R3LiveShifter always uses two resamplers: an input resampler running
  at `1/pitch` for upward shifts (1.0 otherwise) and an output resampler
  running at `1/pitch` for downward shifts (1.0 otherwise), with the
  phase-vocoder stretch sandwiched between (`getInRatio`/`getOutRatio`,
  `readIn`/`readOut`). So one of the two resamplers is always at unity.

## Facet 3 — Analysis representation

R3 (multi-resolution guided phase vocoder):

- Three simultaneous STFT scales (4096/2048/1024 at 48 kHz) with aligned
  centres cut from one shared unwindowed span (`analyseChannel`). Only
  the frequency sub-range each scale is responsible for gets polar
  conversion, to save CPU (`ToPolarSpec`). The 2048 scale doubles as the
  classification scale and carries a one-hop readahead when the hop is
  short enough (≤ 1024; ≤ 512 in single-window mode), otherwise the
  current frame doubles as its own lookahead.
- A `BinClassifier` labels every bin of the classification spectrum as
  harmonic, percussive, or residual with two running median filters: a
  horizontal (over time, per bin, length 9, lag 1) and a vertical (over
  frequency, length 10) filter, thresholded against each other at 2.0
  both ways. A `BinSegmenter` modal-filters those labels across
  frequency (length 18) and reduces them to three boundary frequencies:
  percussive-below, percussive-above, residual-above.
- A `Guide` turns the segmentation, magnitudes, and ratio into a
  per-frame, per-channel `Guidance` bundle: which FFT size renders which
  frequency band (crossovers adapt within 500–1100 Hz and 4–7 kHz by
  descending to local magnitude valleys), phase-lock band layout,
  kick/pre-kick/reset/unlock/channel-lock frequency ranges. Single-window
  mode (R3 `OptionWindowShort`, always in the live shifter) skips all
  adaptation: one 2048 FFT for the whole spectrum, three fixed
  phase-lock bands.
- Optional cepstral formant envelope (`analyseFormant`: log-magnitude
  inverse FFT, lifter cutoff at rate/650, forward FFT, exp, square,
  clamp) applied as a bounded per-bin magnitude correction
  (`adjustFormant`, clamp 60:1, only below 10 kHz).

R2 (single-resolution phase vocoder + onset curves):

- One STFT per channel per chunk (`analyseChunk`: optional sinc
  pre-filter, window-cut with fold or zero-pad to FFT size, forward
  polar transform). Magnitudes feed two onset detectors: a percussive
  detector counting bins with ≥3 dB frame-over-frame rises, and a
  high-frequency detector summing bin-weighted magnitudes; the compound
  detector median-filters the HF stream and its derivative and merges
  the two (`CompoundAudioCurve`). A silence detector flags all-quiet
  frames. No per-bin classification, no segmentation, no guide.

## Facet 4 — Continuity (waveform, phase-over-time, cross-frequency, inter-channel)

R3 `GuidedPhaseAdvance::advance` (shared by stretcher and live shifter):

- Phase-over-time: each bin gets an expected phase from the previous
  input phase plus nominal bin advance over the previous input hop; the
  measured deviation is scaled by the hop ratio and added to the
  previous output phase ("unlocked" advance). The previous hops (not the
  upcoming ones) drive this, since they describe the actually-advanced
  signal.
- Cross-frequency: within each phase-lock band the nearest spectral
  peak (local maximum over ±p bins, p = 1..5 rising with frequency) is
  found per bin for the current and previous frames; non-peak bins
  inherit the peak's measured advance plus a beta-weighted copy of
  their measured offset from the peak (beta rises with ratio and
  frequency). Bins in reset/kick ranges instead copy the input phase
  verbatim; bins in the high-unlock range use the unlocked advance.
- Inter-channel: below 600 Hz (full spectrum with `ChannelsTogether`)
  a channel adopts a louder channel's peak assignment when both agree
  on the previous peak; with stereo `ChannelsTogether` the signal is
  additionally processed mid/side end to end. Unity ratio and silence
  force progressive or full phase resets so the output converges to the
  input instead of drifting.
- Waveform continuity comes from overlap-add with hop-normalised window
  scaling (`outhop / windowScaleFactor`) into a per-scale accumulator
  sized to the longest FFT, mixed across scales per channel.

R2 `modifyChunk`:

- Two phase modes: independent per-bin advance from measured deviation
  (softer/phasier), or the default laminar mode, which walks bins
  top-down and blends each bin's advance toward its upper neighbour's
  when instability is rising in the same direction, with inheritance
  range growing with frequency (three configurable cutoff frequencies,
  defaults 600/1200/12000 Hz, widened for large upward ratios).
- Phase resets copy input phases for the reset frame (all bins in Crisp
  mode; all but 150–1000 Hz in Mixed mode; never in Smooth mode). Unity
  ratio triggers a gradual top-down reset sweep per channel. Mid/side
  processing is available for stereo pairs, same as R3. Waveform
  continuity uses overlap-add normalised by an accumulated window-area
  buffer (`v_divide` by `windowAccumulator` in `writeChunk`), plus an
  optional time-domain smoothing path (double-length windows with sinc
  interpolation) via `OptionSmoothingOn`.

## Facet 5 — Polyphony (global vs local alignment; the skip-rephasing question)

Neither engine performs multi-F0 estimation or per-source alignment, and
— critically for contrast with WSOLA — neither engine ever selects a
waveform skip/duplication point:

- R3 alignment is per-bin and per-band, never global. The hop grid is
  fixed by the ratio servo; only phases and magnitudes are adjusted,
  independently per frequency region (peak neighbourhoods), per scale,
  and per channel (with the low-frequency channel lock as the sole
  cross-channel coupling). A global skip that rephases every partial at
  once is structurally impossible here: there is no single time-shift
  decision shared across the spectrum. The analogous failure is local —
  peak misassignment or over/under-locking makes dense chords sound
  diffuse or metallic per band (the code explicitly trades metallic
  against diffuse for ratios above 2 by unlocking highs), but partials
  cannot precess against each other from a shared rephasing event
  because no such event exists.
- R2 alignment is a single global increment sequence (one output hop
  per chunk, shared across channels in realtime, derived from summed
  magnitudes), but it only redistributes hop sizes around exact-time
  transient anchors — it never skips or repeats input segments. Its
  global decision is "how long is this hop", not "where do we jump".
  Transient phase resets are the closest analog to a rephasing event,
  and they are spectrum-global (Crisp) or nearly so (Mixed keeps
  150–1000 Hz continuous), which is why the API docs warn that Crisp
  resets can audibly interrupt stable sounds coinciding with
  transients. Again, no skip-back precession mode exists: sustained
  polyphony artefacts here are phasiness/smearing from the advance
  estimator, not periodic rephasing wobble.

## Facet 6 — Transients (detection, resets, alternate paths, smear)

R3 transient handling (per channel, frequency-delimited, no alternate
time-domain path):

- Kick detection (`Guide::updateGuidance`): a new percussive region
  below ~40 Hz boundary movement plus a ≥40% rise in sub-200 Hz energy
  raises `kick` over 0 Hz to the percussive boundary; the same test on
  the readahead frame raises `preKick` one hop early. On kick frames the
  phase advancer copies input phase in that low range; on pre-kick
  frames `adjustPreKick` temporarily withholds the magnitude increase
  (`pendingKick`) and restores it at the kick, suppressing pre-smear.
  Only in multi-window mode.
- Broadband phase resets fire when the gap between percussive-above and
  residual-above newly exceeds 4 kHz (reset spans the new percussive
  band, widened to 0 Hz if it reaches below 200 Hz), on silence (full
  spectrum), and at unity (progressive, see facet 4).
- Smear tradeoff: short 1024 scale for highs keeps percussive tops
  tight; the long 4096 scale smears lows but they are the least
  transient-sensitive region; outhops above 256 drop the 1024 scale
  entirely (overlap would be inadequate), audibly softening attacks at
  extreme stretches.

R2 transient handling (global, time-anchored):

- Offline: `StretchCalculator::findPeaks` peak-picks the smoothed onset
  curve into soft peaks (median-window adaptive threshold, 90th
  percentile over ~1 s, 50 ms amnesty) for exact time placement and
  hard peaks (absolute/rise-rate rules: df > 0.4, or 40% single rise,
  or 2×20%, or 3×10% with df > 0.3) for phase resets; `calculate`
  maps anchors proportionally to output time and interpolates hop sizes
  between them, giving hard-peak chunks a unity hop with reset.
- Realtime: `calculateSingle` flags a transient when the onset value
  exceeds 0.35 and rises 10% over the previous frame (suppressed while
  the timing servo diverges >1000 samples or during the 50 ms amnesty),
  returns a negative increment to signal reset, and clamps hops to
  0.3×–2× nominal while servoing accumulated drift back over
  ~50–100 ms. Silence sustained over a window Hop count also forces
  reset. No lookahead: realtime resets land one chunk late by design
  (previous chunk's increment is reused as the phase increment).
- Smear tradeoff is user-visible: Crisp (reset all bins, clearest
  attacks, interrupts coincident stable tones) vs Mixed (spare the
  150–1000 Hz fundamentals) vs Smooth (never reset, phasier attacks).

## Facet 7 — Low frequencies (resolution, window scaling, low-B/E)

What sets LF resolution is the longest analysis window in both engines:

- R3 multi-window dedicates the 4096-point FFT (≈85 ms, ≈11.7 Hz bins
  at 48 kHz) to everything below the adaptive lower crossover
  (500–1100 Hz, default 700 Hz). A low-B fundamental (~62 Hz) spans
  about five bins there — enough for the p=1 peak picker to track it.
  The crossover placement explicitly descends to magnitude valleys so
  partials are not split across scales mid-note. Low notes do not need
  larger windows than this because the peak-locked advance only needs a
  stable local maximum, not a resolved harmonic comb.
- R3 single-window mode and the live shifter render the whole spectrum
  — lows included — with the 2048 FFT (≈23 Hz bins). Low-E (~82 Hz)
  and low-B fundamentals sit 3–4 bins up, so resolution is marginal;
  mitigation is the fixed p=1 phase-lock band over 0–1600 Hz, which at
  least keeps neighbouring bins moving with their local peak. This is
  the documented quality cost of the live/low-CPU path, and the reason
  the API steers bass-heavy material to R3 multi-window.
- R2 uses one window (2048 at 48 kHz, halved/doubled by the window
  options, scaled with sample rate, grown up to 4× for extreme downward
  ratios in realtime). LF resolution is whatever that window gives;
  there is no dedicated LF path. The laminar inherit limits (600 Hz
  default floor, widened for upward ratios) keep low bins advancing
  independently rather than inheriting from noisier neighbours.
- Neither engine has a special sub-band time-domain path for lows; low
  content always passes through the same STFT/overlap-add chain.

## Facet 8 — Resampling (location, design, share of shift, interaction)

- Location/share: as facet 2 — the resampler sits before or after the
  stretcher (live shifter: both sides, one always at unity) and always
  runs at `1/pitchScale`, contributing 100% of the frequency shift. The
  stretcher's effective ratio absorbs the reciprocal so net duration is
  `timeRatio`. `StretchCalculator` is told the post-resampling ratio
  explicitly (`effectivePitchRatio`) so hop servoing stays correct
  across the rate change; R2 queries the live resampler's
  `getEffectiveRatio` for this, R3 too.
- Default design (builtin `BQResampler`, selected by `-DUSE_BQRESAMPLER`
  when `resampler=auto/builtin`, the default): rational-approximation
  polyphase FIR. Each ratio is reduced by Farey search to numerator /
  denominator (denominator ≤ 96000 when often-changing, ≤ 192000 when
  mostly-fixed), realised as a Kaiser-windowed sinc prototype (90 dB
  stopband, 5% transition, 0.975 cutoff at the `FastestTolerable`
  quality every engine requests) evaluated per output phase. The
  achieved ratio differs slightly from the request; `getEffectiveRatio`
  reports the exact fraction so the hop servo can compensate.
- Backends and precedence: the `Resampler` constructor prefers, last
  match wins among compiled-in options: IPP polyphase → Speex →
  BQResampler → libsamplerate (so an explicit libsamplerate build wins
  over builtin). Speex carries a documented warning against
  time-varying pitch. IPP polyphase is noted non-thread-safe in
  practice, guarded by a dedicated mutex in threaded R2.
- Ratio-change behaviour: engines request `RatioOftenChanging` +
  `SmoothRatioChange` in realtime (BQ crossfades old/new filter states
  over ~1 ms with a raised-cosine mixer), `RatioMostlyFixed` +
  `SuddenRatioChange` offline (precomputed phase-sorted coefficients,
  instant switch). The live shifter uniquely uses sudden switching even
  though ratios change per block — a deliberate latency/jitter tradeoff.
- Resampler latency: a short filter transient at stream start (measured
  at runtime by `measureResamplerDelay` in the live shifter and padded
  for); steady-state group delay is a handful of samples relative to
  the window latencies and is not separately reported.

## Facet 9 — Latency (windows, lookahead, overlap, buffering, API reporting)

All figures at 48 kHz (44.1 kHz differs by <10%); resampler group delay
excluded as negligible next to window terms.

- R3Stretcher realtime, multi-window (default): window source span =
  max(2048 + 1024 readahead, 4096) = 4096. Reported start delay =
  half span = 2048 samples ≈ 42.7 ms when pre-resampling, else
  2048/pitchScale (downward shifts report more: the delay is measured
  in output samples after the rate change). Preferred start pad mirrors
  this in input samples. Components: half-window centring (2048) +
  one-hop classification readahead (≤1024, only when hops allow) +
  FIFO/accumulator depth (covered by the same span; drain tail is one
  longest-FFT ≈ 85 ms of decaying overlap, not added latency).
  Theoretical minimum for this topology is the half-span term; the
  implementation reports exactly that.
- R3Stretcher realtime, single-window (`OptionWindowShort`): span =
  max(2048 + 512, 2048) = 2560; reported delay 1280 ≈ 26.7 ms
  (÷ pitchScale when post-resampling).
- R3LiveShifter: full-span pre-pad (2048 default short, 2560 medium),
  reported delay = span + measured input-resampler start transient,
  scaled by the output ratio, plus output-resampler transient, plus a
  block-rate correction of ± up to ~512·|ratio − 1| samples. Practical
  total ≈ 50–60 ms as the header docs state ("50 ms or more").
  Per-call buffering adds one 512 block (≈10.7 ms) of throughput delay
  on top of the algorithmic figure.
- R2 realtime: reported delay = half analysis window = 1024 ≈ 21.3 ms
  (÷ pitchScale when post-resampling — the default for downward
  shifts). No lookahead term (realtime decisions are causal, one chunk
  late). Cheapest latency of all paths, at the quality costs in
  facets 5–7.
- Offline modes report 0/0: half-window prefill plus exact start-skip
  (`round(pad / pitchScale)`) and duration clamping are handled
  internally (`m_startSkip`, `theoreticalOut`).

## Facet 10 — CPU and realtime suitability

- Precision: `process_t` is `double` unless compiled otherwise, so all
  spectral magnitudes, phases, windows, and accumulators are
  double-precision; only FIFO I/O and the resampler interface are
  float. This roughly doubles spectral-path memory traffic vs float.
- Per-frame FFT load per channel per hop: R3 multi-window 3 forward +
  up to 3 inverse (one scale usually drops out per frame in practice
  via band assignment, but all three analyse every frame) + one extra
  forward for the readahead + forward/inverse pair for formant work
  when enabled; R3 single/live 1 + 1 (+readahead only in live-medium);
  R2 1 + 1 plus cheap onset math. FFT backend preference is
  IPP > vDSP > SLEEF > FFTW > builtin > KissDFT-fallback; default
  builds use vDSP on Apple targets, the builtin FFT elsewhere. R3 at
  48 kHz stereo multi-window runs ~6× 2048–4096-point FFTs per ~256
  output samples — the documented "much more CPU" vs R2.
- Other hot costs: per-band peak picking (O(bins × p) comparisons),
  dual median-filter classification per frame (R3), polar↔Cartesian
  conversions (scalar `atan2`/trig; optional Pommier SSE/NEON
  approximations exist but are purely opt-in via
  `-DUSE_POMMIER_MATHFUN`, recommended only for 32-bit mobile), and
  the polyphase resampler (filter length scales with ratio
  denominator; `RatioOftenChanging` caps work per call).
- Realtime safety: realtime paths preallocate everything (rings, fixed
  vectors, per-size FFT banks, resampler states) at construction /
  first `process`; steady-state `process`/`shift`/`retrieve` perform
  no allocation, locking, or blocking. Documented exceptions: buffer
  auto-growth when the caller exceeds the declared max process size,
  rapid swings between extreme ratios (R2 window-bank growth, resampler
  rebuild), error paths, and debug logging above level 1. Atomic
  ratio/hop state (`std::atomic`) with explicit lock-free checks; R3 is
  single-threaded by design, R2 realtime is single-threaded (worker
  threads exist only for offline multichannel). Callback suitability:
  the live shifter's fixed 512-block `shift()` is the intended
  callback shape; the stretcher's variable-output
  process/available/retrieve loop needs a small adapter but is equally
  allocation-free.

## Facet 11 — Ratio changes (fixed vs dynamic, smoothing, divebombs)

- Realtime is fully dynamic in all three processors: `setTimeRatio` /
  `setPitchScale` may be called at any time from the processing thread.
  R3 recomputes the input hop immediately (`calculateHop`) and the
  `StretchCalculator` checkpoints its frame counters at the change,
  then servo-corrects accumulated timing divergence over the next
  ~50–100 ms while clamping per-hop excursions to 0.3×–2× nominal —
  so divebomb-style pitch sweeps and tempo ramps glide without clicks
  or topology switches, at the cost of brief timing slop during the
  correction. The resampler crossfades filter states over ~1 ms per
  change (`SmoothRatioChange`).
- R2 additionally re-solves window/increment sizes live
  (`calculateSizes` via `reconfigure`); sizes it pre-built (base,
  half, double FFT banks) switch for free, anything else allocates
  with a warning. Crossing unity under default pitch mode flips the
  resample-before/after topology and resets resampler state — the
  reason `HighConsistency` (always-after) exists for modulation.
- The live shifter is built for per-block pitch updates (ratio read
  fresh in every `generate`/`readOut` cycle) but uses sudden resampler
  switching, so very fast sweeps can zip where the stretcher would
  crossfade.
- Offline: ratios freeze once `study`/`process` begins (R3 ignores the
  pitch option and always resamples after; R2 likewise); pre-planned
  variation uses key-frame maps (R2 interpolates anchors through
  `StretchCalculator::mapPeaks`; R3 re-solves per-segment ratios in
  `updateRatioFromMap`). State and latency are unaffected by ratio
  magnitude except through the window/hop sizing rules above.

## Facet 12 — Deliberate tradeoffs visible in code

- Two engines, no auto-select: callers pay R3's CPU only by opting in;
  defaults stay R2 for compatibility and cost.
- Resample direction per pitch sign (facet 2): upsampling before the
  stretcher is cheaper for upward shifts, after it for downward —
  default modes take the cheap side, HQ modes the clean side.
- Outhop > 256 drops the 1024 scale (R3) — protects overlap-add
  integrity at the price of duller attacks in extreme stretches.
- Ratio > 2 progressively unlocks high frequencies and narrows the
  channel lock — diffuse phasiness chosen over metallic ringing.
- Unity-ratio special paths everywhere (progressive top-down resets) —
  bit-transparency at 1.0 is worth dedicated machinery.
- Silence-triggered full resets — cheap re-anchoring that prevents
  endless phase drift in quiet tails.
- Readahead gated on hop size, disabled for long hops — lookahead only
  where its window fits the FIFO budget.
- `FastestTolerable` resampling everywhere, sudden switching in the
  live shifter — latency and CPU over inaudible filter differences.
- Double-precision spectral path with float I/O — quality where it
  compounds (phase integration), economy where it does not.
- R2's Mixed-transient default band (150–1000 Hz spared) and laminar
  inheritance floors — musical-fundamental continuity over transient
  crispness in the middle band.

## 13. R3LiveShifter end-to-end trace (separate path)

`RubberBandLiveShifter::shift` → `Impl::shift` →
`R3LiveShifter::shift` (fixed 512) → `readIn` (optional mid/side mix →
input resampler at `getInRatio` → input FIFO) → `generate` (estimate hop
count from input surplus over one window span and output deficit;
per hop: `analyseChannel` = window/FFT/polar/classify/segment/guide →
`GuidedPhaseAdvance::advance` synchronised across channels →
`adjustPreKick` (no-op: always single-windowed) → `synthesiseChannel` =
band-filtered inverse FFTs into accumulators, mix, advance; write mix to
output FIFO, skip input FIFO) → `readOut` (output FIFO → output
resampler at `getOutRatio` → caller buffer, mid/side decode). Ratio and
formant controls are plain atomics read per block; delay/block queries
are O(1) formulas. No `StretchCalculator`, no threading, no dynamic
topology: the only runtime adaptations are hop-count splitting and the
Guide's reset/unlock ranges.

## 14. Contrast notes for our lab (vs WSOLA W20 failure mode)

- The W20 skip-back rephasing wobble has no counterpart in RubberBand:
  neither engine searches for or jumps to waveform alignment points.
  R3's hop grid is ratio-dictated and fixed; all adaptation lives in
  per-bin/per-band phase updates, so sustained polyphony cannot
  precess from a shared skip event. Interval-dependent stability
  (fifths stable, fourths/thirds drifting) is a signature of a global
  time-shift interacting with partial periods — R3 structurally
  excludes that signature; R2's global hops change hop *length*, never
  position, which likewise cannot precess partials against each other.
- Closest analogs to watch if borrowing ideas: R3's frequency-delimited
  phase resets (kick/phase-reset ranges) are the disciplined version of
  "reset on transient" — note they never span the full spectrum except
  on silence/unity, precisely to avoid interrupting coincident stable
  partials; R2's Crisp mode shows what happens without that
  discipline. R3's pendingKick magnitude withholding is a neat
  pre-smear fix with no WSOLA equivalent.
- For live −1/−2 detune specifically: the live shifter's ~50 ms delay
  and single-window LF resolution are the costs of its fixed-block
  simplicity; R3 realtime multi-window halves the LF problem at similar
  delay but needs the variable-output adapter; R2 realtime is the
  lowest-delay option (~21 ms) but gives up per-band phase locking.

## 15. Files and functions actually inspected

Headers/API: `rubberband/RubberBandStretcher.h` (options, latency,
process/retrieve contracts), `rubberband/RubberBandLiveShifter.h`
(fixed-block contract), `src/RubberBandStretcher.cpp` (`Impl` engine
select + forwarders), `src/RubberBandLiveShifter.cpp` (`Impl` always
R3LiveShifter).

R3: `src/finer/R3Stretcher.h` (Limits, Channel/ScaleData, areWeResampling,
getEffectiveRatio, getWindowSourceSize), `src/finer/R3Stretcher.cpp`
(initialise, calculateHop, createResampler, process, consume,
prepareInput, analyseChannel, analyseFormant, adjustFormant,
adjustPreKick, synthesiseChannel, getStartDelay, getSamplesRequired,
ensureInbuf/outbuf, updateRatioFromMap, reset/study),
`src/finer/R3LiveShifter.h` (Limits, getInRatio/getOutRatio,
isSingleWindowed), `src/finer/R3LiveShifter.cpp` (initialise,
createResamplers, measureResamplerDelay, shift, readIn, generate,
readOut, analyseChannel, formant pair, adjustPreKick,
synthesiseChannel), `src/finer/Guide.h` (configuration sizes,
updateGuidance incl. kick/phaseReset/unity/silence/beta/valley),
`src/finer/PhaseAdvance.h` (`GuidedPhaseAdvance::advance` full body),
`src/finer/BinClassifier.h` (`classify`), `src/finer/BinSegmenter.h`
(`segment`), `src/finer/Peak.h` (`findNearestAndNextPeaks`).

R2: `src/faster/R2Stretcher.h` (class shape, cutShiftAndFold),
`src/faster/R2Stretcher.cpp` (constructor sizing, calculateSizes,
configure, reconfigure, study, calculateStretch, process,
getSamplesRequired, latency, option setters),
`src/faster/StretcherProcess.cpp` (resampleBeforeStretching,
consumeChannel, processChunks, processOneChunk, testInbufReadSpace,
processChunkForChannel, calculateIncrements, getIncrements,
analyseChunk, modifyChunk, formantShiftChunk, synthesiseChunk,
writeChunk, writeOutput, available, retrieve),
`src/faster/StretcherChannelData.h` + `.cpp` (buffer layout/sizes),
`src/faster/CompoundAudioCurve.h` + `.cpp`, `PercussiveAudioCurve.cpp`,
`HighFrequencyAudioCurve.cpp`, `SilentAudioCurve.cpp`,
`AudioCurveCalculator.h`, `SincWindow.h`.

Common: `src/common/StretchCalculator.h` + `.cpp` (calculate, mapPeaks,
findPeaks, smoothDF, calculateSingle, expectedOutFrame),
`src/common/Resampler.h` + `.cpp` (backend select, constructor
precedence), `src/common/BQResampler.h` + `.cpp` (QualityParams,
resampleInterleaved, fade, kaiser/sinc/polyphase, pick_params,
phase_data_for, make_filter), `src/common/Window.h` (all windows incl.
Niemitalo pair), `src/common/FFT.h` + `FFT.cpp` (backend preference
order), `src/common/mathmisc.h` + `.cpp` (roundUp/roundUpDiv, princarg,
bin/frequency maps, pickNearestRational), `src/common/sysutils.h`
(process_t, RTENTRY__), `src/common/RingBuffer.h` (peek/skip/write/zero),
`src/common/VectorOpsComplex.h` (Pommier gating), `meson.build` +
`meson_options.txt` (default FFT/resampler), `COMPILING.md` (Pommier
flag), `README.md` + `COPYING` (license).

## 16. Unresolved / not inspected

- Exact steady-state group delay of the BQ polyphase filters in samples
  (code derives it implicitly; only start-up shortfall is measured).
- `src/common/FFT.cpp` per-backend internals beyond the preference
  order; `Thread.cpp`, `Profiler`, `Allocators`, JNI/LADSPA/Vamp/CLI
  frontends; `single/RubberBandSingle.cpp` amalgamation details.
- Behaviour below 8 kHz / above 192 kHz sample rates (clamped with
  warning; not traced further).
- No build or listening test was run (reading-only study); all latency
  figures are derived from the reporting formulas, not measured.

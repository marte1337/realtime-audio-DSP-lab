#pragma once

// LabWsolaV2: causal / small-skip WSOLA research fork (LAB PROTOTYPE).
//
// Status: experimental candidate under evaluation in dsp/lab/Pitch/. NOT
// wired into TechDeathRig / RigParams / tdm_dev. Do not build product
// architecture on it until real-guitar listening validation passes.
//
// This is a line-for-line fork of LabWsolaShift with a two-way tie-break
// mode switch. Research questions (E2): (Q1) can a causal W20 (no forward
// search lookahead, ~20 ms latency) hold pitch/LF/attacks/stability; (Q2)
// can a small-skip landing preference materially reduce skip-back
// rephasing (fourth/fifth precession) on sustained polyphonic material,
// and what artifact (if any) replaces the periodic wobble.
//
// Modes (set via setMode(), studied from reset for clean attribution):
//   Drift (baseline): the LabWsolaShift algorithm verbatim. Proven
//      bit-identical to LabWsolaShift at equal config (test).
//   SmallSkip: identical EXCEPT pegged non-transient frames (skip-back
//      landings): among VALID lags (score within skipBand_ of the max),
//      take the minimal time-map jump |d - dPrev| with d != dPrev (the
//      anti-stick exclusion: J = 0 pins dry, found by probe on the
//      baseline). Drift runs, transient outright-max + floor, the HP
//      detector, NCC scoring, and the resampler are shared untouched -
//      drift-seeking already minimizes skip size among near-perfect lags,
//      so only the landing rule changes.
//
// Study cells (W20 fixed, geometry via setSearch):
//   A = Drift + symmetric (480, 480): the accepted W20 reference.
//   B = Drift + causal (960, 0): Q1 isolation (span 960 preserved,
//      latency 963 = 20.1 ms @-1).
//   C = SmallSkip + symmetric: Q2 isolation.
//   D = SmallSkip + causal: the E2 candidate.
//
// Deliberately NOT changed (attribution: only the marked branches
// differ): window/hop geometry, drift continuation + tie bands, transient
// guard + floor clamp, flux detector, NCC normalization, Hann-equivalent
// raised-cosine OLA, cubic slow read, latency formula W + Dp + C (the
// margin proof reads Dp, so it covers Dp = 0 unchanged).
//
// Deliberately NOT attempted here: candidate-energy score renormalization
// (kept shared so NCC cannot confound the tie-break A/B), spectral
// processing, post-hoc wobble correction, window enlargement, per-bin or
// multi-band decisions. Those follow only if this experiment shows the
// single-alignment line is still viable.
//
// NOTE on "center bias": a literal span-center landing preference would
// land mid-span (jumps ~span/2 - bounded, not small). Minimal-jump
// landing is the honest implementation of "minimize skip size" inside a
// drift architecture: total deletion per cycle is fixed by streaming
// balance, so small-frequent steps vs big-rare steps is the exact A/B
// the experiment arbitrates by ear-relevant metrics, not by assumption.
//

// Algorithm: WSOLA time compression + cubic slow-read resampling - the
// same compress-first/slow-read pairing as LabPitchShift, with waveform
// overlap-add replacing the phase vocoder as the compression engine.
// For target ratio r <= 1 (r = 2^(st/12)): analysis frames of length W
// every Ha = W/2 are each refined by d in [-Dm, +Dp] (default Dm = Dp =
// W/2, symmetric; the latency study admits asymmetric Dm > Dp, see
// setSearch) maximizing NORMALIZED cross-correlation against the
// already-synthesized tail, then overlap-added every Hs = round(r*Ha)
// with a raised-cosine crossfade, subject to DRIFT-SEEKING TIE-BREAK
// with TRANSIENT GUARD:
// among lags within the tie band of the best correlation score, the
// frame takes the lag closest to exact continuation (drift step
// -(Ha-Hs)). Drift IS the shift mechanism, proven: frame advance Hs
// makes the compressed stream 1:1 with the input, so the slow read
// yields input[r*t] (pitch x r); pinned lags (advance Ha) yield
// input[t+wobble] = dry with bounded jitter. Exact continuation always
// scores ~1.0 (it literally is the target samples), so argmax drifts on
// its own; the tie-break exists only so periodic ties drift instead of
// center-pinning (pin = dry - the -1 collapse, found by probe). At the
// -Dm peg the score slides off and argmax jumps back to a high-score lag
// (a WSOLA-aligned skip-back/deletion); drift+jump cycles ARE textbook
// WSOLA pitch shifting, and streaming balance is automatic (bounded
// lags => mean advance Ha). An earlier time-map-debt servo enforced
// mean advance Ha per-frame ("due Ha") and froze all drift, outputting
// dry everywhere - deleted, the model was backwards. Transients
// (HP-energy rise vs a trailing average) take the outright max so
// attacks align instead of smearing through drift.
// The compressed stream (same pitch, r x duration) is read
// back slowly at o*r (pitch x r, duration restored).
//
// Why WSOLA instead of another PV: the PV/multi studies showed LF
// integrity demands long spectral windows, forcing 100+ ms latency.
// WSOLA aligns the composite waveform directly, so a 20-40 ms window
// can serve all strings at once with no per-partial bookkeeping and no
// F0 / zero-crossing / monophonic assumption of any kind.
//
// Polyphony is structural (correlation sees the mix, never a "period"),
// but quality hinges on lock consistency: consecutive frames must lock
// at the same relative phase or joins modulate (flutter/chorusing).
// Span rule: Dm + Dp must cover a strong period of the lowest content
// or skip-backs peg/mush (low-B needs span 778+ samples at 48 kHz -
// probed symmetric; the latency study re-probes it asymmetric). Attacks
// take the outright max via the HP-energy transient guard (see .cpp).
//
// Latency is shift-dependent only through rounding: W + Dp + C samples,
// where C = ceil(3/ar-1) is the smallest margin that provably prevents
// FIFO starvation (burst quantization + resampler lookahead; pops are
// held until tick L so the FIFO prefills - see .cpp for the proof).
// Only the + side of the search costs latency (it reaches into the
// future: frame 0 places once input [0, W+Dp) has arrived); the - side
// reads history already in the ring. Symmetric D = W/2 gives the
// familiar L = 1.5W + C.
// 0 st is an exact bit-exact zero-latency bypass. Offline callers feed
// latencySamples() + tailSamples() extra zeros and drop the first
// latencySamples() outputs.
//
// Config: window length in MILLISECONDS (constant-time behavior at any
// rate), one of {14, 16, 20, 30, 40} for the study (14/16 added for the
// latency study). Set via setConfig() BEFORE reset(). Search asymmetry
// via setSearch() BEFORE reset(). Shift changes take effect on reset(),
// like LabPitchShift; divebomb sweeps are NOT in scope.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].
// Output peak is bounded by roughly the input peak (crossfades only
// blend existing samples; the resampler never boosts).

#include <vector>

namespace tdm
{
namespace lab
{
class LabWsolaV2
{
public:
  static constexpr float kMinShiftSt = -24.0f;
  static constexpr float kMaxShiftSt = 0.0f;
  static constexpr float kDefaultShiftSt = 0.0f;
  static constexpr double kDefaultWindowMs = 30.0;

  LabWsolaV2();

  // Lab config: windowMs must be one of {14.0, 16.0, 20.0, 30.0, 40.0}
  // (pinned study set - no ad-hoc sweep). Takes effect on the next
  // reset(). Throws std::invalid_argument on any other value.
  void setConfig(double windowMs);
  double windowMs() const { return windowMs_; }
  // E2: research variant selector. Takes effect on the next processed
  // frame (safe any time; studied from reset for clean attribution).
  enum class Mode
  {
    Drift = 0, // baseline: LabWsolaShift verbatim (see proof test)
    SmallSkip = 1, // minimal-jump skip-back landings (see header)
  };
  void setMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  // E2 study knob: SmallSkip validity band (score units below the frame
  // max). Default kSkipBandDefault; the study sweeps it to map the
  // jump-size vs join-quality trade. Takes effect on the next frame.
  static constexpr double kSkipBandDefault = 0.05;
  void setSkipBand(double band) { skipBand_ = band; }
  double skipBand() const { return skipBand_; }

  // Latency-study search geometry: lags span [-tolMinus, +tolPlus].
  // E2 extension over LabWsolaShift: tolPlus may be 0 (causal: no
  // forward reach; frame 0 places once [0, W) has arrived). tolMinus
  // must be in [1, W], tolPlus in [0, W] (checked against the
  // configured window at reset() time - throws std::invalid_argument
  // there, not here, so setSearch may precede setConfig). Takes effect
  // on the next reset(). Default (never called, or (0, 0)) is symmetric
  // W/2, which reproduces the accepted baseline bit-exactly (mode
  // Drift). Asymmetric Dm > Dp keeps skip-back span on the history
  // side while cutting lookahead latency; (W, 0) is fully causal.
  void setSearch(int tolMinus, int tolPlus);
  // Derived frame geometry in samples (valid post-reset).
  int frameLen() const { return frameLen_; }
  int analysisHop() const { return hopA_; }
  int synthHop() const { return hopS_; }
  int tolMinus() const { return tolMinus_; }
  int tolPlus() const { return tolPlus_; }
  int tolerance() const { return tolPlus_; } // lookahead side = latency term

  // Off-RT: validates the rate, derives frame geometry + synthesis hop
  // from the shift, (re)allocates all buffers, clears state.
  // Deterministic start: zero history, first frame placed verbatim.
  void reset(double sampleRate);

  // Shift in semitones, clamped to [kMinShiftSt, kMaxShiftSt]; fractional
  // values allowed. Takes effect on the next reset(). Exactly 0.0f (no
  // epsilon) becomes a bit-exact zero-latency bypass on reset().
  void setShiftSt(float semitones);
  void setEnabled(bool enabled);

  float shiftSt() const { return shiftSt_; }
  bool isEnabled() const { return enabled_; }
  // True pitch ratio after hop rounding (valid post-reset).
  double actualRatio() const { return actualRatio_; }
  // Streaming latency in samples (valid post-reset): W + Dp + C, measured
  // exactly (see .cpp). 0 when the exact-0-st bypass is active.
  int latencySamples() const { return latency_; }
  // Extra zeros an offline caller must feed past the input end (beyond
  // the latency) so the final outputs are fully computed. 0 when the
  // exact-0-st bypass is active.
  int tailSamples() const { return bypass0_ ? 0 : tail_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly,
  // as does the exact-0-st bypass. Otherwise emits exactly numFrames
  // samples; output lags the input by latencySamples().
  void processBlock(const float* input, float* output, int numFrames);

private:
  void placeFrame(); // run correlation search + overlap-add one frame
  float compressedAt(long long pos) const; // compressed stream, 0 pre-roll
  void pushOutput(float v); // append to outFifo_

  double windowMs_ = kDefaultWindowMs;
  double sampleRate_ = 0.0;
  float shiftSt_ = kDefaultShiftSt;
  bool enabled_ = false;
  Mode mode_ = Mode::Drift; // E2 tie-break selector
  double skipBand_ = kSkipBandDefault; // E2 validity band (study knob)

  int frameLen_ = 0; // W, from windowMs_
  int hopA_ = 0; // Ha = W/2
  int hopS_ = 0; // Hs = round(r*Ha), derived in reset()
  int wantTolMinus_ = 0; // setSearch request (0,0) = symmetric W/2
  int wantTolPlus_ = 0;
  int tolMinus_ = 0; // Dm, derived in reset()
  int tolPlus_ = 0; // Dp (lookahead side), derived in reset()
  int overlap_ = 0; // Lov = W - Hs (correlation + crossfade length)
  double actualRatio_ = 1.0; // Hs/Ha, the true pitch ratio
  int latency_ = 0; // derived in reset(), see above
  int tail_ = 0; // derived in reset()
  bool bypass0_ = false; // exact-0-st bypass, derived in reset()

  std::vector<float> inRing_; // input history ring (frame search span)
  int inMask_ = 0; // ring size - 1 (power of two)
  long long inCount_ = 0; // input samples consumed so far
  long long nextFrameAt_ = 0; // input count triggering the next frame

  std::vector<float> outAcc_; // compressed-stream OLA accumulator ring
  std::vector<float> outW_; // crossfade weight ring (raised-cosine)
  int outMask_ = 0;
  long long olaPos_ = 0; // absolute compressed position of next frame start
  long long compEmitted_ = 0; // compressed samples finalized so far

  std::vector<float> compRing_; // finalized compressed stream (resample source)
  int compMask_ = 0;
  long long resampOut_ = 0; // absolute output sample index (read = o*r)

  std::vector<float> fadeUp_; // raised-cosine crossfade tables, Lov
  std::vector<float> fadeDown_;
  std::vector<double> scoreBuf_; // per-lag correlation scores (servo pass)

  long long prevDelta_ = 0; // previous frame's chosen lag (advance = Ha+d-dPrev)
  double fluxTrail_ = 0.0; // trailing HP-frame energy (transient detector)
  long long framesPlaced_ = 0; // frames placed since reset (trail seeding)

public:
  // LAB DIAGNOSTIC telemetry (removable; deterministic counters only, no
  // allocation, updated in placeFrame): explains time-map behavior per run.
  struct Telemetry
  {
    long long frames = 0; // placeFrame calls since reset
    long long transientFrames = 0; // frames taking outright max (attacks)
    long long lagChurn = 0; // sum |d - dPrev| (time-map activity, samples)
  };
  Telemetry telemetry() const { return telemetry_; }

  // LAB DIAGNOSTIC frame trace (removable; default OFF). When enabled
  // (off-RT, before processing), placeFrame appends one record per
  // frame: selected lag, drift target, top-3 correlation peaks and the
  // selected lag's score. Read-only w.r.t. DSP state: enabling it
  // cannot change the render (pinned by test). Memory grows per frame,
  // so this is an offline instrument - never enable on the audio
  // thread. Clearing happens on enableTrace() and reset().
  struct FrameTrace
  {
    long long frame = 0; // placeFrame index since reset
    long long nominal = 0; // k*Ha input position
    long long best = 0; // selected lag
    long long cont = 0; // drift-continuation target lag
    long long prevDelta = 0; // previous frame's selected lag
    long long topLag[3] = {0, 0, 0}; // top-3 score lags (ascending-lag
    double topScore[3] = {-2.0, -2.0, -2.0}; // order wins ties; -2 = no search)
    double selScore = -2.0; // selected lag's score (-2 = no search)
    bool searched = false; // false for first frame / silent tail
    bool isTransient = false;
    bool pegged = false;
    bool first = false;
  };
  void enableTrace(bool on);
  const std::vector<FrameTrace>& trace() const { return trace_; }
  // LAB DIAGNOSTIC: copy of the last searched frame's score buffer,
  // ascending lag from -Dm (size Dm+Dp+1; all -2.0 before the first
  // search, -2.0 at lags the last search did not visit).
  std::vector<double> landscape() const;

private:
  Telemetry telemetry_;
  bool traceOn_ = false;
  std::vector<FrameTrace> trace_;

  std::vector<float> outFifo_; // completed samples awaiting emission
  int fifoRead_ = 0;
  int fifoWrite_ = 0;
  int fifoCount_ = 0;
  long long outCount_ = 0; // absolute output tick (pops held until tick L)

  bool firstFrame_ = true;
};
} // namespace lab
} // namespace tdm

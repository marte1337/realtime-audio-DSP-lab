#pragma once

// LabWsolaShift: time-domain polyphonic pitch shifter (LAB PROTOTYPE).
//
// Status: experimental candidate in dsp/lab/Pitch/. NOT wired into
// TechDeathRig / RigParams / tdm_dev. Do not build product architecture
// on it until real-guitar listening validation passes.
//
// Algorithm: WSOLA time compression + cubic slow-read resampling - the
// same compress-first/slow-read pairing as LabPitchShift, with waveform
// overlap-add replacing the phase vocoder as the compression engine.
// For target ratio r <= 1 (r = 2^(st/12)): analysis frames of length W
// every Ha = W/2 are each refined by d in [-D, +D] (D = W/4) maximizing
// NORMALIZED cross-correlation against the already-synthesized tail,
// then overlap-added every Hs = round(r*Ha) with a raised-cosine
// crossfade, subject to DRIFT-SEEKING TIE-BREAK with TRANSIENT GUARD:
// among lags within the tie band of the best correlation score, the
// frame takes the lag closest to exact continuation (drift step
// -(Ha-Hs)). Drift IS the shift mechanism, proven: frame advance Hs
// makes the compressed stream 1:1 with the input, so the slow read
// yields input[r*t] (pitch x r); pinned lags (advance Ha) yield
// input[t+wobble] = dry with bounded jitter. Exact continuation always
// scores ~1.0 (it literally is the target samples), so argmax drifts on
// its own; the tie-break exists only so periodic ties drift instead of
// center-pinning (pin = dry - the -1 collapse, found by probe). At the
// -D peg the score slides off and argmax jumps back to a high-score lag
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
// D covers only part of a 16 ms low-B period, so low-string locks are
// the known risk; there is deliberately NO transient detector in v1
// (pure WSOLA baseline - transient duplication/softening is expected
// and will be measured honestly, not tuned away blind).
//
// Latency is shift-dependent only through rounding: W + D + C samples,
// where C = ceil(3/ar-1) is the smallest margin that provably prevents
// FIFO starvation (burst quantization + resampler lookahead; pops are
// held until tick L so the FIFO prefills - see .cpp for the proof).
// 0 st is an exact bit-exact zero-latency bypass. Offline callers feed
// latencySamples() + tailSamples() extra zeros and drop the first
// latencySamples() outputs.
//
// Config: window length in MILLISECONDS (constant-time behavior at any
// rate), one of {20, 30, 40} for the study. Set via setConfig() BEFORE
// reset(). Shift changes take effect on reset(), like LabPitchShift;
// divebomb sweeps are NOT in scope.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].
// Output peak is bounded by roughly the input peak (crossfades only
// blend existing samples; the resampler never boosts).

#include <vector>

namespace tdm
{
namespace lab
{
class LabWsolaShift
{
public:
  static constexpr float kMinShiftSt = -24.0f;
  static constexpr float kMaxShiftSt = 0.0f;
  static constexpr float kDefaultShiftSt = 0.0f;
  static constexpr double kDefaultWindowMs = 30.0;

  LabWsolaShift();

  // Lab config: windowMs must be one of {20.0, 30.0, 40.0} (pinned study
  // set - no ad-hoc sweep). Takes effect on the next reset(). Throws
  // std::invalid_argument on any other value.
  void setConfig(double windowMs);
  double windowMs() const { return windowMs_; }
  // Derived frame geometry in samples (valid post-reset).
  int frameLen() const { return frameLen_; }
  int analysisHop() const { return hopA_; }
  int synthHop() const { return hopS_; }
  int tolerance() const { return tol_; }

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
  // Streaming latency in samples (valid post-reset): W + D + C, measured
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

  int frameLen_ = 0; // W, from windowMs_
  int hopA_ = 0; // Ha = W/2
  int hopS_ = 0; // Hs = round(r*Ha), derived in reset()
  int tol_ = 0; // D = W/2 (symmetric search; span 2D = W clears low-B)
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

private:
  Telemetry telemetry_;

  std::vector<float> outFifo_; // completed samples awaiting emission
  int fifoRead_ = 0;
  int fifoWrite_ = 0;
  int fifoCount_ = 0;
  long long outCount_ = 0; // absolute output tick (pops held until tick L)

  bool firstFrame_ = true;
};
} // namespace lab
} // namespace tdm

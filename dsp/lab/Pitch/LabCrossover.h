#pragma once

// LabCrossover: complementary 2-band split for lab multi-resolution pitch
// shifting (LAB PROTOTYPE, dsp/lab only).
//
// Topology: low = linear-phase FIR lowpass(x); high = delay_D(x) - low,
// where D is the FIR group delay. The sum is EXACT by construction
// (low + high == x delayed by D, to float rounding) at every frequency:
// no magnitude holes, no level buildup, no phase cancellation from the
// split itself. Whatever imperfection a downstream per-band process adds
// is confined to genuinely-split energy, which is why the transition is
// kept narrow (~200 Hz) rather than gentle.
//
// Why linear-phase FIR and not a cruder split: the subtraction complement
// reconstructs exactly only if the dry-path delay matches the lowpass
// phase delay at ALL frequencies, i.e. constant group delay. A
// minimum-phase IIR (frequency-dependent delay) would leave comb-like
// reconstruction errors. The price is D samples of constant delay on
// both bands plus symmetric FIR ringing; both are measured, not wished
// away (see LabMultiPitch analysis).
//
// Design (pinned for the study, derived per rate in reset()): -6 dB
// point ~300 Hz, Hann-windowed sinc, passband to ~200 Hz (all low-B/E
// chord fundamentals/roots/fifths: 62..220 Hz), stopband from ~400 Hz.
// Tap count scales with the rate for a CONSTANT-TIME delay (~8.3 ms).
// DC gain is normalized to exactly 1 (low keeps DC, high kills it).
//
// Expected signal range: finite floats, nominally [-1, 1]. Gain is
// unity on the sum; per-band peaks can slightly exceed the input peak
// near the transition (complementary linear-phase pairs ring).

#include <vector>

namespace tdm
{
namespace lab
{
class LabCrossover
{
public:
  static constexpr double kCutoffHz = 300.0; // -6 dB point of the lowpass
  static constexpr double kPassHz = 200.0; // response ~flat below this
  static constexpr double kStopHz = 400.0; // first sidelobe floor above this

  LabCrossover() = default;

  // Off-RT: validates the rate (same range as LabPitchShift), designs the
  // windowed-sinc lowpass for it, (re)allocates rings, clears state.
  // Throws std::invalid_argument on a bad rate.
  void reset(double sampleRate);

  // Constant group delay in samples (both bands carry it). Valid post-reset.
  int delaySamples() const { return delay_; }
  int taps() const { return static_cast<int>(coeffs_.size()); }
  double sampleRate() const { return sampleRate_; }

  // Split one sample. No allocation. Deterministic.
  void processSample(float x, float* low, float* high);

  // Block split, out-of-place (lo/hi must not alias input). No allocation.
  void processBlock(const float* input, float* low, float* high, int numFrames);

private:
  double sampleRate_ = 0.0;
  std::vector<float> coeffs_; // symmetric FIR, odd tap count
  int delay_ = 0; // (taps - 1) / 2

  std::vector<float> hist_; // input history ring, taps
  std::vector<float> dry_; // dry delay ring, delay + 1
  long long pos_ = 0;
};
} // namespace lab
} // namespace tdm

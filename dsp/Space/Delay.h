#pragma once

// Delay v1.1: intentionally simple musical ping-pong for leads and ambience.
//
// Topology: two cross-coupled delay lines (L/R). Dry enters the L line
// only; the L->R seed is unity while the R->L return carries the pair
// decay fb^2, so echoes arrive in equal-gain L/R pairs (1,1,F,F,...)
// with a single Time control: genuine alternation with a balanced wet
// center. (Per-echo decay would lean every decaying alternating train
// toward its lead side; equal-gain pairs are the only monotonic
// envelope with exact cumulative balance.) Taps are fractional with
// linear interpolation; the delay length itself is smoothed (~30 ms),
// so Time moves glide in pitch like a tape knob instead of zipping.
// Feedback and damping live inside the loop:
//
//   tapL = lineL[T]              tapR = lineR[T]
//   lineL = x + fb^2 * damp(tapR)   (dry + pair-decay return)
//   lineR = gate(fb) * damp(tapL)   (unity seed, gated near fb=0)
//
// The R-seed gate (linear ramp 0..0.1 on the smoothed fb) keeps fb=0 a
// single L echo; above the knee the seed is exactly unity. Feedback is
// capped at 0.85, so round-trip gain (<= fb^2 = 0.72) stays below unity
// by construction (plus the damping only ever removes energy).
//
// Damping is a fixed gentle one-pole lowpass (~4.5 kHz, not exposed):
// repeats darken as they decay instead of building harshness, and the
// loop can never accumulate highs.
//
// Wet-only outputs (no dry inside): the owner (SpaceProcessor) sums dry
// plus mix-scaled wet, so the dry transient is never touched by this
// class. Disabled (or never reset) writes exact zeros.
//
// Ranges: Time 20..2000 ms (default 220), Feedback 0..0.85 (default
// 0.35, now the per-pair decay). Buffers for the full 2 s are allocated
// in reset() (off-RT); processBlock/processSample allocate nothing,
// lock nothing, do no IO. States snap to 0 below 1e-12 (denormal guard,
// house precedent).
//
// Expected signal range: finite floats; wet taps stay within a small
// multiple of the input (loop gain < 1 guarantees decay, not growth).

#include <cstddef>
#include <vector>

namespace tdm
{
class Delay
{
public:
  static constexpr float kMinTimeMs = 20.0f; // slap territory
  static constexpr float kMaxTimeMs = 2000.0f; // ambient wash
  static constexpr float kDefaultTimeMs = 220.0f; // connected technical-lead neighborhood
  static constexpr float kMinFeedback = 0.0f; // single echo
  static constexpr float kMaxFeedback = 0.85f; // long tail, always < 1 (stability)
  static constexpr float kDefaultFeedback = 0.35f;

  // Fixed internals (not exposed in v1).
  static constexpr float kDampHz = 4500.0f; // loop damping lowpass
  static constexpr float kSmoothTimeMs = 30.0f; // delay-length slew (pitch glide)
  static constexpr float kSmoothGainMs = 10.0f; // feedback smoothing
  static constexpr float kSeedGateKnee = 0.1f; // R-seed gate fully open above this fb

  Delay();

  // Off-RT: validate rate, size lines for the full 2 s, clear states,
  // snap smoothed values to targets (documented deterministic state).
  void reset(double sampleRate);

  // Off-RT setters. Clamped to the v1 ranges.
  void setTimeMs(float ms);
  void setFeedback(float v);
  void setEnabled(bool enabled);

  float timeMs() const { return timeMs_; }
  float feedback() const { return feedback_; }
  bool isEnabled() const { return enabled_; }

  // Wet-only stereo output; zeros when disabled/unreset. outL/outR must be
  // distinct writable buffers (aliasing the input is safe).
  void processBlock(const float* input, float* outL, float* outR, int numFrames);
  // RT-safe per-sample core (used by the block wrapper and SpaceProcessor).
  void processSample(float x, float& wetL, float& wetR);

private:
  void refreshTargets(); // recompute targets from params + rate
  float readInterp(const std::vector<float>& line, size_t pos) const;

  double sampleRate_ = 0.0;
  float timeMs_ = kDefaultTimeMs;
  float feedback_ = kDefaultFeedback;
  bool enabled_ = false; // explicit opt-in; wet is silent until set

  float delayTarget_ = 0.0f; // delay length target, in samples
  float fbTarget_ = kDefaultFeedback;

  float delaySamp_ = 0.0f; // smoothed delay length, in samples
  float fb_ = kDefaultFeedback; // smoothed feedback
  float smoothTimeCoeff_ = 1.0f;
  float smoothGainCoeff_ = 1.0f;
  float dampA_ = 0.0f; // loop damping coefficient

  std::vector<float> lineL_; // circular delay lines, sized in reset()
  std::vector<float> lineR_;
  size_t len_ = 0;
  size_t posL_ = 0;
  size_t posR_ = 0;
  float dampL_ = 0.0f; // loop damping filter states
  float dampR_ = 0.0f;
};
} // namespace tdm

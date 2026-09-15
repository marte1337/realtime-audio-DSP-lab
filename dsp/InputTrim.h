#pragma once

// InputTrim: user-controlled linear input gain before Gate/NAM.
//
// Pure gain only: no EQ, no distortion, no latency, polarity preserved.
// Exists so a quiet guitar/interface can drive the NAM model and sit
// correctly above the gate threshold without touching interface gain.
//
// Smoothing: the applied linear gain follows its target through a one-pole
// lowpass (tau ~10 ms, coefficient recomputed in reset() from the sample
// rate, so timing is rate-independent). A gain step multiplied into the
// signal is the discontinuity source, so the gain itself ramps: even a full
// 30 dB jump settles over ~70 ms, inaudible as zipper yet immediate enough
// for auditioning. Linear-domain smoothing keeps it to one multiply per
// sample with no segment bookkeeping; the target never nears zero (min
// -12 dB ~= 0.25), so there is no denormal concern.
//
// Realtime contract: setter/reset() are off-RT (they allocate nothing, only
// recompute the coefficient). processBlock() is RT-safe: no allocation, no
// locks, no file IO, deterministic, finite output for finite input.
//
// Expected signal range: finite floats, nominally DI guitar within [-1, 1].

namespace tdm
{
class InputTrim
{
public:
  static constexpr float kMinTrimDb = -12.0f;
  static constexpr float kMaxTrimDb = 18.0f;
  static constexpr float kDefaultTrimDb = 0.0f;

  // Fixed internal smoothing time (not exposed in v1).
  static constexpr float kSmoothMs = 10.0f;

  InputTrim();

  // Off-RT: validate rate, recompute smoothing coefficient, snap applied
  // gain exactly to target (documented deterministic state; at the 0 dB
  // default the stage is bit-exact).
  void reset(double sampleRate);

  // Off-RT setter. Clamped to [kMinTrimDb, kMaxTrimDb].
  void setTrimDb(float db);

  float trimDb() const { return trimDb_; }
  // Read-only observation for tests / future metering. RT-safe.
  float currentGain() const { return gain_; }

  // Mono float processing, in-place safe. At 0 dB output equals input.
  void processBlock(const float* input, float* output, int numFrames);

private:
  void refreshDerived(); // recompute target gain / coeff from params + rate

  double sampleRate_ = 0.0;
  float trimDb_ = kDefaultTrimDb;

  float targetGain_ = 1.0f;
  float smoothCoeff_ = 1.0f;
  float gain_ = 1.0f;
};
} // namespace tdm

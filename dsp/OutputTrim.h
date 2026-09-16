#pragma once

// OutputTrim: user-controlled linear output gain after Cabinet IR.
//
// Pure gain only: no EQ, no nonlinear processing, no latency, no clipping,
// no limiting, no normalization, polarity preserved. Exists so captures
// with wildly different baked-in levels (e.g. ~19 dB across our NAMs) can
// be loudness-matched WITHOUT touching Input Trim, which would alter NAM
// drive/saturation/compression. This stage sits after every nonlinear and
// time-based process, so it changes listening level only.
//
// Deliberately NO output protection: internal float audio may legitimately
// exceed unity, and a hot Output Trim preserves values above 0 dBFS as-is.
// The user (or later host/output handling) owns safe audition levels.
//
// Smoothing: the applied linear gain follows its target through a one-pole
// lowpass (tau ~10 ms, coefficient recomputed in reset() from the sample
// rate, so timing is rate-independent) — the same idiom as InputTrim. A
// gain step multiplied into the signal is the discontinuity source, so the
// gain itself ramps: even a full 48 dB jump settles quickly and without
// zipper, yet fast enough for live auditioning. Linear-domain smoothing
// keeps it to one multiply per sample with no segment bookkeeping.
//
// Realtime contract: setter/reset() are off-RT (they allocate nothing, only
// recompute the coefficient). processBlock() is RT-safe: no allocation, no
// locks, no file IO, deterministic, finite output for finite input.
//
// Expected signal range: finite floats, typically post-IR guitar around
// [-1, 1] but possibly hotter; output is input scaled, never clipped.

namespace tdm
{
class OutputTrim
{
public:
  static constexpr float kMinTrimDb = -24.0f;
  static constexpr float kMaxTrimDb = 24.0f;
  static constexpr float kDefaultTrimDb = 0.0f;

  // Fixed internal smoothing time (not exposed in v1).
  static constexpr float kSmoothMs = 10.0f;

  OutputTrim();

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
  // Values above unity pass through unclipped by design.
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

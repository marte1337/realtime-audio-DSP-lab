#pragma once

// TightDrive: pre-NAM conditioning / overdrive for articulate tech-death.
//
// Signal flow (mono float, in-place safe):
//   HPF(Tight) -> preGain(Drive) -> tanh -> presence shelf(Bite)
//     -> fixed 12 kHz lowpass -> out
//
// - Tight trims uncontrolled lows BEFORE the nonlinear stage (the tight-
//   metal move: sub flub removed before amp compression, where post-EQ
//   cannot restore transient definition). First-order HPF, no resonance.
// - Drive pushes a C-infinity tanh shaper (odd-symmetric, so no DC; bounded
//   +/-1, so firm overdrive but never fuzz). The push is intentional: it
//   drives the NAM harder. No output compensation is applied; see gain
//   staging notes below.
// - Bite is a post-shaper presence shelf (parallel form, unconditionally
//   stable): smoother/rounder vs cutting/pick-forward. Placed AFTER the
//   shaper so it never pre-emphasizes fizz into the clipper.
// - The fixed 12 kHz lowpass is anti-fizz/anti-alias insurance above the
//   guitar presence range, not a tone control.
//
// Gain staging (all documented, nothing hidden):
//   pre-drive gain  0..+18 dB (Drive 0..1 -> preGain 1..8 linear)
//   waveshaper out  bounded to +/-1 by tanh
//   presence shelf  -5..+5 dB above ~3 kHz (Bite 0..1)
//   fixed lowpass   unity below ~12 kHz
// Overall level rises with Drive. That is the product (a boost), not a
// loudness trick: judge focus/drive in audition, not raw level.
//
// Smoothing: applied preGain, HPF coefficient, and shelf mix each follow
// their target through a one-pole lowpass (tau ~10 ms, rate-independent
// via reset()), so parameter moves cannot click or zipper. The fixed-LPF
// coefficient needs no smoothing.
//
// Realtime contract: setters/reset() are off-RT (they allocate nothing,
// only recompute coefficients). processBlock() is RT-safe: no allocation,
// no locks, no file IO, deterministic, finite output for finite input.
// Filter states snap to 0 below 1e-12 (denormal guard, gate precedent).
//
// Expected signal range: finite floats, nominally DI guitar within [-1, 1]
// (hotter after Input Trim; tanh keeps the output bounded regardless).

namespace tdm
{
class TightDrive
{
public:
  // Public v1 parameters.
  static constexpr float kMinTight = 0.0f; // little/no tightening
  static constexpr float kMaxTight = 1.0f; // extreme rhythm control
  static constexpr float kDefaultTight = 0.5f;
  static constexpr float kMinDrive = 0.0f; // near-clean conditioning
  static constexpr float kMaxDrive = 1.0f; // firm overdrive, never fuzz
  static constexpr float kDefaultDrive = 0.3f;
  static constexpr float kMinBite = 0.0f; // smoother/rounder
  static constexpr float kMaxBite = 1.0f; // cutting/pick-forward
  static constexpr float kDefaultBite = 0.5f; // neutral (exactly 0 dB shelf)

  // Documented internal constants (fixed in v1, not exposed).
  static constexpr float kHpfMinHz = 40.0f; // transparent for 7-string low B
  static constexpr float kHpfMaxHz = 320.0f; // surgical low-end control
  static constexpr float kPreGainMin = 1.0f; // 0 dB
  static constexpr float kPreGainMax = 8.0f; // ~+18 dB
  static constexpr float kShelfHz = 3000.0f; // presence corner
  static constexpr float kShelfMinDb = -5.0f;
  static constexpr float kShelfMaxDb = 5.0f;
  static constexpr float kPostLpfHz = 12000.0f; // fixed anti-fizz lowpass
  static constexpr float kSmoothMs = 10.0f; // parameter smoothing time

  TightDrive();

  // Off-RT: validate rate, recompute coefficients, zero filter states, snap
  // smoothed values to targets (documented deterministic state).
  void reset(double sampleRate);

  // Off-RT setters. Clamped to the v1 ranges.
  void setTight(float v);
  void setDrive(float v);
  void setBite(float v);
  void setEnabled(bool enabled);

  float tight() const { return tight_; }
  float drive() const { return drive_; }
  float bite() const { return bite_; }
  bool isEnabled() const { return enabled_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly.
  void processBlock(const float* input, float* output, int numFrames);

private:
  void refreshDerived(); // recompute targets/coeffs from params + rate

  double sampleRate_ = 0.0;
  float tight_ = kDefaultTight;
  float drive_ = kDefaultDrive;
  float bite_ = kDefaultBite;
  bool enabled_ = false; // explicit opt-in; rig path unchanged until set

  // Coefficient targets (recomputed by reset()/setters).
  float hpfTarget_ = 0.0f; // one-pole HPF coefficient a
  float preTarget_ = 1.0f; // linear pre-drive gain
  float shelfTarget_ = 0.0f; // parallel-shelf mix k (0 = neutral)
  float lpfA_ = 0.0f; // fixed post lowpass coefficient a
  float shelfHpA_ = 0.0f; // shelf corner HPF coefficient a
  float smoothCoeff_ = 1.0f; // per-sample parameter smoothing coeff

  // Smoothed applied values (follow targets in the RT loop).
  float hpfA_ = 0.0f;
  float preGain_ = 1.0f;
  float shelfK_ = 0.0f;

  // Filter state (scalars only; nothing to preallocate).
  float hpfY_ = 0.0f;
  float hpfXPrev_ = 0.0f;
  float shelfHpY_ = 0.0f;
  float shelfHpXPrev_ = 0.0f;
  float lpfY_ = 0.0f;
};
} // namespace tdm

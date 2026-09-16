#pragma once

// ToneShape v1: post-cabinet tonal shaping for the tech-death family sound.
//
// Signal flow (mono float, in-place safe, all sections linear):
//   Weight low shelf -> Contour mid bell -> Presence high shelf -> out
//
// - Weight trims low / low-mid mass (parallel low shelf, corner ~140 Hz,
//   +/-6 dB). The family-matching move: a weighty 6505 vs a leaner capture
//   meet here without touching sub mud or mid definition.
// - Contour balances low-mid body against separation (parallel bandpass
//   bell, center ~600 Hz, Q ~1, +/-6 dB). Cut for note separation, boost
//   for body on thin captures. The bell keeps Contour out of Weight's
//   low region and Presence's attack region.
// - Presence sets post-cab attack / bite (parallel high shelf, corner
//   ~3.5 kHz, +/-5 dB, same idiom as TightDrive Bite). Post placement is
//   deliberate: it shapes pick attack after the speaker, never fizz into
//   a clipper (there is no clipper here; the stage is fully linear).
//
// All three sections use the parallel form y = x + k*section(x), so the
// neutral (0.5) setting is exactly unity: an enabled unit at defaults is
// bit-exact from reset, and k itself is the only smoothed quantity.
//
// Why a biquad for Contour only: Weight/Presence are shelves and want the
// gentle skirts of one-pole sections (exact by construction, TightDrive
// precedent). A bell built from subtractive one-poles interacts audibly
// with the Weight region (+2-3 dB at 100 Hz at full body — measured while
// designing). Contour instead uses one fixed RBJ bandpass (constant 0 dB
// peak gain form) with a numerically normalized mix, giving true biquad
// skirts and exact +/-6 dB at center. The biquad coefficients are fixed
// per sample rate (only k moves, smoothed), so there is no coefficient
// zipper. This is the first biquad in the repo: flagged for senior DSP
// review (see experiments log).
//
// Deliberately NO fixed anti-fizz lowpass: unlike TightDrive (pre-NAM, pre-
// IR, feeding a saturator), this stage is linear and sits after the cabinet
// IR, so it cannot create fizz or alias products — a fixed LPF would only
// dull existing content. If audition reports harshness that Presence-cut
// cannot fix, that is an amp/IR choice, not a ToneShape gap.
//
// Gain staging (all documented, nothing hidden):
//   weight shelf   -6..+6 dB below ~140 Hz (Weight 0..1; exact at DC)
//   contour bell   -6..+6 dB at ~600 Hz, Q ~1 (Contour 0..1; exact at fc)
//   presence shelf -5..+5 dB nominal above ~3.5 kHz (Presence 0..1; the
//     one-pole curve realizes ~+4 dB at 15 kHz/48 kHz — same idiom as Bite)
// No output compensation: cuts and boosts are the product. DC passes with
// the Weight gain (up to +6 dB on any NAM DC offset — typically tiny; there
// is deliberately no DC blocker in v1, same stance as the rest of the rig).
//
// Smoothing: each mix k follows its target through a one-pole lowpass
// (tau ~10 ms, rate-independent via reset()), so parameter moves bend,
// never step. Filter coefficients need no smoothing (fixed per rate).
//
// Realtime contract: setters/reset() are off-RT (they allocate nothing,
// only recompute coefficients). processBlock() is RT-safe: no allocation,
// no locks, no file IO, deterministic, finite output for finite input.
// Filter states snap to 0 below 1e-12 (denormal guard, gate precedent).
//
// Expected signal range: finite floats, nominally post-IR guitar within
// [-1, 1] (hotter possible; the stage is linear so hot stays proportional).

namespace tdm
{
class ToneShape
{
public:
  // Public v1 parameters (all neutral-bit-exact at default from reset).
  static constexpr float kMinWeight = 0.0f; // lean
  static constexpr float kMaxWeight = 1.0f; // massive
  static constexpr float kDefaultWeight = 0.5f; // 0 dB
  static constexpr float kMinContour = 0.0f; // scooped / separation
  static constexpr float kMaxContour = 1.0f; // full body
  static constexpr float kDefaultContour = 0.5f; // 0 dB
  static constexpr float kMinPresence = 0.0f; // round
  static constexpr float kMaxPresence = 1.0f; // cutting
  static constexpr float kDefaultPresence = 0.5f; // 0 dB

  // Documented internal constants (fixed in v1, not exposed).
  static constexpr float kWeightHz = 140.0f; // low-shelf corner
  static constexpr float kWeightMaxDb = 6.0f; // shelf magnitude
  static constexpr float kContourHz = 600.0f; // bell center
  static constexpr float kContourQ = 1.0f; // bell width (~1 octave)
  static constexpr float kContourMaxDb = 6.0f; // bell magnitude
  static constexpr float kPresenceHz = 3500.0f; // high-shelf corner
  static constexpr float kPresenceMaxDb = 5.0f; // shelf magnitude
  static constexpr float kSmoothMs = 10.0f; // parameter smoothing time

  ToneShape();

  // Off-RT: validate rate, recompute coefficients, zero filter states, snap
  // smoothed mixes to targets (documented deterministic state).
  void reset(double sampleRate);

  // Off-RT setters. Clamped to the v1 ranges.
  void setWeight(float v);
  void setContour(float v);
  void setPresence(float v);
  void setEnabled(bool enabled);

  float weight() const { return weight_; }
  float contour() const { return contour_; }
  float presence() const { return presence_; }
  bool isEnabled() const { return enabled_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly.
  void processBlock(const float* input, float* output, int numFrames);

private:
  void refreshDerived(); // recompute targets/coeffs from params + rate

  double sampleRate_ = 0.0;
  float weight_ = kDefaultWeight;
  float contour_ = kDefaultContour;
  float presence_ = kDefaultPresence;
  bool enabled_ = false; // explicit opt-in; rig path unchanged until set

  // Mix targets (recomputed by reset()/setters).
  float weightTarget_ = 0.0f; // parallel-shelf mix k (0 = neutral)
  float contourTarget_ = 0.0f; // normalized parallel-bell mix (0 = neutral)
  float presenceTarget_ = 0.0f; // parallel-shelf mix k (0 = neutral)
  float smoothCoeff_ = 1.0f; // per-sample mix smoothing coeff

  // Fixed filter coefficients (rate only).
  float lpA_ = 0.0f; // weight lowpass coefficient a
  float hpA_ = 0.0f; // presence corner highpass coefficient a
  float bpB0_ = 0.0f; // contour bandpass feedforward (normalized)
  float bpB2_ = 0.0f; // (b1 is identically 0 in this topology)
  float bpA1_ = 0.0f; // contour bandpass feedback (normalized)
  float bpA2_ = 0.0f;
  float contourNorm_ = 1.0f; // |BP(fc)|, makes center gain exact

  // Smoothed applied mixes (follow targets in the RT loop).
  float weightK_ = 0.0f;
  float contourK_ = 0.0f;
  float presenceK_ = 0.0f;

  // Filter state (scalars only; nothing to preallocate).
  float lpY_ = 0.0f; // weight lowpass
  float bpV1_ = 0.0f; // contour biquad delay states (TDF2)
  float bpV2_ = 0.0f;
  float hpY_ = 0.0f; // presence corner highpass
  float hpXPrev_ = 0.0f;
};
} // namespace tdm

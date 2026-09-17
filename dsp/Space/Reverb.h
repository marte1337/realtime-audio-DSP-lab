#pragma once

// Reverb v1.3: dual-tank cross-coupled plate for guitar ambience.
//
// Topology (replaces the v1.2 single-tank modulated FDN: real-guitar
// audition found it center-heavy, collapsing, still metallic when heard
// alone — one fully-mixed tank is a SINGLE resonant field, so both ears
// ring the same modes at similar levels; sustained tonal input then
// correlates across ears no matter how decorrelated the impulse tail
// measures. Probes confirmed it: a 1760 Hz band sat at side/mid 0.3
// while neighbors measured 1-5, i.e. a centered metallic band):
//   mono in -> fixed 8 ms predelay -> fixed 180 Hz send highpass
//   -> 4 series input-diffusion allpasses
//   -> differential drive (+dif to tank A, -dif to tank B)
//   -> two cross-coupled tanks (4 modulated lines each, per-tank
//      Householder mixing, slow rotation cross-feed between tanks,
//      per-line lowpass-damped T60 gains)
//   -> L = own-tank sum + small cross-tank bleed (mirrored for R)
//   -> wet stereo out
//
// - Two tanks, two modal grids: tank A totals ~269 ms of line, tank B
//   ~277 ms from all-distinct lengths, so no frequency rings identically
//   in both ears. L is A-dominant, R is B-dominant: width is structural,
//   not a post effect, and needs no Delay to exist.
// - Differential drive plants a large initial A-vs-B difference;
//   rotation cross-feed (full A<->B exchange over ~0.35 s) walks it
//   around instead of letting it collapse: the difference NORM is
//   preserved by the rotation, so side energy persists through the tail
//   while its character keeps evolving. There is no favored common
//   mode (a rotation has no real eigenvectors), which is what made the
//   old Householder-8 field drift center-ward.
// - Mixing is exactly energy-preserving (per-tank Householder rows +
//   per-pair rotation, both orthogonal), so with per-line loop gains
//   < 1 the tank is strictly contractive in energy: modulation (which
//   only moves read positions through convex combinations) cannot
//   destabilize it, at any Decay.
// - Tank modulation stays subtle (+/-0.125 ms at 0.11-0.43 Hz per line):
//   modes walked, never chorus. Phases park in reset (deterministic).
// - Each tank line carries a fixed gentle lowpass (~5.5 kHz): highs decay
//   faster than lows, no harsh buildup, no exposed control.
// - Per-line feedback gains derive from the Decay parameter via the T60
//   formula on each line's own length, so decay time is
//   rate-independent by construction; gains are smoothed (~50 ms) so
//   Decay moves morph.
// - Mono fold-down is clean by construction: both channels share the
//   same common content with the same sign, so (L+R)/2 keeps nearly
//   full energy (guards against anti-phase collapse, pinned by test).
// - The fixed send highpass is the low-end control (audition-approved
//   since v1): reverb never sees sub/box mud, so rhythm definition
//   survives even at high Mix.
//
// Wet-only outputs (no dry inside): the owner (SpaceProcessor) sums dry
// plus mix-scaled wet. Disabled (or never reset) writes exact zeros.
//
// Ranges: Decay 0..1 maps to T60 0.25..5 s (default 0.4). Line lengths
// scale from their 48 kHz tunings. All buffers are allocated in reset()
// (off-RT); processBlock/processSample allocate nothing, lock nothing,
// do no IO. States snap below 1e-12.
//
// Expected signal range: finite floats; wet level stays proportional to
// the input (contractive loop, see above).

#include <cstddef>
#include <vector>

namespace tdm
{
class Reverb
{
public:
  static constexpr float kMinDecay = 0.0f; // tight room (~0.25 s)
  static constexpr float kMaxDecay = 1.0f; // large hall (~5 s)
  static constexpr float kDefaultDecay = 0.4f; // lead-room neighborhood

  // Fixed internals (not exposed in v1.3).
  static constexpr float kT60MinSec = 0.25f;
  static constexpr float kT60MaxSec = 5.0f;
  static constexpr float kPreDelayMs = 8.0f; // articulation gap
  static constexpr float kDampHz = 5500.0f; // tank-line damping lowpass
  static constexpr float kSendHpHz = 180.0f; // send highpass (mud control)
  static constexpr float kDiffGain = 0.7f; // input-diffusion allpass gain
  // Wobble excursion: peak detune of recirculating content stays ~1 cent
  // (depth * 2π * fastest LFO << any pitch threshold), while modal
  // frequencies walk far enough to matter over multi-second tails.
  static constexpr float kModDepthMs = 0.25f; // tank wobble excursion
  static constexpr float kSmoothGainMs = 50.0f; // tank-gain morph time
  static constexpr float kCrossTimeSec = 0.35f; // full A<->B exchange time
  // Negative cross-tank bleed: outputs lean differential (wide) while the
  // shared common content keeps mono fold-down safe. This is a deliberate
  // tradeoff knob, not a tuned magic number: more negative = wider field
  // but deeper per-bin mono notches where tanks anti-align (+0.25 measured
  // center-heavy S/M 0.36; -0.20 measured S/M ~2.3 with isolated bins near
  // anti-phase). -0.15 splits the difference: clearly wide, milder bins.
  static constexpr float kOutBleed = -0.15f; // cross-tank output bleed

  Reverb();

  // Off-RT: validate rate, size lines from 48 kHz tunings, clear states,
  // snap tank gains to targets, park modulation phases, derive the fixed
  // rotation coefficients (documented deterministic state).
  void reset(double sampleRate);

  // Off-RT setters. Clamped to the v1.3 ranges.
  void setDecay(float v);
  void setEnabled(bool enabled);

  float decay() const { return decay_; }
  bool isEnabled() const { return enabled_; }

  // Wet-only stereo output; zeros when disabled/unreset. outL/outR must be
  // distinct writable buffers (aliasing the input is safe).
  void processBlock(const float* input, float* outL, float* outR, int numFrames);
  // RT-safe per-sample core (used by the block wrapper and SpaceProcessor).
  void processSample(float x, float& wetL, float& wetR);

private:
  void refreshTargets(); // recompute T60 + per-line gain targets
  float allpassStep(std::vector<float>& buf, size_t& pos, size_t len, float x);
  float readLerp(const std::vector<float>& buf, size_t len, size_t writePos, float delay) const;

  static constexpr int kNumLines = 8; // 4 per tank; tank B starts at kTankB
  static constexpr int kTankB = 4;
  // All-distinct odd lengths @48 kHz: A totals ~269 ms, B ~277 ms, so the
  // two modal grids interleave without coinciding. Shortest line (30 ms)
  // seeds early energy; wet onset lands ~38 ms after the transient.
  static constexpr int kLineBase[8] = {4211, 3361, 2903, 1439,
                                       3889, 3547, 3109, 1729}; // @48 kHz
  static constexpr int kNumDiff = 4;
  static constexpr int kDiffBase[4] = {197, 293, 447, 653}; // @48 kHz
  static constexpr float kModRateHz[8] = {0.11f, 0.13f, 0.17f, 0.23f,
                                          0.29f, 0.31f, 0.37f, 0.43f};

  double sampleRate_ = 0.0;
  float decay_ = kDefaultDecay;
  bool enabled_ = false; // explicit opt-in; wet is silent until set

  float lineTarget_[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float lineGain_[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float smoothGainCoeff_ = 1.0f;
  float dampA_ = 0.0f; // tank-line damping coefficient
  float sendHpA_ = 0.0f; // send highpass coefficient
  float modPhaseInc_[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float modDepthSamp_ = 0.0f; // wobble excursion, in samples
  float crossC_ = 1.0f; // cos(theta): tank self-circulation
  float crossS_ = 0.0f; // sin(theta): tank cross-circulation

  std::vector<float> pre_; // fixed predelay line
  size_t preLen_ = 0;
  size_t prePos_ = 0;
  std::vector<float> line_[kNumLines]; // circular tank lines
  size_t lineLen_[kNumLines] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t linePos_[kNumLines] = {0, 0, 0, 0, 0, 0, 0, 0};
  float dampState_[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float modPhase_[kNumLines] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> diff_[kNumDiff]; // input-diffusion allpass lines
  size_t diffLen_[kNumDiff] = {0, 0, 0, 0};
  size_t diffPos_[kNumDiff] = {0, 0, 0, 0};
  float sendLp_ = 0.0f; // send highpass (via lowpass subtract)
};
} // namespace tdm

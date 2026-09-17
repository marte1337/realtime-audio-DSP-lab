#pragma once

// SpaceProcessor: owns Delay + Reverb, mono in, stereo out.
//
// Routing (series, the classic lead sound):
//   dry -> Delay -> wetDly(L/R)
//   send = dry + delayMix * (wetDlyL + wetDlyR)/2 -> Reverb -> wetVerb(L/R)
//   outL = dry + delayMix * wetDlyL + reverbMix * wetVerbL   (same for R)
//
// - Series, not parallel: the reverb hears the echoes, so repeats sit
//   inside the room instead of floating over it. With delay off the send
//   is pure dry; with reverb off the echoes stay dry.
// - Mixes are ADDITIVE (dry is never scaled): enabling Space, at any mix,
//   leaves the dry transient bit-identical. Mix 0 on a unit is equivalent
//   to that unit off. Both units off writes dry to L and R bit-exactly.
// - Mix gains are smoothed here (~10 ms, the only moving coefficients in
//   this class); Delay/Reverb smooth their own internals. No coefficient
//   is ever shared unsmoothed between stages.
// - Inherent predelay: 8 ms fixed predelay plus the ~30 ms shortest tank
//   line, so the first ~38 ms after any transient are pure dry even with
//   reverb fully on.
//
// Delay and Reverb stay independently usable (own headers, own tests);
// this class only owns routing, mixes, and scratch.
//
// reset() takes the block size (NamStage precedent): three scratch lines
// are allocated here (off-RT). processBlock() allocates nothing, locks
// nothing, does no IO. Deterministic for fixed params + input.

#include "dsp/Space/Delay.h"
#include "dsp/Space/Reverb.h"

namespace tdm
{
class SpaceProcessor
{
public:
  static constexpr float kMinMix = 0.0f; // dry (unit contributes nothing)
  static constexpr float kMaxMix = 1.0f; // full wet add
  static constexpr float kDefaultDelayMix = 0.25f;
  static constexpr float kDefaultReverbMix = 0.20f;

  static constexpr float kSmoothMixMs = 10.0f; // mix smoothing time

  SpaceProcessor();

  // Off-RT: validate, reset both units, snap mixes (documented
  // deterministic state). Stateless itself apart from smoothed mixes, so
  // no scratch and no block-size parameter (unlike NamStage).
  void reset(double sampleRate);

  // Off-RT setters. Delay/Reverb params delegate (clamped there);
  // enables and mixes are clamped here.
  void setDelayEnabled(bool enabled);
  void setDelayTimeMs(float ms);
  void setDelayFeedback(float v);
  void setDelayMix(float v);
  void setReverbEnabled(bool enabled);
  void setReverbDecay(float v);
  void setReverbMix(float v);

  bool isDelayEnabled() const { return delay_.isEnabled(); }
  float delayTimeMs() const { return delay_.timeMs(); }
  float delayFeedback() const { return delay_.feedback(); }
  float delayMix() const { return delayMixTarget_; }
  bool isReverbEnabled() const { return reverb_.isEnabled(); }
  float reverbDecay() const { return reverb_.decay(); }
  float reverbMix() const { return reverbMixTarget_; }
  // True when both units are off (wet is silent, L/R carry dry exactly).
  bool isBypassed() const { return !delay_.isEnabled() && !reverb_.isEnabled(); }

  // Mono in, stereo out. outL/outR must be distinct writable buffers of
  // numFrames (aliasing the input is safe: reads precede writes per
  // sample, but distinct buffers are the contract).
  void processBlock(const float* input, float* outL, float* outR, int numFrames);

private:
  Delay delay_;
  Reverb reverb_;

  double sampleRate_ = 0.0;
  float delayMix_ = kDefaultDelayMix;
  float reverbMix_ = kDefaultReverbMix;
  float delayMixTarget_ = kDefaultDelayMix;
  float reverbMixTarget_ = kDefaultReverbMix;
  float smoothMixCoeff_ = 1.0f;
};
} // namespace tdm

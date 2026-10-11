#pragma once

// AlienDoppler: compact constant-ratio pitch shifter (LAB, dsp/lab/Alien).
// Classic dual-tap Doppler harmonizer in the spirit of the rejected
// PitchShifter v1 stash (reimplemented small, no pitch tracking): output
// is input[t - d(t)] with tap delay ramping at |1 - ratio| samples per
// sample (ratio = 2^(st/12)); rising delay = pitch down, falling = up.
// Two taps run the ramp staggered by half a saw period; each tap sinks to
// zero weight over a short raised-cosine fade around its wrap (masked
// skip, click-free by construction) while the partner sustains, and the
// pair is sum-normalized (convex combination: unity gain, wet peak never
// above input peak). No pitch-synchronous stagger (unlike the stash),
// which costs a systematic phase slip per wrap: measured ratio accuracy
// is ~+/-0.2 st at +/-3..7 st but sags to about -1 st at -12 st (and
// +0.5 st at +12 st). Each wrap permanently advances/retards the summed
// phase by the inter-tap phase gap; pitch-sync stagger (forcing
// integer-cycle alignment) is the known fix if hardware wants tighter
// intervals. Mild combing during the brief overlap adds chorus-y
// character - acceptable for alien warp duty, documented honestly.
//
// Built FOR Warp Delay pitch mode (in-loop use): small footprint, ~35 ms
// latency per pass, deterministic. Direct use is possible but the stash
// prototype already proved this topology unsuitable for transparent
// transpose; musical intervals inside a feedback loop are the target.
//
// Params: semitones (-12..+12, takes effect click-free at next wrap...
// actually immediately: slope change only bends the ramp, no jumps).
// Unity (0 st target) crossfades to bit-exact dry over ~5 ms (the wet
// path is latent, so an instant switch would skip). reset(sr) off-RT;
// process() RT-safe, deterministic, finite-out.

#include "dsp/lab/Alien/AlienDsp.h"

namespace tdm
{
namespace alien
{

class AlienDoppler
{
public:
  static constexpr float kMinSt = -12.0f;
  static constexpr float kMaxSt = 12.0f;
  static constexpr float kDminMs = 5.0f;
  static constexpr float kDmaxMs = 35.0f;
  static constexpr float kFadeFrac = 0.12f; // wrap fade span (fraction of saw)

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    line_.reset(sr, kDmaxMs + 20.0f);
    phaseA_ = 0.0f;
    phaseB_ = 0.5f;
    st_.reset(sr, 30.0f, stTgt_);
    st_.snap(stTgt_);
    wetMix_ = (stTgt_ == 0.0f) ? 0.0f : 1.0f;
    wetStep_ = 1.0f / static_cast<float>(0.005 * sr); // 5 ms fade
  }

  void setSemitones(float st)
  {
    stTgt_ = (st < kMinSt) ? kMinSt : ((st > kMaxSt) ? kMaxSt : st);
    st_.setTarget(stTgt_);
  }
  float semitones() const { return stTgt_; }
  // RT-safe state clear (no allocation). Call at a dip bottom.
  void clear()
  {
    line_.clear();
    phaseA_ = 0.0f;
    phaseB_ = 0.5f;
    st_.snap(stTgt_);
    wetMix_ = (stTgt_ == 0.0f) ? 0.0f : 1.0f;
  }
  int latencySamples() const { return static_cast<int>(kDmaxMs * 0.001f * sr_); }

  float process(float x)
  {
    line_.write(x); // stay primed even while bypassed
    const float st = st_.next();
    const float wantWet = (stTgt_ == 0.0f) ? 0.0f : 1.0f;
    if (wetMix_ < wantWet)
      wetMix_ = (wetMix_ + wetStep_ > wantWet) ? wantWet : wetMix_ + wetStep_;
    else if (wetMix_ > wantWet)
      wetMix_ = (wetMix_ - wetStep_ < wantWet) ? wantWet : wetMix_ - wetStep_;
    if (wetMix_ == 0.0f)
      return x; // settled unity: bit-exact dry
    const float ratio = std::pow(2.0f, st / 12.0f);
    const float rangeSamp = (kDmaxMs - kDminMs) * 0.001f * static_cast<float>(sr_);
    const float dPhase = std::fabs(1.0f - ratio) / rangeSamp;
    const float dmin = kDminMs * 0.001f * static_cast<float>(sr_);
    const float dmax = kDmaxMs * 0.001f * static_cast<float>(sr_);
    const bool down = (ratio < 1.0f);

    phaseA_ += dPhase;
    if (phaseA_ >= 1.0f)
      phaseA_ -= 1.0f;
    phaseB_ += dPhase;
    if (phaseB_ >= 1.0f)
      phaseB_ -= 1.0f;

    const float dA = down ? (dmin + phaseA_ * rangeSamp) : (dmax - phaseA_ * rangeSamp);
    const float dB = down ? (dmin + phaseB_ * rangeSamp) : (dmax - phaseB_ * rangeSamp);
    const float wA = tapWeight(phaseA_);
    const float wB = tapWeight(phaseB_);
    const float a = line_.readAt(dA);
    const float b = line_.readAt(dB);
    const float wet = (wA * a + wB * b) / (wA + wB + 1e-9f);
    const float y = x + wetMix_ * (alienSnap(wet) - x);
    return std::isfinite(y) ? y : x;
  }

  void processBlock(const float* input, float* output, int n)
  {
    for (int k = 0; k < n; ++k)
      output[k] = process(input[k]);
  }

private:
  // Raised-cosine dip to 0 around the wrap point (p=1->0), 1 elsewhere.
  static float tapWeight(float p)
  {
    const float f = kFadeFrac;
    if (p > 1.0f - f)
    {
      const float u = (p - (1.0f - f)) / f; // 0..1 into the wrap
      return 0.5f + 0.5f * std::cos(3.141592653589793f * u);
    }
    if (p < f * 0.5f)
    {
      const float u = p / (f * 0.5f); // 0..1 out of the wrap
      return 0.5f - 0.5f * std::cos(3.141592653589793f * u);
    }
    return 1.0f;
  }

  double sr_ = 48000.0;
  FracDelay line_;
  SmoothParam st_;
  float stTgt_ = 0.0f;
  float phaseA_ = 0.0f, phaseB_ = 0.5f;
  float wetMix_ = 0.0f, wetStep_ = 1.0f;
};

} // namespace alien
} // namespace tdm

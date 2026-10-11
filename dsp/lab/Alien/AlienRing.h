#pragma once

// AlienRing: double-sideband ring modulator (LAB, dsp/lab/Alien). Output
// is input x carrier: every partial f sprouts sum AND difference tones
// (f+fc, |f-fc|). Compare with AlienShift (single sideband): ring mod
// keeps both sidebands, so it sounds denser/clangier at the same carrier
// and collapses to AM tremolo at low carriers. Deliberately minimal:
// carrier frequency + mix. reset(sr) off-RT; process() RT-safe,
// deterministic, finite-out.

#include "dsp/lab/Alien/AlienDsp.h"

namespace tdm
{
namespace alien
{

class AlienRing
{
public:
  static constexpr float kMinCarrierHz = 5.0f;
  static constexpr float kMaxCarrierHz = 2000.0f;

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    osc_.reset(sr, carrierTgt_);
    mix_.reset(sr, 10.0f, mixTgt_);
    mix_.snap(mixTgt_);
  }

  void setCarrierHz(float hz)
  {
    carrierTgt_ =
        (hz < kMinCarrierHz) ? kMinCarrierHz : ((hz > kMaxCarrierHz) ? kMaxCarrierHz : hz);
    osc_.setFreq(carrierTgt_);
  }
  void setMix(float v)
  {
    mixTgt_ = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    mix_.setTarget(mixTgt_);
  }

  float process(float x)
  {
    float s, c;
    osc_.next(s, c);
    (void)c;
    const float wet = x * s;
    const float mx = mix_.next();
    const float y = x + mx * (wet - x);
    return std::isfinite(y) ? y : x;
  }

  void processBlock(const float* input, float* output, int n)
  {
    for (int k = 0; k < n; ++k)
      output[k] = process(input[k]);
  }

private:
  QuadOsc osc_;
  SmoothParam mix_;
  float carrierTgt_ = 150.0f, mixTgt_ = 1.0f;
};

} // namespace alien
} // namespace tdm

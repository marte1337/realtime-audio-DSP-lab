// SLAM lab candidates A-D. See SlamCandidates.h + .research/slam-study/notes.md.

#include "dsp/lab/Slam/SlamCandidates.h"

#include <algorithm>
#include <cmath>

namespace tdm
{
namespace lab
{
namespace
{
float clampF(float v, float lo, float hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

// --- A. SlamPre: HPF guard -> LP band -> fast comp -> soft sat -> DC block ---
void SlamPre::reset(double sampleRate)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  dcGuard_.reset(sr_, 22.0f);
  band_.reset();
  band_.setLowpass(sr_, bandHz_, 0.7f);
  comp_.reset(sr_, -28.0f, 8.0f, 1.5f, 130.0f, 2.2f);
  dcBlock_.reset(sr_, 12.0f);
}
void SlamPre::setBandHz(float hz)
{
  bandHz_ = clampF(hz, kMinBandHz, kMaxBandHz);
  band_.setLowpass(sr_, bandHz_, 0.7f);
}
void SlamPre::setAmount(float v)
{
  amount_ = clampF(v, kMinAmount, kMaxAmount);
}
void SlamPre::setEnabled(bool enabled)
{
  enabled_ = enabled;
}
void SlamPre::processBlock(const float* input, float* output, int numFrames)
{
  if (!enabled_ || numFrames <= 0)
  {
    if (input != output && numFrames > 0)
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float dry = input[i];
    float b = dcGuard_.process(dry);
    b = band_.process(b);
    b = comp_.process(b);
    b = slamSoftSat(b, 2.2f);
    b = dcBlock_.process(b);
    float y = dry + amount_ * b;
    if (!std::isfinite(y))
      y = dry; // finite-safe: branch can never poison the main path
    output[i] = y;
  }
}

// --- B. SlamPostNam: LP band -> hard comp -> ASYMMETRIC sat -> DC block ---
void SlamPostNam::reset(double sampleRate)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  dcGuard_.reset(sr_, 25.0f);
  band_.reset();
  band_.setLowpass(sr_, bandHz_, 0.8f);
  comp_.reset(sr_, -24.0f, 10.0f, 1.0f, 90.0f, 2.0f);
  dcBlock_.reset(sr_, 14.0f);
}
void SlamPostNam::setBandHz(float hz)
{
  bandHz_ = clampF(hz, kMinBandHz, kMaxBandHz);
  band_.setLowpass(sr_, bandHz_, 0.8f);
}
void SlamPostNam::setAmount(float v)
{
  amount_ = clampF(v, kMinAmount, kMaxAmount);
}
void SlamPostNam::setEnabled(bool enabled)
{
  enabled_ = enabled;
}
void SlamPostNam::processBlock(const float* input, float* output, int numFrames)
{
  if (!enabled_ || numFrames <= 0)
  {
    if (input != output && numFrames > 0)
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float dry = input[i];
    float b = dcGuard_.process(dry);
    b = band_.process(b);
    b = comp_.process(b);
    b = slamAsymSat(b, 2.5f, 1.2f); // hot positive lobe: even-harmonic violence
    b = dcBlock_.process(b);        // asymmetric stage needs the blocker
    float y = dry + amount_ * b;
    if (!std::isfinite(y))
      y = dry;
    output[i] = y;
  }
}

// --- C. SlamPostIr: LP band -> gentle comp -> soft sat -> DC block ---
void SlamPostIr::reset(double sampleRate)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  dcGuard_.reset(sr_, 25.0f);
  band_.reset();
  band_.setLowpass(sr_, bandHz_, 0.7f);
  comp_.reset(sr_, -26.0f, 5.0f, 3.0f, 160.0f, 1.8f);
  dcBlock_.reset(sr_, 12.0f);
}
void SlamPostIr::setBandHz(float hz)
{
  bandHz_ = clampF(hz, kMinBandHz, kMaxBandHz);
  band_.setLowpass(sr_, bandHz_, 0.7f);
}
void SlamPostIr::setAmount(float v)
{
  amount_ = clampF(v, kMinAmount, kMaxAmount);
}
void SlamPostIr::setEnabled(bool enabled)
{
  enabled_ = enabled;
}
void SlamPostIr::processBlock(const float* input, float* output, int numFrames)
{
  if (!enabled_ || numFrames <= 0)
  {
    if (input != output && numFrames > 0)
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float dry = input[i];
    float b = dcGuard_.process(dry);
    b = band_.process(b);
    b = comp_.process(b);
    b = slamSoftSat(b, 1.8f);
    b = dcBlock_.process(b);
    float y = dry + amount_ * b;
    if (!std::isfinite(y))
      y = dry;
    output[i] = y;
  }
}

// --- D. SlamImpact: trigger -> burst + low-band bloom, else dry ---
void SlamImpact::reset(double sampleRate)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  trig_.reset(sr_, sens_, refrMs_);
  burst_.reset(sr_, f0_, f1_, decayMs_, 1.5f, 0.75f);
  bloomLp_.reset();
  bloomLp_.setLowpass(sr_, clampF(f0_ * 1.5f, 100.0f, 400.0f), 0.7f);
  bloomDecay_ = static_cast<float>(std::exp(-6.907755278982137 / (0.070 * sr_))); // -60 dB / 70 ms
  bloom_ = 0.0f;
}
void SlamImpact::setBurstHz(float f0Hz, float f1Hz)
{
  f0_ = clampF(f0Hz, 30.0f, 250.0f);
  f1_ = clampF(f1Hz, 30.0f, 250.0f);
  burst_.reset(sr_, f0_, f1_, decayMs_, 1.5f, 0.75f);
  bloomLp_.setLowpass(sr_, clampF(f0_ * 1.5f, 100.0f, 400.0f), 0.7f);
}
void SlamImpact::setDecayMs(float ms)
{
  decayMs_ = clampF(ms, 40.0f, 400.0f);
  burst_.reset(sr_, f0_, f1_, decayMs_, 1.5f, 0.75f);
}
void SlamImpact::setRefractoryMs(float ms)
{
  refrMs_ = clampF(ms, 40.0f, 400.0f);
  trig_.reset(sr_, sens_, refrMs_);
}
void SlamImpact::setSensitivity(float v)
{
  sens_ = clampF(v, kMinSens, kMaxSens);
  trig_.reset(sr_, sens_, refrMs_);
}
void SlamImpact::setAmount(float v)
{
  amount_ = clampF(v, kMinAmount, kMaxAmount);
}
void SlamImpact::setEnabled(bool enabled)
{
  enabled_ = enabled;
}
void SlamImpact::setScheduled(bool scheduled)
{
  trig_.setScheduled(scheduled);
}
void SlamImpact::scheduleFire(int64_t samplePos, float strength)
{
  trig_.scheduleFire(samplePos, strength);
}
void SlamImpact::processBlock(const float* input, float* output, int numFrames)
{
  if (!enabled_ || numFrames <= 0)
  {
    if (input != output && numFrames > 0)
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    if (!enabled_)
      trig_.advance(numFrames);
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float dry = input[i];
    const float s = trig_.feed(dry);
    if (s > 0.0f)
    {
      burst_.trigger(s);
      bloom_ = 2.0f * s; // brief low-band lift of the dry hit itself
    }
    const float low = bloomLp_.process(dry);
    const float burst = burst_.process();
    float y = dry + amount_ * (burst + bloom_ * low);
    if (!std::isfinite(y))
      y = dry;
    output[i] = y;
    bloom_ *= bloomDecay_;
    if (bloom_ < 1e-5f)
      bloom_ = 0.0f;
  }
}

// --- E. SlamGated: trigger opens a decaying blend over a C-style branch ---
void SlamGated::reset(double sampleRate)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  trig_.reset(sr_, sens_, refrMs_);
  dcGuard_.reset(sr_, 25.0f);
  band_.reset();
  band_.setLowpass(sr_, bandHz_, 0.7f);
  comp_.reset(sr_, -26.0f, 5.0f, 3.0f, 160.0f, 1.8f);
  dcBlock_.reset(sr_, 12.0f);
  envDecay_ = static_cast<float>(std::exp(-6.907755278982137 / (gateDecayMs_ * 0.001 * sr_)));
  env_ = 0.0f;
}
void SlamGated::setBandHz(float hz)
{
  bandHz_ = clampF(hz, kMinBandHz, kMaxBandHz);
  band_.setLowpass(sr_, bandHz_, 0.7f);
}
void SlamGated::setBase(float v)
{
  base_ = clampF(v, 0.0f, 1.0f);
}
void SlamGated::setPeak(float v)
{
  peak_ = clampF(v, 0.0f, 2.0f);
}
void SlamGated::setGateDecayMs(float ms)
{
  gateDecayMs_ = clampF(ms, 40.0f, 400.0f);
  envDecay_ = static_cast<float>(std::exp(-6.907755278982137 / (gateDecayMs_ * 0.001 * sr_)));
}
void SlamGated::setSensitivity(float v)
{
  sens_ = clampF(v, 0.0f, 1.0f);
  trig_.reset(sr_, sens_, refrMs_);
}
void SlamGated::setRefractoryMs(float ms)
{
  refrMs_ = clampF(ms, 40.0f, 400.0f);
  trig_.reset(sr_, sens_, refrMs_);
}
void SlamGated::setEnabled(bool enabled)
{
  enabled_ = enabled;
}
void SlamGated::setScheduled(bool scheduled)
{
  trig_.setScheduled(scheduled);
}
void SlamGated::scheduleFire(int64_t samplePos, float strength)
{
  trig_.scheduleFire(samplePos, strength);
}
void SlamGated::processBlock(const float* input, float* output, int numFrames)
{
  if (!enabled_ || numFrames <= 0)
  {
    if (input != output && numFrames > 0)
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    if (!enabled_)
      trig_.advance(numFrames);
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float dry = input[i];
    const float s = trig_.feed(dry);
    if (s > 0.0f && s > env_)
      env_ = s; // fire opens the gate (retrigger takes the max)
    float b = dcGuard_.process(dry);
    b = band_.process(b);
    b = comp_.process(b);
    b = slamSoftSat(b, 1.8f);
    b = dcBlock_.process(b);
    float y = dry + (base_ + peak_ * env_) * b;
    if (!std::isfinite(y))
      y = dry;
    output[i] = y;
    env_ *= envDecay_;
    if (env_ < 1e-4f)
      env_ = 0.0f;
  }
}

} // namespace lab
} // namespace tdm

#include "dsp/Space/SpaceProcessor.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
float onePoleCoeff(double tauSec, double sampleRate)
{
  const double c = 1.0 - std::exp(-1.0 / (tauSec * sampleRate));
  return static_cast<float>(c < 1.0 ? c : 1.0);
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

SpaceProcessor::SpaceProcessor() = default;

void SpaceProcessor::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Space: sample rate must be positive");
  sampleRate_ = sampleRate;
  delay_.reset(sampleRate);
  reverb_.reset(sampleRate);
  smoothMixCoeff_ = onePoleCoeff(kSmoothMixMs / 1000.0, sampleRate);
  // Documented deterministic state: mixes snapped, units fresh.
  delayMix_ = delayMixTarget_;
  reverbMix_ = reverbMixTarget_;
}

void SpaceProcessor::setDelayEnabled(bool enabled)
{
  delay_.setEnabled(enabled);
}

void SpaceProcessor::setDelayTimeMs(float ms)
{
  delay_.setTimeMs(ms);
}

void SpaceProcessor::setDelayFeedback(float v)
{
  delay_.setFeedback(v);
}

void SpaceProcessor::setDelayMix(float v)
{
  delayMixTarget_ = clampf(v, kMinMix, kMaxMix);
}

void SpaceProcessor::setReverbEnabled(bool enabled)
{
  reverb_.setEnabled(enabled);
}

void SpaceProcessor::setReverbDecay(float v)
{
  reverb_.setDecay(v);
}

void SpaceProcessor::setReverbMix(float v)
{
  reverbMixTarget_ = clampf(v, kMinMix, kMaxMix);
}

void SpaceProcessor::processBlock(const float* input, float* outL, float* outR, int numFrames)
{
  assert(input != nullptr && outL != nullptr && outR != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (sampleRate_ <= 0.0)
  {
    // Never reset: no coefficients; pass dry to both sides (house
    // passthrough idiom; buffers are distinct by contract).
    if (outL != input)
      std::memcpy(outL, input, sizeof(float) * static_cast<size_t>(numFrames));
    std::memcpy(outR, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    delayMix_ += (delayMixTarget_ - delayMix_) * smoothMixCoeff_;
    reverbMix_ += (reverbMixTarget_ - reverbMix_) * smoothMixCoeff_;
    const float x = input[i];
    float dwL = 0.0f, dwR = 0.0f;
    delay_.processSample(x, dwL, dwR);
    // Series routing: the room hears the echoes at their audible level.
    const float send = x + delayMix_ * (dwL + dwR) * 0.5f;
    float rwL = 0.0f, rwR = 0.0f;
    reverb_.processSample(send, rwL, rwR);
    // Additive mixes: dry is never scaled, only added to.
    outL[i] = x + delayMix_ * dwL + reverbMix_ * rwL;
    outR[i] = x + delayMix_ * dwR + reverbMix_ * rwR;
  }
}
} // namespace tdm

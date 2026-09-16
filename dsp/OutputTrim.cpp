#include "dsp/OutputTrim.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
float dbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

OutputTrim::OutputTrim()
{
  refreshDerived();
}

void OutputTrim::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("OutputTrim: sample rate must be positive");
  sampleRate_ = sampleRate;
  refreshDerived();
  // Documented deterministic state: applied gain snaps exactly to target,
  // so a fresh reset at 0 dB passes input bit-exactly from the first sample
  // instead of fading in from some other value.
  gain_ = targetGain_;
}

void OutputTrim::setTrimDb(float db)
{
  trimDb_ = clampf(db, kMinTrimDb, kMaxTrimDb);
  refreshDerived();
}

void OutputTrim::refreshDerived()
{
  targetGain_ = dbToLinear(trimDb_);
  if (sampleRate_ > 0.0)
  {
    const double c = 1.0 - std::exp(-1.0 / ((kSmoothMs / 1000.0) * sampleRate_));
    smoothCoeff_ = static_cast<float>(c < 1.0 ? c : 1.0);
  }
}

void OutputTrim::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (sampleRate_ <= 0.0)
  {
    // Never reset: no coefficients; pass through rather than guess.
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    gain_ += (targetGain_ - gain_) * smoothCoeff_;
    // No clamp: over-unity results are preserved for downstream handling.
    output[i] = input[i] * gain_;
  }
}
} // namespace tdm

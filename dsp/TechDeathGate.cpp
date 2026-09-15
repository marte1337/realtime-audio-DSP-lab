#include "dsp/TechDeathGate.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
// One-pole smoothing coefficient for time constant tauSec at sampleRate:
// y += (target - y) * coeff converges ~63% per tauSec.
float onePoleCoeff(double tauSec, double sampleRate)
{
  const double c = 1.0 - std::exp(-1.0 / (tauSec * sampleRate));
  return static_cast<float>(c < 1.0 ? c : 1.0);
}

float dbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

TechDeathGate::TechDeathGate()
{
  refreshDerived();
}

void TechDeathGate::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Gate: sample rate must be positive");
  sampleRate_ = sampleRate;
  refreshDerived();
  // Documented safe state: closed. First pick attack reopens within ~1 ms;
  // no sub-threshold hiss burst can pass after a reset/sample-rate change.
  envelope_ = 0.0f;
  gain_ = floorGain_;
  holdLeft_ = 0;
  open_ = false;
}

void TechDeathGate::setThresholdDb(float db)
{
  thresholdDb_ = clampf(db, kMinThresholdDb, kMaxThresholdDb);
  refreshDerived();
}

void TechDeathGate::setReleaseMs(float ms)
{
  releaseMs_ = clampf(ms, kMinReleaseMs, kMaxReleaseMs);
  refreshDerived();
}

void TechDeathGate::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void TechDeathGate::refreshDerived()
{
  openLevel_ = dbToLinear(thresholdDb_);
  closeLevel_ = dbToLinear(thresholdDb_ - kHysteresisDb);
  floorGain_ = dbToLinear(kFloorDb);
  if (sampleRate_ > 0.0)
  {
    detDecay_ = static_cast<float>(std::exp(-1.0 / ((kDetectorReleaseMs / 1000.0) * sampleRate_)));
    attackCoeff_ = onePoleCoeff(kGainAttackMs / 1000.0, sampleRate_);
    // Release is defined as the 60 dB fall time: gain ~ e^(-t/tau) reaches
    // -60 dB at t = tau * ln(1000).
    const double tauSec = (releaseMs_ / 1000.0) / std::log(1000.0);
    releaseCoeff_ = onePoleCoeff(tauSec, sampleRate_);
    holdSamples_ = static_cast<int>((kHoldMs / 1000.0) * sampleRate_ + 0.5);
  }
}

void TechDeathGate::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!enabled_ || sampleRate_ <= 0.0)
  {
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float x = input[i];
    // Peak detector: instantaneous attack, exponential release.
    const float ax = std::fabs(x);
    if (ax > envelope_)
      envelope_ = ax;
    else
    {
      envelope_ *= detDecay_;
      // Denormal guard: snapped long-silence envelope is exactly 0, which
      // keeps the comparator closed and the FPU out of denormal range.
      if (envelope_ < 1e-12f)
        envelope_ = 0.0f;
    }
    // Hysteresis comparator + hold. In the band between close and open the
    // state freezes, so threshold hover cannot chatter.
    if (envelope_ > openLevel_)
    {
      open_ = true;
      holdLeft_ = holdSamples_;
    }
    else if (envelope_ < closeLevel_)
    {
      if (holdLeft_ > 0)
        --holdLeft_;
      else
        open_ = false;
    }
    // Smoothed gain: fast ramp up, Release ramp down, hard-clamped so the
    // [floor, 1] invariant holds exactly (no drift, no overshoot).
    const float target = open_ ? 1.0f : floorGain_;
    gain_ += (target - gain_) * (target > gain_ ? attackCoeff_ : releaseCoeff_);
    if (gain_ > 1.0f)
      gain_ = 1.0f;
    else if (gain_ < floorGain_)
      gain_ = floorGain_;
    output[i] = x * gain_;
  }
}
} // namespace tdm

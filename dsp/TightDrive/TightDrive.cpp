#include "dsp/TightDrive/TightDrive.h"

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

// One-pole highpass coefficient a for cutoff fcHz (analog-RC matched):
// y[n] = a * (y[n-1] + x[n] - x[n-1]).
float hpfCoeff(double fcHz, double sampleRate)
{
  return static_cast<float>(std::exp(-2.0 * 3.141592653589793 * fcHz / sampleRate));
}

// One-pole lowpass coefficient a for cutoff fcHz: y[n] += (1-a)*(x[n]-y[n]).
float lpfCoeff(double fcHz, double sampleRate)
{
  return static_cast<float>(std::exp(-2.0 * 3.141592653589793 * fcHz / sampleRate));
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

float snapZero(float v)
{
  return (v < 1e-12f && v > -1e-12f) ? 0.0f : v;
}
} // namespace

TightDrive::TightDrive()
{
  refreshDerived();
}

void TightDrive::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("TightDrive: sample rate must be positive");
  sampleRate_ = sampleRate;
  refreshDerived();
  // Documented deterministic state: filters cleared, smoothed values
  // snapped to targets, so the first sample already reflects the settings.
  hpfY_ = 0.0f;
  hpfXPrev_ = 0.0f;
  shelfHpY_ = 0.0f;
  shelfHpXPrev_ = 0.0f;
  lpfY_ = 0.0f;
  hpfA_ = hpfTarget_;
  preGain_ = preTarget_;
  shelfK_ = shelfTarget_;
}

void TightDrive::setTight(float v)
{
  tight_ = clampf(v, kMinTight, kMaxTight);
  refreshDerived();
}

void TightDrive::setDrive(float v)
{
  drive_ = clampf(v, kMinDrive, kMaxDrive);
  refreshDerived();
}

void TightDrive::setBite(float v)
{
  bite_ = clampf(v, kMinBite, kMaxBite);
  refreshDerived();
}

void TightDrive::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void TightDrive::refreshDerived()
{
  // Tight: exponential cutoff map so each tenth of the knob feels even.
  const double fc = kHpfMinHz * std::pow(kHpfMaxHz / kHpfMinHz, tight_);
  preTarget_ = kPreGainMin + (kPreGainMax - kPreGainMin) * drive_;
  // Bite: parallel-shelf mix k with unity at neutral; y = x + k*HP(x).
  const float shelfDb = kShelfMinDb + (kShelfMaxDb - kShelfMinDb) * bite_;
  shelfTarget_ = std::pow(10.0f, shelfDb / 20.0f) - 1.0f;
  if (sampleRate_ > 0.0)
  {
    hpfTarget_ = hpfCoeff(fc, sampleRate_);
    shelfHpA_ = hpfCoeff(kShelfHz, sampleRate_);
    lpfA_ = lpfCoeff(kPostLpfHz, sampleRate_);
    smoothCoeff_ = onePoleCoeff(kSmoothMs / 1000.0, sampleRate_);
  }
}

void TightDrive::processBlock(const float* input, float* output, int numFrames)
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
    // Smoothed parameters: moves bend, never step (zipper-free).
    hpfA_ += (hpfTarget_ - hpfA_) * smoothCoeff_;
    preGain_ += (preTarget_ - preGain_) * smoothCoeff_;
    shelfK_ += (shelfTarget_ - shelfK_) * smoothCoeff_;

    const float x = input[i];
    // Pre-drive highpass (Tight).
    hpfY_ = snapZero(hpfA_ * (hpfY_ + x - hpfXPrev_));
    hpfXPrev_ = x;
    // Saturation (Drive). tanh: smooth, odd-symmetric, bounded.
    const float clipped = std::tanh(preGain_ * hpfY_);
    // Presence shelf (Bite), parallel form: y = x + k*HP(x).
    shelfHpY_ = snapZero(shelfHpA_ * (shelfHpY_ + clipped - shelfHpXPrev_));
    shelfHpXPrev_ = clipped;
    const float present = clipped + shelfK_ * shelfHpY_;
    // Fixed anti-fizz lowpass.
    lpfY_ = snapZero(lpfY_ + (1.0f - lpfA_) * (present - lpfY_));
    output[i] = lpfY_;
  }
}
} // namespace tdm

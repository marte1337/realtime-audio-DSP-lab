#include "dsp/Space/Delay.h"

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

float snapZero(float v)
{
  return (v < 1e-12f && v > -1e-12f) ? 0.0f : v;
}
} // namespace

Delay::Delay()
{
  refreshTargets();
}

void Delay::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Delay: sample rate must be positive");
  sampleRate_ = sampleRate;
  len_ = static_cast<size_t>(kMaxTimeMs / 1000.0 * sampleRate) + 8; // interp margin
  lineL_.assign(len_, 0.0f);
  lineR_.assign(len_, 0.0f);
  posL_ = 0;
  posR_ = 0;
  dampL_ = 0.0f;
  dampR_ = 0.0f;
  dampA_ = static_cast<float>(std::exp(-2.0 * 3.141592653589793 * kDampHz / sampleRate));
  smoothTimeCoeff_ = onePoleCoeff(kSmoothTimeMs / 1000.0, sampleRate);
  smoothGainCoeff_ = onePoleCoeff(kSmoothGainMs / 1000.0, sampleRate);
  refreshTargets();
  // Documented deterministic state: delay length and feedback snapped to
  // targets, lines cleared, so the first echo lands exactly on Time.
  delaySamp_ = delayTarget_;
  fb_ = fbTarget_;
}

void Delay::setTimeMs(float ms)
{
  timeMs_ = clampf(ms, kMinTimeMs, kMaxTimeMs);
  refreshTargets();
}

void Delay::setFeedback(float v)
{
  feedback_ = clampf(v, kMinFeedback, kMaxFeedback);
  refreshTargets();
}

void Delay::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void Delay::refreshTargets()
{
  fbTarget_ = feedback_;
  if (sampleRate_ > 0.0 && len_ > 0)
  {
    const float want = timeMs_ / 1000.0f * static_cast<float>(sampleRate_);
    const float lo = kMinTimeMs / 1000.0f * static_cast<float>(sampleRate_);
    const float hi = static_cast<float>(len_ - 2);
    delayTarget_ = clampf(want, lo < 1.0f ? 1.0f : lo, hi);
  }
}

float Delay::readInterp(const std::vector<float>& line, size_t pos) const
{
  // delaySamp_ < len_ - 1 by construction, so a single wrap add suffices.
  float r = static_cast<float>(pos) - delaySamp_;
  if (r < 0.0f)
    r += static_cast<float>(len_);
  const int i0 = static_cast<int>(r);
  const float frac = r - static_cast<float>(i0);
  const size_t j0 = static_cast<size_t>(i0);
  const size_t j1 = (j0 + 1 == len_) ? 0 : j0 + 1;
  return line[j0] * (1.0f - frac) + line[j1] * frac;
}

void Delay::processSample(float x, float& wetL, float& wetR)
{
  if (!enabled_ || sampleRate_ <= 0.0 || len_ == 0)
  {
    wetL = 0.0f;
    wetR = 0.0f;
    return;
  }
  delaySamp_ += (delayTarget_ - delaySamp_) * smoothTimeCoeff_;
  fb_ += (fbTarget_ - fb_) * smoothGainCoeff_;
  const float tapL = readInterp(lineL_, posL_);
  const float tapR = readInterp(lineR_, posR_);
  dampL_ = snapZero(dampL_ + (1.0f - dampA_) * (tapL - dampL_));
  dampR_ = snapZero(dampR_ + (1.0f - dampA_) * (tapR - dampR_));
  // Pair-equalized ping-pong: dry enters the L line only; the L->R
  // seed is unity (gated near fb=0) while the R->L return carries the
  // pair decay fb^2. Repeats therefore arrive in equal-gain L/R pairs
  // (1,1,F,F,...), so cumulative wet energy balances instead of leaning
  // left; strict alternation and Time meaning are unchanged.
  const float pairGain = fb_ * fb_;
  const float seedGate = fb_ < kSeedGateKnee ? fb_ * (1.0f / kSeedGateKnee) : 1.0f;
  lineL_[posL_] = snapZero(x + pairGain * dampR_);
  lineR_[posR_] = snapZero(seedGate * dampL_);
  wetL = tapL;
  wetR = tapR;
  posL_ = (posL_ + 1 == len_) ? 0 : posL_ + 1;
  posR_ = (posR_ + 1 == len_) ? 0 : posR_ + 1;
}

void Delay::processBlock(const float* input, float* outL, float* outR, int numFrames)
{
  assert(input != nullptr && outL != nullptr && outR != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!enabled_ || sampleRate_ <= 0.0 || len_ == 0)
  {
    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numFrames));
    std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
    processSample(input[i], outL[i], outR[i]);
}
} // namespace tdm

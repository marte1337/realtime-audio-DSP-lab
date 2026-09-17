#include "dsp/Space/Reverb.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
constexpr float kTwoPi = 6.283185307179586f;

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

// Scale a 48 kHz tuning length to the host rate (defensive floor: never
// hit in practice, keeps degenerate rates finite).
size_t scaledLength48k(int base48k, double sampleRate)
{
  const long long n = static_cast<long long>(base48k * sampleRate / 48000.0 + 0.5);
  return static_cast<size_t>(n < 16 ? 16 : n);
}
} // namespace

Reverb::Reverb()
{
  refreshTargets();
}

void Reverb::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Reverb: sample rate must be positive");
  sampleRate_ = sampleRate;
  preLen_ = scaledLength48k(static_cast<int>(kPreDelayMs / 1000.0f * 48000.0f + 0.5f), sampleRate);
  pre_.assign(preLen_, 0.0f);
  prePos_ = 0;
  for (int j = 0; j < kNumLines; ++j)
  {
    lineLen_[j] = scaledLength48k(kLineBase[j], sampleRate);
    line_[j].assign(lineLen_[j], 0.0f);
    linePos_[j] = 0;
    dampState_[j] = 0.0f;
    modPhase_[j] = kTwoPi * static_cast<float>(j) / static_cast<float>(kNumLines);
    modPhaseInc_[j] = kTwoPi * kModRateHz[j] / static_cast<float>(sampleRate);
  }
  modDepthSamp_ = kModDepthMs / 1000.0f * static_cast<float>(sampleRate);
  // Fixed slow rotation: full A<->B exchange over kCrossTimeSec, scaled
  // per rate so the circulation time is rate-independent.
  const float theta = (kTwoPi / 4.0f) / (kCrossTimeSec * static_cast<float>(sampleRate));
  crossC_ = std::cos(theta);
  crossS_ = std::sin(theta);
  for (int k = 0; k < kNumDiff; ++k)
  {
    diffLen_[k] = scaledLength48k(kDiffBase[k], sampleRate);
    diff_[k].assign(diffLen_[k], 0.0f);
    diffPos_[k] = 0;
  }
  sendLp_ = 0.0f;
  dampA_ = static_cast<float>(std::exp(-2.0 * 3.141592653589793 * kDampHz / sampleRate));
  sendHpA_ = static_cast<float>(std::exp(-2.0 * 3.141592653589793 * kSendHpHz / sampleRate));
  smoothGainCoeff_ = onePoleCoeff(kSmoothGainMs / 1000.0, sampleRate);
  refreshTargets();
  // Documented deterministic state: tank gains snapped to targets, every
  // line cleared, modulation phases parked, so a fresh unit starts truly
  // silent and two fresh units agree exactly.
  for (int j = 0; j < kNumLines; ++j)
    lineGain_[j] = lineTarget_[j];
}

void Reverb::setDecay(float v)
{
  decay_ = clampf(v, kMinDecay, kMaxDecay);
  refreshTargets();
}

void Reverb::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void Reverb::refreshTargets()
{
  // T60 mapping first (rate-independent); per-line gains need line lengths.
  const double t60 = kT60MinSec * std::pow(kT60MaxSec / kT60MinSec, decay_);
  for (int j = 0; j < kNumLines; ++j)
  {
    if (sampleRate_ > 0.0 && lineLen_[j] > 0)
    {
      // Feedback for -60 dB after t60 seconds through a line of L samples.
      const double g = std::pow(10.0, -3.0 * lineLen_[j] / (t60 * sampleRate_));
      lineTarget_[j] = static_cast<float>(g < 1.0 ? g : 0.999f);
    }
    else
    {
      lineTarget_[j] = 0.0f;
    }
  }
}

float Reverb::allpassStep(std::vector<float>& buf, size_t& pos, size_t len, float x)
{
  const float delayed = buf[pos];
  const float y = -kDiffGain * x + delayed;
  buf[pos] = snapZero(x + kDiffGain * y);
  pos = (pos + 1 == len) ? 0 : pos + 1;
  return y;
}

float Reverb::readLerp(const std::vector<float>& buf, size_t len, size_t writePos, float delay) const
{
  // delay <= len by construction (wobble only shortens), so one wrap add
  // suffices; the read is a convex combination (non-expansive: modulation
  // can never add energy to the loop).
  float r = static_cast<float>(writePos) - delay;
  if (r < 0.0f)
    r += static_cast<float>(len);
  const int i0 = static_cast<int>(r);
  const float frac = r - static_cast<float>(i0);
  const size_t j0 = static_cast<size_t>(i0);
  const size_t j1 = (j0 + 1 == len) ? 0 : j0 + 1;
  return buf[j0] * (1.0f - frac) + buf[j1] * frac;
}

void Reverb::processSample(float x, float& wetL, float& wetR)
{
  if (!enabled_ || sampleRate_ <= 0.0)
  {
    wetL = 0.0f;
    wetR = 0.0f;
    return;
  }
  // Fixed predelay: the wash onset stands off the pick attack.
  const float delayed = pre_[prePos_];
  pre_[prePos_] = snapZero(x);
  prePos_ = (prePos_ + 1 == preLen_) ? 0 : prePos_ + 1;
  // Send highpass (via lowpass subtract): mud never enters the tanks.
  sendLp_ = snapZero(sendLp_ + (1.0f - sendHpA_) * (delayed - sendLp_));
  const float send = delayed - sendLp_;
  // Input diffusion: four series allpasses densify the early response.
  float dif = send;
  for (int k = 0; k < kNumDiff; ++k)
    dif = allpassStep(diff_[k], diffPos_[k], diffLen_[k], dif);
  // Modulated tank taps + per-line damping states. One tap per line feeds
  // mixing and the stereo output alike.
  float tap[kNumLines];
  float meanA = 0.0f, meanB = 0.0f;
  for (int j = 0; j < kNumLines; ++j)
  {
    lineGain_[j] += (lineTarget_[j] - lineGain_[j]) * smoothGainCoeff_;
    modPhase_[j] += modPhaseInc_[j];
    if (modPhase_[j] >= kTwoPi)
      modPhase_[j] -= kTwoPi;
    const float wobble = modDepthSamp_ * (0.5f + 0.5f * std::sin(modPhase_[j]));
    tap[j] = readLerp(line_[j], lineLen_[j], linePos_[j],
                      static_cast<float>(lineLen_[j]) - wobble);
    dampState_[j] = snapZero(dampState_[j] + (1.0f - dampA_) * (tap[j] - dampState_[j]));
    if (j < kTankB)
      meanA += dampState_[j];
    else
      meanB += dampState_[j];
  }
  meanA *= 0.25f;
  meanB *= 0.25f;
  // Dual-tank mixing: per-tank Householder rows (dense within-tank
  // recirculation) rotated pairwise across tanks (slow cross-circulation
  // that preserves the A-vs-B difference instead of collapsing it).
  // Differential drive (+dif to A, -dif to B) plants the difference;
  // the rotation walks it around without a favored common mode.
  float outA = 0.0f, outB = 0.0f;
  for (int j = 0; j < kTankB; ++j)
  {
    const float rowA = 2.0f * meanA - dampState_[j]; // Householder-4 row
    const float rowB = 2.0f * meanB - dampState_[j + kTankB];
    const float backA = crossC_ * rowA + crossS_ * rowB; // rotation pair
    const float backB = crossC_ * rowB - crossS_ * rowA;
    line_[j][linePos_[j]] = snapZero(dif + lineGain_[j] * backA);
    line_[j + kTankB][linePos_[j + kTankB]] =
        snapZero(-dif + lineGain_[j + kTankB] * backB);
    linePos_[j] = (linePos_[j] + 1 == lineLen_[j]) ? 0 : linePos_[j] + 1;
    linePos_[j + kTankB] =
        (linePos_[j + kTankB] + 1 == lineLen_[j + kTankB]) ? 0 : linePos_[j + kTankB] + 1;
    outA += tap[j];
    outB += tap[j + kTankB];
  }
  // Stereo: own-tank sum plus a small NEGATIVE cross-tank bleed. The two
  // tank sums are mutually uncorrelated (all-distinct line delays), so the
  // bleed sign sets the field: negative leans the outputs differential
  // (wide, S/M ~2) while the surviving common content keeps mono
  // fold-down safe (~0.3 energy). Positive bleed measured center-heavy.
  wetL = 0.25f * (outA + kOutBleed * outB);
  wetR = 0.25f * (outB + kOutBleed * outA);
}

void Reverb::processBlock(const float* input, float* outL, float* outR, int numFrames)
{
  assert(input != nullptr && outL != nullptr && outR != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!enabled_ || sampleRate_ <= 0.0)
  {
    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numFrames));
    std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
    processSample(input[i], outL[i], outR[i]);
}
} // namespace tdm

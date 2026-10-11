#pragma once

// AlienDsp: tiny shared realtime utilities for the Alien FX lab
// (dsp/lab/Alien). Deliberately self-contained (no Slam includes): three
// local users (Laser, Doppler, Warp) share FracDelay, everything else is
// a few lines each. All RT-safe after reset(), no allocation in process().

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace tdm
{
namespace alien
{

inline float alienSnap(float v)
{
  return (std::fabs(v) < 1e-12f) ? 0.0f : v;
}

// Fractional delay line, linear interpolation (house Delay precedent).
// Delay range [1, len-2] samples; out-of-range reads clamp.
class FracDelay
{
public:
  void reset(double sampleRate, float maxMs)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    size_t len = static_cast<size_t>(maxMs * 0.001f * sr + 4.0);
    if (len < 16)
      len = 16;
    line_.assign(len, 0.0f);
    len_ = len;
    w_ = 0;
  }

  // RT-safe state clear (keeps allocation + sizing). For effect-switch
  // handoffs at a dip bottom, never audible.
  void clear()
  {
    std::fill(line_.begin(), line_.end(), 0.0f);
    w_ = 0;
  }

  void write(float x)
  {
    line_[w_] = x;
    ++w_;
    if (w_ >= len_)
      w_ = 0;
  }

  float readAt(float delaySamp) const
  {
    float d = delaySamp;
    if (d < 1.0f)
      d = 1.0f;
    const float maxD = static_cast<float>(len_) - 2.0f;
    if (d > maxD)
      d = maxD;
    const float p = static_cast<float>(w_) - d;
    long i0 = static_cast<long>(std::floor(p));
    const float f = p - static_cast<float>(i0);
    long j0 = i0 % static_cast<long>(len_);
    if (j0 < 0)
      j0 += static_cast<long>(len_);
    long j1 = j0 + 1;
    if (j1 >= static_cast<long>(len_))
      j1 = 0;
    return line_[static_cast<size_t>(j0)] * (1.0f - f) + line_[static_cast<size_t>(j1)] * f;
  }

  double sampleRate() const { return sr_; }

private:
  double sr_ = 48000.0;
  std::vector<float> line_;
  size_t len_ = 0;
  size_t w_ = 0;
};

// One-pole parameter smoother (per-sample). Snap on reset, glide on move.
class SmoothParam
{
public:
  void reset(double sampleRate, float timeMs, float value)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const double ms = (timeMs < 0.5) ? 0.5 : timeMs;
    coef_ = static_cast<float>(1.0 - std::exp(-1.0 / (ms * 0.001 * sr)));
    cur_ = value;
    tgt_ = value;
  }

  void setTarget(float v) { tgt_ = v; }
  void snap(float v)
  {
    cur_ = v;
    tgt_ = v;
  }
  float next()
  {
    cur_ += (tgt_ - cur_) * coef_;
    if (std::fabs(tgt_ - cur_) < 1e-7f) // settle exactly: deterministic tails
      cur_ = tgt_;
    return cur_;
  }
  float value() const { return cur_; }

private:
  float coef_ = 1.0f, cur_ = 0.0f, tgt_ = 0.0f;
};

// Quadrature oscillator: sin/cos pair at an exact per-sample increment
// (direct sin/cos: exact, cheap enough for lab; no amplitude drift ever).
class QuadOsc
{
public:
  void reset(double sampleRate, float freqHz)
  {
    sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
    phase_ = 0.0f;
    setFreq(freqHz);
  }
  void setFreq(float f) { inc_ = 2.0f * 3.141592653589793f * f / static_cast<float>(sr_); }
  void next(float& s, float& c)
  {
    phase_ += inc_;
    if (phase_ > 3.141592653589793f)
      phase_ -= 2.0f * 3.141592653589793f;
    else if (phase_ < -3.141592653589793f)
      phase_ += 2.0f * 3.141592653589793f;
    s = std::sin(phase_);
    c = std::cos(phase_);
  }

private:
  double sr_ = 48000.0;
  float phase_ = 0.0f, inc_ = 0.0f;
};

} // namespace alien
} // namespace tdm

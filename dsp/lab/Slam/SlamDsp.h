#pragma once

// SlamDsp: small DSP building blocks for the SLAM lab study (LAB ONLY,
// dsp/lab/Slam). Header-only so study tools and unit tests share one copy.
//
// Everything here is realtime-safe by construction: no allocation, no locks,
// no IO in process(); sample-rate aware via reset(sr); deterministic;
// denormal-snapped states. Finite input -> finite output (biquad gains are
// bounded; shapers are bounded; envelopes track |x|).
//
// This is study scaffolding, not production DSP: no smoothing on setters
// (the study sets params at reset boundaries), minimal validation.

#include <cmath>
#include <cstdint>

namespace tdm
{
namespace lab
{

inline float slamDbToLin(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

inline float slamDenormalSnap(float v)
{
  return (std::fabs(v) < 1e-12f) ? 0.0f : v;
}

// First-order lowpass, y += a*(x-y). a from -3 dB corner.
class SlamOnePoleLp
{
public:
  void reset(double sampleRate, float cornerHz)
  {
    const double a = 1.0 - std::exp(-2.0 * 3.141592653589793 * cornerHz / sampleRate);
    a_ = static_cast<float>(a);
    s_ = 0.0f;
  }
  float process(float x)
  {
    s_ += a_ * (x - s_);
    s_ = slamDenormalSnap(s_);
    return s_;
  }

private:
  float a_ = 0.0f;
  float s_ = 0.0f;
};

// First-order highpass as x - lp(x): exact DC kill, gentle skirt.
class SlamOnePoleHp
{
public:
  void reset(double sampleRate, float cornerHz) { lp_.reset(sampleRate, cornerHz); }
  float process(float x) { return x - lp_.process(x); }

private:
  SlamOnePoleLp lp_;
};

// RBJ cookbook biquad (transposed Direct Form II). Only the three forms the
// study needs; coefficients recomputed by the set*() calls (off-RT).
class SlamBiquad
{
public:
  void reset()
  {
    s1_ = 0.0f;
    s2_ = 0.0f;
  }
  void setLowpass(double sampleRate, float fcHz, float q)
  {
    const double w = 2.0 * 3.141592653589793 * fcHz / sampleRate;
    const double cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 - cw) / 2.0 / a0;
    b1_ = (1.0 - cw) / a0;
    b2_ = (1.0 - cw) / 2.0 / a0;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  void setHighpass(double sampleRate, float fcHz, float q)
  {
    const double w = 2.0 * 3.141592653589793 * fcHz / sampleRate;
    const double cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 + cw) / 2.0 / a0;
    b1_ = -(1.0 + cw) / a0;
    b2_ = (1.0 + cw) / 2.0 / a0;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  // Constant-0dB-peak-gain bandpass (palm-mute thump isolation).
  void setBandpass(double sampleRate, float fcHz, float q)
  {
    const double w = 2.0 * 3.141592653589793 * fcHz / sampleRate;
    const double cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0_ = alpha / a0;
    b1_ = 0.0;
    b2_ = -alpha / a0;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  float process(float x)
  {
    const double y = b0_ * x + s1_;
    s1_ = slamDenormalSnap(static_cast<float>(b1_ * x - a1_ * y + s2_));
    s2_ = slamDenormalSnap(static_cast<float>(b2_ * x - a2_ * y));
    return static_cast<float>(y);
  }

private:
  double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
  float s1_ = 0.0f, s2_ = 0.0f;
};

// Peak envelope follower with independent attack/release (ms).
class SlamPeakEnv
{
public:
  void reset(double sampleRate, float attackMs, float releaseMs)
  {
    atk_ = coef(sampleRate, attackMs);
    rel_ = coef(sampleRate, releaseMs);
    e_ = 0.0f;
  }
  float process(float x)
  {
    const float ax = std::fabs(x);
    const float c = (ax > e_) ? atk_ : rel_;
    e_ += c * (ax - e_);
    e_ = slamDenormalSnap(e_);
    return e_;
  }
  float value() const { return e_; }
  void snap(float v) { e_ = v; } // re-arm without touching coefficients

private:
  static float coef(double sr, float ms)
  {
    if (ms <= 0.0f)
      return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (ms * 0.001 * sr)));
  }
  float atk_ = 1.0f, rel_ = 1.0f, e_ = 0.0f;
};

// --- Saturators (all odd-symmetric except asymSat; all bounded). ---

// Normalized tanh: unity at |x|=1, small-signal slope drive/tanh(drive).
inline float slamSoftSat(float x, float drive)
{
  const float d = (drive < 0.5f) ? 0.5f : drive;
  return std::tanh(d * x) / std::tanh(d);
}

// Asymmetric tanh: hotter positive lobe adds even harmonics ("amp
// explosion" hypothesis for candidate B). NOT DC-safe: caller DC-blocks.
inline float slamAsymSat(float x, float drivePos, float driveNeg)
{
  if (x >= 0.0f)
    return slamSoftSat(x, drivePos);
  return -slamSoftSat(-x, driveNeg);
}

// Rational hard-ish clip: unity at |x|=1, bounded, sharper knee than tanh.
inline float slamHardSat(float x, float drive)
{
  const float d = (drive < 1.0f) ? 1.0f : drive;
  const float v = x * d;
  return v / (1.0f + std::fabs(v)) * ((1.0f + d) / d);
}

// Simple feedforward peak compressor (branch glue). dB-domain gain computer
// with attack/release-smoothed linear gain. One log10+pow per sample: fine
// for lab/study, acceptable for a DEV audition insert.
class SlamComp
{
public:
  void reset(double sampleRate, float threshDb, float ratio, float attackMs, float releaseMs,
             float makeupLin)
  {
    thresh_ = slamDbToLin(threshDb);
    ratio_ = (ratio < 1.0f) ? 1.0f : ratio;
    makeup_ = makeupLin;
    env_.reset(sampleRate, attackMs, attackMs); // detector follows peaks fast
    gAtk_ = smoothCoef(sampleRate, attackMs);
    gRel_ = smoothCoef(sampleRate, releaseMs);
    g_ = 1.0f;
  }
  float process(float x)
  {
    const float m = env_.process(x);
    float target = 1.0f;
    if (m > thresh_ && m > 1e-9f)
    {
      const float overDb = 20.0f * std::log10(m / thresh_);
      const float grDb = overDb * (1.0f - 1.0f / ratio_);
      target = slamDbToLin(-grDb);
    }
    const float c = (target < g_) ? gAtk_ : gRel_;
    g_ += c * (target - g_);
    return x * g_ * makeup_;
  }

private:
  static float smoothCoef(double sr, float ms)
  {
    if (ms <= 0.0f)
      return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (ms * 0.001 * sr)));
  }
  float thresh_ = 0.1f, ratio_ = 4.0f, makeup_ = 1.0f;
  float gAtk_ = 1.0f, gRel_ = 1.0f, g_ = 1.0f;
  SlamPeakEnv env_;
};

// Triggered decaying sine ("impact burst"). Raised-cosine attack (~1-2 ms)
// then exponential decay to -60 dB in decayMs, with exponential frequency
// sweep f0 -> f1. Retrigger restarts. Silent when idle (zero cost but one
// branch). Bounded: |out| <= gain.
class SlamBurst
{
public:
  void reset(double sampleRate, float f0Hz, float f1Hz, float decayMs, float attackMs, float gain)
  {
    sr_ = sampleRate;
    f0_ = f0Hz;
    f1_ = f1Hz;
    decay_ = static_cast<float>(std::exp(-6.907755278982137 / (decayMs * 0.001 * sampleRate)));
    atkN_ = static_cast<int>(attackMs * 0.001 * sampleRate + 0.5);
    if (atkN_ < 1)
      atkN_ = 1;
    gain_ = gain;
    active_ = false;
    phase_ = 0.0f;
    env_ = 0.0f;
    atkPos_ = 0;
  }
  void trigger(float strength01)
  {
    const float s = (strength01 < 0.0f) ? 0.0f : ((strength01 > 1.0f) ? 1.0f : strength01);
    active_ = (s > 0.0f);
    phase_ = 0.0f;
    env_ = s;
    atkPos_ = 0;
  }
  bool active() const { return active_; }
  float process()
  {
    if (!active_)
      return 0.0f;
    float ramp = 1.0f;
    if (atkPos_ < atkN_)
    {
      const float t = static_cast<float>(atkPos_ + 1) / static_cast<float>(atkN_);
      ramp = 0.5f - 0.5f * std::cos(t * 3.141592653589793f);
      ++atkPos_;
    }
    // Frequency sweeps f0 -> f1 as the envelope falls (env in (0, 1]).
    const float k = (env_ > 1e-6f) ? (-std::log(env_) / 6.907755278982137f) : 1.0f;
    const float kk = (k < 0.0f) ? 0.0f : ((k > 1.0f) ? 1.0f : k);
    const float f = f0_ + (f1_ - f0_) * kk;
    phase_ += 2.0f * 3.141592653589793f * f / static_cast<float>(sr_);
    const float v = std::sin(phase_) * ramp * env_ * gain_;
    env_ *= decay_;
    if (env_ < 1e-4f)
      active_ = false;
    return v;
  }

private:
  double sr_ = 48000.0;
  float f0_ = 70.0f, f1_ = 48.0f, decay_ = 1.0f, gain_ = 1.0f;
  int atkN_ = 64, atkPos_ = 0;
  float phase_ = 0.0f, env_ = 0.0f;
  bool active_ = false;
};

// Causal onset detector: HPF'd flux vs adaptive floor + refractory.
// feed(x) returns > 0 on a crossing sample, else 0. The returned value is a
// rough velocity guess; SlamImpact ignores it and measures the true attack
// peak over a short confirmation window instead (crossing-time sampling
// under-reads velocity badly; the caller re-arms the floor (snapFloor)
// after a CONFIRMED fire. Floor ballistics (30 ms attack / 150 ms
// release) are tuned for repeated palm hits: fast enough that equal
// 8th-note chugs retrigger, slow enough that one hit's own decay never
// retriggers after the refractory gap.
class SlamOnset
{
public:
  void reset(double sampleRate, float fluxHpHz, float overDb, float absFloorDb, float refractoryMs)
  {
    hp_.reset(sampleRate, fluxHpHz);
    fast_.reset(sampleRate, 0.3f, 8.0f);
    floor_.reset(sampleRate, 30.0f, 150.0f);
    ratio_ = slamDbToLin(overDb);
    absFloor_ = slamDbToLin(absFloorDb);
    refrN_ = static_cast<int>(refractoryMs * 0.001 * sampleRate + 0.5);
    refr_ = 0;
  }
  float feed(float x)
  {
    const float h = hp_.process(x);
    const float f = fast_.process(h);
    const float fl = floor_.process(h);
    if (refr_ > 0)
      --refr_;
    if (refr_ == 0 && f > absFloor_ && f > fl * ratio_)
    {
      refr_ = refrN_; // refractory covers the caller's confirm window
      return 0.5f;    // placeholder: caller measures true velocity
    }
    return 0.0f;
  }
  float fastValue() const { return fast_.value(); }
  // Threshold-only update: preserves floor/fast/refractory state (for live
  // DEV sensitivity control; identical decisions to a reset at the same
  // overDb once the floor re-settles).
  void setOverDb(float overDb) { ratio_ = slamDbToLin(overDb); }
  // Re-arm after a confirmed fire (one attack = one trigger).
  void snapFloor(float v) { floor_.snap(v); }

private:
  SlamOnePoleHp hp_;
  SlamPeakEnv fast_;
  SlamPeakEnv floor_;
  float ratio_ = 4.0f, absFloor_ = 1e-4f;
  int refrN_ = 0, refr_ = 0;
};

// Trigger with confirmation + optional schedule, shared by SlamImpact (D)
// and SlamGated (E) so both candidates gate on IDENTICAL attack decisions.
// Detector path: onset crossing -> 8 ms confirm window (fire only if flux
// still >= 45% of the window peak: real attacks sustain, spikes don't).
// Scheduled path (split-tap study use): fires from an ascending
// off-RT-programmed (crossingSample, strength) list with the same 8 ms
// confirm-latency offset the live detector would add. feed() returns the
// fire strength in (0, 1] on the firing sample, else 0. Stamps record the
// CROSSING sample. reset() clears everything incl. the schedule.
class SlamTrigger
{
public:
  static constexpr int kMaxTriggers = 1024;
  struct Hit
  {
    int64_t sample = 0;
    float strength = 0.0f;
  };

  void reset(double sampleRate, float sens01, float refrMs)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const float overDb = 18.0f - 12.0f * sens01; // 0 -> +18 dB, 1 -> +6 dB
    onset_.reset(sr, 600.0f, overDb, -48.0f, refrMs);
    measN_ = static_cast<int>(0.008 * sr + 0.5);
    if (measN_ < 1)
      measN_ = 1;
    measLeft_ = 0;
    measPeak_ = 0.0f;
    crossPos_ = 0;
    pos_ = 0;
    fires_ = 0;
    triggers_ = 0;
    scheduled_ = false;
    schedCount_ = 0;
    schedNext_ = 0;
  }
  // Live sensitivity update (same 18->6 dB mapping as reset): no state
  // cleared, so in-flight confirms and floor adaptation survive.
  void setSensitivity(float sens01)
  {
    const float overDb = 18.0f - 12.0f * sens01;
    onset_.setOverDb(overDb);
  }
  void setScheduled(bool scheduled)
  {
    scheduled_ = scheduled;
    schedNext_ = 0;
  }
  void scheduleFire(int64_t samplePos, float strength)
  {
    if (schedCount_ >= kMaxTriggers)
      return;
    const float s = (strength < 0.0f) ? 0.0f : ((strength > 1.0f) ? 1.0f : strength);
    sched_[schedCount_++] = Hit{samplePos, s};
  }
  float feed(float x)
  {
    float fired = 0.0f;
    if (scheduled_)
    {
      while (schedNext_ < schedCount_ && pos_ >= sched_[schedNext_].sample + measN_)
      {
        record(sched_[schedNext_].sample, sched_[schedNext_].strength);
        fired = sched_[schedNext_].strength;
        ++schedNext_;
      }
      onset_.feed(x); // advance states (output ignored)
    }
    else
    {
      const float cross = onset_.feed(x);
      if (cross > 0.0f && measLeft_ <= 0)
      {
        measLeft_ = measN_;
        measPeak_ = onset_.fastValue();
        crossPos_ = pos_;
      }
      if (measLeft_ > 0)
      {
        const float fv = onset_.fastValue();
        if (fv > measPeak_)
          measPeak_ = fv;
        if (--measLeft_ == 0)
        {
          const float end = onset_.fastValue();
          if (measPeak_ > 1e-6f && end > measPeak_ * 0.45f)
          {
            const float db = 20.0f * std::log10(measPeak_);
            float s = (db + 30.0f) / 24.0f; // -30 dBFS -> 0, -6 dBFS -> 1
            if (s < 0.15f)
              s = 0.15f;
            if (s > 1.0f)
              s = 1.0f;
            onset_.snapFloor(measPeak_);
            record(crossPos_, s);
            fired = s;
          }
        }
      }
    }
    ++pos_;
    return fired;
  }
  void advance(int n) { pos_ += (n > 0 ? n : 0); } // keep stamps true when bypassed
  int64_t fireCount() const { return fires_; }
  int triggerCount() const { return (triggers_ < kMaxTriggers) ? triggers_ : kMaxTriggers; }
  Hit triggerAt(int i) const { return ring_[(i < 0 || i >= kMaxTriggers) ? 0 : i]; }

private:
  void record(int64_t stamp, float s)
  {
    if (triggers_ < kMaxTriggers)
      ring_[triggers_++] = Hit{stamp, s};
    ++fires_;
  }
  SlamOnset onset_;
  int measN_ = 384, measLeft_ = 0;
  float measPeak_ = 0.0f;
  int64_t crossPos_ = 0, pos_ = 0, fires_ = 0;
  int triggers_ = 0;
  Hit ring_[kMaxTriggers];
  bool scheduled_ = false;
  int schedCount_ = 0, schedNext_ = 0;
  Hit sched_[kMaxTriggers];
};

} // namespace lab
} // namespace tdm

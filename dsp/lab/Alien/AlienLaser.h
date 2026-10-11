#pragma once

// AlienLaser: Doppler pitch-sweep effect (LAB, dsp/lab/Alien). A single
// fractional delay line whose length follows musical trajectories; moving
// delay transposes by the Doppler ratio 1 - dD/dt (rising delay = pitch
// down, falling = pitch up). Inspired by the rejected PitchShifter v1
// stash (dual-tap masked-wrap Doppler, unsuitable for transparency but
// the right physics for INTENTIONAL sweeps); reimplemented here as a
// single wrap-free line: every trajectory is continuous, so output can
// never click by construction (pitch may jump at triangle turnarounds -
// that flip IS the laser character).
//
// Modes (Direction): Down = dive-then-recover chirp, Up = rise-then-settle
// chirp, Cycle = repeating triangle wobble. Triggered chirps are asymmetric
// out-and-back (fast exponential out over 35% of sweepMs, cosine return)
// and always end where they started. Retriggering (or flipping direction)
// mid-flight first glides the delay home over ~4 ms — a fast zip, never a
// step — then starts the fresh trajectory. (True hard snap-back is a
// documented follow-up if hardware wants it.)
//
// Loop: delay output feeds back through damping LP + resonant LPF + tanh
// (the saturator bounds loop energy, so resonance can scream without ever
// running away) with feedback <= 0.7. Dry/wet mix (0 = bit-exact dry).
//
// Params: direction, sweepMs (30..500), depthMs excursion (2..80),
// feedback, resoQ (0.7..8), mix. trigger() fires a one-shot (atomic,
// UI-safe; in Cycle mode it re-syncs the LFO phase). Triggered modes rest
// at base delay until the first trigger. reset(sr) off-RT; process()
// RT-safe, deterministic, finite-out.

#include <atomic>

#include "dsp/lab/Alien/AlienDsp.h"

namespace tdm
{
namespace alien
{

class AlienLaser
{
public:
  enum class Direction
  {
    Down = 0, // dive chirp (or falling-first wobble)
    Up = 1, // rise chirp (or rising-first wobble)
    Cycle = 2 // repeating triangle (Down picks first flank)
  };

  static constexpr float kMinSweepMs = 30.0f;
  static constexpr float kMaxSweepMs = 500.0f;
  static constexpr float kMinDepthMs = 2.0f;
  static constexpr float kMaxDepthMs = 80.0f;
  static constexpr float kBaseDelayMs = 2.0f; // sweep floor (small base latency)
  static constexpr float kMaxFeedback = 0.7f;
  static constexpr float kMinQ = 0.7f;
  static constexpr float kMaxQ = 8.0f;

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    line_.reset(sr, kBaseDelayMs + kMaxDepthMs + 50.0f);
    mix_.reset(sr, 10.0f, mixTgt_);
    mix_.snap(mixTgt_);
    fb_.reset(sr, 20.0f, fbTgt_);
    fb_.snap(fbTgt_);
    depth_.reset(sr, 30.0f, depthMs_); // tape-like excursion moves
    depth_.snap(depthMs_);
    phase_ = 0.0f;
    chirpActive_ = false;
    chirpPos_ = 0;
    chirpSweep_ = liveSweepSamp();
    returnLeft_ = 0;
    returnN_ = static_cast<int>(0.004 * sr + 0.5);
    if (returnN_ < 1)
      returnN_ = 1;
    returnFrom_ = 0.0f;
    frac_ = 0.0f;
    dirActive_ = dir_;
    damp_ = 0.0f;
    b0_ = b1_ = b2_ = a1_ = a2_ = 0.0f;
    s1_ = s2_ = 0.0f;
    qCached_ = -1.0f;
    trig_.store(false, std::memory_order_relaxed);
  }

  void setDirection(Direction d) { dir_ = d; }
  void setSweepMs(float ms)
  {
    sweepMs_ = (ms < kMinSweepMs) ? kMinSweepMs : ((ms > kMaxSweepMs) ? kMaxSweepMs : ms);
  }
  void setDepthMs(float ms)
  {
    depthMs_ = (ms < kMinDepthMs) ? kMinDepthMs : ((ms > kMaxDepthMs) ? kMaxDepthMs : ms);
    depth_.setTarget(depthMs_);
  }
  void setFeedback(float v)
  {
    fbTgt_ = (v < 0.0f) ? 0.0f : ((v > kMaxFeedback) ? kMaxFeedback : v);
    fb_.setTarget(fbTgt_);
  }
  void setReso(float q) { qTgt_ = (q < kMinQ) ? kMinQ : ((q > kMaxQ) ? kMaxQ : q); }
  void setMix(float v)
  {
    mixTgt_ = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    mix_.setTarget(mixTgt_);
  }
  void trigger() { trig_.store(true, std::memory_order_relaxed); } // UI thread

  // RT-safe state clear (no allocation). Call at a dip bottom.
  void clear()
  {
    line_.clear();
    mix_.snap(mixTgt_);
    fb_.snap(fbTgt_);
    depth_.snap(depthMs_);
    phase_ = 0.0f;
    chirpActive_ = false;
    chirpPos_ = 0;
    chirpSweep_ = liveSweepSamp();
    returnLeft_ = 0;
    returnFrom_ = 0.0f;
    frac_ = 0.0f;
    dirActive_ = dir_;
    damp_ = 0.0f;
    s1_ = s2_ = 0.0f;
    qCached_ = -1.0f;
    trig_.store(false, std::memory_order_relaxed);
  }

  float process(float x)
  {
    if (trig_.exchange(false, std::memory_order_relaxed))
      restart(); // retrigger/mode change: glide home, then start
    if (dir_ != dirActive_)
    {
      dirActive_ = dir_;
      restart(); // direction flip mid-flight would jump the delay
    }
    const float excSamp = depth_.next() * 0.001f * static_cast<float>(sr_);
    const float baseSamp = kBaseDelayMs * 0.001f * static_cast<float>(sr_);
    float frac = 0.0f; // 0..1 excursion position this sample
    if (returnLeft_ > 0)
    {
      // Return glide: raised-cosine home over ~4 ms (fast zip with zero
      // slope at both ends, never a step), then hand off exact-zero.
      if (--returnLeft_ == 0)
      {
        frac = 0.0f;
        if (dirActive_ == Direction::Cycle)
          phase_ = 0.0f;
        else
        {
          chirpActive_ = true;
          chirpPos_ = 0;
          chirpSweep_ = liveSweepSamp();
        }
      }
      else
      {
        const float q = 1.0f - static_cast<float>(returnLeft_) / static_cast<float>(returnN_);
        frac = returnFrom_ * (0.5f + 0.5f * std::cos(3.141592653589793f * q));
      }
    }
    else if (dirActive_ == Direction::Cycle)
    {
      // Live period tracking: only the phase RATE changes, phase itself
      // is continuous, so knob moves bend pitch without ever stepping.
      phase_ += 1.0f / (2.0f * liveSweepSamp());
      if (phase_ >= 1.0f)
        phase_ -= 1.0f;
      frac = (phase_ < 0.5f) ? (phase_ * 2.0f) : (2.0f - phase_ * 2.0f);
    }
    else if (chirpActive_)
    {
      const float p = static_cast<float>(chirpPos_) / static_cast<float>(chirpSweep_);
      frac = chirpShape((p > 1.0f) ? 1.0f : p);
      ++chirpPos_;
      if (chirpPos_ > chirpSweep_)
        chirpActive_ = false; // rest at base until next trigger
    }
    frac_ = frac;
    float delaySamp = baseSamp + frac * excSamp;
    if (dirActive_ == Direction::Up)
      delaySamp = baseSamp + excSamp - frac * excSamp;

    // Resonant LPF coeffs (RBJ cookbook), recomputed on Q change only.
    if (qTgt_ != qCached_)
    {
      qCached_ = qTgt_;
      const float fc = 2500.0f; // fixed loop resonance center (lab)
      const float w0 = 2.0f * 3.141592653589793f * fc / static_cast<float>(sr_);
      const float alpha = std::sin(w0) / (2.0f * qTgt_);
      const float cw = std::cos(w0);
      const float a0 = 1.0f + alpha;
      b0_ = (1.0f - cw) / (2.0f * a0);
      b1_ = (1.0f - cw) / a0;
      b2_ = b0_;
      a1_ = -2.0f * cw / a0;
      a2_ = (1.0f - alpha) / a0;
    }

    const float wet = line_.readAt(delaySamp);
    // Loop: damping LP -> resonant LPF -> tanh -> fb (bounded by design).
    damp_ += 0.45f * (wet - damp_); // ~4.5 kHz one-pole damping (house value)
    const float r = b0_ * damp_ + s1_;
    s1_ = alienSnap(b1_ * damp_ - a1_ * r + s2_);
    s2_ = alienSnap(b2_ * damp_ - a2_ * r);
    const float fb = fb_.next();
    line_.write(x + fb * std::tanh(2.0f * alienSnap(r)));
    const float mx = mix_.next();
    const float y = x + mx * (alienSnap(wet) - x);
    return std::isfinite(y) ? y : x;
  }

  void processBlock(const float* input, float* output, int n)
  {
    for (int k = 0; k < n; ++k)
      output[k] = process(input[k]);
  }

  int latencySamples() const
  {
    return static_cast<int>((kBaseDelayMs + depthMs_) * 0.001f * sr_);
  }

private:
  int liveSweepSamp() const
  {
    int s = static_cast<int>(sweepMs_ * 0.001f * sr_ + 0.5f);
    return (s < 1) ? 1 : s;
  }
  // Glide home from wherever the delay is, then hand off to the fresh
  // trajectory (restart is safe at any moment, including mid-glide).
  void restart()
  {
    returnFrom_ = frac_;
    returnLeft_ = returnN_;
    chirpActive_ = false;
  }
  // Fraction 0..1 of excursion at chirp phase p in [0,1]: fast
  // exponential out (first 35%), cosine return. f(0)=f(1)=0.
  static float chirpShape(float p)
  {
    if (p < 0.35f)
    {
      const float u = p / 0.35f;
      return (1.0f - std::exp(-3.0f * u)) / (1.0f - std::exp(-3.0f));
    }
    const float u = (p - 0.35f) / 0.65f;
    return 0.5f + 0.5f * std::cos(3.141592653589793f * u);
  }

  double sr_ = 48000.0;
  FracDelay line_;
  SmoothParam mix_, fb_, depth_;
  float mixTgt_ = 1.0f, fbTgt_ = 0.0f, qTgt_ = 0.7f;
  Direction dir_ = Direction::Down, dirActive_ = Direction::Down;
  float sweepMs_ = 120.0f, depthMs_ = 12.0f;
  float phase_ = 0.0f;
  bool chirpActive_ = false;
  int chirpPos_ = 0, chirpSweep_ = 1;
  int returnLeft_ = 0, returnN_ = 1;
  float returnFrom_ = 0.0f, frac_ = 0.0f;
  float damp_ = 0.0f;
  float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, s1_ = 0, s2_ = 0;
  float qCached_ = -1.0f;
  std::atomic<bool> trig_{false};
};

} // namespace alien
} // namespace tdm

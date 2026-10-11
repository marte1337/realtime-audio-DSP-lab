#pragma once

// DevAlien: DEV-only Alien FX router for the AppKit lab app (LAB ONLY,
// dsp/lab/Alien). Implements AlienInsert at the rig's post-IR tap:
// exactly ONE effect (Frequency | Laser | Warp) renders at a time.
// Effect/enable changes dip through dry (64 down / 5 ms up) so switching
// never clicks even though the three effects have different latencies;
// at the dip bottom the incoming effect is clear()ed (RT-safe, no
// allocation) and pre-rolled with recent dry history, so no stale repeats
// leak across and no 0->signal edge re-emerges a latency later. Parameter
// setters are lock-free atomic stores adopted at each process() call;
// the cores smooth internally, so knob moves are click-free. Effect
// cores always run at mix 1; the router owns the single master mix.
//
// Realtime contract: UI setters lock-free; process() allocates nothing,
// locks nothing, calls no AppKit. reset() is off-RT (allocates).

#include <atomic>
#include <vector>

#include "dsp/AlienInsert.h"
#include "dsp/lab/Alien/AlienLaser.h"
#include "dsp/lab/Alien/AlienShift.h"
#include "dsp/lab/Alien/AlienWarp.h"

namespace tdm
{
namespace alien
{

class DevAlien : public AlienInsert
{
public:
  enum class Effect
  {
    Frequency = 0,
    Laser = 1,
    Warp = 2
  };

  void reset(double sampleRate, int maxBlockSize) override
  {
    (void)maxBlockSize;
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    shift_.reset(sr);
    shift_.setMix(1.0f);
    laser_.reset(sr);
    laser_.setMix(1.0f);
    warp_.reset(sr);
    warp_.setMix(1.0f);
    hist_.assign(static_cast<size_t>(sr), 0.0f); // 1 s dry history (off-RT)
    hw_ = 0;
    hcount_ = 0;
    adoptAll(); // park cores at UI targets (fresh zero-state branches)
    active_ = loadEffect();
    m_ = 0.0f;
    phase_ = Steady;
    dipStep_ = 1.0f / 64.0f;
    riseStep_ = 1.0f / static_cast<float>(0.005 * sr); // 5 ms rise
    if (riseStep_ <= 0.0f || riseStep_ > 1.0f)
      riseStep_ = 1.0f / 256.0f;
  }

  void process(const float* input, float* output, int numFrames) override
  {
    if (numFrames <= 0)
      return;
    adoptAll();
    const bool en = enabled_.load(std::memory_order_relaxed);
    const Effect want = loadEffect();
    if (phase_ == Off && en)
    {
      active_ = want; // first enable: start fresh, pre-roll, rise in
      clearActive();
      preRoll();
      phase_ = Rise;
    }
    if (want != active_ && phase_ == Steady)
      phase_ = Dip;
    if (!en && phase_ == Steady)
      phase_ = Dip; // enable-off dips out the same way
    const float mixTgt = en ? mix_.load(std::memory_order_relaxed) : 0.0f;
    for (int i = 0; i < numFrames; ++i)
    {
      const float dry = input[i];
      pushHistory(dry); // recorded in every phase so enable/switch pre-rolls
      float wet = dry;
      if (phase_ != Off)
      {
        wet = (active_ == Effect::Frequency) ? shift_.process(dry)
            : (active_ == Effect::Laser)     ? laser_.process(dry)
                                             : warp_.process(dry);
      }
      if (phase_ == Dip)
      {
        m_ -= dipStep_;
        if (m_ <= 0.0f)
        {
          m_ = 0.0f;
          if (!en)
          {
            phase_ = Off;
          }
          else
          {
            active_ = want;
            clearActive(); // fresh state at the inaudible bottom
            preRoll();     // replay dry history: no 0->signal edge later
            phase_ = Rise;
          }
        }
      }
      else if (phase_ == Rise)
      {
        // Glide BOTH ways: mix can drop mid-rise (slider drag), and
        // snapping m down to it would jump the output in one sample.
        if (m_ < mixTgt)
        {
          m_ += riseStep_;
          if (m_ >= mixTgt)
          {
            m_ = mixTgt;
            phase_ = Steady;
          }
        }
        else if (m_ > mixTgt)
        {
          m_ -= dipStep_;
          if (m_ <= mixTgt)
          {
            m_ = mixTgt;
            phase_ = Steady;
          }
        }
        else
          phase_ = Steady;
      }
      else if (phase_ == Steady)
      {
        if (m_ < mixTgt) // glide live mix moves (click-safe knob drags)
        {
          m_ += riseStep_;
          if (m_ > mixTgt)
            m_ = mixTgt;
        }
        else if (m_ > mixTgt)
        {
          m_ -= dipStep_;
          if (m_ < mixTgt)
            m_ = mixTgt;
        }
      }
      output[i] = dry + m_ * (wet - dry);
    }
  }

  int latencySamples() const override
  {
    return (active_ == Effect::Frequency) ? shift_.latencySamples()
        : (active_ == Effect::Laser)      ? laser_.latencySamples()
                                          : warp_.latencySamples();
  }

  // --- UI thread (also safe pre-start) ---
  void setEnabled(bool e) { enabled_.store(e, std::memory_order_relaxed); }
  void setEffect(Effect e) { effect_.store(static_cast<int>(e), std::memory_order_relaxed); }
  void setMix(float v) { mix_.store(clamp01(v), std::memory_order_relaxed); }
  void setShiftHz(float hz) { shiftHz_.store(hz, std::memory_order_relaxed); }
  void setLaserDir(int d) { laserDir_.store(d, std::memory_order_relaxed); }
  void setSweepMs(float ms) { sweepMs_.store(ms, std::memory_order_relaxed); }
  void setDepthMs(float ms) { depthMs_.store(ms, std::memory_order_relaxed); }
  void setWarpMode(int m) { warpMode_.store(m, std::memory_order_relaxed); }
  void setWarpTimeMs(float ms) { warpTime_.store(ms, std::memory_order_relaxed); }
  void setWarpFb(float v) { warpFb_.store(v, std::memory_order_relaxed); }
  void setWarpShiftHz(float hz) { warpShiftHz_.store(hz, std::memory_order_relaxed); }
  void setWarpSt(float st) { warpSt_.store(st, std::memory_order_relaxed); }
  void triggerLaser() { laser_.trigger(); }

  bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }
  Effect effect() const { return loadEffect(); }
  // Lock-free UI/probe readbacks (stored targets, like the rig handoff).
  float mix01() const { return mix_.load(std::memory_order_relaxed); }
  float shiftHz() const { return shiftHz_.load(std::memory_order_relaxed); }
  int laserDir() const { return laserDir_.load(std::memory_order_relaxed); }
  float sweepMs() const { return sweepMs_.load(std::memory_order_relaxed); }
  float depthMs() const { return depthMs_.load(std::memory_order_relaxed); }
  int warpMode() const { return warpMode_.load(std::memory_order_relaxed); }
  float warpTimeMs() const { return warpTime_.load(std::memory_order_relaxed); }
  float warpFb() const { return warpFb_.load(std::memory_order_relaxed); }
  float warpShiftHz() const { return warpShiftHz_.load(std::memory_order_relaxed); }
  float warpSt() const { return warpSt_.load(std::memory_order_relaxed); }

private:
  enum Phase
  {
    Steady = 0,
    Dip = 1,
    Rise = 2,
    Off = 3
  };

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

  Effect loadEffect() const
  {
    const int e = effect_.load(std::memory_order_relaxed);
    return (e == 1) ? Effect::Laser : (e == 2) ? Effect::Warp : Effect::Frequency;
  }

  void adoptAll()
  {
    shift_.setShiftHz(shiftHz_.load(std::memory_order_relaxed));
    const int ld = laserDir_.load(std::memory_order_relaxed);
    laser_.setDirection(ld == 1 ? AlienLaser::Direction::Up
                        : ld == 2 ? AlienLaser::Direction::Cycle
                                  : AlienLaser::Direction::Down);
    laser_.setSweepMs(sweepMs_.load(std::memory_order_relaxed));
    laser_.setDepthMs(depthMs_.load(std::memory_order_relaxed));
    warp_.setMode(warpMode_.load(std::memory_order_relaxed) == 1 ? AlienWarp::Mode::PitchWarp
                                                                : AlienWarp::Mode::FreqWarp);
    const float wt = warpTime_.load(std::memory_order_relaxed);
    warp_.setTimeMs((wt < 20.0f) ? 20.0f : ((wt > 600.0f) ? 600.0f : wt)); // pre-roll budget
    warp_.setFeedback(warpFb_.load(std::memory_order_relaxed));
    warp_.setShiftHz(warpShiftHz_.load(std::memory_order_relaxed));
    warp_.setSemitones(warpSt_.load(std::memory_order_relaxed));
  }

  void clearActive()
  {
    if (active_ == Effect::Frequency)
      shift_.clear();
    else if (active_ == Effect::Laser)
      laser_.clear();
    else
      warp_.clear();
  }

  void pushHistory(float dry)
  {
    if (hist_.empty())
      return;
    hist_[(size_t)hw_] = dry;
    if (++hw_ >= static_cast<int>(hist_.size()))
      hw_ = 0;
    if (hcount_ < static_cast<int>(hist_.size()))
      ++hcount_;
  }

  // Replay the last L dry samples through the fresh effect so its delay
  // memories hold signal, not zeros: without this the 0->signal edge
  // re-emerges as a click exactly one latency after every switch. Bounded
  // (<= 1 s, warp time clamped to 600 ms in adoption); runs once per
  // switch/enable, never per block. Worst case ~30k samples of warp DSP
  // (~5 ms @48k) on a human switch action: acceptable DEV transient.
  void preRoll()
  {
    int l = latencySamples();
    if (l > hcount_)
      l = hcount_;
    if (l <= 0 || hist_.empty())
      return;
    int rd = hw_ - l;
    if (rd < 0)
      rd += static_cast<int>(hist_.size());
    const int h = static_cast<int>(hist_.size());
    for (int j = 0; j < l; ++j)
    {
      const float x = hist_[(size_t)rd];
      if (active_ == Effect::Frequency)
        shift_.process(x);
      else if (active_ == Effect::Laser)
        laser_.process(x);
      else
        warp_.process(x);
      if (++rd >= h)
        rd = 0;
    }
  }

  AlienShift shift_;
  AlienLaser laser_;
  AlienWarp warp_;

  std::atomic<bool> enabled_{false};
  std::atomic<int> effect_{0};
  std::atomic<float> mix_{1.0f};
  std::atomic<float> shiftHz_{150.0f};
  std::atomic<int> laserDir_{0};
  std::atomic<float> sweepMs_{120.0f};
  std::atomic<float> depthMs_{12.0f};
  std::atomic<int> warpMode_{0};
  std::atomic<float> warpTime_{220.0f};
  std::atomic<float> warpFb_{0.5f};
  std::atomic<float> warpShiftHz_{100.0f};
  std::atomic<float> warpSt_{3.0f};

  Effect active_ = Effect::Frequency;
  Phase phase_ = Off;
  float m_ = 0.0f, dipStep_ = 1.0f / 64.0f, riseStep_ = 1.0f / 256.0f;
  std::vector<float> hist_;
  int hw_ = 0, hcount_ = 0;
};

} // namespace alien
} // namespace tdm

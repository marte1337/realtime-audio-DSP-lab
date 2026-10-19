#pragma once

// DevSpecFx: DEV-only Special FX router for the AppKit lab app (LAB ONLY,
// dsp/lab/SpecialFx). Two SpecLaser engines sit at the rig's pre-gain and
// post-IR taps; exactly ONE tap is wet at a time (insert A/B) while the
// other stays primed via history preroll, so position comparison is
// instant and click-free. Shared atomic params (voice, control,
// expression, speed, sensitivity, mix, trigger, insert) are pushed to
// both engines per block; per-tap macros dip through dry on enable,
// voice, and insert changes (64 down / 5 ms up), with a 50 ms history
// preroll at each dip bottom so no 0->signal edge re-emerges (DevAlien
// lesson). Engines that reach macro 0 stop processing (live_ flag) but
// their taps keep recording history, so reactivation is seamless.
//
// Realtime contract: UI setters lock-free; process() allocates nothing,
// locks nothing, calls no AppKit. reset() is off-RT (allocates).

#include <atomic>
#include <vector>

#include "dsp/SpecInsert.h"
#include "dsp/lab/SpecialFx/SpecLaser.h"

namespace tdm
{
namespace specfx
{

class DevSpecFx
{
public:
  enum class Insert
  {
    Pre = 0,
    Post = 1
  };
  using Voice = SpecLaser::Voice;
  using Control = SpecLaser::Control;

  DevSpecFx() : preTap_(this, true), postTap_(this, false) {}

  SpecInsert& preTap() { return preTap_; }
  SpecInsert& postTap() { return postTap_; }

  // --- UI thread (also safe pre-start) ---
  void setEnabled(bool e) { enabled_.store(e, std::memory_order_relaxed); }
  void setVoice(Voice v) { voice_.store(static_cast<int>(v), std::memory_order_relaxed); }
  void setControl(Control c) { control_.store(static_cast<int>(c), std::memory_order_relaxed); }
  void setExpression(float v) { expression_.store(clamp01(v), std::memory_order_relaxed); }
  void setSpeed(float v) { speed_.store(clamp01(v), std::memory_order_relaxed); }
  void setSensitivity(float v) { sensitivity_.store(clamp01(v), std::memory_order_relaxed); }
  void setMix(float v) { mix_.store(clamp01(v), std::memory_order_relaxed); }
  void setTrigger(bool t) { trigger_.store(t, std::memory_order_relaxed); }
  void setToeRiseMs(float ms) { toeRiseMs_.store(ms, std::memory_order_relaxed); }
  void setToeReturnMs(float ms) { toeReturnMs_.store(ms, std::memory_order_relaxed); }
  void setDive(bool d) { dive_.store(d, std::memory_order_relaxed); }
  void setDiveDepthSt(float st) { diveDepth_.store(st, std::memory_order_relaxed); }
  void setDiveTimeMs(float ms) { diveTimeMs_.store(ms, std::memory_order_relaxed); }
  void setDiveReturnMs(float ms) { diveReturnMs_.store(ms, std::memory_order_relaxed); }
  void setInsert(Insert i) { insert_.store(static_cast<int>(i), std::memory_order_relaxed); }

  bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }
  Voice voice() const { return loadVoice(); }
  Control control() const
  {
    return (control_.load(std::memory_order_relaxed) == 1) ? Control::Envelope : Control::Manual;
  }
  Insert insert() const
  {
    return (insert_.load(std::memory_order_relaxed) == 1) ? Insert::Post : Insert::Pre;
  }
  bool trigger() const { return trigger_.load(std::memory_order_relaxed); }
  bool dive() const { return dive_.load(std::memory_order_relaxed); }
  float toeRiseMs() const { return toeRiseMs_.load(std::memory_order_relaxed); }
  float toeReturnMs() const { return toeReturnMs_.load(std::memory_order_relaxed); }
  float diveDepthSt() const { return diveDepth_.load(std::memory_order_relaxed); }
  float diveTimeMs() const { return diveTimeMs_.load(std::memory_order_relaxed); }
  float diveReturnMs() const { return diveReturnMs_.load(std::memory_order_relaxed); }
  float mix01() const { return mix_.load(std::memory_order_relaxed); }
  float expression() const { return expression_.load(std::memory_order_relaxed); }
  float speed() const { return speed_.load(std::memory_order_relaxed); }
  float sensitivity() const { return sensitivity_.load(std::memory_order_relaxed); }
  // Live smoothed expression of the pre/post engines (telemetry/probe).
  float preExpressionPos() const { return pre_.expressionPos(); }
  float postExpressionPos() const { return post_.expressionPos(); }

private:
  enum Phase
  {
    Steady = 0,
    Dip = 1,
    Rise = 2,
    Off = 3
  };

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

  Voice loadVoice() const
  {
    const int v = voice_.load(std::memory_order_relaxed);
    return (v == 1) ? Voice::Dual : (v == 2) ? Voice::Ring : (v == 3) ? Voice::Detune : Voice::Whammy;
  }

  struct TapState
  {
    Phase phase = Off;
    float m = 0.0f;
    bool live = false;
    Voice appliedVoice = Voice::Whammy;
    std::vector<float> hist;
    int hw = 0, hcount = 0;
  };

  class Tap : public SpecInsert
  {
  public:
    Tap(DevSpecFx* o, bool isPre) : o_(o), isPre_(isPre) {}

    void reset(double sampleRate, int maxBlockSize) override
    {
      (void)maxBlockSize;
      const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
      o_->engine(isPre_).reset(sr);
      st_.hist.assign(static_cast<size_t>(0.1 * sr) + 16, 0.0f); // 100 ms (off-RT)
      st_.hw = 0;
      st_.hcount = 0;
      st_.phase = Off;
      st_.m = 0.0f;
      st_.live = false;
      st_.appliedVoice = o_->loadVoice();
      o_->engine(isPre_).setVoice(st_.appliedVoice);
      o_->adopt(o_->engine(isPre_));
    }

    void process(const float* input, float* output, int numFrames) override
    {
      o_->runTap(*this, input, output, numFrames);
    }

    int latencySamples() const override { return o_->latencyOf(isPre_); }

    DevSpecFx* o_;
    bool isPre_;
    TapState st_;
  };

  SpecLaser& engine(bool isPre) { return isPre ? pre_ : post_; }
  int latencyOf(bool isPre) const { return isPre ? pre_.latencySamples() : post_.latencySamples(); }

  void adopt(SpecLaser& e)
  {
    e.setVoice(loadVoice());
    e.setControl(control());
    e.setExpression(expression_.load(std::memory_order_relaxed));
    e.setSpeed(speed_.load(std::memory_order_relaxed));
    e.setSensitivity(sensitivity_.load(std::memory_order_relaxed));
    e.setTrigger(trigger_.load(std::memory_order_relaxed));
    e.setToeRiseMs(toeRiseMs_.load(std::memory_order_relaxed));
    e.setToeReturnMs(toeReturnMs_.load(std::memory_order_relaxed));
    e.setDive(dive_.load(std::memory_order_relaxed));
    e.setDiveDepthSt(diveDepth_.load(std::memory_order_relaxed));
    e.setDiveTimeMs(diveTimeMs_.load(std::memory_order_relaxed));
    e.setDiveReturnMs(diveReturnMs_.load(std::memory_order_relaxed));
  }

  void runTap(Tap& tap, const float* input, float* output, int numFrames)
  {
    if (numFrames <= 0)
      return;
    SpecLaser& e = engine(tap.isPre_);
    TapState& st = tap.st_;
    adopt(e); // setVoice retargets gains; the dip below masks the move
    const bool en = enabled_.load(std::memory_order_relaxed);
    const bool selected = (insert() == Insert::Pre) == tap.isPre_;
    const float mixTgt = mix_.load(std::memory_order_relaxed);
    const float target = (en && selected) ? mixTgt : 0.0f;
    const Voice want = loadVoice();
    if (!st.live)
      st.appliedVoice = want; // idle taps track voice silently
    else if (want != st.appliedVoice && st.phase == Steady)
      st.phase = Dip;
    for (int i = 0; i < numFrames; ++i)
    {
      const float dry = input[i];
      pushHistory(st, dry); // recorded in every phase for preroll
      float wet = dry;
      if (st.live)
        wet = e.process(dry);
      if (st.phase == Dip)
      {
        st.m -= dipStep_;
        if (st.m <= 0.0f)
        {
          st.m = 0.0f;
          st.live = false;
          st.phase = Off;
        }
      }
      else if (st.phase == Off)
      {
        if (target > 0.0f)
        {
          st.appliedVoice = want; // (re)activate: fresh state, preroll, rise
          e.setVoice(want);
          e.clear();
          preRoll(st, e);
          st.live = true;
          st.phase = Rise;
        }
      }
      else if (st.phase == Rise)
      {
        // Glide BOTH ways: target can drop mid-rise (insert/mix moves),
        // and snapping m down to it would jump the output in one sample.
        if (st.m < target)
        {
          st.m += riseStep_;
          if (st.m >= target)
          {
            st.m = target;
            st.phase = Steady;
          }
        }
        else if (st.m > target)
        {
          st.m -= dipStep_;
          if (st.m <= target)
          {
            st.m = target;
            if (target <= 0.0f)
            {
              st.live = false;
              st.phase = Off;
            }
            else
              st.phase = Steady;
          }
        }
        else
          st.phase = Steady;
      }
      else // Steady
      {
        if (target <= 0.0f)
          st.phase = Dip; // enable/insert off: dip out next samples
        else if (st.m < target)
        {
          st.m += riseStep_; // glide live mix moves (click-safe drags)
          if (st.m > target)
            st.m = target;
        }
        else if (st.m > target)
        {
          st.m -= dipStep_;
          if (st.m < target)
            st.m = target;
        }
      }
      output[i] = dry + st.m * (wet - dry);
    }
  }

  void pushHistory(TapState& st, float dry)
  {
    if (st.hist.empty())
      return;
    st.hist[(size_t)st.hw] = dry;
    if (++st.hw >= static_cast<int>(st.hist.size()))
      st.hw = 0;
    if (st.hcount < static_cast<int>(st.hist.size()))
      ++st.hcount;
  }

  // Replay the last 50 ms of dry through the fresh engine (doppler Dmax
  // is 35 ms): delay lines and the follower settle on real signal, so
  // activation exposes no 0->signal edge. Bounded, once per transition.
  void preRoll(TapState& st, SpecLaser& e)
  {
    const int h = static_cast<int>(st.hist.size());
    if (h <= 0)
      return;
    int l = h / 2; // 50 ms of the 100 ms ring
    if (l > st.hcount)
      l = st.hcount;
    if (l <= 0)
      return;
    int rd = st.hw - l;
    if (rd < 0)
      rd += h;
    for (int j = 0; j < l; ++j)
    {
      e.process(st.hist[(size_t)rd]);
      if (++rd >= h)
        rd = 0;
    }
  }

  SpecLaser pre_, post_;
  Tap preTap_, postTap_;

  std::atomic<bool> enabled_{false};
  std::atomic<int> voice_{0};
  std::atomic<int> control_{0};
  std::atomic<float> expression_{0.0f};
  std::atomic<float> speed_{SpecLaser::kDefaultSpeed};
  std::atomic<float> sensitivity_{SpecLaser::kDefaultSensitivity};
  std::atomic<float> mix_{1.0f};
  std::atomic<bool> trigger_{false};
  std::atomic<float> toeRiseMs_{SpecLaser::kDefaultToeRiseMs};
  std::atomic<float> toeReturnMs_{SpecLaser::kDefaultToeReturnMs};
  std::atomic<bool> dive_{false};
  std::atomic<float> diveDepth_{SpecLaser::kDefaultDiveDepthSt};
  std::atomic<float> diveTimeMs_{SpecLaser::kDefaultDiveTimeMs};
  std::atomic<float> diveReturnMs_{SpecLaser::kDefaultDiveReturnMs};
  std::atomic<int> insert_{0};

  float dipStep_ = 1.0f / 64.0f;
  float riseStep_ = 1.0f / 240.0f; // 5 ms @48k (rate-agnostic enough for DEV)
};

} // namespace specfx
} // namespace tdm

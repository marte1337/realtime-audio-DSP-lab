#pragma once

// DevSpecFx: DEV-only Special FX router for the AppKit lab app (LAB ONLY,
// dsp/lab/SpecialFx). Two SpecLaser engines sit at the rig's pre-gain and
// post-IR taps; exactly ONE tap is wet at a time (insert A/B) while the
// other stays primed via history preroll, so position comparison is
// instant and click-free. Shared atomic params (voice, expression
// direction/depth, expression, mix, insert) plus the gesture slots are
// pushed to both engines per block; per-tap macros dip through dry on
// enable, voice, and insert changes (64 down / 5 ms up), with a 50 ms
// history preroll at each dip bottom so no 0->signal edge re-emerges
// (DevAlien lesson). Taps at macro 0 stop processing (live_ flag) but
// keep recording history, so reactivation is seamless.
//
// INDEPENDENT SECTIONS: expression and gestures enable separately but
// share ONE tap target (either on keeps the tap live — no stacked
// engines, no double processing, one Pre/Post insert for both).
// Expression-off parks the normal legs at unity behind a dry leg, so
// the selected voice can never colour the idle signal — while gestures
// still fire (dry base -> depth -> dry). Expression-on renders the
// voice maps; gestures override and return to the live base. Switching
// the expression section mid-gesture morphs gains over ~5 ms (bounded,
// no dip needed); switching gestures off mid-hold gracefully releases.
//
// GESTURE SLOTS (MIDI-ready): 8 independent slots, each a full gesture
// config (voice/dir/depth/motion/time/return) in relaxed atomics, edited
// via setSlot* (Save/Reset) and read back via slot*. Triggering is by
// STABLE SLOT ID and never depends on which slot the UI displays:
// pressSlot(i) / releaseSlot(i) / oneShotSlot(i) pack a
// generation-counter command adopted once per block (newest wins; a
// press+release inside one block coalesces to the release — correct for
// sub-block taps). Momentary (press/release pair, e.g. footswitch down/
// up) and one-shot (single message, e.g. Program Change: press runs the
// Time trajectory, then auto-returns over Return) are both supported so
// a future FCB1010/CoreMIDI layer needs no DSP changes. Slot config
// edits adopt live, even mid-gesture (depth/time changes reshape the
// running gesture continuously).
//
// Realtime contract: UI setters lock-free; process() allocates nothing,
// locks nothing, calls no AppKit. reset() is off-RT (allocates).

#include <atomic>
#include <vector>

#include "dsp/SpecInsert.h"
#include "dsp/lab/SpecialFx/SpecGesture.h"
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

  DevSpecFx() : preTap_(this, true), postTap_(this, false)
  {
    for (int i = 0; i < kNumGestureSlots; ++i)
    {
      const GestureConfig c = defaultGestureConfig(i);
      slotVoice_[i].store(c.voice, std::memory_order_relaxed);
      slotPack_[i].store(pack(c.dir, c.motion), std::memory_order_relaxed);
      slotDepth_[i].store(c.depthSt, std::memory_order_relaxed);
      slotTime_[i].store(c.timeMs, std::memory_order_relaxed);
      slotRet_[i].store(c.retMs, std::memory_order_relaxed);
    }
  }

  SpecInsert& preTap() { return preTap_; }
  SpecInsert& postTap() { return postTap_; }

  // --- UI thread (also safe pre-start) ---
  void setExpressionEnabled(bool e) { exprEnabled_.store(e, std::memory_order_relaxed); }
  void setGesturesEnabled(bool e) { gestEnabled_.store(e, std::memory_order_relaxed); }
  void setVoice(Voice v) { voice_.store(static_cast<int>(v), std::memory_order_relaxed); }
  void setExprDirection(GestureDir d)
  {
    exprDir_.store(d == GestureDir::Down ? 1 : 0, std::memory_order_relaxed);
  }
  void setExprDepth(float st) { exprDepth_.store(st, std::memory_order_relaxed); } // eng clamps
  void setExpression(float v) { expression_.store(clamp01(v), std::memory_order_relaxed); }
  void setMix(float v) { mix_.store(clamp01(v), std::memory_order_relaxed); }
  void setInsert(Insert i) { insert_.store(static_cast<int>(i), std::memory_order_relaxed); }

  // --- Gesture slots (UI thread; 0-based stable IDs) ---
  void setSlotVoice(int slot, Voice v)
  {
    if (validSlot(slot))
      slotVoice_[slot].store(static_cast<int>(v), std::memory_order_relaxed);
  }
  void setSlotDir(int slot, GestureDir d)
  {
    if (validSlot(slot))
      slotPack_[slot].store(pack(d, slotMotion(slot)), std::memory_order_relaxed);
  }
  void setSlotMotion(int slot, GestureMotion m)
  {
    if (validSlot(slot))
      slotPack_[slot].store(pack(slotDir(slot), m), std::memory_order_relaxed);
  }
  void setSlotDepthSt(int slot, float st)
  {
    if (validSlot(slot))
      slotDepth_[slot].store(st, std::memory_order_relaxed); // engine clamps
  }
  void setSlotTimeMs(int slot, float ms)
  {
    if (validSlot(slot))
      slotTime_[slot].store(ms, std::memory_order_relaxed);
  }
  void setSlotRetMs(int slot, float ms)
  {
    if (validSlot(slot))
      slotRet_[slot].store(ms, std::memory_order_relaxed);
  }
  void resetSlot(int slot) // restore factory default (Reset Slot)
  {
    if (!validSlot(slot))
      return;
    const GestureConfig c = defaultGestureConfig(slot);
    slotVoice_[slot].store(c.voice, std::memory_order_relaxed);
    slotPack_[slot].store(pack(c.dir, c.motion), std::memory_order_relaxed);
    slotDepth_[slot].store(c.depthSt, std::memory_order_relaxed);
    slotTime_[slot].store(c.timeMs, std::memory_order_relaxed);
    slotRet_[slot].store(c.retMs, std::memory_order_relaxed);
  }
  // Gesture commands by slot ID (momentary pair + one-shot; gen-guarded).
  void pressSlot(int slot) { pushCmd(slot, 1); }
  void releaseSlot(int slot) { pushCmd(slot, 2); }
  void oneShotSlot(int slot) { pushCmd(slot, 3); }

  bool isExpressionEnabled() const { return exprEnabled_.load(std::memory_order_relaxed); }
  bool isGesturesEnabled() const { return gestEnabled_.load(std::memory_order_relaxed); }
  Voice voice() const { return loadVoice(); }
  GestureDir exprDirection() const
  {
    return (exprDir_.load(std::memory_order_relaxed) == 1) ? GestureDir::Down : GestureDir::Up;
  }
  float exprDepth() const { return exprDepth_.load(std::memory_order_relaxed); }
  Insert insert() const
  {
    return (insert_.load(std::memory_order_relaxed) == 1) ? Insert::Post : Insert::Pre;
  }
  float mix01() const { return mix_.load(std::memory_order_relaxed); }
  float expression() const { return expression_.load(std::memory_order_relaxed); }
  Voice slotVoice(int slot) const
  {
    return validSlot(slot) ? intToVoice(slotVoice_[slot].load(std::memory_order_relaxed))
                           : Voice::Whammy;
  }
  GestureDir slotDir(int slot) const
  {
    return (validSlot(slot) && (slotPack_[slot].load(std::memory_order_relaxed) & 1) != 0)
        ? GestureDir::Down
        : GestureDir::Up;
  }
  GestureMotion slotMotion(int slot) const
  {
    return (validSlot(slot) && (slotPack_[slot].load(std::memory_order_relaxed) & 2) != 0)
        ? GestureMotion::FromDepth
        : GestureMotion::ToDepth;
  }
  float slotDepthSt(int slot) const
  {
    return validSlot(slot) ? slotDepth_[slot].load(std::memory_order_relaxed) : 12.0f;
  }
  float slotTimeMs(int slot) const
  {
    return validSlot(slot) ? slotTime_[slot].load(std::memory_order_relaxed) : 100.0f;
  }
  float slotRetMs(int slot) const
  {
    return validSlot(slot) ? slotRet_[slot].load(std::memory_order_relaxed) : 100.0f;
  }
  // Live smoothed expression of the pre/post engines (telemetry/probe).
  float preExpressionPos() const { return pre_.expressionPos(); }
  float postExpressionPos() const { return post_.expressionPos(); }
  bool preGestureActive() const { return pre_.gestureActive(); }
  bool postGestureActive() const { return post_.gestureActive(); }
  float preGestureDisp() const { return pre_.gestureDisp(); }
  float postGestureDisp() const { return post_.gestureDisp(); }

private:
  enum Phase
  {
    Steady = 0,
    Dip = 1,
    Rise = 2,
    Off = 3
  };

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }
  static bool validSlot(int s) { return s >= 0 && s < kNumGestureSlots; }
  static int pack(GestureDir d, GestureMotion m)
  {
    return (d == GestureDir::Down ? 1 : 0) | (m == GestureMotion::FromDepth ? 2 : 0);
  }

  static Voice intToVoice(int v)
  {
    if (v == 1)
      return Voice::Dual;
    if (v == 2)
      return Voice::Ring;
    if (v == 3)
      return Voice::DualWham;
    return Voice::Whammy; // 0 + unknown ids (safe default)
  }

  Voice loadVoice() const { return intToVoice(voice_.load(std::memory_order_relaxed)); }

  void pushCmd(int slot, unsigned op) // op: 1 press, 2 release, 3 one-shot
  {
    if (!validSlot(slot) || op == 0)
      return;
    const unsigned gen = (cmdGen_ += 1); // UI thread only
    gestCmd_.store((gen << 10) | (static_cast<unsigned>(slot) << 2) | op, std::memory_order_relaxed);
  }

  GestureConfig readSlot(int slot) const
  {
    GestureConfig c;
    c.voice = slotVoice_[slot].load(std::memory_order_relaxed);
    const int p = slotPack_[slot].load(std::memory_order_relaxed);
    c.dir = ((p & 1) != 0) ? GestureDir::Down : GestureDir::Up;
    c.motion = ((p & 2) != 0) ? GestureMotion::FromDepth : GestureMotion::ToDepth;
    c.depthSt = slotDepth_[slot].load(std::memory_order_relaxed);
    c.timeMs = slotTime_[slot].load(std::memory_order_relaxed);
    c.retMs = slotRet_[slot].load(std::memory_order_relaxed);
    return c;
  }

  struct TapState
  {
    Phase phase = Off;
    float m = 0.0f;
    bool live = false;
    Voice appliedVoice = Voice::Whammy;
    unsigned adoptedGen = 0; // per-tap: BOTH engines see every command
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
      st_.adoptedGen = 0;
      o_->engine(isPre_).setVoice(st_.appliedVoice);
      o_->adopt(o_->engine(isPre_), st_);
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

  void adopt(SpecLaser& e, TapState& st)
  {
    const bool exprEn = exprEnabled_.load(std::memory_order_relaxed);
    const bool gestEn = gestEnabled_.load(std::memory_order_relaxed);
    e.setVoice(loadVoice());
    e.setExprDirection(exprDirection());
    e.setExprDepth(exprDepth_.load(std::memory_order_relaxed));
    e.setExpression(expression_.load(std::memory_order_relaxed));
    e.setNormalActive(exprEn);
    e.setGestAllowed(gestEn);
    // Gestures off mid-hold releases gracefully (no stuck states); the
    // release call is idempotent once the gesture reaches Idle.
    if (!gestEn && e.gestureActive())
      e.releaseGesture(e.gestureSlot());
    // Active-slot config adopts every block (live edits reshape a held
    // gesture); the gen-guarded command applies press/release/one-shot.
    e.setGestureConfig(readSlot(cmdSlot_));
    const unsigned cmd = gestCmd_.load(std::memory_order_relaxed);
    const unsigned gen = cmd >> 10;
    if (gen != st.adoptedGen)
    {
      st.adoptedGen = gen;
      const int slot = static_cast<int>((cmd >> 2) & 0xFF);
      const unsigned op = cmd & 3u;
      if (validSlot(slot))
      {
        cmdSlot_ = slot;
        e.setGestureConfig(readSlot(slot));
        if (op == 1)
          e.pressGesture(slot);
        else if (op == 2)
          e.releaseGesture(slot);
        else if (op == 3)
          e.oneShotGesture(slot);
      }
    }
  }

  void runTap(Tap& tap, const float* input, float* output, int numFrames)
  {
    if (numFrames <= 0)
      return;
    SpecLaser& e = engine(tap.isPre_);
    TapState& st = tap.st_;
    adopt(e, st); // setVoice retargets gains; the dip below masks the move
    const bool exprEn = exprEnabled_.load(std::memory_order_relaxed);
    const bool gestEn = gestEnabled_.load(std::memory_order_relaxed);
    const bool selected = (insert() == Insert::Pre) == tap.isPre_;
    const float mixTgt = mix_.load(std::memory_order_relaxed);
    // Shared target: either section keeps the tap live (one engine, one
    // blend — never stacked). Both off dips to installed-but-off dry.
    const float target = ((exprEn || gestEn) && selected) ? mixTgt : 0.0f;
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
  // is 35 ms): delay lines and the smoother settle on real signal, so
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

  std::atomic<bool> exprEnabled_{false};
  std::atomic<bool> gestEnabled_{false};
  std::atomic<int> voice_{0};
  std::atomic<int> exprDir_{0};
  std::atomic<float> exprDepth_{kMaxGestureDepthSt};
  std::atomic<float> expression_{0.0f};
  std::atomic<float> mix_{1.0f};
  std::atomic<int> insert_{0};

  std::atomic<int> slotVoice_[kNumGestureSlots];
  std::atomic<int> slotPack_[kNumGestureSlots]; // bit0 dir, bit1 motion
  std::atomic<float> slotDepth_[kNumGestureSlots];
  std::atomic<float> slotTime_[kNumGestureSlots];
  std::atomic<float> slotRet_[kNumGestureSlots];
  std::atomic<unsigned> gestCmd_{0}; // gen(22) | slot(8) | op(2)
  unsigned cmdGen_ = 0;              // UI thread only

  int cmdSlot_ = 0; // audio thread only: last commanded slot

  float dipStep_ = 1.0f / 64.0f;
  float riseStep_ = 1.0f / 240.0f; // 5 ms @48k (rate-agnostic enough for DEV)
};

} // namespace specfx
} // namespace tdm

#pragma once

// SpecLaser: expression-driven Laser engine (LAB, dsp/lab/SpecialFx).
// Independent take on the reference "laser" concept: one normalized
// expression position (0..1) drives preset-like per-voice parameter maps
// (heel/toe endpoints) under performance-gesture control. Four creative
// voices:
//
//   Whammy:  single pitch voice, Direction Up 0 -> +D st (V1, the
//            accepted classic path) or Down 0 -> -D st (V3 down leg)
//   Dual:    contrary-motion pair, V1 0 -> -D st, V2 0 -> +D st
//   Ring:    ring-mod carrier sweep 60 -> 2000 Hz (log, fixed map)
//   DualWham: experimental upward pair, A 0 -> +D st (V1) + B 0 -> +2D
//            via a V2 -> V3 Doppler cascade (v1: unaligned, extreme)
//
// D = expression Depth (default 12 st; Ring ignores it). Direction is
// the single source of truth for Whammy polarity (no separate up/down
// voice identities, no contradictory states); Dual/DualWham spread
// upward only; Ring is fixed-map (its accepted character).
//
// Pitch voices reuse AlienDoppler (masked-wrap variable delay): slope
// changes only bend the ramp, so expression sweeps are continuous by
// construction, polyphonic-safe (no pitch detection), and cheap. The
// documented tradeoff vs a WSOLA engine (GuitarTranspose): static
// accuracy degrades toward the extremes and is INPUT-DEPENDENT
// (measured on raw sines: -12 renders -10.5..-16 depending on f0;
// +12 renders ~+11.2; below -12 the pitch smears toward mush, so
// gesture depth caps at 12). For expressive sweeps ("warped harmonies")
// that character is accepted (hardware likes the aggressive artificial
// pitch); pitch-sync stagger is the known follow-up if tighter
// intervals are ever wanted. Production GuitarTranspose was
// deliberately NOT used: 16 ms latency, upward-shift limitations, and
// resync warble under continuous sweeps make it the wrong primitive
// here. Ring reuses AlienRing (carrier is NOT smoothed there, so this
// engine glides it per sample).
//
// DualWham B (+24 target) cascades two +12 Doppler stages rather than
// extending the shared primitive: no silent clamp, no Alien side
// effects. Cost: B lags A by ~35 ms nominal (moving taps — exact
// alignment is impossible), double sag, double artifacts. v1 keeps the
// skew unaligned (extreme character); ears decide if it needs work.
//
// NEUTRAL BEHAVIOR (hardware finding, probed on the checkpoint tree):
// Whammy/Dual heels are bit-exact dry (modulo ~1e-6 gain-glide stall
// residue, pitch-exact); Ring heel is full 60 Hz AM coloration (its
// ACCEPTED character, preserved); the old Detune heel detuned
// (+/-6 cents + LFO wobble) and Detune was REMOVED for it. The
// structural fix is independent enables: expression-off parks the
// normal legs at unity and carries a dry leg, so output is bit-dry no
// matter which voice is selected — while gestures still fire.
//
// PERFORMANCE GESTURES (one engine, two paths, no stale state):
// A gesture config (voice/dir/depth/motion/time/return) drives a
// displacement ramp d and a mix ramp m. The depth target T is a POSITION
// in the gesture voice's map (pitch voices always target pos 1;
// Whammy polarity comes from Direction selecting the leg).
//   Path E (gesture == normal-effective: same voice, and same Whammy
//   direction): g = max/min(base, d-bound, T) overrides the normal
//   expression (the legacy Toe shape, BIT-EXACT at Fast-Toe settings;
//   golden-pinned). A Whammy-Down gesture over Whammy-Up normal is
//   Path X (different legs) — polarity can never cancel silently.
//   Path X (different effective voice): the normal voice renders the
//   base on the normal legs (V1/V2/V3/R1) while the gesture voice
//   renders g on DEDICATED gesture legs (V4 up / V5 down / R2 ring /
//   V6+V7 DualWham-B cascade, parked at unity and primed every sample,
//   so a press retargets from dry — the legacy Dive shape, bit-exact
//   from heel), crossfaded by m. To-Depth sweeps heel -> T absolutely;
//   From-Depth anchors at the live base so the sweep converges exactly
//   to the current normal state.
// To Depth: press sweeps to T over Time, hold, release returns over
// Return. From Depth: press snaps to T behind a 10 ms engage fade
// (target snap, slew+fade masked — never a discontinuous DSP direction
// change), sweeps back to live normal over Time, converging exactly;
// early release fades out over Return. One-shot auto-releases.
// Newest trigger wins; routines are per-sample continuous, so
// interruption/retrigger/early-release can't click.
// Expression-off rendering: base forced to heel, normal gains gated
// off (except while a Path-E gesture plays the normal legs), dry leg
// carries (1-m). Gesture presses are ignored unless the gesture
// section allows them (router-owned flag).
// Gesture slew (musical portamento) is SEPARATE from the 5 ms
// anti-zipper smoother + doppler 30 ms slew (click prevention), which
// only round the gesture corners.
//
// A doppler leg's target must never change SIGN mid-engagement (the tap
// mirror jumps ~0.4): gesture legs have fixed sign roles (V4/V6/V7 >= 0,
// V5 <= 0), unity parking engages the bypass fade, and normal-leg sign
// flips only happen behind router dips (voice switches).
//
// All legs process every sample (normal + gesture stay primed even at
// zero gain; parked dopplers cost one ring write + early return, so the
// extra legs are ~free); selection only retargets gains, so no switch
// can expose stale delay audio. Enable/voice/insert transitions
// (dip + clear + history preroll) live in the DevSpecFx taps, not here.
//
// Realtime contract: setters are plain (the router pushes shared
// atomics per block; gestures arrive as press/release/oneShot commands
// by stable slot ID — MIDI/CoreMIDI-ready, never AppKit-coupled);
// process() allocates nothing, locks nothing. reset()/clear() are
// off-RT or dip-bottom only.

#include <cmath>

#include "dsp/lab/Alien/AlienDoppler.h"
#include "dsp/lab/Alien/AlienDsp.h"
#include "dsp/lab/Alien/AlienRing.h"
#include "dsp/lab/SpecialFx/SpecGesture.h"

namespace tdm
{
namespace specfx
{

class SpecLaser
{
public:
  enum class Voice
  {
    Whammy = 0,
    Dual = 1,
    Ring = 2,
    DualWham = 3
  };

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    v1_.reset(sr);
    v2_.reset(sr);
    v3_.reset(sr);
    v4_.reset(sr);
    v5_.reset(sr);
    v6_.reset(sr);
    v7_.reset(sr);
    ring_.reset(sr);
    ring_.setMix(1.0f);
    ring2_.reset(sr);
    ring2_.setMix(1.0f);
    pos_.reset(sr, 5.0f, 0.0f);
    pos_.snap(0.0f);
    carrier_.reset(sr, 5.0f, kRingHeelHz);
    carrier_.snap(kRingHeelHz);
    carrier2_.reset(sr, 5.0f, kRingHeelHz);
    carrier2_.snap(kRingHeelHz);
    for (int i = 0; i < 10; ++i)
    {
      gain_[i] = 0.0f;
      gainTgt_[i] = 0.0f;
    }
    applyVoiceGains();
    for (int i = 0; i < 10; ++i)
      gain_[i] = (i < 4) ? gainTgt_[i] : 0.0f;
    exprDir_ = GestureDir::Up;
    exprDepth_ = kMaxGestureDepthSt;
    normalActive_ = true;
    gestAllowed_ = true;
    disarm();
    setGestureConfig(defaultGestureConfig(0)); // Fast Toe shape
    updateGestSteps();
  }

  // RT-safe state clear for dip-bottom handoffs: flush delay audio.
  // Gesture runtime is KEPT (a held gesture survives enable/voice/insert
  // dips and re-emerges from the preroll); gains snap (macro is 0 here,
  // so inaudible); call setVoice() first.
  void clear()
  {
    v1_.clear();
    v2_.clear();
    v3_.clear();
    v4_.clear();
    v5_.clear();
    v6_.clear();
    v7_.clear();
    pathE_ = pathE();
    pos_.snap(posTarget());
    for (int i = 0; i < 10; ++i)
      gain_[i] = effGainTgt(i); // snap incl. live gesture (macro is 0 here)
  }

  void setVoice(Voice v)
  {
    voice_ = v;
    applyVoiceGains();
  }
  void setExpression(float e) { manual_ = clamp01(e); }
  void setExprDirection(GestureDir d)
  {
    exprDir_ = d;
    applyVoiceGains(); // Whammy polarity reselects its leg
  }
  void setExprDepth(float st)
  {
    exprDepth_ = (st < 0.0f) ? 0.0f : ((st > kMaxGestureDepthSt) ? kMaxGestureDepthSt : st);
  }
  void setNormalActive(bool a) { normalActive_ = a; } // expression section enable
  void setGestAllowed(bool a) { gestAllowed_ = a; }   // gesture section enable

  // Gesture config: pushed per block (live edits adopt mid-gesture).
  void setGestureConfig(const GestureConfig& c)
  {
    cfgVoice_ = c.voice;
    cfgDir_ = c.dir;
    cfgMotion_ = c.motion;
    cfgEnd_ = gestureDepthEnd(c.voice, c.dir);
    cfgTgt_ = gestureTargetPos(cfgEnd_, c.depthSt);
    cfgTimeMs_ = clampMs(c.timeMs);
    cfgRetMs_ = clampMs(c.retMs);
    gestGainTgt_[0] = gestGainTgt_[1] = gestGainTgt_[2] = gestGainTgt_[3] = gestGainTgt_[4] = 0.0f;
    if (c.voice == kVoiceWhammy)
      gestGainTgt_[(c.dir == GestureDir::Up) ? 0 : 1] = 1.0f; // V4 up / V5 down
    else if (c.voice == kVoiceDual)
    {
      gestGainTgt_[0] = 0.5f;
      gestGainTgt_[1] = 0.5f;
    }
    else if (c.voice == kVoiceRing)
      gestGainTgt_[2] = 1.0f;
    else if (c.voice == kVoiceDualWham)
    {
      gestGainTgt_[0] = 0.5f; // A on V4, B via the V6 -> V7 cascade
      gestGainTgt_[4] = 0.5f;
    }
    updateGestSteps();
  }
  // Gesture commands (block boundary, by stable slot ID; newest wins).
  // Unknown voice ids are ignored (unsupported, documented); presses
  // while the gesture section is off are ignored (no-op, stays dry).
  void pressGesture(int slot)
  {
    if (!gestAllowed_ || !validGestVoice(cfgVoice_))
      return;
    activeSlot_ = slot;
    oneShot_ = false;
    phase_ = Phase::Press;
    if (cfgMotion_ == GestureMotion::FromDepth)
      gestD_ = 1.0f; // engage AT depth; the mix fade masks the snap
    // To Depth: ramps chase 1 from wherever they are (re-press safe).
  }
  void releaseGesture(int slot)
  {
    if (slot != activeSlot_ || phase_ == Phase::Idle)
      return; // stale/foreign release: ignore (no stuck states)
    phase_ = Phase::Release;
  }
  void oneShotGesture(int slot)
  {
    pressGesture(slot);
    if (slot == activeSlot_ && phase_ != Phase::Idle)
      oneShot_ = true;
  }

  Voice voice() const { return voice_; }
  float expressionPos() const { return pos_.value(); } // smoothed 0..1 (telemetry)
  bool gestureActive() const { return phase_ != Phase::Idle; }
  int gestureSlot() const { return activeSlot_; }
  float gestureDisp() const { return gestD_; } // displacement ramp (telemetry)
  float gestureMix() const { return gestM_; }  // crossfade ramp (telemetry)
  int latencySamples() const { return v1_.latencySamples(); } // doppler bound (stable)

  float process(float x)
  {
    // Sanitize first: a NaN/Inf sample would poison the smoothed
    // position permanently (comparisons go false, state sticks).
    // Upstream garbage becomes a zero, never a wedged effect.
    if (!std::isfinite(x))
      x = 0.0f;

    advanceGesture(); // d/m ramps (no-op when idle)
    pathE_ = pathE();
    // Path X (cross-voice): normal renders the base, gesture renders g
    // on the dedicated legs, crossfaded by the EFFECTIVE mix (gated by
    // path so Path E never touches the mix). Path changes only on voice
    // switches, which the router dip masks.
    const bool pathX = (phase_ != Phase::Idle) && !pathE_;
    const float mix = pathX ? gestM_ : 0.0f;

    const float pos = pos_.next(); // 5 ms smoothed, exact settle
    pos_.setTarget(posTarget());

    // Per-voice maps from expression (normal legs; gesture legs below).
    const float st1 = exprDepth_ * pos;   // up map (+D)
    const float stD1 = -exprDepth_ * pos; // down map (-D)
    const float stD2 = exprDepth_ * pos;  // second up map (+D)
    float n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;
    if (voice_ == Voice::Whammy)
    {
      if (exprDir_ == GestureDir::Up)
        n1 = st1; // accepted classic path (V1)
      else
        n3 = stD1; // working downward path (V3)
    }
    else if (voice_ == Voice::Dual)
    {
      n1 = stD1;
      n2 = stD2;
    }
    else if (voice_ == Voice::DualWham)
    {
      n1 = st1;  // A: +D on V1
      n2 = stD2; // B: +D cascade stages on V2 -> V3 (B input routed below)
      n3 = stD2;
    }
    // else Ring voice: dopplers park at unity (bit-exact dry legs)
    v1_.setSemitones(n1);
    v2_.setSemitones(n2);
    v3_.setSemitones(n3);
    // Gesture legs (Path X): the SAME maps at the gesture position g
    // (depth lives in the target, so signs stay fixed: V4/V6/V7 >= 0,
    // V5 <= 0 — no mid-engagement sign flips, ever). Parked at unity
    // otherwise.
    const float g = gestPos(pathX);
    v4_.setSemitones(pathX ? 12.0f * g : 0.0f);
    v5_.setSemitones(pathX ? -12.0f * g : 0.0f);
    v6_.setSemitones(pathX ? 12.0f * g : 0.0f);
    v7_.setSemitones(pathX ? 12.0f * g : 0.0f);
    const float fc = kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, pos); // log sweep
    carrier_.setTarget(fc);
    ring_.setCarrierHz(carrier_.next());
    const float fc2 = pathX ? kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, g) : kRingHeelHz;
    carrier2_.setTarget(fc2);
    ring2_.setCarrierHz(carrier2_.next());

    // All legs run (primed); gains crossfade (~5 ms) on select. Slots
    // 0..3 are the normal voice (V3 doubles as DualWham-B stage 2, fed
    // from V2's output); 4..8 are the gesture legs (V6 -> V7 cascade
    // for DualWham-B, exactly 0 when no cross-voice gesture is active);
    // slot 9 is the dry leg (expression-off rendering only).
    const float o1 = v1_.process(x);
    const float o2 = v2_.process(x);
    const float ro = ring_.process(x);
    const float o3 = v3_.process(voice_ == Voice::DualWham ? o2 : x);
    const float o4 = v4_.process(x);
    const float o5 = v5_.process(x);
    const float r2o = ring2_.process(x);
    const float o6 = v6_.process(x);
    const float o7 = v7_.process(o6);
    const float wet = glideGain(0, mix) * o1 + glideGain(1, mix) * o2 + glideGain(2, mix) * ro
        + glideGain(3, mix) * o3 + glideGain(4, mix) * o4 + glideGain(5, mix) * o5
        + glideGain(6, mix) * r2o + glideGain(7, mix) * o6 + glideGain(8, mix) * o7
        + glideGain(9, mix) * x;
    return std::isfinite(wet) ? wet : x;
  }

private:
  enum class Phase
  {
    Idle = 0,
    Press = 1,   // To: sweeping to depth; From: engaging (mix -> 1)
    Sweep = 2,   // From only: converging to live normal over Time
    Release = 3  // returning (To: d -> 0; From: mix -> 0, d holds)
  };

  static constexpr float kRingHeelHz = 60.0f;
  static constexpr float kRingToeHz = 2000.0f;

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

  static bool validGestVoice(int v)
  {
    return v == kVoiceWhammy || v == kVoiceDual || v == kVoiceRing || v == kVoiceDualWham;
  }

  void disarm()
  {
    phase_ = Phase::Idle;
    activeSlot_ = -1;
    gestD_ = 0.0f;
    gestM_ = 0.0f;
    oneShot_ = false;
  }

  void advanceGesture()
  {
    if (phase_ == Phase::Idle)
      return;
    if (cfgMotion_ == GestureMotion::ToDepth)
    {
      gestD_ = advanceGest(gestD_, (phase_ == Phase::Release) ? 0.0f : 1.0f, timeStep_, retStep_);
      gestM_ = gestD_; // locked (the legacy dive shape: sweep + fade together)
      if (phase_ == Phase::Release && gestD_ == 0.0f)
        disarm();
      else if (oneShot_ && phase_ == Phase::Press && gestD_ == 1.0f)
        phase_ = Phase::Release; // one-shot: auto-return from the top
    }
    else // FromDepth
    {
      if (phase_ == Phase::Press)
      {
        gestM_ = advanceGest(gestM_, 1.0f, engageStep_, engageStep_);
        if (gestM_ == 1.0f)
          phase_ = Phase::Sweep;
      }
      else if (phase_ == Phase::Sweep)
      {
        gestD_ = advanceGest(gestD_, 0.0f, timeStep_, timeStep_);
        gestM_ = advanceGest(gestM_, 0.0f, timeStep_, timeStep_);
        if (gestD_ == 0.0f && gestM_ == 0.0f)
          disarm(); // converged exactly to live normal (one-shot included)
      }
      else // Release (early): fade the mix out, displacement holds
      {
        gestM_ = advanceGest(gestM_, 0.0f, retStep_, retStep_);
        if (gestM_ == 0.0f)
          disarm();
      }
    }
  }

  // Effective-voice match: same voice, and for Whammy the same polarity
  // (a Whammy-Down gesture over Whammy-Up normal takes Path X — the legs
  // differ, so polarity can never cancel silently). Dual/DualWham have
  // no direction; Ring shares its carrier leg across sweep directions.
  bool matchesNormal() const
  {
    if (cfgVoice_ != static_cast<int>(voice_))
      return false;
    return voice_ != Voice::Whammy || cfgDir_ == exprDir_;
  }
  bool pathE() const { return phase_ != Phase::Idle && matchesNormal(); }

  float base() const { return normalActive_ ? manual_ : 0.0f; } // expr-off parks at heel

  // Gesture position: displacement bound by the depth target. Path E
  // and Path X From anchor at the LIVE base (expression moves under
  // gestures stay continuous; release/converge always land on the
  // current normal state). Path X To sweeps heel -> target absolutely.
  float gestPos(bool pathX) const
  {
    if (pathX && cfgMotion_ == GestureMotion::ToDepth)
      return (cfgEnd_ == 1) ? ((gestD_ < cfgTgt_) ? gestD_ : cfgTgt_)
                            : (((1.0f - gestD_) > cfgTgt_) ? (1.0f - gestD_) : cfgTgt_);
    const float b = base();
    const float gv = (cfgEnd_ == 1) ? ((gestD_ < cfgTgt_) ? gestD_ : cfgTgt_)
                                   : (((1.0f - gestD_) > cfgTgt_) ? (1.0f - gestD_) : cfgTgt_);
    return (cfgEnd_ == 1) ? ((b > gv) ? b : gv) : ((b < gv) ? b : gv);
  }

  float posTarget() const
  {
    if (phase_ == Phase::Idle || !matchesNormal())
      return base(); // idle, or Path X (normal renders the base)
    return gestPos(false); // Path E: gesture overrides expression
  }

  // Voice -> [v1, v2, ring, v3down]; V3 doubles as DualWham-B stage 2
  // (fed from V2, gain 0.5). Slots 4..8 are the gesture legs
  // (voice-independent, driven by the gesture mix); slot 9 is the dry
  // leg (expression-off rendering only, always 0 here).
  void applyVoiceGains()
  {
    gainTgt_[0] = gainTgt_[1] = gainTgt_[2] = gainTgt_[3] = 0.0f;
    if (voice_ == Voice::Whammy)
      gainTgt_[(exprDir_ == GestureDir::Up) ? 0 : 3] = 1.0f;
    else if (voice_ == Voice::Dual)
    {
      gainTgt_[0] = 0.5f;
      gainTgt_[1] = 0.5f;
    }
    else if (voice_ == Voice::Ring)
      gainTgt_[2] = 1.0f;
    else // DualWham: A on V1, B via the V2 -> V3 cascade (V2 internal)
    {
      gainTgt_[0] = 0.5f;
      gainTgt_[3] = 0.5f;
    }
  }

  // Effective gain target. Normal slots gate on the expression section
  // (off parks them — except while a Path-E gesture plays the normal
  // legs); idle+enabled returns the voice target EXACTLY. Gesture slots
  // scale by the mix (exactly 0 without a cross-voice gesture). The dry
  // slot carries (1-m) only for expression-off idle/Path-X rendering.
  float effGainTgt(int i, float mix) const
  {
    if (i >= 9)
      return (normalActive_ || pathE_) ? 0.0f : (1.0f - mix);
    if (i >= 4)
      return (mix == 0.0f) ? 0.0f : mix * gestGainTgt_[i - 4];
    if (!normalActive_ && !pathE_)
      return 0.0f;
    if (mix == 0.0f)
      return gainTgt_[i];
    return (1.0f - mix) * gainTgt_[i];
  }
  float effGainTgt(int i) const
  {
    const bool pathX = (phase_ != Phase::Idle) && !pathE_;
    return effGainTgt(i, pathX ? gestM_ : 0.0f);
  }

  // NOTE (legacy behavior, kept for golden-exactness): near 1.0 the
  // float grid stalls this glide ~7e-6 short of the 1e-7 snap window, so
  // gains after a mix excursion settle NEAR-exact, not bit-exact
  // (inaudible: 0.00006 dB; next dip-bottom clear() re-snaps). Snapping
  // earlier would change the voice-select transient the goldens pin.
  float glideGain(int i, float mix)
  {
    const float t = effGainTgt(i, mix);
    float g = gain_[i];
    g += gainCoef_ * (t - g);
    if (std::fabs(t - g) < 1e-7f)
      g = t;
    gain_[i] = g;
    return g;
  }

  static float clampMs(float ms)
  {
    return (ms < 0.0f) ? 0.0f : ((ms > kMaxGestureMs) ? kMaxGestureMs : ms);
  }

  static float advanceGest(float g, float tgt, float riseStep, float retStep)
  {
    if (g < tgt)
    {
      g += riseStep; // linear full-scale ramp; step >= 1 snaps exactly
      if (g > tgt)
        g = tgt;
    }
    else if (g > tgt)
    {
      g -= retStep;
      if (g < tgt)
        g = tgt;
    }
    return g;
  }

  void updateGestSteps()
  {
    const float sr = static_cast<float>(sr_);
    timeStep_ = gestStep(cfgTimeMs_, sr);
    retStep_ = gestStep(cfgRetMs_, sr);
    engageStep_ = gestStep(kGestureEngageMs, sr);
    gainCoef_ = 1.0f - std::exp(-1.0f / (0.005f * sr)); // 5 ms
  }

  static float gestStep(float ms, float sr)
  {
    if (ms <= 0.0001f || sr <= 0.0f)
      return 1.0f; // instant: advance snaps to target (legacy exact)
    const float s = 1.0f / (ms * 0.001f * sr);
    return (s > 1.0f) ? 1.0f : s;
  }

  double sr_ = 48000.0;
  alien::AlienDoppler v1_, v2_, v3_; // v3 = down leg / DualWham-B stage 2
  alien::AlienDoppler v4_, v5_;      // gesture legs: up (>= 0) / down (<= 0)
  alien::AlienDoppler v6_, v7_;      // gesture DualWham-B cascade (>= 0)
  alien::AlienRing ring_, ring2_;    // normal / gesture ring
  alien::SmoothParam pos_, carrier_, carrier2_;
  Voice voice_ = Voice::Whammy;
  GestureDir exprDir_ = GestureDir::Up;
  float exprDepth_ = kMaxGestureDepthSt;
  float manual_ = 0.0f;
  bool normalActive_ = true; // expression section enable (router-owned)
  bool gestAllowed_ = true;  // gesture section enable (router-owned)
  bool pathE_ = false;       // latched per process()/clear()
  // Gesture config (block-boundary adopted) + runtime.
  int cfgVoice_ = 0;
  GestureDir cfgDir_ = GestureDir::Up;
  GestureMotion cfgMotion_ = GestureMotion::ToDepth;
  int cfgEnd_ = 1;
  float cfgTgt_ = 1.0f, cfgTimeMs_ = 50.0f, cfgRetMs_ = 100.0f;
  float gestGainTgt_[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  Phase phase_ = Phase::Idle;
  int activeSlot_ = -1;
  bool oneShot_ = false;
  float gestD_ = 0.0f, gestM_ = 0.0f;
  float timeStep_ = 1.0f, retStep_ = 1.0f, engageStep_ = 1.0f;
  float gainCoef_ = 1.0f;
  float gain_[10] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float gainTgt_[10] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace specfx
} // namespace tdm

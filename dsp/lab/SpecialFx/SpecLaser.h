#pragma once

// SpecLaser: expression-driven Laser engine (LAB, dsp/lab/SpecialFx).
// Independent take on the reference "laser" concept: one normalized
// expression position (0..1) drives preset-like per-voice parameter maps
// (heel/toe endpoints) under performance-gesture control. Five creative
// voices:
//
//   WhamUp:   single pitch voice 0 -> +12 st (classic toe-up octave)
//   Dual:     contrary-motion pair, V1 0 -> -12 st, V2 0 -> +12 st
//   Ring:     ring-mod carrier sweep 60 -> 2000 Hz (log)
//   Detune:   micro-voice chorus, spread + LFO depth from expression
//   WhamDown: single pitch voice 0 -> -12 st (continuous dive side)
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
// PERFORMANCE GESTURES (one engine, two paths, no stale state):
// A gesture config (voice/dir/depth/motion/time/return) drives a
// displacement ramp d and a mix ramp m. The depth target T is a POSITION
// in the gesture voice's map (full depth = the map end that sounds like
// the configured Direction; WhamDown flips because its pos 1 IS
// pitch-down).
//   Path E (gesture voice == normal voice): g = max/min(base, d-bound,
//   T) overrides the normal expression (the legacy Toe shape, BIT-EXACT
//   at Fast-Toe settings; golden-pinned). Covers Toe, Reverse Toe,
//   same-voice dives/sweeps. The live-base anchoring means changing
//   expression mid-gesture and releasing always land on the CURRENT
//   normal state — never a stored value, never a jump.
//   Path X (different voice): the normal voice renders the base on the
//   normal legs (V1/V2/V3/R1) while the gesture voice renders g on
//   DEDICATED gesture legs (V4 up / V5 down / R2 ring, parked at unity
//   and primed every sample, so a press retargets from dry — the legacy
//   Dive shape, bit-exact from heel), crossfaded by m. To-Depth sweeps
//   heel -> T absolutely; From-Depth anchors at the live base so the
//   sweep converges exactly to the current normal state.
// To Depth: press sweeps to T over Time, hold, release returns over
// Return. From Depth: press snaps to T behind a 10 ms engage fade
// (target snap, slew+fade masked — never a discontinuous DSP direction
// change), sweeps back to live normal over Time, converging exactly;
// early release fades out over Return. One-shot auto-releases.
// Newest trigger wins; routines are per-sample continuous, so
// interruption/retrigger/early-release can't click. Detune is not a
// supported gesture voice (press ignored, documented); a normal-voice
// change mid-gesture re-paths live (the router dip masks it).
// Gesture slew (musical portamento) is SEPARATE from the 5 ms
// anti-zipper smoother + doppler 30 ms slew (click prevention), which
// only round the gesture corners.
//
// A doppler leg's target must never change SIGN mid-engagement (the tap
// mirror jumps ~0.4): gesture legs have fixed sign roles (V4 >= 0,
// V5 <= 0), unity parking engages the bypass fade, and normal-leg sign
// flips only happen behind router dips.
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
    WhamUp = 0,
    Dual = 1,
    Ring = 2,
    Detune = 3,
    WhamDown = 4
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
    ring_.reset(sr);
    ring_.setMix(1.0f);
    ring2_.reset(sr);
    ring2_.setMix(1.0f);
    lfo_.reset(sr, 1.0f);
    pos_.reset(sr, 5.0f, 0.0f);
    pos_.snap(0.0f);
    carrier_.reset(sr, 5.0f, kRingHeelHz);
    carrier_.snap(kRingHeelHz);
    carrier2_.reset(sr, 5.0f, kRingHeelHz);
    carrier2_.snap(kRingHeelHz);
    for (int i = 0; i < 8; ++i)
    {
      gain_[i] = 0.0f;
      gainTgt_[i] = 0.0f;
    }
    applyVoiceGains();
    for (int i = 0; i < 8; ++i)
      gain_[i] = (i < 5) ? gainTgt_[i] : 0.0f;
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
    pos_.snap(posTarget());
    for (int i = 0; i < 8; ++i)
      gain_[i] = effGainTgt(i); // snap incl. live gesture (macro is 0 here)
  }

  void setVoice(Voice v)
  {
    voice_ = v;
    applyVoiceGains();
  }
  void setExpression(float e) { manual_ = clamp01(e); }

  // Gesture config: pushed per block (live edits adopt mid-gesture).
  void setGestureConfig(const GestureConfig& c)
  {
    cfgVoice_ = c.voice;
    cfgMotion_ = c.motion;
    cfgEnd_ = gestureDepthEnd(c.voice == static_cast<int>(Voice::WhamDown), c.dir);
    cfgTgt_ = gestureTargetPos(cfgEnd_, c.depthSt);
    cfgTimeMs_ = clampMs(c.timeMs);
    cfgRetMs_ = clampMs(c.retMs);
    gestGainTgt_[0] = gestGainTgt_[1] = gestGainTgt_[2] = 0.0f;
    if (c.voice == static_cast<int>(Voice::WhamUp))
      gestGainTgt_[0] = 1.0f;
    else if (c.voice == static_cast<int>(Voice::WhamDown))
      gestGainTgt_[1] = 1.0f;
    else if (c.voice == static_cast<int>(Voice::Dual))
    {
      gestGainTgt_[0] = 0.5f;
      gestGainTgt_[1] = 0.5f;
    }
    else if (c.voice == static_cast<int>(Voice::Ring))
      gestGainTgt_[2] = 1.0f;
    updateGestSteps();
  }
  // Gesture commands (block boundary, by stable slot ID; newest wins).
  // Detune/unknown voices are ignored (unsupported, documented).
  void pressGesture(int slot)
  {
    if (!validGestVoice(cfgVoice_))
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
    // Path X (cross-voice): normal renders the base, gesture renders g
    // on the dedicated legs, crossfaded by the EFFECTIVE mix (gated by
    // path so Path E never touches the mix). Path changes only on voice
    // switches, which the router dip masks.
    const bool pathX = (phase_ != Phase::Idle) && (cfgVoice_ != static_cast<int>(voice_));
    const float mix = pathX ? gestM_ : 0.0f;

    const float pos = pos_.next(); // 5 ms smoothed, exact settle
    pos_.setTarget(posTarget());

    // Per-voice maps from expression (normal legs; gesture legs below).
    const float st1 = 12.0f * pos;  // whammy toe-up octave
    const float stD1 = -12.0f * pos; // dual contrary pair
    const float stD2 = 12.0f * pos;
    float n1 = 0.0f, n2 = 0.0f;
    if (voice_ == Voice::WhamUp)
      n1 = st1;
    else if (voice_ == Voice::Dual)
    {
      n1 = stD1;
      n2 = stD2;
    }
    else if (voice_ == Voice::Detune)
    {
      // Chorus: symmetric micro-voices around a spread center, LFO in
      // opposite phase. Heel = subtle doubling, toe = wide wobble.
      const float rate = 0.15f * std::pow(4.0f / 0.15f, pos); // 0.15..4 Hz log
      lfo_.setFreq(rate);
      float s, c;
      lfo_.next(s, c);
      (void)c;
      const float center = (6.0f + 18.0f * pos) / 100.0f; // cents -> st
      const float dev = (2.0f + 28.0f * pos) / 100.0f;
      n1 = center + s * dev;
      n2 = -(center + s * dev);
    }
    // else Ring/WhamDown: V1/V2 park at unity (bit-exact dry legs)
    v1_.setSemitones(n1);
    v2_.setSemitones(n2);
    // V3: the dedicated downward leg, now expression-driven for the
    // WhamDown voice (monotonic 0 -> -12 map, unity-parked otherwise).
    v3_.setSemitones(voice_ == Voice::WhamDown ? -12.0f * pos : 0.0f);
    // Gesture legs (Path X): the SAME maps at the gesture position g
    // (depth lives in the target, so signs stay fixed: V4 >= 0, V5 <= 0
    // — no mid-engagement sign flips, ever). Parked at unity otherwise.
    // Path X To sweeps heel -> target absolutely (the legacy dive shape:
    // depth 0 crossfades to dry); Path E and Path X From anchor at the
    // live base (override / converge semantics).
    const float g = gestPos(pathX);
    v4_.setSemitones(pathX ? 12.0f * g : 0.0f);
    v5_.setSemitones(pathX ? -12.0f * g : 0.0f);
    const float fc = kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, pos); // log sweep
    carrier_.setTarget(fc);
    ring_.setCarrierHz(carrier_.next());
    const float fc2 = pathX ? kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, g) : kRingHeelHz;
    carrier2_.setTarget(fc2);
    ring2_.setCarrierHz(carrier2_.next());

    // All legs run (primed); gains crossfade (~5 ms) on select.
    // Detune rides the doppler legs (slot 3 scales their mean); slots
    // 5..7 are the gesture legs (exactly 0 when no cross-voice gesture
    // is active: wet sum exact).
    const float o1 = v1_.process(x);
    const float o2 = v2_.process(x);
    const float ro = ring_.process(x);
    const float o3 = v3_.process(x);
    const float o4 = v4_.process(x);
    const float o5 = v5_.process(x);
    const float r2o = ring2_.process(x);
    const float wet = glideGain(0, mix) * o1 + glideGain(1, mix) * o2 + glideGain(2, mix) * ro
        + glideGain(3, mix) * 0.5f * (o1 + o2) + glideGain(4, mix) * o3 + glideGain(5, mix) * o4
        + glideGain(6, mix) * o5 + glideGain(7, mix) * r2o;
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
    return v == static_cast<int>(Voice::WhamUp) || v == static_cast<int>(Voice::Dual)
        || v == static_cast<int>(Voice::Ring) || v == static_cast<int>(Voice::WhamDown);
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

  // Gesture position: displacement bound by the depth target. Path E
  // and Path X From anchor at the LIVE base (expression moves under
  // gestures stay continuous; release/converge always land on the
  // current normal state). Path X To sweeps heel -> target absolutely.
  float gestPos(bool pathX) const
  {
    if (pathX && cfgMotion_ == GestureMotion::ToDepth)
      return (cfgEnd_ == 1) ? ((gestD_ < cfgTgt_) ? gestD_ : cfgTgt_)
                            : (((1.0f - gestD_) > cfgTgt_) ? (1.0f - gestD_) : cfgTgt_);
    const float gv = (cfgEnd_ == 1) ? ((gestD_ < cfgTgt_) ? gestD_ : cfgTgt_)
                                   : (((1.0f - gestD_) > cfgTgt_) ? (1.0f - gestD_) : cfgTgt_);
    return (cfgEnd_ == 1) ? ((manual_ > gv) ? manual_ : gv) : ((manual_ < gv) ? manual_ : gv);
  }

  float posTarget() const
  {
    if (phase_ == Phase::Idle || cfgVoice_ != static_cast<int>(voice_))
      return manual_; // idle, or Path X (normal renders the base)
    return gestPos(false); // Path E: gesture overrides expression
  }

  // Voice -> [v1, v2, ring, detuneLegs, v3down]; detune sets slot 3 only
  // (process scales the doppler-leg mean through it). Slots 5..7 are the
  // gesture legs (voice-independent, driven by the gesture mix).
  void applyVoiceGains()
  {
    gainTgt_[0] = gainTgt_[1] = gainTgt_[2] = gainTgt_[3] = gainTgt_[4] = 0.0f;
    if (voice_ == Voice::WhamUp)
      gainTgt_[0] = 1.0f;
    else if (voice_ == Voice::Dual)
    {
      gainTgt_[0] = 0.5f;
      gainTgt_[1] = 0.5f;
    }
    else if (voice_ == Voice::Ring)
      gainTgt_[2] = 1.0f;
    else if (voice_ == Voice::WhamDown)
      gainTgt_[4] = 1.0f;
    else
      gainTgt_[3] = 1.0f; // detune: legs at 0.5 via process mix
  }

  // Effective gain target: idle mix returns the voice target EXACTLY
  // (gesture slots contribute 0); held cross-voice gestures scale the
  // normal voice by (1-m) and the gesture voice by m.
  float effGainTgt(int i, float mix) const
  {
    if (i >= 5)
      return (mix == 0.0f) ? 0.0f : mix * gestGainTgt_[i - 5];
    if (mix == 0.0f)
      return gainTgt_[i];
    return (1.0f - mix) * gainTgt_[i];
  }
  float effGainTgt(int i) const
  {
    const bool pathX = (phase_ != Phase::Idle) && (cfgVoice_ != static_cast<int>(voice_));
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
  alien::AlienDoppler v1_, v2_, v3_; // v3 = dedicated downward leg (WhamDown)
  alien::AlienDoppler v4_, v5_;      // gesture legs: up (>= 0) / down (<= 0)
  alien::AlienRing ring_, ring2_;    // normal / gesture ring
  alien::QuadOsc lfo_;
  alien::SmoothParam pos_, carrier_, carrier2_;
  Voice voice_ = Voice::WhamUp;
  float manual_ = 0.0f;
  // Gesture config (block-boundary adopted) + runtime.
  int cfgVoice_ = 0;
  GestureMotion cfgMotion_ = GestureMotion::ToDepth;
  int cfgEnd_ = 1;
  float cfgTgt_ = 1.0f, cfgTimeMs_ = 50.0f, cfgRetMs_ = 100.0f;
  float gestGainTgt_[3] = {1.0f, 0.0f, 0.0f};
  Phase phase_ = Phase::Idle;
  int activeSlot_ = -1;
  bool oneShot_ = false;
  float gestD_ = 0.0f, gestM_ = 0.0f;
  float timeStep_ = 1.0f, retStep_ = 1.0f, engageStep_ = 1.0f;
  float gainCoef_ = 1.0f;
  float gain_[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float gainTgt_[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace specfx
} // namespace tdm

#pragma once

// SpecLaser: expression-driven Laser engine (LAB, dsp/lab/SpecialFx).
// Independent take on the reference "laser" concept: one normalized
// expression position (0..1) drives preset-like per-voice parameter maps
// (heel/toe endpoints), sourced from Manual control or an envelope
// follower, with momentary Toe (up) and Divebomb (down) gestures. Four
// experimental voices:
//
//   Whammy: single pitch voice 0 -> +12 st (classic toe-up octave)
//   Dual:   contrary-motion pair, V1 0 -> -12 st, V2 0 -> +12 st
//   Ring:   ring-mod carrier sweep 60 -> 2000 Hz (log)
//   Detune: micro-voice chorus, spread + LFO depth from expression
//
// Pitch voices reuse AlienDoppler (masked-wrap variable delay): slope
// changes only bend the ramp, so expression sweeps are continuous by
// construction, polyphonic-safe (no pitch detection), and cheap. The
// documented tradeoff vs a WSOLA engine (GuitarTranspose): static
// accuracy degrades toward the extremes and is INPUT-DEPENDENT
// (measured on raw sines: -12 renders -10.5..-16 depending on f0;
// +12 renders ~+11.2; below -12 the pitch smears toward mush, so dive
// depth caps at 12). For expressive sweeps ("warped harmonies") that
// character is accepted (hardware likes the aggressive artificial
// pitch); pitch-sync stagger is the known follow-up if tighter
// intervals are ever wanted. Production GuitarTranspose was
// deliberately NOT used: 16 ms latency, upward-shift limitations, and
// resync warble under continuous sweeps make it the wrong primitive
// here. Ring reuses AlienRing (carrier is NOT smoothed there, so this
// engine glides it per sample).
//
// Gestures (performance controls, all continuous by construction):
//   Toe:  momentary button -> toeGest 0..1 at Toe Rise/Return rates;
//         expression = max(base, toeGest) in Manual mode (ignored in
//         Envelope, reference semantics). Rise/Return 0 = instant =
//         the legacy fast stab, BIT-EXACT at defaults (golden-pinned).
//   Dive: momentary button -> diveGest 0..1 at Dive Time/Return rates;
//         dedicated V3 doppler leg sweeps 0 -> -depth (monotonic, so it
//         never crosses zero mid-signal; only the press/release unity
//         engagement flips direction, masked by the bypass fade) while
//         the voice crossfades toward it. Genuine negative pitch in
//         EVERY voice (uniform, predictable), both control modes. V1/V2
//         keep their normal targets throughout (primed for release).
// Precedence: base < Toe < Dive. Both gestures track their buttons
// independently at all times; the OUTPUT is a continuous function of
// (base, toeGest, diveGest), so every press/release/order restores
// smoothly at the released gesture's own rate — never a stored value
// that can go stale, never a jump. Both-held: dive wins; releasing it
// reveals the live toe underneath (slewed).
// Gesture slew (20..1000 ms musical portamento) is SEPARATE from the
// 5 ms anti-zipper smoother + doppler 30 ms slew (click prevention),
// which only round the gesture corners.
//
// All voices process every sample (doppler lines stay primed even at
// zero gain); voice selection only retargets gains, so no voice switch
// can expose stale delay audio. Enable/voice/insert transitions
// (dip + clear + history preroll) live in the DevSpecFx taps, not here.
//
// Realtime contract: setters are plain (the router pushes shared
// atomics per block, later also MIDI/CoreMIDI-friendly: gestures are
// bools + ms, never AppKit-coupled); process() allocates nothing,
// locks nothing. reset()/clear() are off-RT or dip-bottom only.

#include <cmath>

#include "dsp/lab/Alien/AlienDoppler.h"
#include "dsp/lab/Alien/AlienDsp.h"
#include "dsp/lab/Alien/AlienRing.h"

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
    Detune = 3
  };
  enum class Control
  {
    Manual = 0,
    Envelope = 1
  };

  // Follower calibration: detector pre-gain = 0.5 * 16^sensitivity.
  // Default 0.6 -> ~2.5x: typical DI peaks (~0.3) reach full sweep on
  // hard hits; post-amp taps (hotter) turn Sensitivity down.
  static constexpr float kDefaultSpeed = 0.6f;
  static constexpr float kDefaultSensitivity = 0.6f;
  // Gesture defaults: toe 0/0 ms = instant = legacy fast stab (exact);
  // dive 300/200 ms = bar-throw down, spring back up.
  static constexpr float kDefaultToeRiseMs = 0.0f;
  static constexpr float kDefaultToeReturnMs = 0.0f;
  static constexpr float kDefaultDiveTimeMs = 300.0f;
  static constexpr float kDefaultDiveReturnMs = 200.0f;
  static constexpr float kDefaultDiveDepthSt = 12.0f;
  static constexpr float kMaxDiveDepthSt = 12.0f; // validated ceiling (probe)
  static constexpr float kMaxGestureMs = 2000.0f;

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    v1_.reset(sr);
    v2_.reset(sr);
    v3_.reset(sr);
    ring_.reset(sr);
    ring_.setMix(1.0f);
    lfo_.reset(sr, 1.0f);
    pos_.reset(sr, 5.0f, 0.0f);
    pos_.snap(0.0f);
    carrier_.reset(sr, 5.0f, kRingHeelHz);
    carrier_.snap(kRingHeelHz);
    for (int i = 0; i < 5; ++i)
    {
      gain_[i] = 0.0f;
      gainTgt_[i] = 0.0f;
    }
    applyVoiceGains();
    for (int i = 0; i < 5; ++i)
      gain_[i] = (i == 4) ? 0.0f : gainTgt_[i];
    env_ = 0.0f;
    toeGest_ = 0.0f;
    diveGest_ = 0.0f;
    updateFollowerCoefs();
    updateGestureSteps();
  }

  // RT-safe state clear for dip-bottom handoffs: flush delay audio and
  // the follower; keep oscillator phases continuous. Voice gains snap
  // (macro is 0 here, so inaudible); call setVoice() first.
  void clear()
  {
    v1_.clear();
    v2_.clear();
    v3_.clear();
    env_ = 0.0f;
    pos_.snap(posTarget());
    for (int i = 0; i < 5; ++i)
      gain_[i] = effGainTgt(i); // snap incl. live dive (macro is 0 here)
  }

  void setVoice(Voice v)
  {
    voice_ = v;
    applyVoiceGains();
  }
  void setControl(Control c) { control_ = c; }
  void setExpression(float e) { manual_ = clamp01(e); }
  void setSpeed(float s)
  {
    speed_ = clamp01(s);
    updateFollowerCoefs();
  }
  void setSensitivity(float s) { sens_ = clamp01(s); }
  void setTrigger(bool t) { trig_ = t; } // momentary toe button (Manual only)
  void setToeRiseMs(float ms)
  {
    toeRiseMs_ = clampMs(ms);
    updateGestureSteps();
  }
  void setToeReturnMs(float ms)
  {
    toeReturnMs_ = clampMs(ms);
    updateGestureSteps();
  }
  void setDive(bool d) { dive_ = d; } // momentary divebomb button (both modes)
  void setDiveDepthSt(float st)
  {
    diveDepth_ = (st < 0.0f) ? 0.0f : ((st > kMaxDiveDepthSt) ? kMaxDiveDepthSt : st);
  }
  void setDiveTimeMs(float ms)
  {
    diveTimeMs_ = clampMs(ms);
    updateGestureSteps();
  }
  void setDiveReturnMs(float ms)
  {
    diveReturnMs_ = clampMs(ms);
    updateGestureSteps();
  }

  Voice voice() const { return voice_; }
  float expressionPos() const { return pos_.value(); } // smoothed 0..1 (telemetry)
  float envelopeLevel() const { return env_; }         // raw follower (telemetry)
  float toeGesture() const { return toeGest_; }        // gesture values (telemetry)
  float diveGesture() const { return diveGest_; }
  int latencySamples() const { return v1_.latencySamples(); } // doppler bound (stable)

  float process(float x)
  {
    // Sanitize first: a NaN/Inf sample would poison the follower and the
    // smoothed position permanently (comparisons go false, state sticks).
    // Upstream garbage becomes a zero, never a wedged effect.
    if (!std::isfinite(x))
      x = 0.0f;
    // Envelope follower: peak detector with speed-scaled attack/release
    // on sensitivity-gained input.
    const float sensGain = 0.5f * std::pow(16.0f, sens_);
    const float inst = std::fabs(x) * sensGain;
    env_ += ((inst > env_) ? atk_ : rel_) * (inst - env_);
    env_ = alien::alienSnap(env_);

    // Gestures track their buttons independently (linear full-scale
    // ramps at rise/return rates; 0 ms = snap). Application below is a
    // continuous function of (base, toe, dive): precedence without jumps.
    toeGest_ = advanceGest(toeGest_, trig_ ? 1.0f : 0.0f, toeRiseStep_, toeReturnStep_);
    diveGest_ = advanceGest(diveGest_, dive_ ? 1.0f : 0.0f, diveTimeStep_, diveReturnStep_);

    const float pos = pos_.next(); // 5 ms smoothed, exact settle
    pos_.setTarget(posTarget());

    // Per-voice maps from expression (normal V1/V2; dive bends V1 below).
    const float st1 = 12.0f * pos;  // whammy toe-up octave
    const float stD1 = -12.0f * pos; // dual contrary pair
    const float stD2 = 12.0f * pos;
    float n1 = 0.0f, n2 = 0.0f;
    if (voice_ == Voice::Whammy)
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
    // else Ring voice: dopplers park at unity (bit-exact dry legs)
    v1_.setSemitones(n1);
    v2_.setSemitones(n2);
    // Dive leg V3: dedicated 0 -> -depth trajectory (monotonic, so it
    // NEVER crosses zero mid-signal; only the press/release unity
    // engagement flips direction, masked by the doppler's bypass fade).
    // V1/V2 keep their normal targets throughout (primed for release).
    v3_.setSemitones(diveGest_ == 0.0f ? 0.0f : diveGest_ * -diveDepth_);
    const float fc = kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, pos); // log sweep
    carrier_.setTarget(fc);
    ring_.setCarrierHz(carrier_.next());

    // All voices run (primed); gains crossfade (~5 ms) on select.
    // Detune rides the doppler legs (slot 3 scales their mean); slot 4
    // is the V3 dive leg (exactly 0 when dive is idle: wet sum exact).
    const float o1 = v1_.process(x);
    const float o2 = v2_.process(x);
    const float ro = ring_.process(x);
    const float o3 = v3_.process(x);
    const float wet = glideGain(0) * o1 + glideGain(1) * o2 + glideGain(2) * ro
        + glideGain(3) * 0.5f * (o1 + o2) + glideGain(4) * o3;
    return std::isfinite(wet) ? wet : x;
  }

private:
  static constexpr float kRingHeelHz = 60.0f;
  static constexpr float kRingToeHz = 2000.0f;

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

  float posTarget() const
  {
    if (control_ == Control::Envelope)
      return clamp01(env_); // toe ignored (reference semantics)
    return (manual_ > toeGest_) ? manual_ : toeGest_; // toe pushes up from base
  }

  // Voice -> [v1Gain, v2Gain, ringGain, detuneLegGain]; detune sets
  // slot 3 only (process scales the doppler-leg mean through it).
  // Slot 4 is the V3 dive leg (voice-independent, driven by diveGest).
  void applyVoiceGains()
  {
    gainTgt_[0] = gainTgt_[1] = gainTgt_[2] = gainTgt_[3] = 0.0f;
    if (voice_ == Voice::Whammy)
      gainTgt_[0] = 1.0f;
    else if (voice_ == Voice::Dual)
    {
      gainTgt_[0] = 0.5f;
      gainTgt_[1] = 0.5f;
    }
    else if (voice_ == Voice::Ring)
      gainTgt_[2] = 1.0f;
    else
      gainTgt_[3] = 1.0f; // detune: legs at 0.5 via process mix
  }

  // Effective gain target: idle dive returns the voice target
  // EXACTLY (slot 4 contributes 0); held dives crossfade the voice
  // toward the V3 dive leg (slots 0..3 scaled by (1-d), slot 4 = d).
  float effGainTgt(int i) const
  {
    if (i == 4)
      return diveGest_;
    if (diveGest_ == 0.0f)
      return gainTgt_[i];
    return (1.0f - diveGest_) * gainTgt_[i];
  }

  float glideGain(int i)
  {
    const float t = effGainTgt(i);
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

  void updateGestureSteps()
  {
    const float sr = static_cast<float>(sr_);
    toeRiseStep_ = gestStep(toeRiseMs_, sr);
    toeReturnStep_ = gestStep(toeReturnMs_, sr);
    diveTimeStep_ = gestStep(diveTimeMs_, sr);
    diveReturnStep_ = gestStep(diveReturnMs_, sr);
  }

  static float gestStep(float ms, float sr)
  {
    if (ms <= 0.0001f || sr <= 0.0f)
      return 1.0f; // instant: advance snaps to target (legacy exact)
    const float s = 1.0f / (ms * 0.001f * sr);
    return (s > 1.0f) ? 1.0f : s;
  }

  void updateFollowerCoefs()
  {
    // Speed 0..1 -> attack 100..1 ms, release 1000..30 ms (log).
    const float atkMs = 100.0f * std::pow(0.01f, speed_);
    const float relMs = 1000.0f * std::pow(0.03f, speed_);
    atk_ = 1.0f - std::exp(-1.0f / (atkMs * 0.001f * static_cast<float>(sr_)));
    rel_ = 1.0f - std::exp(-1.0f / (relMs * 0.001f * static_cast<float>(sr_)));
    gainCoef_ = 1.0f - std::exp(-1.0f / (0.005f * static_cast<float>(sr_))); // 5 ms
  }

  double sr_ = 48000.0;
  alien::AlienDoppler v1_, v2_, v3_; // v3 = dedicated dive leg (parks at unity)
  alien::AlienRing ring_;
  alien::QuadOsc lfo_;
  alien::SmoothParam pos_, carrier_;
  Voice voice_ = Voice::Whammy;
  Control control_ = Control::Manual;
  float manual_ = 0.0f, speed_ = kDefaultSpeed, sens_ = kDefaultSensitivity;
  bool trig_ = false, dive_ = false;
  float toeRiseMs_ = kDefaultToeRiseMs, toeReturnMs_ = kDefaultToeReturnMs;
  float diveTimeMs_ = kDefaultDiveTimeMs, diveReturnMs_ = kDefaultDiveReturnMs;
  float diveDepth_ = kDefaultDiveDepthSt;
  float toeGest_ = 0.0f, diveGest_ = 0.0f;
  float toeRiseStep_ = 1.0f, toeReturnStep_ = 1.0f;
  float diveTimeStep_ = 1.0f, diveReturnStep_ = 1.0f;
  float env_ = 0.0f, atk_ = 1.0f, rel_ = 1.0f, gainCoef_ = 1.0f;
  float gain_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float gainTgt_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace specfx
} // namespace tdm

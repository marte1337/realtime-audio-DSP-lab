#pragma once

// SpecLaser: expression-driven Laser engine (LAB, dsp/lab/SpecialFx).
// Independent take on the reference "laser" concept: one normalized
// expression position (0..1) drives preset-like per-voice parameter maps
// (heel/toe endpoints), sourced from Manual control or an envelope
// follower, with a momentary Toe trigger. Four experimental voices:
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
// accuracy sags at extremes (~-1 st at -12 st, ~+0.5 st at +12 st),
// chorus-y overlap, and direction flips jump taps. For expressive
// sweeps ("warped harmonies") that character is acceptable; pitch-sync
// stagger is the known follow-up if hardware wants tighter intervals.
// Production GuitarTranspose was deliberately NOT used: 16 ms latency,
// upward-shift limitations, and resync warble under continuous sweeps
// make it the wrong primitive here. Ring reuses AlienRing (carrier is
// NOT smoothed there, so this engine glides it per sample).
//
// All voices process every sample (doppler lines stay primed even at
// zero gain); voice selection only retargets gains, so no voice switch
// can expose stale delay audio. Enable/voice/insert transitions
// (dip + clear + history preroll) live in the DevSpecFx taps, not here.
//
// Realtime contract: setters are plain (the router pushes shared
// atomics per block); process() allocates nothing, locks nothing.
// reset()/clear() are off-RT or dip-bottom only.

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

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    v1_.reset(sr);
    v2_.reset(sr);
    ring_.reset(sr);
    ring_.setMix(1.0f);
    lfo_.reset(sr, 1.0f);
    pos_.reset(sr, 5.0f, 0.0f);
    pos_.snap(0.0f);
    carrier_.reset(sr, 5.0f, kRingHeelHz);
    carrier_.snap(kRingHeelHz);
    for (int i = 0; i < 4; ++i)
    {
      gain_[i] = 0.0f;
      gainTgt_[i] = 0.0f;
    }
    applyVoiceGains();
    for (int i = 0; i < 4; ++i)
      gain_[i] = gainTgt_[i];
    env_ = 0.0f;
    updateFollowerCoefs();
  }

  // RT-safe state clear for dip-bottom handoffs: flush delay audio and
  // the follower; keep oscillator phases continuous. Voice gains snap
  // (macro is 0 here, so inaudible); call setVoice() first.
  void clear()
  {
    v1_.clear();
    v2_.clear();
    env_ = 0.0f;
    pos_.snap(posTarget());
    for (int i = 0; i < 4; ++i)
      gain_[i] = gainTgt_[i];
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
  void setTrigger(bool t) { trig_ = t; } // momentary toe (Manual only)

  Voice voice() const { return voice_; }
  float expressionPos() const { return pos_.value(); } // smoothed 0..1 (telemetry)
  float envelopeLevel() const { return env_; }         // raw follower (telemetry)
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

    const float pos = pos_.next(); // 5 ms smoothed, exact settle
    pos_.setTarget(posTarget());

    // Per-voice maps from expression (all computed; gains select).
    const float st1 = 12.0f * pos;  // whammy toe-up octave
    const float stD1 = -12.0f * pos; // dual contrary pair
    const float stD2 = 12.0f * pos;
    if (voice_ == Voice::Whammy)
    {
      v1_.setSemitones(st1);
      v2_.setSemitones(0.0f);
    }
    else if (voice_ == Voice::Dual)
    {
      v1_.setSemitones(stD1);
      v2_.setSemitones(stD2);
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
      v1_.setSemitones(center + s * dev);
      v2_.setSemitones(-(center + s * dev));
    }
    else // Ring voice: dopplers park at unity (bit-exact dry legs)
    {
      v1_.setSemitones(0.0f);
      v2_.setSemitones(0.0f);
    }
    const float fc = kRingHeelHz * std::pow(kRingToeHz / kRingHeelHz, pos); // log sweep
    carrier_.setTarget(fc);
    ring_.setCarrierHz(carrier_.next());

    // All voices run (primed); gains crossfade (~5 ms) on select.
    // Detune rides the doppler legs (slot 3 scales their mean).
    const float o1 = v1_.process(x);
    const float o2 = v2_.process(x);
    const float ro = ring_.process(x);
    const float wet = glideGain(0) * o1 + glideGain(1) * o2 + glideGain(2) * ro
        + glideGain(3) * 0.5f * (o1 + o2);
    return std::isfinite(wet) ? wet : x;
  }

private:
  static constexpr float kRingHeelHz = 60.0f;
  static constexpr float kRingToeHz = 2000.0f;

  static float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

  float posTarget() const
  {
    if (control_ == Control::Envelope)
      return clamp01(env_);
    return trig_ ? 1.0f : manual_; // momentary toe in Manual only
  }

  // Voice -> [v1Gain, v2Gain, ringGain, detuneLegGain]; detune sets
  // slot 3 only (process scales the doppler-leg mean through it).
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

  float glideGain(int i)
  {
    const float t = gainTgt_[i];
    float g = gain_[i];
    g += gainCoef_ * (t - g);
    if (std::fabs(t - g) < 1e-7f)
      g = t;
    gain_[i] = g;
    return g;
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
  alien::AlienDoppler v1_, v2_;
  alien::AlienRing ring_;
  alien::QuadOsc lfo_;
  alien::SmoothParam pos_, carrier_;
  Voice voice_ = Voice::Whammy;
  Control control_ = Control::Manual;
  float manual_ = 0.0f, speed_ = kDefaultSpeed, sens_ = kDefaultSensitivity;
  bool trig_ = false;
  float env_ = 0.0f, atk_ = 1.0f, rel_ = 1.0f, gainCoef_ = 1.0f;
  float gain_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float gainTgt_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace specfx
} // namespace tdm

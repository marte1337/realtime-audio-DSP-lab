#pragma once

// AlienWarp: delay with a transformation inside the feedback loop (LAB,
// dsp/lab/Alien). Each repeat passes through the transform once more, so
// repeats evolve: repeat k is T^k of the input.
//
// Mode FreqWarp: fixed-Hz frequency shift per repeat (AlienShift core:
// +100 Hz gives +100/+200/+300... increasingly inharmonic). Mode
// PitchWarp: musical-interval Doppler shift per repeat (AlienDoppler:
// +3 st gives +3/+6/+9...). Loop extras: one-pole damping LP (~5 kHz,
// stops up-shifted highs piling at Nyquist), fixed 60 Hz HP (stops
// down-shifted lows piling at DC), unity-slope soft saturator
// x/(1+|x|/3) (repeats distort progressively; small-signal gain exactly
// 1, so loop gain stays fb < 1 and the loop provably cannot run away),
// feedback cap 0.9.
//
// Latency accounting: the in-loop transform adds its own latency to
// every pass (FreqWarp: 511 samples; PitchWarp: ~35 ms), so repeat k
// lands at k*(timeMs + in-loop ms), not k*timeMs. Musically this just
// stretches the repeat spacing; it is measured and documented, not
// compensated (compensating would cost lookahead).
//
// Params: timeMs (40..1500), feedback (0..0.9), mode, shiftHz
// (-200..+200, freq mode), semitones (-12..+12, pitch mode), mix.
// Mode/shift changes are not masked: the circulating signal bends to
// the new transform (continuous), only time jumps... time itself is
// smoothed (tape-like, house Delay precedent). mix 0 = bit-exact dry.
// reset(sr) off-RT; process() RT-safe, deterministic, finite-out.

#include "dsp/lab/Alien/AlienDoppler.h"
#include "dsp/lab/Alien/AlienDsp.h"
#include "dsp/lab/Alien/AlienShift.h"

namespace tdm
{
namespace alien
{

class AlienWarp
{
public:
  enum class Mode
  {
    FreqWarp = 0, // fixed-Hz shift per repeat
    PitchWarp = 1 // interval shift per repeat
  };

  static constexpr float kMinTimeMs = 40.0f;
  static constexpr float kMaxTimeMs = 1500.0f;
  static constexpr float kMaxFeedback = 0.9f;
  static constexpr float kMinShiftHz = -200.0f;
  static constexpr float kMaxShiftHz = 200.0f;

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    line_.reset(sr, kMaxTimeMs + 50.0f);
    shifter_.reset(sr);
    shifter_.setShiftHz(shiftHzTgt_);
    shifter_.setMix(1.0f);
    doppler_.reset(sr);
    doppler_.setSemitones(stTgt_);
    time_.reset(sr, 30.0f, timeMsTgt_); // tape-like time moves
    time_.snap(timeMsTgt_);
    fb_.reset(sr, 20.0f, fbTgt_);
    fb_.snap(fbTgt_);
    mix_.reset(sr, 10.0f, mixTgt_);
    mix_.snap(mixTgt_);
    dampLp_ = 0.0f;
    hpLp_ = 0.0f;
  }

  void setMode(Mode m) { mode_ = m; }
  void setTimeMs(float ms)
  {
    timeMsTgt_ = (ms < kMinTimeMs) ? kMinTimeMs : ((ms > kMaxTimeMs) ? kMaxTimeMs : ms);
    time_.setTarget(timeMsTgt_);
  }
  void setFeedback(float v)
  {
    fbTgt_ = (v < 0.0f) ? 0.0f : ((v > kMaxFeedback) ? kMaxFeedback : v);
    fb_.setTarget(fbTgt_);
  }
  void setShiftHz(float hz)
  {
    shiftHzTgt_ =
        (hz < kMinShiftHz) ? kMinShiftHz : ((hz > kMaxShiftHz) ? kMaxShiftHz : hz);
    shifter_.setShiftHz(shiftHzTgt_);
  }
  void setSemitones(float st) // clamped by the Doppler core
  {
    stTgt_ = st;
    doppler_.setSemitones(st);
  }
  void setMix(float v)
  {
    mixTgt_ = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    mix_.setTarget(mixTgt_);
  }
  // RT-safe state clear (no allocation). Call at a dip bottom.
  void clear()
  {
    line_.clear();
    shifter_.clear();
    doppler_.clear();
    time_.snap(timeMsTgt_);
    fb_.snap(fbTgt_);
    mix_.snap(mixTgt_);
    dampLp_ = 0.0f;
    hpLp_ = 0.0f;
  }

  float process(float x)
  {
    const float dSamp = time_.next() * 0.001f * static_cast<float>(sr_);
    const float tap = line_.readAt(dSamp);
    // In-loop transform (always runs: no stale shifter state on return).
    const float tsh = shifter_.process(tap);
    const float tpd = doppler_.process(tap);
    const float transformed = (mode_ == Mode::FreqWarp) ? tsh : tpd;
    // Damping LP + DC-blocking HP + unity-slope saturator.
    dampLp_ += 0.5f * (transformed - dampLp_); // ~4 kHz one-pole
    const float hp = dampLp_ - hpLp_;
    hpLp_ += 0.008f * (dampLp_ - hpLp_); // ~60 Hz HP (as LP subtract)
    const float sat = hp / (1.0f + std::fabs(hp) / 3.0f);
    const float fb = fb_.next();
    line_.write(x + fb * alienSnap(sat));
    // Wet output: the transformed tap (repeat k = T^k), pre-saturation
    // so the first repeats stay clean and only recirculation distorts.
    const float mx = mix_.next();
    const float y = x + mx * (alienSnap(transformed) - x);
    return std::isfinite(y) ? y : x;
  }

  void processBlock(const float* input, float* output, int n)
  {
    for (int k = 0; k < n; ++k)
      output[k] = process(input[k]);
  }

  int latencySamples() const
  {
    const int loop = (mode_ == Mode::FreqWarp) ? shifter_.latencySamples()
                                               : doppler_.latencySamples();
    return static_cast<int>(timeMsTgt_ * 0.001f * sr_) + loop;
  }

private:
  double sr_ = 48000.0;
  FracDelay line_;
  AlienShift shifter_;
  AlienDoppler doppler_;
  SmoothParam time_, fb_, mix_;
  float timeMsTgt_ = 220.0f, fbTgt_ = 0.5f, mixTgt_ = 1.0f;
  float shiftHzTgt_ = 100.0f, stTgt_ = 3.0f;
  Mode mode_ = Mode::FreqWarp;
  float dampLp_ = 0.0f, hpLp_ = 0.0f;
};

} // namespace alien
} // namespace tdm

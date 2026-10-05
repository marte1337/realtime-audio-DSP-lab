#pragma once

// SLAM lab candidates A-D (LAB ONLY, dsp/lab/Slam). Parallel low-mass
// branches and one transient-triggered impact generator for the SLAM
// architectural study. See .research/slam-study/notes.md.
//
// Common contract (mono float, in-place safe):
//   reset(sampleRate)   - off-RT, clears all state, (re)derives coefficients
//   processBlock(in, out, n) - RT-safe: no alloc/locks/IO, deterministic,
//     finite-out for finite-in
//   setEnabled(false)   - exact copy-through (bit-exact bypass)
// All filters are causal minimum-phase: added latency is 0 samples
// (plus ordinary filter group delay, no lookahead anywhere).
//
// Calibration note: branch makeups were chosen so amount=1.0 adds roughly
// +4..+8 dB of low-band energy on palm-mute material (measured in the study,
// not assumed). The study sweeps amount {0.5, 1.0, 1.5} regardless.

#include <cstdint>

#include "dsp/lab/Slam/SlamDsp.h"

namespace tdm
{
namespace lab
{

// A. SlamPre: parallel low-band compressor + soft saturation for insertion
// BEFORE the amp (study tries before AND after TightDrive). Hypothesis:
// feeding extra mass INTO the amp feels like the amp exploding. Risk: mush.
class SlamPre
{
public:
  static constexpr float kMinBandHz = 40.0f;
  static constexpr float kMaxBandHz = 500.0f;
  static constexpr float kDefaultBandHz = 140.0f;
  static constexpr float kMinAmount = 0.0f;
  static constexpr float kMaxAmount = 2.0f;

  void reset(double sampleRate);
  void setBandHz(float hz); // LP corner of the extracted mass band
  void setAmount(float v);  // branch blend, 0 = dry (still exact)
  void setEnabled(bool enabled);
  void processBlock(const float* input, float* output, int numFrames);

private:
  double sr_ = 48000.0;
  float bandHz_ = kDefaultBandHz;
  float amount_ = 1.0f;
  bool enabled_ = true;
  SlamOnePoleHp dcGuard_;
  SlamBiquad band_;
  SlamComp comp_;
  SlamOnePoleHp dcBlock_;
};

// B. SlamPostNam: parallel low-band compressor + ASYMMETRIC saturation for
// insertion between NAM and IR (cabinet shapes the branch too). Hypothesis:
// asymmetric clipping on dense amp output adds even-harmonic violence that
// still reads as one overloaded amp/cab. Risk: fizz, mud, harshness.
class SlamPostNam
{
public:
  static constexpr float kMinBandHz = 60.0f;
  static constexpr float kMaxBandHz = 600.0f;
  static constexpr float kDefaultBandHz = 200.0f;
  static constexpr float kMinAmount = 0.0f;
  static constexpr float kMaxAmount = 2.0f;

  void reset(double sampleRate);
  void setBandHz(float hz);
  void setAmount(float v);
  void setEnabled(bool enabled);
  void processBlock(const float* input, float* output, int numFrames);

private:
  double sr_ = 48000.0;
  float bandHz_ = kDefaultBandHz;
  float amount_ = 1.0f;
  bool enabled_ = true;
  SlamOnePoleHp dcGuard_;
  SlamBiquad band_;
  SlamComp comp_;
  SlamOnePoleHp dcBlock_;
};

// C. SlamPostIr: parallel low-band compressor + gentle saturation on the
// finished amp+cab tone. Hypothesis: maximum separation between main-guitar
// clarity and added mass = most controllable impact. Risk: sounds like a
// parallel bass effect instead of one coherent guitar.
class SlamPostIr
{
public:
  static constexpr float kMinBandHz = 60.0f;
  static constexpr float kMaxBandHz = 600.0f;
  static constexpr float kDefaultBandHz = 220.0f;
  static constexpr float kMinAmount = 0.0f;
  static constexpr float kMaxAmount = 2.0f;

  void reset(double sampleRate);
  void setBandHz(float hz);
  void setAmount(float v);
  void setEnabled(bool enabled);
  void processBlock(const float* input, float* output, int numFrames);

private:
  double sr_ = 48000.0;
  float bandHz_ = kDefaultBandHz;
  float amount_ = 1.0f;
  bool enabled_ = true;
  SlamOnePoleHp dcGuard_;
  SlamBiquad band_;
  SlamComp comp_;
  SlamOnePoleHp dcBlock_;
};

// D. SlamImpact: transient-triggered impact. An onset detector watches the
// input; on a crossing it measures the attack peak over an 8 ms confirmation
// window and, if the peak still clears the adaptive floor, fires (a) a short
// decaying low sine burst with downward pitch sweep and (b) a brief low-band
// bloom of the dry signal itself. The burst starts ~8 ms after the crossing
// (inaudible delay for a 80-200 ms decay, masked by the attack itself).
// Sustains, gaps, and soft playing pass untouched (up to the bloom tail of a
// previous hit). Hypothesis: mass WHEN the hit occurs reads as physical
// impact without inter-note mud. Risk: mistriggers, machine-gun repetition,
// synthetic "808-drop" character.
//
// The bloom lowpass corner tracks the burst band (1.5x the sweep start):
// pre-amp inserts need the burst + bloom ABOVE the drive HPF (~113 Hz at
// tight 0.5) to survive, while post-IR inserts can live in true sub/low.
//
// Trigger telemetry: the last kMaxTriggers fire stamps (CROSSING sample
// index since reset + measured strength) are kept in a fixed ring for the
// study/tests to read off-RT. fireCount() never saturates.
class SlamImpact
{
public:
  static constexpr int kMaxTriggers = SlamTrigger::kMaxTriggers;
  static constexpr float kMinAmount = 0.0f;
  static constexpr float kMaxAmount = 2.0f;
  static constexpr float kMinSens = 0.0f; // deaf (threshold +18 dB over floor)
  static constexpr float kMaxSens = 1.0f; // eager (threshold +6 dB over floor)

  using Trigger = SlamTrigger::Hit;

  void reset(double sampleRate);
  void setBurstHz(float f0Hz, float f1Hz); // sweep start/end, 30..250 Hz
  void setDecayMs(float ms);              // burst -60 dB time, 40..400 ms
  void setRefractoryMs(float ms);         // min time between fires, 40..400
  void setSensitivity(float v);           // 0..1, default 0.5
  void setAmount(float v);                // burst+bloom blend, 0 = dry
  void setEnabled(bool enabled);
  // Scheduled mode (study/harness use): the internal detector is bypassed
  // and fires come from an ascending off-RT-programmed list of
  // (crossingSample, strength) pairs, e.g. detected at a different tap
  // (raw-DI attacks are far clearer than post-IR flux). The burst still
  // starts 8 ms after each stamp, reproducing live behavior. reset()
  // clears the schedule and returns to detector mode.
  void setScheduled(bool scheduled);
  void scheduleFire(int64_t samplePos, float strength); // ascending, <= kMaxTriggers
  void processBlock(const float* input, float* output, int numFrames);

  int64_t fireCount() const { return trig_.fireCount(); }
  int triggerCount() const { return trig_.triggerCount(); }
  Trigger triggerAt(int i) const { return trig_.triggerAt(i); }

private:
  double sr_ = 48000.0;
  float f0_ = 70.0f, f1_ = 48.0f, decayMs_ = 130.0f, refrMs_ = 90.0f;
  float sens_ = 0.5f, amount_ = 1.0f;
  bool enabled_ = true;
  SlamTrigger trig_;
  SlamBurst burst_;
  SlamBiquad bloomLp_;
  float bloom_ = 0.0f; // extra low-band gain, decays after each fire
  float bloomDecay_ = 1.0f;
};

// E. SlamGated: transient-GATED parallel low-band branch (post-IR intent).
// Same SlamTrigger decisions as D, but instead of a synthetic burst the
// fire opens a decaying blend envelope over a C-style low-band
// comp+saturation branch: out = dry + (base + peak*env) * branch. Answers
// "burst vs gated saturation": gated saturation keeps all added energy
// derived from the guitar itself (no oscillator), at the cost of a slower,
// fatter impact. base/peak/decay shape always-on vs momentary character.
class SlamGated
{
public:
  static constexpr int kMaxTriggers = SlamTrigger::kMaxTriggers;
  static constexpr float kMinBandHz = 60.0f;
  static constexpr float kMaxBandHz = 600.0f;
  static constexpr float kDefaultBandHz = 220.0f;

  using Trigger = SlamTrigger::Hit;

  void reset(double sampleRate);
  void setBandHz(float hz);
  void setBase(float v);         // continuous floor blend, 0..1 (default 0.15)
  void setPeak(float v);         // fire-posed blend added on top, 0..2 (default 1.2)
  void setGateDecayMs(float ms); // blend -60 dB time, 40..400 ms (default 120)
  void setSensitivity(float v);  // 0..1, default 0.5
  void setRefractoryMs(float ms);
  void setEnabled(bool enabled);
  void setScheduled(bool scheduled);
  void scheduleFire(int64_t samplePos, float strength);
  void processBlock(const float* input, float* output, int numFrames);

  int64_t fireCount() const { return trig_.fireCount(); }
  int triggerCount() const { return trig_.triggerCount(); }
  Trigger triggerAt(int i) const { return trig_.triggerAt(i); }

private:
  double sr_ = 48000.0;
  float bandHz_ = kDefaultBandHz;
  float base_ = 0.15f, peak_ = 1.2f, gateDecayMs_ = 120.0f;
  float sens_ = 0.5f, refrMs_ = 90.0f;
  bool enabled_ = true;
  SlamTrigger trig_;
  SlamOnePoleHp dcGuard_;
  SlamBiquad band_;
  SlamComp comp_;
  SlamOnePoleHp dcBlock_;
  float env_ = 0.0f; // 0..1 gate envelope, set to strength on fire
  float envDecay_ = 1.0f;
};

} // namespace lab
} // namespace tdm

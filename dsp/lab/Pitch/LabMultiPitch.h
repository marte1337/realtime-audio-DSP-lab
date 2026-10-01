#pragma once

// LabMultiPitch: two-band multi-resolution pitch shifter (LAB PROTOTYPE).
//
// Status: experimental candidate under evaluation in dsp/lab/Pitch/. NOT
// wired into TechDeathRig / RigParams / tdm_dev.
//
// Goal: keep A's (4096/1024) low-frequency integrity while approaching D's
// (2048/256) transient and mid/high behavior. The single-resolution study
// showed N=2048 already loses chord fifths and low-B roots, while N=1024
// breaks low fundamentals outright - so the low band keeps the long
// window and only the mids/highs take the short one.
//
// Topology (split FIRST, then shift, then aligned sum):
//
//   x -> LabCrossover -> low  -> LabPitchShift(4096/1024) ---------------> (+)
//                      -> high -> LabPitchShift(2048/256) -> delay align -> (+) -> y
//
// Why split-first and not shift-then-blend: two independent PVs assign
// independent arbitrary phases to the same partial, so any energy present
// in BOTH bands risks cancellation on the sum. Split-first confines that
// risk to genuinely-split transition energy only; shift-then-blend would
// duplicate the entire transition band through both shifters.
//
// How the five crossover hazards are handled:
// - phase cancellation: confined, not eliminated, by a narrow (~200 Hz)
//   transition - each partial lives almost wholly in one band. Probed by
//   measurement (crossover-region partial stability), not assumed.
// - double-processing / level buildup: structurally impossible - the
//   split is a partition (low + high == delayed x exactly).
// - holes: structurally impossible for the same reason (pinned by test
//   to ~1e-6 relative on the complementary sum).
// - latency mismatch: the fast high path is padded with exactly
//   (L_low - L_high) samples (derived per shift at reset), so both bands
//   emerge sample-aligned. Total latency = crossoverD + L_low.
// - transient misalignment: same alignment - attacks emerge time-aligned
//   (low part smeared by its long window, high part tight). No double
//   attacks by construction.
//
// Latency honesty: the long low path forces A-like end-to-end latency
// (plus ~8.3 ms of crossover delay). Low-frequency integrity fundamentally
// requires a long observation span; emitting the highs early to "feel"
// faster would misalign the bands and comb on the sum. Not done, not
// faked in offline renders either.
//
// Two bands are sufficient because the requirement splits cleanly: only
// sub-~200 Hz content needs the long window (the study showed 2048-class
// resolves everything above that), and every added band is another
// crossover's worth of split-phase risk. A third band needs measured
// cause, not speculation.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].

#include <vector>

#include "dsp/lab/Pitch/LabCrossover.h"
#include "dsp/lab/Pitch/LabPitchShift.h"

namespace tdm
{
namespace lab
{
class LabMultiPitch
{
public:
  static constexpr int kLowFftSize = 4096; // A-low: low-E/B + chord roots
  static constexpr int kLowHop = 1024;
  static constexpr int kHighFftSize = 2048; // D-high: transients + mids/highs
  static constexpr int kHighHop = 256;

  LabMultiPitch();

  // Off-RT: validates the rate, designs the crossover, resets both bands
  // for the stored shift, derives the alignment delay. Deterministic
  // start (zero history everywhere). Throws std::invalid_argument.
  void reset(double sampleRate);

  // Same shift semantics as LabPitchShift (clamped, reset-gated, exact
  // 0.0f bypasses the WHOLE processor bit-exactly with latency 0).
  void setShiftSt(float semitones);
  void setEnabled(bool enabled);

  float shiftSt() const { return shiftSt_; }
  bool isEnabled() const { return enabled_; }
  // Streaming latency (valid post-reset): crossover delay + low-band
  // latency (the high path is padded up to it). 0 at exact 0 st.
  int latencySamples() const { return latency_; }
  // Offline tail (valid post-reset): the low band's tail (both bands see
  // the full padded stream; alignment is output-side). 0 at exact 0 st.
  int tailSamples() const { return bypass0_ ? 0 : tail_; }
  int crossoverDelay() const { return xover_.delaySamples(); } // valid post-reset
  int alignDelay() const { return align_; } // high-path pad, valid post-reset

  // Mono float processing, in-place safe. Disabled / exact-0-st bypass
  // copies exactly. Otherwise output lags input by latencySamples().
  void processBlock(const float* input, float* output, int numFrames);

private:
  double sampleRate_ = 0.0;
  float shiftSt_ = LabPitchShift::kDefaultShiftSt;
  bool enabled_ = false;
  bool bypass0_ = false; // exact-0-st whole-processor bypass, from reset()
  int latency_ = 0; // derived in reset()
  int tail_ = 0; // derived in reset()
  int align_ = 0; // L_low - L_high, derived in reset()

  LabCrossover xover_;
  LabPitchShift low_;
  LabPitchShift high_;

  std::vector<float> alignRing_; // high-path pad, align + 1
  long long alignPos_ = 0;
};
} // namespace lab
} // namespace tdm

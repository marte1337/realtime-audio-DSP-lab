#pragma once

// LabWsolaLive: realtime-safe audition wrapper around LabWsolaShift.
//
// LAB AUDITION SCAFFOLDING, not production architecture. Exists so the W20
// candidate can be played through live (guitar -> WSOLA -> rig) with:
// - constant-latency bypass: disabled output is the dry signal delayed by
//   exactly the shifter latency L, so toggling never changes feel/latency,
//   only pitch content. (A zero-latency bypass would flam by L samples.)
// - click-free toggling: an atomic enable request ramps a 128-sample
//   linear crossfade between dry and wet on the audio thread. No locks,
//   no allocation, no branching on uninitialized state in processBlock.
// - fixed shift per run: shift is set at prepare() (off-RT) and takes
//   effect via reset; changing shift live would need reset (allocation +
//   state flush) so the audition requires stop/change/start instead.
//
// Config is pinned to the study winner: 20 ms window. Shift must be in
// [-7, 0]; 0 selects the shifter's exact zero-latency bypass (L = 0, the
// wrapper degenerates to a wire). Rates 8000..192000 (device rate).
//
// Threading: prepare() and the getters are control-thread only (call
// prepare() before audio starts); setEnabled() may be called from any
// thread while running (single atomic request flag, applied per block);
// processBlock() is audio-thread only. processBlock is in-place safe and
// block-size deterministic (the ramp advances per sample, never per block).
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].

#include <atomic>
#include <vector>

#include "dsp/lab/Pitch/LabWsolaShift.h"

namespace tdm
{
namespace lab
{
class LabWsolaLive
{
public:
  static constexpr double kWindowMs = 20.0; // pinned study winner
  static constexpr float kMinShiftSt = -7.0f; // validated audition range...
  static constexpr float kMaxShiftSt = 0.0f; // ...deeper shifts need bigger windows
  static constexpr int kRampSamples = 128; // toggle crossfade length

  LabWsolaLive();

  // Off-RT: validates shift/rate/block (throws std::invalid_argument),
  // resets the shifter (permanently enabled from here on - bypass is the
  // wrapper's latency-matched dry path, never the shifter's hard
  // switch), sizes the dry delay, parks the ramp at the start state.
  // maxBlockFrames bounds processBlock calls (the engine chunks by its
  // maxBlock); larger blocks would alias the delay ring. Deterministic:
  // same args => same state.
  void prepare(double sampleRate, float shiftSt, bool startEnabled, int maxBlockFrames);

  // Any-thread live toggle request (atomic; applied on the audio thread
  // at the next processBlock, then ramped over kRampSamples).
  void setEnabled(bool enabled);
  // Target state (the request, for UI print). The audible state follows
  // within kRampSamples after the request is picked up.
  bool isEnabled() const { return enabledReq_.load(std::memory_order_acquire); }

  double sampleRate() const { return sampleRate_; }
  float shiftSt() const { return shiftSt_; }
  // Streaming latency in samples (valid post-prepare): the shifter's
  // latency L; BOTH paths (wet and bypassed dry) carry exactly this.
  int latencySamples() const { return latency_; }

  // Audio-thread only, in-place safe. Emits exactly numFrames samples.
  void processBlock(const float* input, float* output, int numFrames);

private:
  LabWsolaShift shifter_;
  double sampleRate_ = 0.0;
  float shiftSt_ = 0.0f;
  int latency_ = 0;

  std::vector<float> dryDelay_; // latency-matching ring, size L+B
  long long delayWrite_ = 0; // absolute write cursor (starts at L)

  std::atomic<bool> enabledReq_{true}; // any-thread request...
  float ramp_ = 1.0f; // ...audio-thread ramped follower, 0 = dry, 1 = wet
};
} // namespace lab
} // namespace tdm

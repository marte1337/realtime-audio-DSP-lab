#pragma once

// DevSlam: DEV-only SLAM flavour router for the AppKit audition app
// (LAB ONLY, dsp/lab/Slam). Lives behind the three TechDeathRig SLAM
// seams (pre-drive / post-NAM / post-IR); production never installs it.
//
// Flavours (study candidates, unchanged algorithms):
//   Push   = SlamPre @140 Hz, pre-drive tap (study A1)
//   Crush  = SlamPostNam @200 Hz, post-NAM tap (study B)
//   Mass   = SlamPostIr @220 Hz, post-IR tap (study C)
//   Impact = split-tap dhyb: SlamTrigger pre-drive + burst/bloom render
//            post-IR (study dhyb voice: 70->48 Hz, 130 ms, gain 0.75,
//            bloom LP 105 Hz, bloom 2.0x). E/SlamGated is NOT exposed
//            (rejected by the study).
//
// Exactly ONE flavour renders output at a time. Flavour/enable changes
// dip through dry (macro m: 4 ms down, then 8 ms up) so switching never
// clicks and never overlaps flavours; dry is untouched throughout (all
// flavours are dry + contribution). Amount (0..1 per flavour, UI shows
// 0..100%) halves toward its target each block; at steady state with
// m == 1 the output shortcut copies the flavour render directly, so
// Amount 100% reproduces the study candidate bit-exactly.
//
// Realtime contract: UI setters are lock-free atomic stores adopted at
// the pre-drive tap (first tap each block/chunk -- call it first).
// Audio processing allocates nothing, locks nothing, calls no AppKit.
// reset() is off-RT (allocates the wet scratch) and idempotent (the rig
// calls it once per installed tap). Impact telemetry
// (impactFireCount()) is an atomic the UI tick polls; the trigger LED
// derives from it.
//
// Threading note: activeFlavor()/macro()/currentAmount() read
// audio-thread state: call only with audio stopped or from
// single-threaded tests.

#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/SlamInsert.h"
#include "dsp/lab/Slam/SlamCandidates.h"

namespace tdm
{
namespace lab
{

class DevSlam
{
public:
  enum class Flavor
  {
    Push = 0,
    Crush = 1,
    Mass = 2,
    Impact = 3
  };

  static constexpr float kDefaultAmount01 = 1.0f;

  DevSlam();

  // --- UI thread (also safe pre-start) ---
  void setEnabled(bool enabled);
  void setFlavor(Flavor f);
  void setAmount01(float v); // 0..1 (UI: 0..100%), clamped
  bool isEnabled() const { return enabledTarget_.load(std::memory_order_relaxed); }
  Flavor flavor() const { return static_cast<Flavor>(flavorTarget_.load(std::memory_order_relaxed)); }
  float amount01() const { return amountTarget_.load(std::memory_order_relaxed); }
  // Monotonic Impact fire counter for the trigger LED. Bumped on the
  // audio thread whenever the detector confirms a hit (even at Amount 0,
  // so "firing but quiet" stays distinguishable from "not firing").
  int64_t impactFireCount() const { return uiFireCount_.load(std::memory_order_relaxed); }

  // --- Seam taps (install into TechDeathRig pre-start) ---
  SlamInsert& preDrive() { return preTap_; }
  SlamInsert& postNam() { return postNamTap_; }
  SlamInsert& postIr() { return postIrTap_; }

  // --- Single-threaded/test introspection (audio stopped only) ---
  Flavor activeFlavor() const { return activeFlavor_; }
  float macro() const { return m_; }
  float currentAmount() const { return amount_; }

private:
  enum Tap
  {
    PreDrive = 0,
    PostNam = 1,
    PostIr = 2
  };
  // One SlamInsert per tap, all served by this router.
  class TapAdapter : public SlamInsert
  {
  public:
    TapAdapter() = default;
    void bind(DevSlam* owner, Tap tap)
    {
      owner_ = owner;
      tap_ = tap;
    }
    void reset(double sampleRate, int maxBlockSize) override;
    void process(const float* input, float* output, int numFrames) override;
    int latencySamples() const override { return 0; }

  private:
    DevSlam* owner_ = nullptr;
    Tap tap_ = PreDrive;
  };
  friend class TapAdapter;

  enum Phase
  {
    Steady = 0,
    Dip = 1, // m -> 0 on the outgoing flavour, then adopt target
    Rise = 2 // m -> (enabled ? 1 : 0) on the incoming flavour
  };

  void resetAudio(double sampleRate, int maxBlockSize); // off-RT, idempotent
  void adoptTargets(); // pre-drive entry: block-boundary adoption
  void adoptNow(bool tEn, Flavor tFl); // deferred dip bottom (or already dry): switch
  void advanceRamp(); // per-sample macro step (active tap only)
  bool detectorArmed() const;
  void pushPending(int64_t absSample, float strength);
  void processTap(Tap tap, const float* input, float* output, int numFrames); // RT
  // Render one Impact sample post-IR (mirrors SlamImpact::processBlock's
  // formula exactly, so Amount 100% == study dhyb bit-exactly).
  float renderImpactSample(float dry, int64_t absSample);
  void clearImpactPending();

  // UI targets (atomics) + audio working state.
  std::atomic<bool> enabledTarget_{false};
  std::atomic<int> flavorTarget_{0};
  std::atomic<float> amountTarget_{kDefaultAmount01};
  std::atomic<int64_t> uiFireCount_{0};
  int64_t lastTrigCount_ = 0;

  bool activeEnabled_ = false;
  Flavor activeFlavor_ = Flavor::Push;
  float amount_ = kDefaultAmount01; // halving toward target per block
  float m_ = 0.0f; // contribution macro: 0 = dry, 1 = full flavour
  Phase phase_ = Steady;
  bool adoptPending_ = false; // dip bottom reached: adopt at next block start
  int rampLeft_ = 0;
  float rampStep_ = 0.0f;
  int dipN_ = 192; // 4 ms @ 48 kHz (rederived per rate)
  int riseN_ = 384; // 8 ms @ 48 kHz

  double sr_ = 48000.0;
  std::vector<float> wet_; // flavour render scratch, sized maxBlock

  SlamPre push_;
  SlamPostNam crush_;
  SlamPostIr mass_;
  // Impact split voice (study dhyb parameters).
  SlamTrigger trig_;
  SlamBurst burst_;
  SlamBiquad bloomLp_;
  float bloom_ = 0.0f;
  float bloomDecay_ = 1.0f;
  // Pending Impact fires: (absolute fire sample, strength) FIFO. Fires
  // land one sample after the detector's return sample, matching
  // scheduled SlamImpact (study dhyb: stamp + 384; the live detector
  // returns at crossing + 383). Absolute positions keep block-edge
  // spill exact. The 90 ms refractory vs sub-90 ms blocks means at most
  // one live entry; four slots keep oversized test blocks safe.
  static constexpr int kMaxPending = 4;
  int64_t pendAbs_[kMaxPending] = {0, 0, 0, 0};
  float pendStr_[kMaxPending] = {0.0f, 0.0f, 0.0f, 0.0f};
  int pendCount_ = 0;
  int64_t preAbs_ = 0; // pre-drive timeline (detector feed position)
  int64_t postAbs_ = 0; // post-IR timeline (burst render position)

  TapAdapter preTap_, postNamTap_, postIrTap_;
};

} // namespace lab
} // namespace tdm

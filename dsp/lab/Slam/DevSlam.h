#pragma once

// DevSlam: DEV-only SLAM flavour router for the AppKit audition app
// (LAB ONLY, dsp/lab/Slam). Lives behind the three TechDeathRig SLAM
// seams (pre-drive / post-NAM / post-IR); production never installs it.
//
// Flavours (study candidates, unchanged algorithms):
//   Push   = SlamPre @140 Hz, pre-drive tap (study A1)
//   Crush  = SlamPostNam @200 Hz, post-NAM tap (study B)
//   Mass   = SlamPostIr @220 Hz, post-IR tap (study C)
//   Impact = split-tap: SlamTrigger pre-drive + post-IR burst/bloom render.
//            ImpactVoice::Legacy reproduces the study dhyb voice bit-exactly
//            (70->48 Hz, 130 ms, gain 0.75, bloom LP 105 Hz, 2.0x); the v2
//            voices (Sub/Thump/Punch, SlamImpactV2) keep the detector and
//            change only the generated event + adaptive burst gain.
//            E/SlamGated is NOT exposed (rejected by the study).
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
// DEV-ONLY Impact diagnostics (branch solo, debug gain, 180 Hz ref,
// branch/gain/limiter telemetry): adopted per block, glided where noted.
// Normal + 1x + ref-off is bit-identical to the pre-diagnostic path
// (fast path: soloM == 0, dbgG == 1 park exactly via snap).
//
// Threading note: activeFlavor()/macro()/currentAmount() read
// audio-thread state: call only with audio stopped or from
// single-threaded tests.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/SlamInsert.h"
#include "dsp/lab/Slam/SlamCandidates.h"
#include "dsp/lab/Slam/SlamImpactV2.h"

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

  enum class ImpactVoice
  {
    Legacy = 0, // v1 study dhyb, bit-exact (tests/probe reference; no UI)
    Sub = 1, // v2: v1 band + adaptive gain (intentional sub voice)
    Thump = 2, // v2: 140->85 Hz burst + mild sat (audition default)
    Punch = 3 // v2: body + saturated mid knock (most aggressive)
  };

  static constexpr float kDefaultAmount01 = 1.0f;
  // v2 detector study: 0.6 doubles groove-8th refires vs 0.5 (3/6 -> 6/6
  // synthetic; breakdown 4/4 either way) while 16ths stay gated (2/8) and
  // silence/noise behavior is unchanged. Live-adjustable in the DEV app.
  static constexpr float kDefaultImpactSens = 0.6f;
  // Target-ratio law default: High step. 100% = deliberately obvious (the
  // hardware brief: +11 dB over the faint live legacy branch, matching the
  // +12 dB solo-1x-faint/4x-obvious observation); Amount 50-70% then lands
  // in the useful -9..-6 dB band. Steps: Low -12 / Med -6 / High -2.
  static constexpr float kDefaultTargetDb = -2.0f;

  enum class ImpactGainLaw
  {
    LegacyAdaptive = 0, // burst scales WITH rig lows (default, unchanged)
    TargetRatio = 1 // burst peak aims at targetDb_ below recent main level
  };

  DevSlam();

  // --- UI thread (also safe pre-start) ---
  void setEnabled(bool enabled);
  void setFlavor(Flavor f);
  void setAmount01(float v); // 0..1 (UI: 0..100%), clamped
  void setImpactVoice(ImpactVoice v); // staged; dips when Impact is active
  void setImpactSensitivity(float v); // 0..1 trigger threshold, live, no dip
  // Test/probe-only Impact overdrive (v2 voices only, Legacy untouched):
  // multiplies the v2 contribution for audibility-threshold probing. No UI.
  void setImpactTestBoost(float v); // 0..2, clamped, default 1
  // DEV-ONLY Impact diagnostics (tiny DEV-app controls, never production):
  void setImpactDebugSolo(bool solo); // branch-only output (dry muted)
  void setImpactDebugGain(float v); // 1..8 post-voice multiplier, default 1
  void setImpactTestRef(bool ref); // 180 Hz routing-proof burst (v2 path)
  // DEV-ONLY Impact gain law (v2 voices; Legacy has no adaptive gain):
  // LegacyAdaptive (default) or TargetRatio with a targetDb_ step. Adopted
  // per block, no dip (future fires use the new law; in-flight rings keep
  // their sampled gain, glide catch-up stays smooth/up-only).
  void setImpactGainLaw(ImpactGainLaw l);
  void setImpactTargetDb(float db); // clamped to [-30, 0] by the voice
  bool isEnabled() const { return enabledTarget_.load(std::memory_order_relaxed); }
  Flavor flavor() const { return static_cast<Flavor>(flavorTarget_.load(std::memory_order_relaxed)); }
  float amount01() const { return amountTarget_.load(std::memory_order_relaxed); }
  ImpactVoice impactVoice() const
  {
    return static_cast<ImpactVoice>(impactVoiceTarget_.load(std::memory_order_relaxed));
  }
  float impactSensitivity() const { return impactSensTarget_.load(std::memory_order_relaxed); }
  bool isImpactDebugSolo() const { return soloTarget_.load(std::memory_order_relaxed); }
  float impactDebugGain() const { return dbgGainTarget_.load(std::memory_order_relaxed); }
  bool isImpactTestRef() const { return refTarget_.load(std::memory_order_relaxed); }
  ImpactGainLaw impactGainLaw() const
  {
    return static_cast<ImpactGainLaw>(lawTarget_.load(std::memory_order_relaxed));
  }
  float impactTargetDb() const { return targetDbTarget_.load(std::memory_order_relaxed); }
  // Monotonic Impact fire counter for the trigger LED. Bumped on the
  // audio thread whenever the detector confirms a hit (even at Amount 0,
  // so "firing but quiet" stays distinguishable from "not firing").
  int64_t impactFireCount() const { return uiFireCount_.load(std::memory_order_relaxed); }
  // Live Impact telemetry (audio-published atomics for the 10 Hz UI tick):
  float impactTeleBranch() const { return teleBranch_.load(std::memory_order_relaxed); }
  float impactTeleGain() const { return teleGain_.load(std::memory_order_relaxed); }
  float impactTeleLim() const { return teleLim_.load(std::memory_order_relaxed); }

  // --- Seam taps (install into TechDeathRig pre-start) ---
  SlamInsert& preDrive() { return preTap_; }
  SlamInsert& postNam() { return postNamTap_; }
  SlamInsert& postIr() { return postIrTap_; }

  // --- Single-threaded/test introspection (audio stopped only) ---
  Flavor activeFlavor() const { return activeFlavor_; }
  ImpactVoice activeImpactVoice() const { return activeVoice_; }
  float macro() const { return m_; }
  float currentAmount() const { return amount_; }
  float currentImpactSens() const { return sens_; }
  float impactLimiterMin() const { return v2_.limiterMin(); }
  float impactLastFireGain() const { return v2_.lastFireGain(); } // v2 voices only
  ImpactGainLaw activeImpactGainLaw() const
  {
    return v2_.gainLaw() == SlamImpactV2::GainLaw::TargetRatio ? ImpactGainLaw::TargetRatio
                                                               : ImpactGainLaw::LegacyAdaptive;
  }
  float activeImpactTargetDb() const { return v2_.targetDb(); }
  float impactLastFireRig() const { return v2_.lastFireRig(); }
  float impactLastFireMain() const { return v2_.lastFireMain(); }
  float impactLastReqGain() const { return v2_.lastReqGain(); }
  float impactLastLimiterReq() const { return v2_.lastLimiterReq(); }

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
  void publishTele(bool impactActive); // post-IR exit: telemetry atoms
  void adoptTargets(); // pre-drive entry: block-boundary adoption
  void adoptNow(bool tEn, Flavor tFl, ImpactVoice tVoice); // dip bottom (or dry): switch
  void advanceRamp(); // per-sample macro step (active tap only)
  bool detectorArmed() const;
  void pushPending(int64_t absSample, float strength);
  void processTap(Tap tap, const float* input, float* output, int numFrames); // RT
  // Legacy Impact render (v1): mirrors SlamImpact::processBlock's formula
  // exactly, so Amount 100% == study dhyb bit-exactly. FROZEN.
  float renderImpactSample(float dry, int64_t absSample);
  // v2 Impact render (Sub/Thump/Punch): same pending-queue timing, voice
  // event + adaptive gain via SlamImpactV2.
  float renderV2Sample(float dry, int64_t absSample);
  void clearImpactPending();

  // UI targets (atomics) + audio working state.
  std::atomic<bool> enabledTarget_{false};
  std::atomic<int> flavorTarget_{0};
  std::atomic<float> amountTarget_{kDefaultAmount01};
  std::atomic<int> impactVoiceTarget_{static_cast<int>(ImpactVoice::Thump)};
  std::atomic<float> impactSensTarget_{kDefaultImpactSens};
  std::atomic<float> impactBoostTarget_{1.0f}; // test-only, v2-only
  std::atomic<bool> soloTarget_{false}; // DEV debug: branch solo
  std::atomic<float> dbgGainTarget_{1.0f}; // DEV debug: 1..8x
  std::atomic<bool> refTarget_{false}; // test/DEV: 180 Hz routing proof
  std::atomic<int> lawTarget_{0}; // DEV: ImpactGainLaw (0 = LegacyAdaptive)
  std::atomic<float> targetDbTarget_{kDefaultTargetDb}; // DEV: target step dB
  std::atomic<int64_t> uiFireCount_{0};
  std::atomic<float> teleBranch_{0.0f}; // branch peak env (linear)
  std::atomic<float> teleGain_{0.0f}; // last adaptive fire gain
  std::atomic<float> teleLim_{1.0f}; // current limiter GR (1 = transparent)
  int64_t lastTrigCount_ = 0;

  bool activeEnabled_ = false;
  Flavor activeFlavor_ = Flavor::Push;
  ImpactVoice activeVoice_ = ImpactVoice::Thump;
  float amount_ = kDefaultAmount01; // halving toward target per block
  float sens_ = kDefaultImpactSens; // adopted per block, no dip
  float boost_ = 1.0f; // test-only v2 multiplier, adopted per block
  float soloM_ = 0.0f, soloT_ = 0.0f; // debug solo mix (5 ms glide + snap)
  float dbgG_ = 1.0f, dbgT_ = 1.0f; // debug gain (5 ms glide + snap)
  bool ref_ = false; // 180 Hz routing proof (adopted per block)
  float benv_ = 0.0f; // branch peak envelope for telemetry
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
  // Impact split voice: shared detector + Legacy v1 render (FROZEN study
  // dhyb parameters) or v2 voice render (Sub/Thump/Punch).
  SlamTrigger trig_;
  SlamBurst burst_;
  SlamBiquad bloomLp_;
  float bloom_ = 0.0f;
  float bloomDecay_ = 1.0f;
  SlamImpactV2 v2_;
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

// Impact telemetry text convention: single source of truth shared by the
// DEV UI and tests (prevents UI/format drift). Branch peak in dB ("--" when
// idle), adaptive gain linear ("--" only when fully idle — the gain persists
// after its fire), limiter as dB of gain reduction: "0.0" means the limiter
// is WIDE OPEN (healthy), negative when limiting. The documented 0.25 floor
// is on the LINEAR gain atom; 0.0 dB GR ⟺ linear ~1.0, so "lim 0.0 dB" can
// never mean a clamped limiter.
struct SlamTeleText
{
  std::string branch, gain, lim;
};
inline SlamTeleText slamTeleText(float br, float gn, float lm)
{
  SlamTeleText t;
  char b[32];
  if (br <= 1e-6f)
    t.branch = "--";
  else
  {
    std::snprintf(b, sizeof b, "%+.1f dB", 20.0 * std::log10(br));
    t.branch = b;
  }
  if (gn <= 1e-6f && br <= 1e-6f)
    t.gain = "--";
  else
  {
    std::snprintf(b, sizeof b, "%.2f", (double)gn);
    t.gain = b;
  }
  if (lm >= 0.999f)
    t.lim = "0.0";
  else
  {
    std::snprintf(b, sizeof b, "%+.1f", 20.0 * std::log10(lm));
    t.lim = b;
  }
  return t;
}

} // namespace lab
} // namespace tdm

#pragma once

// Rig: Input -> Input Trim -> TechDeathGate -> TightDrive -> NAM A2
//   -> Cabinet IR -> ToneShape -> Space -> Output Trim -> Output.
//
// Everything through ToneShape is mono (multi-channel input is averaged
// to mono: avoids the +6 dB surprise of summing; stereo width tricks come
// later). Space is the first stereo stage (mono in, L/R out); Output Trim
// is applied per channel by two deterministic instances, then channels
// map as: 1 out -> the exact (L+R)/2 fold-down, 2 outs -> L/R, more ->
// the pair cyclically.
// Stages without a loaded asset bypass transparently; the gate bypasses
// exactly until explicitly enabled, preserving Milestone 0 behavior.
// Input Trim defaults to 0 dB, at which it passes input bit-exactly.
// TightDrive is disabled by default and bypasses exactly when off.
// ToneShape is disabled by default and bypasses exactly when off (and is
// bit-exact at neutral settings from reset when enabled).
// Delay/Reverb are disabled by default and contribute nothing until
// explicitly enabled (additive mixes: dry is never scaled).
// Output Trim defaults to 0 dB, at which it passes input bit-exactly; it
// only changes post-chain listening level, never NAM drive.
//
// Threading contract (developer-app era, DSP algorithms untouched):
// - The atomic parameter setters below (setGateEnabled, setGateThresholdDb,
//   setGateReleaseMs, setInputTrimDb, setDriveEnabled, setTight, setDrive,
//   setBite, setShapeEnabled, setWeight, setContour, setPresence,
//   setDelayEnabled, setDelayTimeMs, setDelayFeedback, setDelayMix,
//   setReverbEnabled, setReverbDecay, setReverbMix, setOutputTrimDb,
//   setParams) are safe to call from ANY thread,
//   including the GUI/control thread while audio runs. They only perform a
//   clamped lock-free atomic store: no allocation, no locks, no DSP touch.
// - processBlock() picks up changed values once per call, at the block
//   boundary, before processing the first sample. The audio thread is the
//   only writer of stage internals, so stage code stays single-threaded
//   exactly as before. Refreshing Gate/Drive coefficients is a few bounded
//   transcendentals and only happens for values that actually changed.
// - reset()/loadNam()/loadIr()/clearNam()/clearIr() are OFF-RT ONLY and must
//   be called with audio stopped (v0 policy: Stop -> load -> Start). They
//   allocate, touch files, and rewrite stage state.
// - The "off-RT setter" wording in the stage headers still applies when a
//   stage is driven directly. The rig is the thread-safe facade; GUI/host
//   code must go through the rig, never through the stages.

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/InputTrim.h"
#include "dsp/NamStage.h"
#include "dsp/RigParams.h"
#include "dsp/Gate/TechDeathGate.h"
#include "dsp/TightDrive/TightDrive.h"
#include "dsp/ToneShape/ToneShape.h"
#include "dsp/Space/SpaceProcessor.h"
#include "dsp/OutputTrim.h"

namespace tdm
{
class TechDeathRig
{
public:
  // Off-RT: sizes scratch, resets all stages, pushes current parameters.
  void reset(double sampleRate, int maxBlockSize);

  // Off-RT loaders; require reset() first so the host rate is known.
  // NOT thread-safe: call only with audio stopped.
  void loadNam(const std::string& path);
  void loadIr(const std::string& path);
  void clearNam() { nam_.clear(); }
  void clearIr() { ir_.clear(); }

  bool hasNam() const { return nam_.hasModel(); }
  bool hasIr() const { return ir_.hasIr(); }
  // Expected asset rates in Hz, or negative when unknown / nothing loaded.
  // Used by hosts to fail Start loudly on a sample-rate mismatch.
  double namExpectedSampleRate() const { return nam_.expectedSampleRate(); }
  double irSampleRate() const { return ir_.loadedSampleRate(); }
  double sampleRate() const { return sampleRate_; }
  int maxBlockSize() const { return maxBlock_; }

  // Any-thread setters: clamped lock-free store; applied at the next block
  // boundary inside processBlock(). Safe to call while audio runs.
  // Gate controls. Disabled by default: bypass preserves Milestone 0
  // gain/tone exactly until the gate is explicitly enabled.
  void setGateEnabled(bool enabled) { gateEnabled_.store(enabled, std::memory_order_relaxed); }
  void setGateThresholdDb(float db)
  {
    gateThreshDb_.store(std::clamp(db, TechDeathGate::kMinThresholdDb, TechDeathGate::kMaxThresholdDb),
                        std::memory_order_relaxed);
  }
  void setGateReleaseMs(float ms)
  {
    gateRelMs_.store(std::clamp(ms, TechDeathGate::kMinReleaseMs, TechDeathGate::kMaxReleaseMs),
                     std::memory_order_relaxed);
  }
  bool isGateEnabled() const { return gateEnabled_.load(std::memory_order_relaxed); }
  float gateThresholdDb() const { return gateThreshDb_.load(std::memory_order_relaxed); }
  float gateReleaseMs() const { return gateRelMs_.load(std::memory_order_relaxed); }

  // Input Trim control. Defaults to 0 dB, which passes input bit-exactly
  // and preserves Milestone 0 behavior.
  void setInputTrimDb(float db)
  {
    inTrimDb_.store(std::clamp(db, InputTrim::kMinTrimDb, InputTrim::kMaxTrimDb), std::memory_order_relaxed);
  }
  float inputTrimDb() const { return inTrimDb_.load(std::memory_order_relaxed); }

  // TightDrive controls. Disabled by default: bypass preserves the previous
  // rig behavior exactly until explicitly enabled.
  void setDriveEnabled(bool enabled) { driveEnabled_.store(enabled, std::memory_order_relaxed); }
  void setTight(float v)
  {
    tightParam_.store(std::clamp(v, TightDrive::kMinTight, TightDrive::kMaxTight), std::memory_order_relaxed);
  }
  void setDrive(float v)
  {
    driveParam_.store(std::clamp(v, TightDrive::kMinDrive, TightDrive::kMaxDrive), std::memory_order_relaxed);
  }
  void setBite(float v)
  {
    biteParam_.store(std::clamp(v, TightDrive::kMinBite, TightDrive::kMaxBite), std::memory_order_relaxed);
  }
  bool isDriveEnabled() const { return driveEnabled_.load(std::memory_order_relaxed); }
  float tight() const { return tightParam_.load(std::memory_order_relaxed); }
  float drive() const { return driveParam_.load(std::memory_order_relaxed); }
  float bite() const { return biteParam_.load(std::memory_order_relaxed); }

  // ToneShape controls. Disabled by default: bypass preserves the previous
  // rig behavior exactly until explicitly enabled.
  void setShapeEnabled(bool enabled) { shapeEnabled_.store(enabled, std::memory_order_relaxed); }
  void setWeight(float v)
  {
    weight_.store(std::clamp(v, ToneShape::kMinWeight, ToneShape::kMaxWeight), std::memory_order_relaxed);
  }
  void setContour(float v)
  {
    contour_.store(std::clamp(v, ToneShape::kMinContour, ToneShape::kMaxContour), std::memory_order_relaxed);
  }
  void setPresence(float v)
  {
    presence_.store(std::clamp(v, ToneShape::kMinPresence, ToneShape::kMaxPresence), std::memory_order_relaxed);
  }
  bool isShapeEnabled() const { return shapeEnabled_.load(std::memory_order_relaxed); }
  float weight() const { return weight_.load(std::memory_order_relaxed); }
  float contour() const { return contour_.load(std::memory_order_relaxed); }
  float presence() const { return presence_.load(std::memory_order_relaxed); }

  // Space controls (Delay + Reverb). Both units disabled by default: wet
  // is silent until explicitly enabled, preserving the dry chain exactly.
  void setDelayEnabled(bool enabled) { delayEnabled_.store(enabled, std::memory_order_relaxed); }
  void setDelayTimeMs(float ms)
  {
    delayTimeMs_.store(std::clamp(ms, Delay::kMinTimeMs, Delay::kMaxTimeMs), std::memory_order_relaxed);
  }
  void setDelayFeedback(float v)
  {
    delayFb_.store(std::clamp(v, Delay::kMinFeedback, Delay::kMaxFeedback), std::memory_order_relaxed);
  }
  void setDelayMix(float v)
  {
    delayMix_.store(std::clamp(v, SpaceProcessor::kMinMix, SpaceProcessor::kMaxMix), std::memory_order_relaxed);
  }
  void setReverbEnabled(bool enabled) { reverbEnabled_.store(enabled, std::memory_order_relaxed); }
  void setReverbDecay(float v)
  {
    reverbDecay_.store(std::clamp(v, Reverb::kMinDecay, Reverb::kMaxDecay), std::memory_order_relaxed);
  }
  void setReverbMix(float v)
  {
    reverbMix_.store(std::clamp(v, SpaceProcessor::kMinMix, SpaceProcessor::kMaxMix), std::memory_order_relaxed);
  }
  bool isDelayEnabled() const { return delayEnabled_.load(std::memory_order_relaxed); }
  float delayTimeMs() const { return delayTimeMs_.load(std::memory_order_relaxed); }
  float delayFeedback() const { return delayFb_.load(std::memory_order_relaxed); }
  float delayMix() const { return delayMix_.load(std::memory_order_relaxed); }
  bool isReverbEnabled() const { return reverbEnabled_.load(std::memory_order_relaxed); }
  float reverbDecay() const { return reverbDecay_.load(std::memory_order_relaxed); }
  float reverbMix() const { return reverbMix_.load(std::memory_order_relaxed); }

  // Output Trim control. Defaults to 0 dB, which passes input bit-exactly
  // and preserves existing behavior. Post-chain only: loudness matching
  // without touching NAM saturation or drive response.
  void setOutputTrimDb(float db)
  {
    outTrimDb_.store(std::clamp(db, OutputTrim::kMinTrimDb, OutputTrim::kMaxTrimDb), std::memory_order_relaxed);
  }
  float outputTrimDb() const { return outTrimDb_.load(std::memory_order_relaxed); }

  // Any-thread bulk access: snapshot for display, bulk store for presets.
  // setParams() stores only; values reach DSP at the next block boundary.
  RigParams params() const;
  void setParams(const RigParams& p);

  // RT-safe after reset(). inputs[nIn][nFrames] -> outputs[nOut][nOut].
  // Changed parameters are applied once here, at the block boundary.
  void processBlock(const float* const* inputs, int numInputChannels, float* const* outputs,
                    int numOutputChannels, int numFrames);

private:
  // Audio/reset-thread only: push changed atomic values into the stages.
  void syncParamsToStages();
  // Audio/reset-thread only: unconditional push (used by reset()).
  void pushAllParamsToStages();

  InputTrim trim_;
  TechDeathGate gate_;
  TightDrive drive_;
  NamStage nam_;
  CabIrStage ir_;
  ToneShape shape_;
  SpaceProcessor space_;
  // Dual trim instances (one per stereo channel): identical deterministic
  // units with identical target histories produce identical gains, so the
  // dry path stays bit-exact versus the old single instance while L/R can
  // never skew during a trim move (scalar trim commutes with the linear
  // Space stage, so this placement is exact, not approximate).
  OutputTrim outTrimL_;
  OutputTrim outTrimR_;
  std::vector<float> mono_; // internal scratch, sized maxBlock_
  std::vector<float> left_; // space stereo scratch, sized maxBlock_
  std::vector<float> right_; // space stereo scratch, sized maxBlock_
  double sampleRate_ = 0.0;
  int maxBlock_ = 0;

  // Control-visible parameters. Written by any thread via the setters,
  // read by the audio thread at block boundaries. All lock-free on the
  // targets we ship (arm64/x86_64); asserted below.
  std::atomic<float> inTrimDb_{InputTrim::kDefaultTrimDb};
  std::atomic<bool> gateEnabled_{false};
  std::atomic<float> gateThreshDb_{TechDeathGate::kDefaultThresholdDb};
  std::atomic<float> gateRelMs_{TechDeathGate::kDefaultReleaseMs};
  std::atomic<bool> driveEnabled_{false};
  std::atomic<float> tightParam_{TightDrive::kDefaultTight};
  std::atomic<float> driveParam_{TightDrive::kDefaultDrive};
  std::atomic<float> biteParam_{TightDrive::kDefaultBite};
  std::atomic<bool> shapeEnabled_{false};
  std::atomic<float> weight_{ToneShape::kDefaultWeight};
  std::atomic<float> contour_{ToneShape::kDefaultContour};
  std::atomic<float> presence_{ToneShape::kDefaultPresence};
  std::atomic<bool> delayEnabled_{false};
  std::atomic<float> delayTimeMs_{Delay::kDefaultTimeMs};
  std::atomic<float> delayFb_{Delay::kDefaultFeedback};
  std::atomic<float> delayMix_{SpaceProcessor::kDefaultDelayMix};
  std::atomic<bool> reverbEnabled_{false};
  std::atomic<float> reverbDecay_{Reverb::kDefaultDecay};
  std::atomic<float> reverbMix_{SpaceProcessor::kDefaultReverbMix};
  std::atomic<float> outTrimDb_{OutputTrim::kDefaultTrimDb};

  // Last values pushed into the stages. Audio/reset-thread only.
  float appliedInTrimDb_ = InputTrim::kDefaultTrimDb;
  bool appliedGateEnabled_ = false;
  float appliedGateThreshDb_ = TechDeathGate::kDefaultThresholdDb;
  float appliedGateRelMs_ = TechDeathGate::kDefaultReleaseMs;
  bool appliedDriveEnabled_ = false;
  float appliedTight_ = TightDrive::kDefaultTight;
  float appliedDrive_ = TightDrive::kDefaultDrive;
  float appliedBite_ = TightDrive::kDefaultBite;
  bool appliedShapeEnabled_ = false;
  float appliedWeight_ = ToneShape::kDefaultWeight;
  float appliedContour_ = ToneShape::kDefaultContour;
  float appliedPresence_ = ToneShape::kDefaultPresence;
  bool appliedDelayEnabled_ = false;
  float appliedDelayTimeMs_ = Delay::kDefaultTimeMs;
  float appliedDelayFb_ = Delay::kDefaultFeedback;
  float appliedDelayMix_ = SpaceProcessor::kDefaultDelayMix;
  bool appliedReverbEnabled_ = false;
  float appliedReverbDecay_ = Reverb::kDefaultDecay;
  float appliedReverbMix_ = SpaceProcessor::kDefaultReverbMix;
  float appliedOutTrimDb_ = OutputTrim::kDefaultTrimDb;
};

static_assert(std::atomic<float>::is_always_lock_free, "Rig param handoff needs lock-free atomic<float>");
static_assert(std::atomic<bool>::is_always_lock_free, "Rig param handoff needs lock-free atomic<bool>");
} // namespace tdm

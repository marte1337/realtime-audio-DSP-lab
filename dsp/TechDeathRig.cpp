#include "dsp/TechDeathRig.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace tdm
{
namespace
{
// Engine priming shift for reset-at-0: the engine's exact-0-st reset mode
// is a zero-latency wire that would ignore later live shifts, so a
// requested 0 st resets the engine here instead. Inaudible: the wrapper
// outputs latency-matched dry at shift 0 regardless of engine state, and
// live 0 -> N adoption then works without a restart.
constexpr float kTransposePrimeShiftSt = -2.0f;
} // namespace

void TechDeathRig::reset(double sampleRate, int maxBlockSize)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Rig: sample rate must be positive");
  if (maxBlockSize <= 0)
    throw std::runtime_error("Rig: max block size must be positive");
  sampleRate_ = sampleRate;
  maxBlock_ = maxBlockSize;
  mono_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
  left_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
  right_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
  tuner_.reset(sampleRate);
  trim_.reset(sampleRate);
  gate_.reset(sampleRate);
  // Production transpose: default baseline config (never reconfigured),
  // permanently enabled internally (bypass lives in the wrapper below).
  {
    const float wantSt = transposeSt_.load(std::memory_order_relaxed);
    transposeEngine_.setShiftSt(wantSt == 0.0f ? kTransposePrimeShiftSt : wantSt);
    transposeEngine_.setEnabled(true);
    transposeEngine_.reset(sampleRate);
    transposeLatency_ = transposeEngine_.latencySamples();
    transposeWet_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    transposeDry_.assign(static_cast<size_t>(transposeLatency_ + maxBlockSize), 0.0f);
    transposeDelayWrite_ = transposeLatency_;
  }
  if (transpose_ != nullptr)
    transpose_->reset(sampleRate, maxBlockSize);
  if (slamPre_ != nullptr)
    slamPre_->reset(sampleRate, maxBlockSize);
  if (slamPostNam_ != nullptr)
    slamPostNam_->reset(sampleRate, maxBlockSize);
  if (slamPostIr_ != nullptr)
    slamPostIr_->reset(sampleRate, maxBlockSize);
  drive_.reset(sampleRate);
  shape_.reset(sampleRate);
  space_.reset(sampleRate);
  outTrimL_.reset(sampleRate);
  outTrimR_.reset(sampleRate);
  nam_.reset(sampleRate, maxBlockSize);
  ir_.reset(sampleRate);
  // Stages keep their own stored values across reset, but push unconditionally
  // so the atomic truth and the stage state can never drift apart.
  pushAllParamsToStages();
}

void TechDeathRig::loadNam(const std::string& path)
{
  if (sampleRate_ <= 0.0 || maxBlock_ <= 0)
    throw std::runtime_error("Rig: call reset() before loading a NAM model");
  nam_.loadModel(path, sampleRate_, maxBlock_);
}

void TechDeathRig::loadIr(const std::string& path)
{
  if (sampleRate_ <= 0.0 || maxBlock_ <= 0)
    throw std::runtime_error("Rig: call reset() before loading an IR");
  ir_.loadIr(path, sampleRate_);
}

RigParams TechDeathRig::params() const
{
  RigParams p;
  p.inputTrimDb = inputTrimDb();
  p.gateEnabled = isGateEnabled();
  p.gateThresholdDb = gateThresholdDb();
  p.gateReleaseMs = gateReleaseMs();
  p.transposeEnabled = isTransposeEnabled();
  p.transposeSemitones = transposeSemitones();
  p.tunerEnabled = isTunerEnabled();
  p.driveEnabled = isDriveEnabled();
  p.tight = tight();
  p.drive = drive();
  p.bite = bite();
  p.shapeEnabled = isShapeEnabled();
  p.weight = weight();
  p.contour = contour();
  p.presence = presence();
  p.delayEnabled = isDelayEnabled();
  p.delayTimeMs = delayTimeMs();
  p.delayFeedback = delayFeedback();
  p.delayMix = delayMix();
  p.reverbEnabled = isReverbEnabled();
  p.reverbDecay = reverbDecay();
  p.reverbMix = reverbMix();
  p.outputTrimDb = outputTrimDb();
  return p;
}

void TechDeathRig::setParams(const RigParams& p)
{
  setInputTrimDb(p.inputTrimDb);
  setGateEnabled(p.gateEnabled);
  setGateThresholdDb(p.gateThresholdDb);
  setGateReleaseMs(p.gateReleaseMs);
  setTransposeEnabled(p.transposeEnabled);
  setTransposeSemitones(p.transposeSemitones);
  setTunerEnabled(p.tunerEnabled);
  setDriveEnabled(p.driveEnabled);
  setTight(p.tight);
  setDrive(p.drive);
  setBite(p.bite);
  setShapeEnabled(p.shapeEnabled);
  setWeight(p.weight);
  setContour(p.contour);
  setPresence(p.presence);
  setDelayEnabled(p.delayEnabled);
  setDelayTimeMs(p.delayTimeMs);
  setDelayFeedback(p.delayFeedback);
  setDelayMix(p.delayMix);
  setReverbEnabled(p.reverbEnabled);
  setReverbDecay(p.reverbDecay);
  setReverbMix(p.reverbMix);
  setOutputTrimDb(p.outputTrimDb);
}

void TechDeathRig::syncParamsToStages()
{
  const float inTrim = inTrimDb_.load(std::memory_order_relaxed);
  if (inTrim != appliedInTrimDb_)
  {
    trim_.setTrimDb(inTrim);
    appliedInTrimDb_ = inTrim;
  }
  const bool gateEn = gateEnabled_.load(std::memory_order_relaxed);
  if (gateEn != appliedGateEnabled_)
  {
    gate_.setEnabled(gateEn);
    appliedGateEnabled_ = gateEn;
  }
  const float gateTh = gateThreshDb_.load(std::memory_order_relaxed);
  if (gateTh != appliedGateThreshDb_)
  {
    gate_.setThresholdDb(gateTh);
    appliedGateThreshDb_ = gateTh;
  }
  const float gateRel = gateRelMs_.load(std::memory_order_relaxed);
  if (gateRel != appliedGateRelMs_)
  {
    gate_.setReleaseMs(gateRel);
    appliedGateRelMs_ = gateRel;
  }
  const bool transposeEn = transposeEnabled_.load(std::memory_order_relaxed);
  if (transposeEn != appliedTransposeEnabled_)
    appliedTransposeEnabled_ = transposeEn; // engage ramp moves in runTranspose()
  const float transposeSt = transposeSt_.load(std::memory_order_relaxed);
  if (transposeSt != appliedTransposeSt_)
  {
    // Live shift adoption (RT-safe: bounded, no alloc). The engine was
    // primed nonzero at reset, so 0 <-> N moves work without a restart.
    transposeEngine_.setShiftSt(transposeSt);
    appliedTransposeSt_ = transposeSt;
  }
  const bool tunerEn = tunerEnabled_.load(std::memory_order_relaxed);
  if (tunerEn != appliedTunerEnabled_)
  {
    tuner_.setEnabled(tunerEn); // flag flip (+ bounded ring clear); audio untouched
    appliedTunerEnabled_ = tunerEn;
  }
  const bool driveEn = driveEnabled_.load(std::memory_order_relaxed);
  if (driveEn != appliedDriveEnabled_)
  {
    drive_.setEnabled(driveEn);
    appliedDriveEnabled_ = driveEn;
  }
  const float tight = tightParam_.load(std::memory_order_relaxed);
  if (tight != appliedTight_)
  {
    drive_.setTight(tight);
    appliedTight_ = tight;
  }
  const float drive = driveParam_.load(std::memory_order_relaxed);
  if (drive != appliedDrive_)
  {
    drive_.setDrive(drive);
    appliedDrive_ = drive;
  }
  const float bite = biteParam_.load(std::memory_order_relaxed);
  if (bite != appliedBite_)
  {
    drive_.setBite(bite);
    appliedBite_ = bite;
  }
  const bool shapeEn = shapeEnabled_.load(std::memory_order_relaxed);
  if (shapeEn != appliedShapeEnabled_)
  {
    shape_.setEnabled(shapeEn);
    appliedShapeEnabled_ = shapeEn;
  }
  const float weight = weight_.load(std::memory_order_relaxed);
  if (weight != appliedWeight_)
  {
    shape_.setWeight(weight);
    appliedWeight_ = weight;
  }
  const float contour = contour_.load(std::memory_order_relaxed);
  if (contour != appliedContour_)
  {
    shape_.setContour(contour);
    appliedContour_ = contour;
  }
  const float presence = presence_.load(std::memory_order_relaxed);
  if (presence != appliedPresence_)
  {
    shape_.setPresence(presence);
    appliedPresence_ = presence;
  }
  const bool delayEn = delayEnabled_.load(std::memory_order_relaxed);
  if (delayEn != appliedDelayEnabled_)
  {
    space_.setDelayEnabled(delayEn);
    appliedDelayEnabled_ = delayEn;
  }
  const float delayMs = delayTimeMs_.load(std::memory_order_relaxed);
  if (delayMs != appliedDelayTimeMs_)
  {
    space_.setDelayTimeMs(delayMs);
    appliedDelayTimeMs_ = delayMs;
  }
  const float delayFb = delayFb_.load(std::memory_order_relaxed);
  if (delayFb != appliedDelayFb_)
  {
    space_.setDelayFeedback(delayFb);
    appliedDelayFb_ = delayFb;
  }
  const float delayMix = delayMix_.load(std::memory_order_relaxed);
  if (delayMix != appliedDelayMix_)
  {
    space_.setDelayMix(delayMix);
    appliedDelayMix_ = delayMix;
  }
  const bool reverbEn = reverbEnabled_.load(std::memory_order_relaxed);
  if (reverbEn != appliedReverbEnabled_)
  {
    space_.setReverbEnabled(reverbEn);
    appliedReverbEnabled_ = reverbEn;
  }
  const float reverbDecay = reverbDecay_.load(std::memory_order_relaxed);
  if (reverbDecay != appliedReverbDecay_)
  {
    space_.setReverbDecay(reverbDecay);
    appliedReverbDecay_ = reverbDecay;
  }
  const float reverbMix = reverbMix_.load(std::memory_order_relaxed);
  if (reverbMix != appliedReverbMix_)
  {
    space_.setReverbMix(reverbMix);
    appliedReverbMix_ = reverbMix;
  }
  const float outTrim = outTrimDb_.load(std::memory_order_relaxed);
  if (outTrim != appliedOutTrimDb_)
  {
    outTrimL_.setTrimDb(outTrim);
    outTrimR_.setTrimDb(outTrim);
    appliedOutTrimDb_ = outTrim;
  }
}

void TechDeathRig::pushAllParamsToStages()
{
  trim_.setTrimDb(inTrimDb_.load(std::memory_order_relaxed));
  gate_.setEnabled(gateEnabled_.load(std::memory_order_relaxed));
  gate_.setThresholdDb(gateThreshDb_.load(std::memory_order_relaxed));
  gate_.setReleaseMs(gateRelMs_.load(std::memory_order_relaxed));
  drive_.setEnabled(driveEnabled_.load(std::memory_order_relaxed));
  drive_.setTight(tightParam_.load(std::memory_order_relaxed));
  drive_.setDrive(driveParam_.load(std::memory_order_relaxed));
  drive_.setBite(biteParam_.load(std::memory_order_relaxed));
  shape_.setEnabled(shapeEnabled_.load(std::memory_order_relaxed));
  shape_.setWeight(weight_.load(std::memory_order_relaxed));
  shape_.setContour(contour_.load(std::memory_order_relaxed));
  shape_.setPresence(presence_.load(std::memory_order_relaxed));
  space_.setDelayEnabled(delayEnabled_.load(std::memory_order_relaxed));
  space_.setDelayTimeMs(delayTimeMs_.load(std::memory_order_relaxed));
  space_.setDelayFeedback(delayFb_.load(std::memory_order_relaxed));
  space_.setDelayMix(delayMix_.load(std::memory_order_relaxed));
  space_.setReverbEnabled(reverbEnabled_.load(std::memory_order_relaxed));
  space_.setReverbDecay(reverbDecay_.load(std::memory_order_relaxed));
  space_.setReverbMix(reverbMix_.load(std::memory_order_relaxed));
  outTrimL_.setTrimDb(outTrimDb_.load(std::memory_order_relaxed));
  outTrimR_.setTrimDb(outTrimDb_.load(std::memory_order_relaxed));
  appliedInTrimDb_ = inTrimDb_.load(std::memory_order_relaxed);
  appliedGateEnabled_ = gateEnabled_.load(std::memory_order_relaxed);
  appliedGateThreshDb_ = gateThreshDb_.load(std::memory_order_relaxed);
  appliedGateRelMs_ = gateRelMs_.load(std::memory_order_relaxed);
  appliedTransposeEnabled_ = transposeEnabled_.load(std::memory_order_relaxed);
  appliedTransposeSt_ = transposeSt_.load(std::memory_order_relaxed);
  appliedTunerEnabled_ = tunerEnabled_.load(std::memory_order_relaxed);
  tuner_.setEnabled(appliedTunerEnabled_);
  // Park the transpose ramps at reset (deterministic start: no cross-reset
  // ramp state). Engine shift itself was set in reset().
  transposeEngage_ = appliedTransposeEnabled_ ? 1.0f : 0.0f;
  transposeRamp_ = (appliedTransposeEnabled_ && appliedTransposeSt_ != 0.0f) ? 1.0f : 0.0f;
  appliedDriveEnabled_ = driveEnabled_.load(std::memory_order_relaxed);
  appliedTight_ = tightParam_.load(std::memory_order_relaxed);
  appliedDrive_ = driveParam_.load(std::memory_order_relaxed);
  appliedBite_ = biteParam_.load(std::memory_order_relaxed);
  appliedShapeEnabled_ = shapeEnabled_.load(std::memory_order_relaxed);
  appliedWeight_ = weight_.load(std::memory_order_relaxed);
  appliedContour_ = contour_.load(std::memory_order_relaxed);
  appliedPresence_ = presence_.load(std::memory_order_relaxed);
  appliedDelayEnabled_ = delayEnabled_.load(std::memory_order_relaxed);
  appliedDelayTimeMs_ = delayTimeMs_.load(std::memory_order_relaxed);
  appliedDelayFb_ = delayFb_.load(std::memory_order_relaxed);
  appliedDelayMix_ = delayMix_.load(std::memory_order_relaxed);
  appliedReverbEnabled_ = reverbEnabled_.load(std::memory_order_relaxed);
  appliedReverbDecay_ = reverbDecay_.load(std::memory_order_relaxed);
  appliedReverbMix_ = reverbMix_.load(std::memory_order_relaxed);
  appliedOutTrimDb_ = outTrimDb_.load(std::memory_order_relaxed);
}

void TechDeathRig::runTranspose(float* io, int numFrames)
{
  if (numFrames <= 0 || transposeDry_.empty())
    return; // pre-reset: wire (io untouched)
  // Tap dry BEFORE the engine runs (in-place safe), then render the hot
  // engine into scratch. The engine always runs so engagement never meets
  // cold rings; the tap below selects wire / dry-late / wet.
  const int ring = static_cast<int>(transposeDry_.size());
  for (int i = 0; i < numFrames; ++i)
    transposeDry_[static_cast<size_t>((transposeDelayWrite_ + i) % ring)] = io[i];
  transposeEngine_.processBlock(io, transposeWet_.data(), numFrames);

  const float wetTarget = (appliedTransposeEnabled_ && appliedTransposeSt_ != 0.0f) ? 1.0f : 0.0f;
  const float engageTarget = appliedTransposeEnabled_ ? 1.0f : 0.0f;
  const int lat = transposeLatency_;
  // Steady-state fast paths: exact copies, no rounding drift.
  if (transposeEngage_ == 1.0f && engageTarget == 1.0f)
  {
    if (transposeRamp_ == 1.0f && wetTarget == 1.0f)
    {
      for (int i = 0; i < numFrames; ++i)
        io[i] = transposeWet_[static_cast<size_t>(i)];
      transposeDelayWrite_ += numFrames;
      return;
    }
    if (transposeRamp_ == 0.0f && wetTarget == 0.0f)
    {
      for (int i = 0; i < numFrames; ++i)
        io[i] = transposeDry_[static_cast<size_t>((transposeDelayWrite_ + i - lat) % ring)];
      transposeDelayWrite_ += numFrames;
      return;
    }
  }
  else if (transposeEngage_ == 0.0f && engageTarget == 0.0f)
  {
    transposeDelayWrite_ += numFrames; // wire: io untouched, ring stays hot
    return;
  }
  // Transitions: per-sample engage + wet ramps (DevTranspose pattern).
  const float step = 1.0f / static_cast<float>(kTransposeRampSamples);
  for (int i = 0; i < numFrames; ++i)
  {
    if (transposeRamp_ < wetTarget)
      transposeRamp_ = std::min(wetTarget, transposeRamp_ + step);
    else if (transposeRamp_ > wetTarget)
      transposeRamp_ = std::max(wetTarget, transposeRamp_ - step);
    if (transposeEngage_ < engageTarget)
      transposeEngage_ = std::min(engageTarget, transposeEngage_ + step);
    else if (transposeEngage_ > engageTarget)
      transposeEngage_ = std::max(engageTarget, transposeEngage_ - step);
    const float dryLate = transposeDry_[static_cast<size_t>((transposeDelayWrite_ + i - lat) % ring)];
    const float wet = transposeWet_[static_cast<size_t>(i)];
    const float active = (transposeRamp_ == 1.0f)
        ? wet
        : (transposeRamp_ == 0.0f) ? dryLate : dryLate + transposeRamp_ * (wet - dryLate);
    const float wire = io[i];
    io[i] = (transposeEngage_ == 1.0f)
        ? active
        : (transposeEngage_ == 0.0f) ? wire : wire + transposeEngage_ * (active - wire);
  }
  transposeDelayWrite_ += numFrames;
}

void TechDeathRig::processBlock(const float* const* inputs, int numInputChannels, float* const* outputs,
                                int numOutputChannels, int numFrames)
{
  assert(inputs != nullptr && outputs != nullptr && numInputChannels >= 1 && numOutputChannels >= 1
         && numFrames >= 0 && maxBlock_ > 0);
  if (numFrames <= 0)
    return;
  // Block-boundary handoff: the only place the audio thread adopts
  // control-thread parameter stores. No allocation, no locks; stage
  // refreshes are a few bounded coefficient recomputes, and only for
  // values that actually changed since the previous block.
  syncParamsToStages();
  int remaining = numFrames;
  int offset = 0;
  while (remaining > 0)
  {
    const int m = remaining < maxBlock_ ? remaining : maxBlock_;
    for (int i = 0; i < m; ++i)
    {
      double acc = 0.0;
      for (int c = 0; c < numInputChannels; ++c)
        acc += inputs[c][offset + i];
      mono_[static_cast<size_t>(i)] = static_cast<float>(acc / numInputChannels);
    }
    tuner_.feedBlock(mono_.data(), m); // raw-mono side-chain tap (read-only)
    trim_.processBlock(mono_.data(), mono_.data(), m);
    gate_.processBlock(mono_.data(), mono_.data(), m);
    if (transpose_ != nullptr)
      transpose_->process(mono_.data(), mono_.data(), m); // DEV A/B substitute wins
    else
      runTranspose(mono_.data(), m); // production transpose (exact wire when off)
    if (slamPre_ != nullptr)
      slamPre_->process(mono_.data(), mono_.data(), m); // DEV SLAM pre-drive tap
    drive_.processBlock(mono_.data(), mono_.data(), m);
    nam_.processBlock(mono_.data(), mono_.data(), m);
    if (slamPostNam_ != nullptr)
      slamPostNam_->process(mono_.data(), mono_.data(), m); // DEV SLAM post-NAM tap
    ir_.processBlock(mono_.data(), mono_.data(), m);
    if (slamPostIr_ != nullptr)
      slamPostIr_->process(mono_.data(), mono_.data(), m); // DEV SLAM post-IR tap
    shape_.processBlock(mono_.data(), mono_.data(), m);
    // Space is the first stereo stage: mono in, L/R out. Both trim
    // instances see identical target histories, so their gains agree
    // sample-exactly and the dry path matches the old single instance.
    space_.processBlock(mono_.data(), left_.data(), right_.data(), m);
    outTrimL_.processBlock(left_.data(), left_.data(), m);
    outTrimR_.processBlock(right_.data(), right_.data(), m);
    if (numOutputChannels == 1)
    {
      // Exact mono fold-down: (x + x) * 0.5f is bit-exact for the dry
      // (bypassed) case, standard fold for wet.
      for (int i = 0; i < m; ++i)
        outputs[0][offset + i] = (left_[static_cast<size_t>(i)] + right_[static_cast<size_t>(i)]) * 0.5f;
    }
    else
    {
      for (int c = 0; c < numOutputChannels; ++c)
      {
        const float* src = (c % 2 == 0) ? left_.data() : right_.data();
        for (int i = 0; i < m; ++i)
          outputs[c][offset + i] = src[static_cast<size_t>(i)];
      }
    }
    offset += m;
    remaining -= m;
  }
}
} // namespace tdm

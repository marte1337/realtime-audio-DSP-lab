#include "dsp/TechDeathRig.h"

#include <cassert>
#include <stdexcept>

namespace tdm
{
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
  trim_.reset(sampleRate);
  gate_.reset(sampleRate);
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
    trim_.processBlock(mono_.data(), mono_.data(), m);
    gate_.processBlock(mono_.data(), mono_.data(), m);
    drive_.processBlock(mono_.data(), mono_.data(), m);
    nam_.processBlock(mono_.data(), mono_.data(), m);
    ir_.processBlock(mono_.data(), mono_.data(), m);
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

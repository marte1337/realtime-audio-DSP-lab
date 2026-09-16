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
  trim_.reset(sampleRate);
  gate_.reset(sampleRate);
  drive_.reset(sampleRate);
  outTrim_.reset(sampleRate);
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
  const float outTrim = outTrimDb_.load(std::memory_order_relaxed);
  if (outTrim != appliedOutTrimDb_)
  {
    outTrim_.setTrimDb(outTrim);
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
  outTrim_.setTrimDb(outTrimDb_.load(std::memory_order_relaxed));
  appliedInTrimDb_ = inTrimDb_.load(std::memory_order_relaxed);
  appliedGateEnabled_ = gateEnabled_.load(std::memory_order_relaxed);
  appliedGateThreshDb_ = gateThreshDb_.load(std::memory_order_relaxed);
  appliedGateRelMs_ = gateRelMs_.load(std::memory_order_relaxed);
  appliedDriveEnabled_ = driveEnabled_.load(std::memory_order_relaxed);
  appliedTight_ = tightParam_.load(std::memory_order_relaxed);
  appliedDrive_ = driveParam_.load(std::memory_order_relaxed);
  appliedBite_ = biteParam_.load(std::memory_order_relaxed);
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
    outTrim_.processBlock(mono_.data(), mono_.data(), m);
    for (int c = 0; c < numOutputChannels; ++c)
      for (int i = 0; i < m; ++i)
        outputs[c][offset + i] = mono_[static_cast<size_t>(i)];
    offset += m;
    remaining -= m;
  }
}
} // namespace tdm

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
  nam_.reset(sampleRate, maxBlockSize);
  ir_.reset(sampleRate);
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

void TechDeathRig::processBlock(const float* const* inputs, int numInputChannels, float* const* outputs,
                                int numOutputChannels, int numFrames)
{
  assert(inputs != nullptr && outputs != nullptr && numInputChannels >= 1 && numOutputChannels >= 1
         && numFrames >= 0 && maxBlock_ > 0);
  if (numFrames <= 0)
    return;
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
    for (int c = 0; c < numOutputChannels; ++c)
      for (int i = 0; i < m; ++i)
        outputs[c][offset + i] = mono_[static_cast<size_t>(i)];
    offset += m;
    remaining -= m;
  }
}
} // namespace tdm

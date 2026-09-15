#include "dsp/NamStage.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace tdm
{
NamStage::NamStage() = default;
NamStage::~NamStage() = default;

void NamStage::loadModel(const std::string& path, double sampleRate, int maxBlockSize)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("NamStage: sample rate must be positive");
  if (maxBlockSize <= 0)
    throw std::runtime_error("NamStage: max block size must be positive");

  std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(path));
  if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
    throw std::runtime_error("NamStage: model must be mono (1 in / 1 out)");

  // M0 has no sample-rate converter (see docs/milestone0-smoke.md risks):
  // the model must natively match the host rate.
  const double expected = model->GetExpectedSampleRate();
  if (expected > 0.0 && std::fabs(expected - sampleRate) > 1.0)
    throw std::runtime_error("NamStage: model expects " + std::to_string(expected)
                             + " Hz but host runs at " + std::to_string(sampleRate)
                             + " Hz (resampling not implemented in M0)");

  model_ = std::move(model);
  reset(sampleRate, maxBlockSize);
}

void NamStage::clear()
{
  model_.reset();
}

double NamStage::expectedSampleRate() const
{
  return model_ ? model_->GetExpectedSampleRate() : -1.0;
}

void NamStage::reset(double sampleRate, int maxBlockSize)
{
  sampleRate_ = sampleRate;
  maxBlock_ = maxBlockSize;
  inScratch_.assign(static_cast<size_t>(maxBlockSize), 0.0);
  outScratch_.assign(static_cast<size_t>(maxBlockSize), 0.0);
  ptrsIn_[0] = inScratch_.data();
  ptrsOut_[0] = outScratch_.data();
  if (model_)
  {
    model_->Reset(sampleRate, maxBlockSize);
    model_->prewarm();
  }
}

void NamStage::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!model_ || maxBlock_ <= 0)
  {
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  int remaining = numFrames;
  int offset = 0;
  while (remaining > 0)
  {
    const int m = remaining < maxBlock_ ? remaining : maxBlock_;
    for (int i = 0; i < m; ++i)
      inScratch_[static_cast<size_t>(i)] = static_cast<double>(input[offset + i]);
    model_->process(ptrsIn_, ptrsOut_, m);
    for (int i = 0; i < m; ++i)
      output[offset + i] = static_cast<float>(outScratch_[static_cast<size_t>(i)]);
    offset += m;
    remaining -= m;
  }
}
} // namespace tdm

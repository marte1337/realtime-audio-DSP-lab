#include "dsp/CabIrStage.h"
#include "dsp/WavFile.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
void CabIrStage::loadIr(const std::string& path, double hostSampleRate, int maxTaps)
{
  if (hostSampleRate <= 0.0)
    throw std::runtime_error("CabIr: host sample rate must be positive");
  if (maxTaps <= 0)
    throw std::runtime_error("CabIr: max taps must be positive");

  MonoWav wav = loadWavMono(path);
  if (std::fabs(wav.sampleRate - hostSampleRate) > 1.0)
    throw std::runtime_error("CabIr: IR is " + std::to_string(wav.sampleRate) + " Hz but host runs at "
                             + std::to_string(hostSampleRate) + " Hz (resampling not implemented in M0)");

  int taps = static_cast<int>(wav.samples.size());
  if (taps > maxTaps)
    taps = maxTaps;
  if (taps <= 0)
    throw std::runtime_error("CabIr: IR is empty: " + path);

  // Documented level decision: same compensation as the upstream NAM plugin
  // so a shared IR reads at the same loudness in both hosts.
  const float gain = std::pow(10.0f, kReferenceGainDb / 20.0f) * (48000.0f / static_cast<float>(hostSampleRate));
  weights_.resize(static_cast<size_t>(taps));
  for (int k = 0; k < taps; ++k)
    weights_[static_cast<size_t>(k)] = gain * wav.samples[static_cast<size_t>(k)];
  history_.assign(static_cast<size_t>(taps), 0.0f);
  length_ = taps;
  pos_ = 0;
  loadedSr_ = hostSampleRate;
}

void CabIrStage::clear()
{
  weights_.clear();
  history_.clear();
  length_ = 0;
  pos_ = 0;
}

void CabIrStage::reset(double sampleRate)
{
  (void)sampleRate; // history clear only; weights are SR-baked at load time
  history_.assign(history_.size(), 0.0f);
  pos_ = 0;
}

void CabIrStage::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (length_ <= 0)
  {
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  const int len = length_;
  for (int i = 0; i < numFrames; ++i)
  {
    const float x = input[i];
    history_[static_cast<size_t>(pos_)] = x;
    double acc = 0.0;
    int idx = pos_;
    for (int k = 0; k < len; ++k)
    {
      acc += static_cast<double>(weights_[static_cast<size_t>(k)]) * history_[static_cast<size_t>(idx)];
      if (--idx < 0)
        idx += len;
    }
    output[i] = static_cast<float>(acc);
    if (++pos_ >= len)
      pos_ = 0;
  }
}
} // namespace tdm

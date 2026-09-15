#pragma once

// Minimal WAV support for M0: mono-averaging loader + float32 writer.
// File IO only; never call from the realtime thread.

#include <string>
#include <vector>

namespace tdm
{
struct MonoWav
{
  std::vector<float> samples; // mono, nominally [-1, 1]
  double sampleRate = 0.0;
  int sourceChannels = 0;
};

// Throws std::runtime_error on any failure. Accepts PCM 8/16/24/32-bit and
// float32/float64, mono or stereo (stereo is averaged to mono).
MonoWav loadWavMono(const std::string& path);

// Writes 32-bit float WAV, interleaved from per-channel pointers.
// Throws std::runtime_error on failure.
void writeWavFloat32(const std::string& path, const float* const* channels, int numChannels, int numFrames,
                     double sampleRate);
} // namespace tdm

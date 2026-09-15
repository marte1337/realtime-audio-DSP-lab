#pragma once

// Milestone 0 cabinet IR: mono direct-form FIR convolver.
//
// Matches the upstream NAM plugin's IR staging gain (-18 dB ref comp) so
// levels translate 1:1, and documents it loudly instead of hiding it.
// Like NamStage, loading/reset run off-RT; processBlock() is RT-safe.

#include <string>
#include <vector>

namespace tdm
{
class CabIrStage
{
public:
  static constexpr int kMaxTaps = 8192;
  // Reference gain compensation, applied to the weights at load time:
  // gain = 10^(-18/20) * 48000 / hostRate (see upstream ImpulseResponse).
  static constexpr float kReferenceGainDb = -18.0f;

  // Load a WAV IR (off-RT). Throws on any failure, including sample-rate
  // mismatch: M0 does not resample, the IR must match the host rate.
  void loadIr(const std::string& path, double hostSampleRate, int maxTaps = kMaxTaps);
  void clear();

  bool hasIr() const { return length_ > 0; }
  int length() const { return length_; }
  double loadedSampleRate() const { return loadedSr_; }

  // Clears history only (weights survive). Call after sample-rate changes
  // only with a matching rate; reload the IR after an SR change.
  void reset(double sampleRate);

  // Mono float processing, in-place safe. Bypasses (copies) with no IR.
  void processBlock(const float* input, float* output, int numFrames);

private:
  std::vector<float> weights_; // h[0..L-1], gain-compensated
  std::vector<float> history_; // ring of past inputs, size L
  int length_ = 0;
  int pos_ = 0;
  double loadedSr_ = 0.0;
};
} // namespace tdm

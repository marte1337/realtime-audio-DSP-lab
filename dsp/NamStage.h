#pragma once

// Milestone 0: owns one NAM neural-amp model for the rig.
//
// Realtime contract: loadModel()/clear()/reset() are NOT realtime-safe
// (they allocate, touch files, and prewarm). processBlock() IS realtime-safe
// once reset() has run: no allocation, no locks, no file IO.

#include <memory>
#include <string>
#include <vector>

namespace nam
{
class DSP;
}

namespace tdm
{
class NamStage
{
public:
  NamStage();
  ~NamStage();
  NamStage(const NamStage&) = delete;
  NamStage& operator=(const NamStage&) = delete;

  // Load a .nam file (off-RT). Throws std::runtime_error on any failure,
  // including sample-rate mismatch: M0 has no resampler, so the model must
  // natively match the host rate. Leaves any previously loaded model in place.
  void loadModel(const std::string& path, double sampleRate, int maxBlockSize);
  void clear();

  bool hasModel() const { return model_ != nullptr; }
  // Expected model rate in Hz, or -1 when unknown / no model.
  double expectedSampleRate() const;

  // (Re)size scratch and reset model state (off-RT context).
  void reset(double sampleRate, int maxBlockSize);

  // Mono float processing, in-place safe. Bypasses (copies) with no model.
  // Large blocks are chunked internally so the model never sees more than
  // maxBlockSize frames per call.
  void processBlock(const float* input, float* output, int numFrames);

private:
  std::unique_ptr<nam::DSP> model_;
  double sampleRate_ = 0.0;
  int maxBlock_ = 0;
  // float<->double (NAM_SAMPLE) conversion scratch, sized maxBlock_.
  std::vector<double> inScratch_;
  std::vector<double> outScratch_;
  double* ptrsIn_[1] = {nullptr};
  double* ptrsOut_[1] = {nullptr};
};
} // namespace tdm

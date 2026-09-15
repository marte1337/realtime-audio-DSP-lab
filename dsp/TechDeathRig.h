#pragma once

// Rig: Input -> Input Trim -> TechDeathGate -> NAM A2 -> Cabinet IR -> Output.
//
// Mono internal path. Multi-channel input is averaged to mono (documented
// choice: avoids the +6 dB surprise of summing; stereo width tricks come
// later). Output broadcasts the mono result to every channel.
// Stages without a loaded asset bypass transparently; the gate bypasses
// exactly until explicitly enabled, preserving Milestone 0 behavior.
// Input Trim defaults to 0 dB, at which it passes input bit-exactly.

#include <string>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/InputTrim.h"
#include "dsp/NamStage.h"
#include "dsp/TechDeathGate.h"

namespace tdm
{
class TechDeathRig
{
public:
  // Off-RT: sizes scratch, resets both stages.
  void reset(double sampleRate, int maxBlockSize);

  // Off-RT loaders; require reset() first so the host rate is known.
  void loadNam(const std::string& path);
  void loadIr(const std::string& path);
  void clearNam() { nam_.clear(); }
  void clearIr() { ir_.clear(); }

  bool hasNam() const { return nam_.hasModel(); }
  bool hasIr() const { return ir_.hasIr(); }
  double sampleRate() const { return sampleRate_; }
  int maxBlockSize() const { return maxBlock_; }

  // Gate controls (off-RT setters). Disabled by default: bypass preserves
  // Milestone 0 gain/tone exactly until the gate is explicitly enabled.
  void setGateEnabled(bool enabled) { gate_.setEnabled(enabled); }
  void setGateThresholdDb(float db) { gate_.setThresholdDb(db); }
  void setGateReleaseMs(float ms) { gate_.setReleaseMs(ms); }
  bool isGateEnabled() const { return gate_.isEnabled(); }

  // Input Trim control (off-RT setter). Defaults to 0 dB, which passes
  // input bit-exactly and preserves Milestone 0 behavior.
  void setInputTrimDb(float db) { trim_.setTrimDb(db); }
  float inputTrimDb() const { return trim_.trimDb(); }

  // RT-safe after reset(). inputs[nIn][nFrames] -> outputs[nOut][nOut].
  void processBlock(const float* const* inputs, int numInputChannels, float* const* outputs,
                    int numOutputChannels, int numFrames);

private:
  InputTrim trim_;
  TechDeathGate gate_;
  NamStage nam_;
  CabIrStage ir_;
  std::vector<float> mono_; // internal scratch, sized maxBlock_
  double sampleRate_ = 0.0;
  int maxBlock_ = 0;
};
} // namespace tdm

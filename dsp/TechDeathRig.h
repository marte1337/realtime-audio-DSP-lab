#pragma once

// Rig: Input -> Input Trim -> TechDeathGate -> TightDrive -> NAM A2
//   -> Cabinet IR -> Output Trim -> Output.
//
// Mono internal path. Multi-channel input is averaged to mono (documented
// choice: avoids the +6 dB surprise of summing; stereo width tricks come
// later). Output broadcasts the mono result to every channel.
// Stages without a loaded asset bypass transparently; the gate bypasses
// exactly until explicitly enabled, preserving Milestone 0 behavior.
// Input Trim defaults to 0 dB, at which it passes input bit-exactly.
// TightDrive is disabled by default and bypasses exactly when off.
// Output Trim defaults to 0 dB, at which it passes input bit-exactly; it
// only changes post-chain listening level, never NAM drive.

#include <string>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/InputTrim.h"
#include "dsp/NamStage.h"
#include "dsp/TechDeathGate.h"
#include "dsp/TightDrive.h"
#include "dsp/OutputTrim.h"

namespace tdm
{
class TechDeathRig
{
public:
  // Off-RT: sizes scratch, resets all stages.
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

  // TightDrive controls (off-RT setters). Disabled by default: bypass
  // preserves the previous rig behavior exactly until explicitly enabled.
  void setDriveEnabled(bool enabled) { drive_.setEnabled(enabled); }
  void setTight(float v) { drive_.setTight(v); }
  void setDrive(float v) { drive_.setDrive(v); }
  void setBite(float v) { drive_.setBite(v); }
  bool isDriveEnabled() const { return drive_.isEnabled(); }

  // Output Trim control (off-RT setter). Defaults to 0 dB, which passes
  // input bit-exactly and preserves existing behavior. Post-chain only:
  // loudness matching without touching NAM saturation or drive response.
  void setOutputTrimDb(float db) { outTrim_.setTrimDb(db); }
  float outputTrimDb() const { return outTrim_.trimDb(); }

  // RT-safe after reset(). inputs[nIn][nFrames] -> outputs[nOut][nOut].
  void processBlock(const float* const* inputs, int numInputChannels, float* const* outputs,
                    int numOutputChannels, int numFrames);

private:
  InputTrim trim_;
  TechDeathGate gate_;
  TightDrive drive_;
  NamStage nam_;
  CabIrStage ir_;
  OutputTrim outTrim_;
  std::vector<float> mono_; // internal scratch, sized maxBlock_
  double sampleRate_ = 0.0;
  int maxBlock_ = 0;
};
} // namespace tdm

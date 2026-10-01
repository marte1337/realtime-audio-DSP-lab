#pragma once

// TransposeInsert: production-safe seam for an optional mono transpose stage
// inside TechDeathRig (between Gate and TightDrive).
//
// The rig owns nothing here: it holds a raw non-owning pointer (null by
// default) and calls process() in place when set. The interface carries no
// DSP, no lab types, and no third-party dependencies, so production targets
// (tdm_render, tdm_live, tdm_dev, the rig unit tests) gain no JUCE, TONE3000,
// benchmark, or lab symbols by including it. Only DEV/benchmark binaries
// provide an implementation (see dsp/lab/Pitch/DevTranspose.h).
//
// Threading: setTransposeInsert()/reset() are OFF-RT ONLY (call with audio
// stopped, before start). process() is RT-safe after reset().

namespace tdm
{
class TransposeInsert
{
public:
  virtual ~TransposeInsert() = default;

  // Off-RT: validate rate/block, (re)allocate, clear state. Called by
  // TechDeathRig::reset() when an insert is installed, and may also be
  // called directly pre-start. Must be idempotent.
  virtual void reset(double sampleRate, int maxBlockSize) = 0;

  // RT-safe after reset(). In-place safe mono processing: exactly numFrames
  // samples are read from input and written to output (which may alias).
  // numFrames never exceeds the maxBlockSize from reset().
  virtual void process(const float* input, float* output, int numFrames) = 0;

  // Algorithmic latency in samples, valid post-reset. Used for honest live
  // latency accounting only; the live path never compensates it.
  virtual int latencySamples() const = 0;
};
} // namespace tdm

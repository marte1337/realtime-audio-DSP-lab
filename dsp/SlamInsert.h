#pragma once

// SlamInsert: production-safe seam for DEV-only SLAM audition inserts
// inside TechDeathRig. Three tap positions exist (see
// TechDeathRig::setSlamPreDrive/setSlamPostNam/setSlamPostIr):
//
//   Gate -> Transpose -> [pre-drive] -> TightDrive -> NAM ->
//   [post-NAM] -> IR -> [post-IR] -> ToneShape
//
// The rig owns nothing here: it holds raw non-owning pointers (null by
// default) and calls process() in place when set. The interface carries
// no DSP, no lab types, and no third-party dependencies, so production
// targets gain no lab symbols by including it. Only DEV binaries provide
// an implementation (see dsp/lab/Slam/DevSlam.h). With all three null
// the rig is bit-identical to the unseamed path (TransposeInsert
// precedent).
//
// Threading: the setSlam*() installers and reset() are OFF-RT ONLY (call
// with audio stopped, before start). process() is RT-safe after reset().

namespace tdm
{
class SlamInsert
{
public:
  virtual ~SlamInsert() = default;

  // Off-RT: validate rate/block, (re)allocate, clear state. Called by
  // TechDeathRig::reset() for every installed insert. Must be idempotent
  // (one router object may serve all three taps and be reset 3x).
  virtual void reset(double sampleRate, int maxBlockSize) = 0;

  // RT-safe after reset(). In-place safe mono processing: exactly numFrames
  // samples are read from input and written to output (which may alias).
  // numFrames never exceeds the maxBlockSize from reset().
  virtual void process(const float* input, float* output, int numFrames) = 0;

  // Algorithmic latency in samples, valid post-reset.
  virtual int latencySamples() const = 0;
};
} // namespace tdm

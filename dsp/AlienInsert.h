#pragma once

// AlienInsert: production-safe seam for the DEV-only Alien FX lab insert
// inside TechDeathRig. One tap position (see TechDeathRig::setAlienPostIr):
//
//   ... -> NAM -> IR -> [SLAM post-IR] -> [Alien post-IR] -> ToneShape -> ...
//
// Post-IR (pre-ToneShape) processes the amplified/cabinet-filtered guitar
// while letting ToneShape tame shifted harshness afterwards. The rig owns
// nothing: it holds a raw non-owning pointer (null by default) and calls
// process() in place when set. The interface carries no DSP, no lab
// types, and no third-party dependencies, so production targets gain no
// lab symbols by including it. Only DEV binaries provide an
// implementation (see dsp/lab/Alien/DevAlien.h). Null leaves the rig
// bit-identical to the unseamed path (SlamInsert/TransposeInsert
// precedent).
//
// Threading: setAlienPostIr() and reset() are OFF-RT ONLY (call with
// audio stopped, before start). process() is RT-safe after reset().

namespace tdm
{
class AlienInsert
{
public:
  virtual ~AlienInsert() = default;

  // Off-RT: validate rate/block, (re)allocate, clear state. Called by
  // TechDeathRig::reset(). Must be idempotent.
  virtual void reset(double sampleRate, int maxBlockSize) = 0;

  // RT-safe after reset(). In-place safe mono processing: exactly numFrames
  // samples are read from input and written to output (which may alias).
  // numFrames never exceeds the maxBlockSize from reset().
  virtual void process(const float* input, float* output, int numFrames) = 0;

  // Algorithmic latency in samples, valid post-reset.
  virtual int latencySamples() const = 0;
};
} // namespace tdm

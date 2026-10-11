#pragma once

// SpecInsert: production-safe seam for the DEV-only Special FX lab
// inserts inside TechDeathRig. Two tap positions (pre-gain and post-IR)
// so the lab can A/B insertion points without touching production
// routing:
//
//   ... -> Transpose -> [SLAM pre] -> [Spec pre] -> TightDrive -> NAM -> ...
//   ... -> IR -> [SLAM post-IR] -> [Alien post-IR] -> [Spec post] -> ToneShape -> ...
//
// The rig owns nothing: raw non-owning pointers (null by default),
// process() in place when set. The interface carries no DSP, no lab
// types, and no third-party dependencies, so production targets gain no
// lab symbols by including it. Only DEV binaries provide an
// implementation (see dsp/lab/SpecialFx/DevSpecFx.h). Null leaves the
// rig bit-identical to the unseamed path (SlamInsert/AlienInsert
// precedent).
//
// Threading: setSpecPre()/setSpecPost() and reset() are OFF-RT ONLY
// (call with audio stopped, before start). process() is RT-safe after
// reset().

namespace tdm
{
class SpecInsert
{
public:
  virtual ~SpecInsert() = default;

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

// LabMultiPitch: complementary-split dual-resolution pitch shift. See header.

#include "dsp/lab/Pitch/LabMultiPitch.h"

#include <cstring>

namespace tdm
{
namespace lab
{
LabMultiPitch::LabMultiPitch()
{
  low_.setConfig(kLowFftSize, kLowHop);
  high_.setConfig(kHighFftSize, kHighHop);
  low_.setEnabled(true); // bands are infrastructure: always on internally;
  high_.setEnabled(true); // the top-level enabled_/bypass0_ gates do the muxing
}

void LabMultiPitch::reset(double sampleRate)
{
  xover_.reset(sampleRate); // validates the rate first (throws)
  sampleRate_ = sampleRate;
  bypass0_ = (shiftSt_ == 0.0f); // exact equality, like LabPitchShift

  low_.setShiftSt(shiftSt_);
  high_.setShiftSt(shiftSt_);
  low_.reset(sampleRate);
  high_.reset(sampleRate);

  // The high path is always faster (smaller N AND smaller N-Ha term at
  // any r < 1); pad it up to the low path. Guarded, not assumed.
  align_ = low_.latencySamples() - high_.latencySamples();
  if (align_ < 0)
    align_ = 0;
  latency_ = bypass0_ ? 0 : xover_.delaySamples() + low_.latencySamples();
  tail_ = low_.tailSamples();

  alignRing_.assign(static_cast<size_t>(align_ + 1), 0.0f);
  alignPos_ = 0;
}

void LabMultiPitch::setShiftSt(float semitones)
{
  shiftSt_ = semitones < LabPitchShift::kMinShiftSt
      ? LabPitchShift::kMinShiftSt
      : (semitones > LabPitchShift::kMaxShiftSt ? LabPitchShift::kMaxShiftSt : semitones);
}

void LabMultiPitch::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void LabMultiPitch::processBlock(const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (!enabled_ || bypass0_)
  {
    // Exact bypass of the WHOLE processor (in-place is a no-op). Note this
    // deliberately skips the crossover: even with both bands bypassing,
    // the FIR/subtract path would add float error and D delay.
    if (output != input)
      std::memcpy(output, input, static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }
  if (sampleRate_ <= 0.0)
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f; // enabled but never reset: silence, like LabPitchShift
    return;
  }
  // Sample-by-sample: block-size-independent by construction (which is
  // what makes the determinism test across block sizes meaningful).
  const int ringN = static_cast<int>(alignRing_.size());
  for (int i = 0; i < numFrames; ++i)
  {
    const float x = input[i]; // read first: in-place safe
    float lo = 0.0f, hi = 0.0f;
    xover_.processSample(x, &lo, &hi);
    float loOut = 0.0f, hiOut = 0.0f;
    low_.processBlock(&lo, &loOut, 1);
    high_.processBlock(&hi, &hiOut, 1);
    alignRing_[static_cast<size_t>(alignPos_ % ringN)] = hiOut;
    const float hiAligned = alignPos_ < align_ ? 0.0f
                                               : alignRing_[static_cast<size_t>((alignPos_ - align_) % ringN)];
    ++alignPos_;
    output[i] = loOut + hiAligned;
  }
}
} // namespace lab
} // namespace tdm

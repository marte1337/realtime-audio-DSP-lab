// LabWsolaLive: see header. No allocation, locks, or IO on the audio
// thread: prepare() sizes everything; processBlock() only indexes rings
// and advances plain counters.

#include "dsp/lab/Pitch/LabWsolaLive.h"

#include <stdexcept>

namespace tdm
{
namespace lab
{
// The live-toggle request flag must never lower to a lock: fail the
// build loudly on any platform where bool atomics are not lock-free.
static_assert(std::atomic<bool>::is_always_lock_free, "LabWsolaLive needs lock-free bool atomics");

LabWsolaLive::LabWsolaLive() = default;

void LabWsolaLive::prepare(double sampleRate, float shiftSt, bool startEnabled, int maxBlockFrames)
{
  if (!(shiftSt >= kMinShiftSt && shiftSt <= kMaxShiftSt))
    throw std::invalid_argument("LabWsolaLive: shift must be in [-7, 0]");
  if (maxBlockFrames <= 0)
    throw std::invalid_argument("LabWsolaLive: maxBlockFrames must be positive");
  shifter_.setConfig(kWindowMs); // throws only on logic error (pinned const)
  shifter_.setEnabled(true); // permanently on: bypass lives in the wrapper
  shifter_.setShiftSt(shiftSt); // clamped by the shifter; range checked above
  shifter_.reset(sampleRate); // validates rate, allocates, clears state
  sampleRate_ = sampleRate;
  shiftSt_ = shiftSt;
  latency_ = shifter_.latencySamples();
  // Ring holds L+B slots: block writes of up to B samples can never lap
  // the read-back tap (aliases sit >= L+B away, always in the future).
  dryDelay_.assign(static_cast<size_t>(latency_ + maxBlockFrames), 0.0f);
  delayWrite_ = latency_; // cursor starts at L: read-back (t-L) never negative
  enabledReq_.store(startEnabled, std::memory_order_release);
  ramp_ = startEnabled ? 1.0f : 0.0f;
}

void LabWsolaLive::setEnabled(bool enabled)
{
  enabledReq_.store(enabled, std::memory_order_release);
}

void LabWsolaLive::processBlock(const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (sampleRate_ <= 0.0)
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f; // prepared-never: silence, like the shifter
    return;
  }
  const float target = enabledReq_.load(std::memory_order_acquire) ? 1.0f : 0.0f;
  const int ring = static_cast<int>(dryDelay_.size()); // L+B, >= 2 always
  // Pass 1: tap dry into the latency ring BEFORE the shifter runs, so an
  // in-place call (input == output) cannot clobber it.
  for (int i = 0; i < numFrames; ++i)
    dryDelay_[static_cast<size_t>((delayWrite_ + i) % ring)] = input[i];
  // Pass 2: wet (the shifter is itself in-place safe).
  shifter_.processBlock(input, output, numFrames);
  // Pass 3: mix latency-matched dry with wet. Read-back trails the write
  // cursor by exactly L; with a size-L+B ring that slot always holds
  // input[t-L] (zeros pre-roll, and the cursor starts at L so the index
  // never goes negative). Steady states skip the mix (bit-exact paths,
  // cheaper); only the ramp window pays for the blend.
  if (ramp_ == 1.0f && target == 1.0f)
  {
    // Steady wet: output already holds the shifter render.
  }
  else if (ramp_ == 0.0f && target == 0.0f)
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = dryDelay_[static_cast<size_t>((delayWrite_ + i - latency_) % ring)];
  }
  else
  {
    // Started mid-ramp: once the ramp parks mid-block, remaining samples
    // take the exact paths, so block edges can never change a sample's
    // treatment (block-size determinism).
    for (int i = 0; i < numFrames; ++i)
    {
      const float dryLate = dryDelay_[static_cast<size_t>((delayWrite_ + i - latency_) % ring)];
      if (ramp_ < target)
      {
        ramp_ += 1.0f / kRampSamples;
        if (ramp_ > target)
          ramp_ = target;
      }
      else if (ramp_ > target)
      {
        ramp_ -= 1.0f / kRampSamples;
        if (ramp_ < target)
          ramp_ = target;
      }
      if (ramp_ == 1.0f)
      {
        // Exact wet (already in output).
      }
      else if (ramp_ == 0.0f)
      {
        output[i] = dryLate;
      }
      else
      {
        output[i] = dryLate + ramp_ * (output[i] - dryLate);
      }
    }
  }
  delayWrite_ += numFrames;
}
} // namespace lab
} // namespace tdm

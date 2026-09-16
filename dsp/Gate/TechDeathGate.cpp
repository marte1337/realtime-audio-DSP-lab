#include "dsp/Gate/TechDeathGate.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
// One-pole smoothing coefficient for time constant tauSec at sampleRate:
// y += (target - y) * coeff converges ~63% per tauSec.
float onePoleCoeff(double tauSec, double sampleRate)
{
  const double c = 1.0 - std::exp(-1.0 / (tauSec * sampleRate));
  return static_cast<float>(c < 1.0 ? c : 1.0);
}

float dbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

TechDeathGate::TechDeathGate()
{
  refreshDerived();
}

void TechDeathGate::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("Gate: sample rate must be positive");
  sampleRate_ = sampleRate;
  refreshDerived();
  // Documented safe state: closed. First pick attack reopens within ~1 ms;
  // no sub-threshold hiss burst can pass after a reset/sample-rate change.
  envelope_ = 0.0f;
  gain_ = floorGain_;
  relStage_ = floorGain_;
  holdLeft_ = 0;
  confirmLeft_ = 0;
  state_ = State::Closed;
}

void TechDeathGate::setThresholdDb(float db)
{
  thresholdDb_ = clampf(db, kMinThresholdDb, kMaxThresholdDb);
  refreshDerived();
}

void TechDeathGate::setReleaseMs(float ms)
{
  releaseMs_ = clampf(ms, kMinReleaseMs, kMaxReleaseMs);
  refreshDerived();
}

void TechDeathGate::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void TechDeathGate::refreshDerived()
{
  openLevel_ = dbToLinear(thresholdDb_);
  retriggerLevel_ = dbToLinear(thresholdDb_ + kRetriggerMarginDb);
  closeLevel_ = dbToLinear(thresholdDb_ - kHysteresisDb);
  floorGain_ = dbToLinear(kFloorDb);
  if (sampleRate_ > 0.0)
  {
    detDecay_ = static_cast<float>(std::exp(-1.0 / ((kDetectorReleaseMs / 1000.0) * sampleRate_)));
    attackCoeff_ = onePoleCoeff(kGainAttackMs / 1000.0, sampleRate_);
    // Release is defined as the 60 dB fall time of the two-stage cascade.
    // With both stages sharing tau, the fade follows (1 + t/tau) * e^(-t/tau)
    // and reaches -60 dB at t ~= 9.23 * tau, so tau = Release / 9.23 keeps
    // the documented timing while starting the fade with zero slope.
    const double tauSec = (releaseMs_ / 1000.0) / 9.23;
    releaseCoeff_ = onePoleCoeff(tauSec, sampleRate_);
    holdSamples_ = static_cast<int>((kHoldMs / 1000.0) * sampleRate_ + 0.5);
    confirmSamples_ = static_cast<int>((kCloseConfirmMs / 1000.0) * sampleRate_ + 0.5);
  }
}

void TechDeathGate::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!enabled_ || sampleRate_ <= 0.0)
  {
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    const float x = input[i];
    // Peak detector: instantaneous attack, exponential release.
    const float ax = std::fabs(x);
    if (ax > envelope_)
      envelope_ = ax;
    else
    {
      envelope_ *= detDecay_;
      // Denormal guard: snapped long-silence envelope is exactly 0, which
      // keeps the comparator closed and the FPU out of denormal range.
      if (envelope_ < 1e-12f)
        envelope_ = 0.0f;
    }
    // State machine: CLOSED -> OPEN -> CLOSING -> CLOSED.
    // CLOSED trusts the normal Threshold: initial picks engage with zero
    // delay and no permanent penalty. OPEN behaves as before, with the
    // hold bridging ripple valleys. CLOSING keeps the release fade
    // running but only a convincing new attack (above the retrigger bar)
    // reopens it — residual tail beating, however long it lasts, cannot
    // cycle the gate. CLOSED is restored after 250 ms continuously below
    // the close level; any louder peak restarts that confirmation.
    switch (state_)
    {
    case State::Closed:
      if (envelope_ > openLevel_)
      {
        state_ = State::Open;
        holdLeft_ = holdSamples_;
      }
      break;
    case State::Open:
      if (envelope_ > openLevel_)
        holdLeft_ = holdSamples_;
      else if (envelope_ < closeLevel_)
      {
        if (holdLeft_ > 0)
          --holdLeft_;
        else
        {
          state_ = State::Closing;
          confirmLeft_ = confirmSamples_;
        }
      }
      break;
    case State::Closing:
      if (envelope_ > retriggerLevel_)
      {
        state_ = State::Open;
        holdLeft_ = holdSamples_;
      }
      else if (envelope_ > closeLevel_)
        confirmLeft_ = confirmSamples_; // tail still alive: restart confirmation
      else if (confirmLeft_ > 0)
        --confirmLeft_;
      else
        state_ = State::Closed;
      break;
    }
    // Smoothed gain: fast ramp up in OPEN; otherwise the two-stage cascade
    // (relStage_ leads, gain_ follows with the same coefficient) keeps the
    // v1.1 S-shaped fade. Hard-clamped so the [floor, 1] invariant holds
    // exactly (no drift, no overshoot).
    if (state_ == State::Open)
    {
      gain_ += (1.0f - gain_) * attackCoeff_;
      relStage_ = gain_;
    }
    else
    {
      relStage_ += (floorGain_ - relStage_) * releaseCoeff_;
      gain_ += (relStage_ - gain_) * releaseCoeff_;
    }
    if (gain_ > 1.0f)
      gain_ = 1.0f;
    else if (gain_ < floorGain_)
      gain_ = floorGain_;
    if (relStage_ > 1.0f)
      relStage_ = 1.0f;
    else if (relStage_ < floorGain_)
      relStage_ = floorGain_;
    output[i] = x * gain_;
  }
}
} // namespace tdm

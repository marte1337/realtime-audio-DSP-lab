#pragma once

// TechDeathGate: realtime-safe input noise gate for tight tech-death rhythm.
//
// Detector: peak follower with instantaneous attack and ~1 ms exponential
// release, so intentional pick transients open the gate with zero detector
// latency while the envelope still bridges single waveform cycles.
// Comparator: fixed 6 dB hysteresis (open above Threshold, close below
// Threshold - 6 dB) so a decaying note hovering at the threshold cannot
// chatter. Hold: after the envelope drops below the close level the gate
// waits a fixed 8 ms before closing, bridging peak-detector ripple valleys
// on low sustained notes (open E2 ripple period is ~6.1 ms).
// Gain: never jumps. A one-pole smoother drives gain toward 1 (fast
// internal attack, ~0.15 ms) or toward a -80 dB floor (Release-controlled
// decay). Threshold/Release moves bend the trajectory without steps, so no
// extra parameter smoothing is needed.
//
// Realtime contract: setters/reset() are off-RT (they may be called on the
// audio thread only if the host guarantees it; they allocate nothing but
// recompute coefficients). processBlock() is RT-safe: no allocation, no
// locks, no file IO, deterministic, finite output for finite input.
//
// Expected signal range: finite floats, nominally within [-1, 1] DI guitar.

#include <string>

namespace tdm
{
class TechDeathGate
{
public:
  // Public v1 parameters.
  static constexpr float kMinThresholdDb = -60.0f; // very permissive
  static constexpr float kMaxThresholdDb = -20.0f; // extremely tight
  static constexpr float kDefaultThresholdDb = -40.0f;
  static constexpr float kMinReleaseMs = 10.0f; // machine-like stops
  static constexpr float kMaxReleaseMs = 500.0f; // natural sustain
  static constexpr float kDefaultReleaseMs = 50.0f;

  // Documented internal constants (fixed in v1, not exposed).
  static constexpr float kHysteresisDb = 6.0f; // close level = threshold - 6 dB
  static constexpr float kHoldMs = 8.0f; // close delay bridging ripple valleys
  static constexpr float kDetectorReleaseMs = 1.0f; // peak-follower fall time
  static constexpr float kGainAttackMs = 0.15f; // opening ramp, click-free
  static constexpr float kFloorDb = -80.0f; // closed suppression depth

  TechDeathGate();

  // Off-RT: (re)compute coefficients, return to documented safe state:
  // closed (gain = floor, envelope = 0), so no startup hiss burst passes.
  void reset(double sampleRate);

  // Parameter setters (off-RT context). Values are clamped to the v1 ranges.
  void setThresholdDb(float db);
  void setReleaseMs(float ms);
  void setEnabled(bool enabled);

  float thresholdDb() const { return thresholdDb_; }
  float releaseMs() const { return releaseMs_; }
  bool isEnabled() const { return enabled_; }
  // Read-only observation for tests / future metering. RT-safe.
  float currentGain() const { return gain_; }
  bool isOpen() const { return open_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly.
  void processBlock(const float* input, float* output, int numFrames);

private:
  void refreshDerived(); // recompute coeffs/thresholds from params + rate

  double sampleRate_ = 0.0;
  float thresholdDb_ = kDefaultThresholdDb;
  float releaseMs_ = kDefaultReleaseMs;
  bool enabled_ = false; // explicit opt-in; rig behavior unchanged until set

  // Derived (refreshed by reset()/setters).
  float openLevel_ = 0.01f; // linear peak open threshold
  float closeLevel_ = 0.005f; // linear peak close threshold (hysteresis)
  float detDecay_ = 0.0f; // per-sample detector release multiplier
  float attackCoeff_ = 1.0f; // per-sample gain-opening one-pole coeff
  float releaseCoeff_ = 1.0f; // per-sample gain-closing one-pole coeff
  float floorGain_ = 0.0001f; // linear closed gain (-80 dB)
  int holdSamples_ = 0;

  // State (scalars only; nothing to preallocate).
  float envelope_ = 0.0f;
  float gain_ = 0.0001f;
  int holdLeft_ = 0;
  bool open_ = false;
};
} // namespace tdm

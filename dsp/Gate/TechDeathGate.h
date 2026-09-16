#pragma once

// TechDeathGate: realtime-safe input noise gate for tight tech-death rhythm.
//
// Detector: peak follower with instantaneous attack and ~1 ms exponential
// release, so intentional pick transients open the gate with zero detector
// latency while the envelope still bridges single waveform cycles.
// State machine: CLOSED -> OPEN -> CLOSING -> CLOSED.
// CLOSED: fully shut; the envelope opens the gate above Threshold with
//   zero delay, so legitimate initial picks always engage normally.
// OPEN: normal behavior; once the envelope stays below the close level
//   (Threshold - 6 dB) for the fixed 8 ms hold, the gate commits to
//   CLOSING. The hold bridges peak-detector ripple valleys on low
//   sustained notes (open E2 ripple period is ~6.1 ms).
// CLOSING: the release fade runs toward closed, but the gate no longer
//   trusts the ordinary open threshold: residual decay, beating, and
//   sympathetic-string movement must not repeatedly reopen it (the
//   sustained-note stutter). Reopening needs a convincing new attack,
//   i.e. the envelope above Threshold + 12 dB, however long the tail
//   takes — there is no expiry after which faint residue may retrigger.
//   The gate returns to CLOSED only once the envelope has stayed below
//   the close level continuously for 250 ms (any louder peak restarts
//   that confirmation), so a beating tail can never cycle open/closed.
//   Tradeoff: intentional repicks softer than Threshold + 12 dB that land
//   while a previous tail is still alive stay shut until the tail dies;
//   anything hotter reopens within the same sample.
// Gain: never jumps. Gain opens toward 1 through a fast internal attack
// (~0.15 ms). Closing runs through a two-stage cascade (an intermediate
// follower feeds the applied gain, both sharing the Release coefficient),
// which starts the fade with near-zero slope and steepens mid-release. The
// S-shaped knee removes the instantaneous slope discontinuity of a
// single-pole fade at the hold/release boundary — the hard stop that was
// heard as excessive hardness — while Release keeps its meaning: total
// 60 dB fall time from the start of the fade.
// Retrigger discipline (v1.2): the fixed 60 ms post-close window proved to
// be the wrong abstraction — a sustained note keeps decaying and beating
// long after any fixed window expires, and each re-crossing of the normal
// threshold then stutters the gate open again. The CLOSING state above
// replaces it: the elevated retrigger bar persists for the whole closing
// phase instead of expiring on a timer.
// Threshold/Release moves bend the trajectory without steps, so no extra
// parameter smoothing is needed.
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
  // Public v1 parameters. Range retuned from real-guitar auditioning
  // (2026-09-16): the old -60..-20 dB span wasted most of its travel on
  // unusably aggressive values. -80 dB is very permissive (lets quiet
  // pickup/residue detail through), -35 dB is extremely tight, and the
  // -55 dB default is the current audition reference.
  static constexpr float kMinThresholdDb = -80.0f; // very permissive
  static constexpr float kMaxThresholdDb = -35.0f; // extremely tight
  static constexpr float kDefaultThresholdDb = -55.0f;
  static constexpr float kMinReleaseMs = 10.0f; // machine-like stops
  static constexpr float kMaxReleaseMs = 500.0f; // natural sustain
  static constexpr float kDefaultReleaseMs = 50.0f;

  // Documented internal constants (fixed in v1, not exposed).
  static constexpr float kHysteresisDb = 6.0f; // close level = threshold - 6 dB
  // v1.2: retrigger bar while CLOSING = threshold + 12 dB. Sized from
  // probes: the worst sustained-tail beat peak measured -44.1 dBFS at a
  // -50 dB threshold (+5.9 dB), so +12 dB holds ~6 dB headroom over tail
  // beating while real picks (tens of dB hotter) clear it effortlessly.
  static constexpr float kRetriggerMarginDb = 12.0f;
  // v1.2: CLOSING returns to CLOSED after this long with the envelope
  // continuously below the close level. Sized from probes: the longest
  // sub-close valley in a beating tail measured 20.7 ms, so 250 ms cannot
  // be faked by beating but still restores normal threshold semantics
  // promptly after real silence.
  static constexpr float kCloseConfirmMs = 250.0f;
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
  // True only in OPEN. CLOSING already reports false: the close decision
  // is made and the fade is running, even though gain is still falling.
  bool isOpen() const { return state_ == State::Open; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly.
  void processBlock(const float* input, float* output, int numFrames);

private:
  void refreshDerived(); // recompute coeffs/thresholds from params + rate

  // Gate state: CLOSED (fully shut, normal threshold) -> OPEN (passing) ->
  // CLOSING (fade running, elevated retrigger bar) -> CLOSED.
  enum class State : unsigned char
  {
    Closed,
    Open,
    Closing
  };

  double sampleRate_ = 0.0;
  float thresholdDb_ = kDefaultThresholdDb;
  float releaseMs_ = kDefaultReleaseMs;
  bool enabled_ = false; // explicit opt-in; rig behavior unchanged until set

  // Derived (refreshed by reset()/setters).
  float openLevel_ = 0.01f; // linear peak open threshold (CLOSED -> OPEN)
  float retriggerLevel_ = 0.04f; // elevated reopen bar while CLOSING
  float closeLevel_ = 0.005f; // linear peak close threshold (hysteresis)
  float detDecay_ = 0.0f; // per-sample detector release multiplier
  float attackCoeff_ = 1.0f; // per-sample gain-opening one-pole coeff
  float releaseCoeff_ = 1.0f; // per-sample gain-closing one-pole coeff
  float floorGain_ = 0.0001f; // linear closed gain (-80 dB)
  int holdSamples_ = 0;
  int confirmSamples_ = 0; // CLOSING -> CLOSED confirmation length

  // State (scalars only; nothing to preallocate).
  float envelope_ = 0.0f;
  float gain_ = 0.0001f;
  float relStage_ = 0.0001f; // release-cascade intermediate follower
  int holdLeft_ = 0;
  int confirmLeft_ = 0; // sub-close samples still needed to reach CLOSED
  State state_ = State::Closed;
};
} // namespace tdm

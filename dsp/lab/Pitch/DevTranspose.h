#pragma once

// DevTranspose: DEV-only A/B transpose stage (LAB/DEV, never production).
//
// Holds OUR production GuitarTranspose engine (dsp/Pitch) and the frozen
// TONE3000 reference engine side by side behind the TechDeathRig
// transpose seam (Gate -> transpose -> TightDrive). Exactly one engine
// feeds the rig at a time; the selector never touches unrelated rig
// state and each engine keeps its own config. The "ours" side IS the
// production DSP (same class the rig owns); DEV adds only the A/B
// wrapper, the reference engine, and off-RT Config overrides.
//
// Engine availability depends on the build:
// - tdm_transpose_dev defines TDM_HAVE_TONE3000: both engines live.
// - unit tests / any other target: GT2 only (no JUCE, no TONE3000 source).
// Selecting Tone3000 without TDM_HAVE_TONE3000 throws (control thread).
//
// Shared live controls (any thread, applied at block boundaries):
// - enabled: latency-matched bypass (dry delayed by the active engine's
//   latency, 128-sample ramp), the LabWsolaLive house pattern. There is no
//   separate Off engine: bypass IS the off state, with constant feel.
// - engine: Gt2 <-> Tone3000 with a short crossfade (both engines are kept
//   warm by default so the switch is immediate and fair).
// - shiftSt: shared semitones in [-12, +12] (float internally; integers are
//   the normal use, whammy sweeps stay architecturally possible). Applied
//   live to both engines at the block boundary (GT2 adopts per sample, the
//   TONE3000 engine adopts on the next sample; both documented RT-safe).
//
// TONE3000 reference controls (live, any thread):
// - windowMs: 20/30/40/60 (engine-documented live window switch, no reset).
// - tonalityHz: 0 = off (pure shift), else 1000..20000 crossover Hz.
// Defaults are the auditioned reference: 30 ms, Tonality OFF. The reference
// algorithm is frozen: this wrapper only forwards documented live params.
//
// OUR GT2 DEV controls (OFF-RT ONLY: validated at configure time, take
// effect on the next reset()/start; changing them needs an audio restart
// because GT2 derives geometry and allocates rings at reset):
// - the full GuitarTranspose::Config (window/floor/corr/fades/thresholds/
//   onset detector/refractory/history/resync). Every field is validated and
//   clamped; unknown fields are never invented. knownGoodGt2() returns the
//   exact Config that passed the hardware audition (the GT2 defaults).
//
// Realtime contract: configureGt2/resetGt2ToBaseline/reset are off-RT
// (reset allocates rings). All other setters are lock-free atomic stores.
// process() is RT-safe: no allocation, no locks, no I/O; per-block work is
// the two engines (or one with standby off) plus bounded ramps.
//
// Expected signal range: finite floats, nominally gated guitar in [-1, 1].

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dsp/TransposeInsert.h"
#include "dsp/Pitch/GuitarTranspose.h"

namespace tdm
{
namespace lab
{
class DevTranspose : public tdm::TransposeInsert
{
public:
  enum class Engine
  {
    Gt2 = 0,
    Tone3000 = 1,
  };

  static constexpr float kMinShiftSt = -12.0f;
  static constexpr float kMaxShiftSt = 12.0f;
  static constexpr int kRampSamples = 128; // bypass ramp + engine-switch crossfade
  static constexpr int kDefaultT3kWindowMs = 30;
  static constexpr float kDefaultT3kTonalityHz = 0.0f; // off

  // Exact GT2 configuration that passed the hardware audition: the
  // PRODUCTION baseline (GuitarTranspose::Config{}), single-sourced.
  // "Restore GT2 Baseline" restores exactly what production runs.
  // Returned by value; compare field-wise in tests to catch drift.
  static GuitarTranspose::Config knownGoodGt2() { return GuitarTranspose::Config{}; }
  static bool hasTone3000()
  {
#ifdef TDM_HAVE_TONE3000
    return true;
#else
    return false;
#endif
  }

  DevTranspose();
  ~DevTranspose() override;

  DevTranspose(const DevTranspose&) = delete;
  DevTranspose& operator=(const DevTranspose&) = delete;

  // ---- OFF-RT GT2 configuration (audio stopped; takes effect on reset) ----
  // Validated immediately (throws std::invalid_argument); stored on success.
  void configureGt2(const GuitarTranspose::Config& cfg);
  void resetGt2ToBaseline() { configureGt2(knownGoodGt2()); }
  const GuitarTranspose::Config& gt2Config() const { return gt2Cfg_; }
  // Standby warming (off-RT): when true (default) both engines render every
  // block so A/B switches are immediate; when false only the active engine
  // renders (production-like CPU, cold standby after a switch).
  void setWarmStandby(bool warm) { warmStandby_ = warm; }
  bool warmStandby() const { return warmStandby_; }

  // ---- Live controls (any thread; applied at block boundaries) ----
  void setEnabled(bool enabled) { enabledReq_.store(enabled, std::memory_order_release); }
  // Throws (control thread) on Tone3000 without TDM_HAVE_TONE3000; invalid
  // values never reach the audio thread.
  void setEngine(Engine engine);
  void setShiftSt(float semitones);
  // TONE3000 live controls (throw on invalid window/tonality; no-ops for
  // audio when TDM_HAVE_TONE3000 is off, but still stored for display).
  void setT3kWindowMs(int ms);
  void setT3kTonalityHz(float hz);

  bool isEnabled() const { return enabledReq_.load(std::memory_order_acquire); }
  Engine engine() const { return static_cast<Engine>(engineReq_.load(std::memory_order_acquire)); }
  float shiftSt() const { return shiftReq_.load(std::memory_order_acquire); }
  int t3kWindowMs() const { return t3kWindowReq_.load(std::memory_order_acquire); }
  float t3kTonalityHz() const { return t3kTonalityReq_.load(std::memory_order_acquire); }

  // ---- TransposeInsert (off-RT reset, RT process) ----
  void reset(double sampleRate, int maxBlockSize) override;
  void process(const float* input, float* output, int numFrames) override;
  int latencySamples() const override;

  double sampleRate() const { return sampleRate_; }
  int maxBlockSize() const { return maxBlock_; }
  // Post-reset introspection (control thread): per-engine latencies for
  // honest accounting, and the shift GT2 was reset at (exact 0.0 selects
  // GT2's zero-latency bypass until the next reset: live shifts away from
  // a 0-start need an audio restart on the GT2 side).
  int gt2LatencySamples() const { return gt2Latency_; }
  int t3kLatencySamples() const;
  float resetShiftSt() const { return resetShift_; }

private:
  int activeLatency() const; // audio-thread: selected engine's current latency
  int bypassLatency() const; // audio-thread: dry delay for the bypass path

  GuitarTranspose gt2_;
  GuitarTranspose::Config gt2Cfg_; // off-RT only
  bool warmStandby_ = true; // off-RT only

#ifdef TDM_HAVE_TONE3000
  struct T3kState; // defined in the .cpp (owns Transpose + juce buffer)
  std::unique_ptr<T3kState> t3k_;
#endif

  double sampleRate_ = 0.0;
  int maxBlock_ = 0;
  int gt2Latency_ = 0; // fixed at reset
  int t3kMaxLatency_ = 0; // 60 ms window at rate, fixed at reset
  float resetShift_ = 0.0f; // shift adopted at reset (GT2 0-start caveat)

  std::vector<float> dryDelay_; // latency-matching ring
  long long delayWrite_ = 0;
  std::vector<float> scratchA_; // GT2 render
  std::vector<float> scratchB_; // T3K render

  // Control-visible requests (any-thread setters, audio-thread reader).
  std::atomic<bool> enabledReq_{true};
  std::atomic<int> engineReq_{0};
  std::atomic<float> shiftReq_{0.0f};
  std::atomic<int> t3kWindowReq_{kDefaultT3kWindowMs};
  std::atomic<float> t3kTonalityReq_{kDefaultT3kTonalityHz};

  // Audio-thread followers (block-boundary adoption).
  float ramp_ = 1.0f; // 0 = dry, 1 = wet
  Engine activeEngine_ = Engine::Gt2;
  float switchRamp_ = 1.0f; // engine crossfade position 0..1
  Engine switchFrom_ = Engine::Gt2;
  float appliedShift_ = 0.0f;
  int appliedT3kWindow_ = kDefaultT3kWindowMs;
  float appliedT3kTonality_ = kDefaultT3kTonalityHz;
};

const char* devEngineName(DevTranspose::Engine engine);
bool parseDevEngine(const std::string& s, DevTranspose::Engine& engine);
} // namespace lab
} // namespace tdm

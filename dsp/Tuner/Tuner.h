#pragma once

// Tuner: production chromatic tuner analyzer (side-chain observer).
//
// A YIN-style normalized-difference pitch detector for clean monophonic
// electric guitar/bass, plus note/cents math and a control-side display
// smoother. It OBSERVES audio: feedBlock() only reads its input, so the
// rig output is bit-identical with analysis on or off (see the
// transparency regression in tests/TestTuner.cpp).
//
// Signal flow: TechDeathRig taps the averaged raw mono input BEFORE
// InputTrim and calls feedBlock() every sub-block. Analysis runs at a
// control cadence (one bounded YIN pass over a fixed window per hop),
// never per sample.
//
// Design (48 kHz reference; geometry derives from the rate at reset):
// multirate YIN. The tap is lowpassed (2nd-order Butterworth, 3 kHz)
// and decimated to a ~12 kHz internal rate; a coarse YIN pass over a
// ~85 ms decimated window finds the dip (absolute threshold, shortest
// period first = the octave guard); a narrow full-rate difference band
// around the coarse lag is then refined parabolically for cents-level
// accuracy. Hop is half a window (~43 ms); at most one analysis runs
// per feedBlock call (strict RT bound).
// - RMS level gate (~-60 dBFS) forces invalid on silence and skips the
//   YIN passes when idle; NaN/Inf input can only yield invalid, never
//   a bogus reading or a crash
// - result published through a seqlock (single audio writer, any-thread
//   readers, no locks, no allocation)
//
// CPU (measured, see tests/TestTuner.cpp): one analysis is ~0.4 ms
// worst-case on this machine's class (<1% of one core at the hop
// cadence); see the test report for the exact figure.
//
// Display smoothing lives in TunerDisplay below (control side only):
// note hysteresis + cents smoothing + invalid persistence. Tests drive
// the detector raw (accuracy) and the display (stability) separately.
//
// Realtime contract: reset()/setEnabled() are called at block
// boundaries or off-RT (setEnabled only flips a flag + memsets the
// ring: bounded, no alloc). feedBlock() is RT-safe (bounded memcpy +
// at most one analysis pass, no allocation, no locks, no I/O).
// result() is safe from any thread. TunerDisplay is control-side only.
//
// Expected signal range: finite floats, nominally raw guitar DI in
// [-1, 1]. Hotter/colder inputs work; the level gate is absolute.

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace tdm
{
// Single published analysis outcome. Plain data; transport is the
// Tuner seqlock (see result()).
struct TunerResult
{
  bool valid = false; // true: pitched signal, fields meaningful
  float frequencyHz = 0.0f; // refined fundamental estimate
  float cents = 0.0f; // deviation from nearest note, (-50, +50]
  int midiNote = 0; // nearest MIDI note number (69 = A4)
  float confidence = 0.0f; // 0..1 periodicity confidence (1 - CMND)
};

// Scientific-pitch note derived from a frequency.
struct TunerNote
{
  int midi = 69; // nearest MIDI note (69 = A4 = refHz)
  int octave = 4; // scientific pitch octave (E2 -> 2)
  float cents = 0.0f; // (-50, +50] deviation from midi
  char name[4] = {'A', '\0', '\0', '\0'}; // "E", "F#", ... (NUL-terminated)
};

class Tuner
{
public:
  static constexpr float kMinHz = 40.0f; // useful 8-string low range
  static constexpr float kMaxHz = 1500.0f; // well above any guitar harmonic need
  static constexpr float kDefaultRefHz = 440.0f; // A4
  static constexpr float kYinThreshold = 0.10f; // CMND absolute threshold
  static constexpr float kLevelGateDb = -60.0f; // RMS gate for invalid

  Tuner();

  // Off-RT/block-boundary: derive rate-scaled geometry, (re)allocate the
  // ring + YIN workspace, clear all state. Deterministic.
  void reset(double sampleRate);

  // Block-boundary: enable/disable analysis. Disabling skips all work in
  // feedBlock (one branch); enabling clears the ring so stale audio can
  // never produce a reading. Never touches the fed-through audio.
  void setEnabled(bool enabled);
  bool isEnabled() const { return enabled_; }

  // RT-safe observer: taps numFrames of mono input (read-only), runs at
  // most one bounded analysis pass per call. In-place/alias safe (input
  // is never written).
  void feedBlock(const float* mono, int numFrames);

  // Any-thread snapshot of the latest published analysis (seqlock read;
  // bounded retries, then best-effort copy: always safe for UI tick).
  void result(TunerResult& out) const;

  double sampleRate() const { return sampleRate_; }
  int windowSamples() const { return window_; }
  int hopSamples() const { return hop_; }
  int analysesRun() const { return analysesRun_; } // RT counter, read off-RT

  // Frequency -> note math (pure; shared by the analyzer and tests).
  // refHz is the A4 calibration (440 default; later UI-able).
  static TunerNote noteFor(float freqHz, float refHz = kDefaultRefHz);

private:
  void analyze(); // one bounded YIN pass over the ring tail; publishes

  double sampleRate_ = 0.0;
  bool enabled_ = false;
  int window_ = 0; // full-rate refine window (rate-scaled)
  int hop_ = 0; // full-rate samples between analyses (rate-scaled)
  int tauMin_ = 0; // shortest full-rate lag (kMaxHz)
  int tauMax_ = 0; // longest full-rate lag (kMinHz)
  int ringMask_ = 0;
  std::vector<float> ring_; // full-rate power-of-two sample ring
  long long writePos_ = 0;
  // Decimated coarse stage (~12 kHz internal rate).
  int decFactor_ = 1; // full-rate samples per decimated sample
  int windowDec_ = 0;
  int hopDec_ = 0;
  int tauMinDec_ = 0;
  int tauMaxDec_ = 0;
  int ringDecMask_ = 0;
  std::vector<float> ringDec_;
  long long writeDec_ = 0;
  long long sinceAnalysis_ = 0; // decimated samples since last analysis
  int decPhase_ = 0;
  double lpB0_ = 1.0, lpB1_ = 0.0, lpB2_ = 0.0; // anti-alias biquad
  double lpA1_ = 0.0, lpA2_ = 0.0;
  double lpZ1_ = 0.0, lpZ2_ = 0.0;
  std::vector<float> yin_; // difference workspace, size tauMax + 1 (shared)
  std::vector<float> linear_; // linearized span, size window + tauMax (shared)
  int analysesRun_ = 0; // audio-thread only

  // Seqlock publication (single writer = audio thread).
  mutable std::atomic<unsigned> seq_{0};
  TunerResult published_; // written between odd/even seq pair
};

// Control-side display smoother (UI tick only, never RT). Feeds on raw
// snapshots and produces a stable display state: a note change needs
// stock of consecutive agreements (hysteresis), cents are lightly
// smoothed, and brief invalids do not blank the display.
class TunerDisplay
{
public:
  struct State
  {
    bool valid = false;
    int midiNote = 0;
    float cents = 0.0f; // smoothed deviation
    float frequencyHz = 0.0f; // last raw frequency at/near this note
    const char* name = "--"; // held display note name
    int octave = 0;
  };

  TunerDisplay();

  void reset();
  // Feed one raw snapshot (call at UI cadence, e.g. 10 Hz).
  void update(const TunerResult& raw);
  const State& state() const { return state_; }

  // Test introspection.
  int pendingNote() const { return pendingMidi_; }
  int pendingCount() const { return pendingCount_; }

private:
  static const char* nameFor(int midi);

  State state_;
  int pendingMidi_ = -1; // candidate note awaiting hysteresis stock
  int pendingCount_ = 0;
  int invalidCount_ = 0;
  float smoothCents_ = 0.0f;
  bool haveSmooth_ = false;
};
} // namespace tdm

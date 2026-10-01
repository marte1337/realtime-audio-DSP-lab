#pragma once

// BenchTone3000: TONE3000 Transpose benchmark wrapper (LAB RESEARCH ONLY).
//
// Wraps the ACTUAL shipped TONE3000 transpose DSP (correlation-spliced
// delay line with onset re-sync) at the pinned study commit, compiled in
// place from OUTSIDE this repo (TDM_T3K_DIR, default
// /tmp/tdm-tone3000-study/tone3000-plugin). Nothing external is copied
// here; only this shim (our code) lives in dsp/lab/bench/.
//
// Provenance: https://github.com/tone-3000/tone3000-plugin
//   main @ b8461cc8b0d12639cfd6b1d8f399f7eb25a2503c (MIT, (c) 2026 TONE3000)
//   engine: plugin/src/Transpose.cpp (original project code; the earlier
//   Signalsmith-based engine was fully removed upstream).
// The engine needs JUCE 9.0.3 (TDM_JUCE_DIR, AGPLv3/commercial dual) for
// juce_core + juce_audio_basics + juce_audio_formats + juce_dsp. Local
// research benchmarking only: same quarantine as the GPL bench deps (no
// distribution, no product code, isolated targets).
//
// Configuration: fixed semitone shift (<= 0, setShiftSemitones),
// selectable delay-buffer window (20/30/40/60 ms) + optional Tonality
// crossover Hz (0 = off, the default pure shift). STEP/divebomb sweeps
// are NOT exercised: one fixed ratio per run, per the BenchShifter
// contract. Power stays on for the run ( wetMix fully ramped before any
// metric window; all metric windows start >= 0.6 s).
//
// Latency: latencySamples() reports the engine's own nominal figure
// (floor + buffer)/2, the same alignment TONE3000's own bench uses; the
// harness ALSO measures observed onset latency independently.
// tailSamples() is 0: the engine is fully causal (no lookahead, no
// flush requirement); the dMax headroom is inside latencySamples().
//
// Determinism: reset() reconstructs a fresh engine instance (off-RT),
// so every render starts from identical state.

#include <memory>

#include "dsp/lab/bench/BenchShifter.h"

namespace tdm
{
namespace bench
{
class BenchTone3000 : public BenchShifter
{
public:
  enum class Window
  {
    Ms20 = 0,
    Ms30 = 1,
    Ms40 = 2,
    Ms60 = 3,
  };

  // tonalityHz: 0 = off (pure shift); else the dry-bypass crossover Hz.
  explicit BenchTone3000(Window window = Window::Ms30, float tonalityHz = 0.0f);
  ~BenchTone3000() override;

  const char* id() const override;
  const char* config() const override;
  void prepare(double sampleRate, int maxBlock) override;
  void setShiftSemitones(double semitones) override;
  void reset() override;
  void processBlock(const float* input, float* output, int n) override;
  int latencySamples() const override;
  int tailSamples() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace bench
} // namespace tdm

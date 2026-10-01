#pragma once

// BenchSignalsmith: Signalsmith Stretch 1.4.0 (MIT) + linear 0.6.4
// benchmark wrapper (header-only dependency; nothing to link).
//
// Configuration: presetDefault(1, sampleRate) — 120 ms block / 30 ms
// hop @48 kHz nominal — plus setTransposeSemitones(st). Pitch comes from
// the library's spectral frequency remap; time runs at rate 1 (equal
// input/output counts per process call). Fixed seed (deterministic;
// the library only draws randomness for >2x slowdown diffusion, which a
// fixed detune never reaches).
//
// Latency: inputLatency() + outputLatency() as reported by the library
// (120 ms nominal @48 kHz); the harness independently measures observed
// onset latency.
//
// Realtime note: the library asserts zero allocation inside process()
// (tmp buffers only grow); prepare() additionally pre-warms with a
// silent process call so first audible output never allocates. The
// 120 ms latency dominates any live use regardless.
//
// PIMPL: Signalsmith headers appear only in the .cpp. Included from
// TDM_BENCH_DEPS in place (never copied into this repo).

#include <memory>

#include "dsp/lab/bench/BenchShifter.h"

namespace tdm
{
namespace bench
{
class BenchSignalsmith : public BenchShifter
{
public:
  BenchSignalsmith();
  ~BenchSignalsmith() override;

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

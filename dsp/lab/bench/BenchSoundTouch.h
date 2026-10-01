#pragma once

// BenchSoundTouch: SoundTouch 2.4.1 (LGPL-2.1) benchmark wrapper.
//
// Configuration: SoundTouch class (pitch = resample + reciprocal TD
// stretch), float samples, setPitchSemiTones(st), DEFAULT auto
// sequence/seek/overlap tables (73/18/8 ms @44.1 kHz nominal — the
// library's recommended out-of-box operating point for quality; the
// benchmark does not hand-tune them). No QuickSeek.
//
// Latency: SoundTouch exposes no exact start-delay API; latencySamples()
// reports the steady-state pipeline depth (numUnprocessedSamples after
// priming, measured in prepare) and the harness independently measures
// observed onset latency. The two are reported side by side, never
// conflated.
//
// Realtime note (from the architecture study + header docs): correlation
// per batch is synchronous/unbounded per call, FIFOs must be pre-grown
// for allocation-free steady state, and setRate-class calls redesign the
// AA filter (fresh allocs) — acceptable for offline benchmarking; live
// use needs the callback-safety analysis in the benchmark report.
//
// PIMPL: SoundTouch headers appear only in the .cpp. Sources compile
// from TDM_BENCH_DEPS/soundtouch in place (never copied into this repo).

#include <memory>

#include "dsp/lab/bench/BenchShifter.h"

namespace tdm
{
namespace bench
{
class BenchSoundTouch : public BenchShifter
{
public:
  BenchSoundTouch();
  ~BenchSoundTouch() override;

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

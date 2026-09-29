#pragma once

// BenchRubberBand: Rubber Band 4.0.0 (e4296ac) benchmark wrapper.
//
// Modes:
//   R2Realtime: RubberBandStretcher, R2/Faster engine, realtime process
//     mode. Options: ProcessRealTime | EngineFaster | TransientsMixed |
//     DetectorCompound | PhaseLaminar | ThreadingNever | WindowStandard |
//     SmoothingOff | FormantShifted | PitchHighQuality | ChannelsTogether.
//     This is the shortest-latency realtime-capable R2 path from the
//     architecture study (~21.3 ms @48 kHz). Mixed transients selected
//     by probe (Sep 2026): Crisp/Mixed/Smooth are identical on KS
//     sustains, fourth dyads, and chug attacks (+/-0.02), but Crisp
//     smears steady pure sines (dual-tone beating, -1.6% oracle error)
//     while Mixed/Smooth are exact — Mixed keeps the oracle table
//     honest at zero guitar cost. Shifted formants match our own
//     algorithms (no preservation) for a fair comparison;
//     quality-first resampler because latency is set by the window term
//     either way. setMaxProcessSize(maxBlock) on prepare (the library's
//     documented realtime precondition).
//   Live: RubberBandLiveShifter (dedicated R3LiveShifter class, fixed
//     512-frame blocks, ~50-60 ms documented). Wrapper batches input to
//     the fixed block size; latencySamples() = getStartDelay() (content
//     mapping). Live output EMERGENCE adds +0..blockSize samples by onset
//     phase — a real playing-feel term, measured by the harness onset
//     probe and stated separately, never folded into this figure.
//
// PIMPL: Rubber Band headers appear only in the .cpp. Source compiles
// from TDM_BENCH_DEPS/rubberband/single/RubberBandSingle.cpp in place
// (never copied into this repo); Apple builds use vDSP + Accelerate.

#include <memory>

#include "dsp/lab/bench/BenchShifter.h"

namespace tdm
{
namespace bench
{
class BenchRubberBand : public BenchShifter
{
public:
  enum class Mode
  {
    R2Realtime = 0, // id "rb2"
    Live = 1, // id "rblive"
  };

  explicit BenchRubberBand(Mode mode = Mode::R2Realtime);
  ~BenchRubberBand() override;

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

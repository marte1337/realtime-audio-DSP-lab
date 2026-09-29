#pragma once

// BenchShifter: common conceptual interface for EXTERNAL pitch-shifter
// benchmark wrappers (LAB RESEARCH ONLY).
//
// These wrappers adapt mature third-party pitch implementations to one
// fixed-block mono interface so the offline benchmark harness and (where
// callback-safe) the lab live host can drive them exactly like our own
// algorithms. The external SOURCES live outside this repo
// (TDM_BENCH_DEPS, default /tmp/tdm-pitch-study) and are NEVER copied
// into TechDeathMachine source. Only these shims (our code, no copied
// implementation) live here under dsp/lab/bench/.
//
// Licensing (research/benchmark use only, no distribution, no product
// integration):
//   Rubber Band 4.0.0 (e4296ac): GPL-2-or-later or commercial.
//     Benchmarking locally is fine; shipping it (or derivatives) in a
//     closed product requires the commercial licence.
//   SoundTouch 2.4.1 (tarball, git SHA unresolved): LGPL-2.1-or-later.
//   Signalsmith Stretch 1.4.0 (a670068) + linear 0.6.4 (de55e6a): MIT.
//
// Fidelity rules for every wrapper:
//   - Use each library's PUBLIC API with documented-recommended settings
//     for realtime/low-latency pitch shifting; no private/header hacks.
//   - Fixed detune set off-RT; no parameter automation inside a run.
//   - processBlock adapts variable native I/O to fixed n-in/n-out via a
//     preallocated internal FIFO (no allocation on the audio path after
//     prepare). Startup underruns emit zeros (offline callers compensate
//     with latencySamples()+tailSamples(), same convention as our own
//     study harnesses).
//   - latencySamples() reports the library's OWN algorithmic latency
//     figure where one exists (plus any wrapper batching term, stated in
//     the wrapper header); the harness ALSO measures observed onset
//     latency independently. The two must agree within a block.
//   - Deterministic where the library allows (fixed seeds, no threads).

namespace tdm
{
namespace bench
{
class BenchShifter
{
public:
  virtual ~BenchShifter() = default;

  // Short stable id for tables/paths (e.g. "rb2", "st", "ss").
  virtual const char* id() const = 0;
  // Human configuration string for reports (library/version/options).
  virtual const char* config() const = 0;

  // Off-RT: (re)construct + configure the engine, preallocate FIFOs and
  // pre-warm any lazy internal buffers. maxBlock bounds processBlock.
  virtual void prepare(double sampleRate, int maxBlock) = 0;
  // Off-RT: fixed detune in semitones (<= 0). Takes effect on reset().
  virtual void setShiftSemitones(double semitones) = 0;
  // Off-RT: return to constructed state (deterministic start).
  virtual void reset() = 0;

  // Fixed n-in/n-out mono float processing, in-place safe. Emits exactly
  // n samples; output lags input by latencySamples().
  virtual void processBlock(const float* input, float* output, int n) = 0;

  // Algorithmic latency in samples (valid post-prepare): library figure
  // + documented wrapper batching. 0 only for a true bypass.
  virtual int latencySamples() const = 0;
  // Extra zeros an offline caller must feed past the input end (beyond
  // the latency) so final outputs flush. 0 only for a true bypass.
  virtual int tailSamples() const = 0;
};
} // namespace bench
} // namespace tdm

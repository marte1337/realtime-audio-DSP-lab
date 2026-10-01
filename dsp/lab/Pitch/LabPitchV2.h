#pragma once

// LabPitchV2: PV-D transient/reset research fork (LAB PROTOTYPE).
//
// Status: experimental candidate under evaluation in dsp/lab/Pitch/. NOT
// wired into TechDeathRig / RigParams / tdm_dev. Do not build product
// architecture on it until real-guitar listening validation passes.
//
// This is a line-for-line fork of LabPitchShift with a four-way mode
// switch. Research question: do PV-D's chord and low-frequency
// limitations come mainly from simplistic phase/transient handling or
// from the short 2048-sample spectral window itself?
//
// Modes (set via setMode(), safe any time but studied from reset):
//   A (baseline): the LabPitchShift algorithm verbatim. Proven
//      bit-identical to LabPitchShift at equal config (test).
//   B (freq-delimited reset): on transient frames, reset ONLY bins at
//      or above kResetSplitHz (probe: 1000 Hz); lower bins propagate
//      and lock exactly as on non-transient frames. Tests whether the
//      all-spectrum reset destroys low/chord continuity that the short
//      window would otherwise preserve.
//   C (transient dominance): on transient frames, scale the frame's
//      OLA accumulator contribution by kTransientBoost (probe: 2.0)
//      WITHOUT scaling its window-sum, so the attack reconstructs at
//      full strength at its true time position instead of being diluted
//      ~8:1 by neighbor-frame overlap. Tests whether PV attack
//      softening is WOLA dilution rather than phase error. Bounded:
//      only flagged transient frames, magnitude <= boost x input.
//   D (both B and C).
//
// Deliberately NOT changed (attribution: geometry, detector, resampler,
// latency formula, state handling, and all constants are shared;
// only the marked branches differ):
//   FFT/window/hop geometry (studied at PV-D 2048/256 @48 kHz)
//   flux detector (full-spectrum, same threshold)
//   phase propagation + nearest-peak locking
//   same-bin mapping, Hann/Hann WOLA, cubic slow read
//   latency formula (N + 1) + (N - Ha) * (1/r - 1), identical all modes
//
// Deliberately NOT attempted here: multi-resolution, WSOLA hybrid,
// external DSP libraries, new resampler, vertical phase prediction,
// large rewrite. Those follow only if this experiment attributes the
// failure to handling rather than resolution.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].

#include <complex>
#include <vector>

#include "dsp/lab/Pitch/LabFft.h"

namespace tdm
{
namespace lab
{
class LabPitchV2
{
public:
  static constexpr float kMinShiftSt = -24.0f;
  static constexpr float kMaxShiftSt = 0.0f;
  static constexpr float kDefaultShiftSt = 0.0f;
  static constexpr int kDefaultFftSize = 2048; // PV-D geometry default
  static constexpr int kDefaultHop = 256;
  // Probe constants (research parameters, not tuned product values):
  static constexpr double kResetSplitHz = 1000.0; // B: reset bins >= this
  static constexpr float kTransientBoost = 2.0f; // C: transient OLA dominance

  enum class Mode
  {
    A_Baseline = 0,
    B_FreqReset = 1,
    C_TimeAnchor = 2,
    D_Both = 3
  };

  LabPitchV2();

  // Lab config: fftSize must be a power of two in [256, 16384]; hop must
  // divide fftSize and lie in [fftSize/8, fftSize/2]. Takes effect on the
  // next reset(). Throws std::invalid_argument on bad values.
  void setConfig(int fftSize, int hop);
  int fftSize() const { return fftSize_; }
  int hop() const { return hop_; }

  // Research variant selector. Takes effect on the next processed frame
  // (safe any time; studied from reset for clean attribution).
  void setMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }

  // Off-RT: validates the rate, derives the synthesis hop from the shift,
  // (re)allocates all frame/state buffers, clears state. Deterministic
  // start: zero history, so output fades in from silence over the first
  // window (no burst).
  void reset(double sampleRate);

  // Shift in semitones, clamped to [kMinShiftSt, kMaxShiftSt]; fractional
  // values allowed. Takes effect on the next reset(). Exactly 0.0f
  // becomes a bit-exact zero-latency bypass on reset().
  void setShiftSt(float semitones);
  void setEnabled(bool enabled);

  float shiftSt() const { return shiftSt_; }
  bool isEnabled() const { return enabled_; }
  int synthHop() const { return synthHop_; }
  double actualRatio() const { return actualRatio_; }
  // Streaming latency in samples (valid post-reset): identical formula
  // and value in all four modes (geometry unchanged).
  int latencySamples() const { return latency_; }
  int tailSamples() const { return bypass0_ ? 0 : fftSize_ + hop_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly,
  // as does the exact-0-st bypass. Otherwise emits exactly numFrames
  // samples; output lags the input by latencySamples().
  void processBlock(const float* input, float* output, int numFrames);

private:
  void processFrame();
  float stretchedAt(long long pos) const;
  void pushOutput(float v);

  int fftSize_ = kDefaultFftSize;
  int hop_ = kDefaultHop;
  double sampleRate_ = 0.0;
  float shiftSt_ = kDefaultShiftSt;
  bool enabled_ = false;
  Mode mode_ = Mode::A_Baseline;

  int synthHop_ = kDefaultHop;
  double actualRatio_ = 1.0;
  int latency_ = kDefaultFftSize + 1;
  bool bypass0_ = false;
  int resetSplitBin_ = 0; // first bin >= kResetSplitHz, derived in reset()

  LabFft fft_;
  std::vector<float> window_;
  std::vector<double> omega_;

  std::vector<float> inHist_;
  std::vector<float> inHop_;
  int inHopCount_ = 0;

  std::vector<std::complex<float>> spec_;
  std::vector<double> mag_;
  std::vector<double> phase_;
  std::vector<double> prevPhase_;
  std::vector<double> prevMag_;
  std::vector<double> synthPhase_;
  std::vector<int> peakOfBin_;
  std::vector<int> peakBins_;
  std::vector<double> fluxHist_;
  int fluxCount_ = 0;

  std::vector<float> outAcc_;
  std::vector<float> outSum_;
  long long olaPos_ = 0;

  std::vector<float> compRing_;
  int compMask_ = 0;
  long long compEmitted_ = 0;
  long long resampOut_ = 0;

  std::vector<float> outFifo_;
  int fifoRead_ = 0;
  int fifoWrite_ = 0;
  int fifoCount_ = 0;

  bool firstFrame_ = true;
};
} // namespace lab
} // namespace tdm

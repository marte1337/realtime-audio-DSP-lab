#pragma once

// LabPitchShift: phase-vocoder downward pitch shifter (LAB PROTOTYPE).
//
// Status: experimental candidate under evaluation in dsp/lab/Pitch/. NOT
// wired into TechDeathRig / RigParams / tdm_dev. Do not build product
// architecture on it until real-guitar listening validation passes.
//
// Algorithm: time-compress with a same-bin phase vocoder, then restore
// duration with a slow fractional read (cubic interpolation). For a
// target ratio r <= 1 (r = 2^(st/12)): the PV runs synthesis hop
// Hs = round(r*Ha) <= Ha (same pitch, r x duration, SAME bin mapping -
// carriers untouched), and the output reads the compressed stream at
// o*r (pitch x r, duration restored). Pitch error from hop rounding is
// <= 0.5/Ha (< 0.001 st at default config); the resample rate tracks the
// actual rounded hop, so duration is exact and pitch is self-consistent.
//
// Why compress+interpolate and NOT spectral remapping: an earlier
// revision moved each synthesis bin's magnitude from a fractional
// analysis bin (m/ratio) with copied locked phases. Probes convicted
// it: a single E3 sine at -7 st came back at the right pitch but 0.14x
// level. Root cause: phases copied to different bin indices lose
// carrier coherence - adjacent synthesis carriers rendering one partial
// land at arbitrary relative phases (~166 deg apart measured) and
// partially cancel, and different carriers of identical content beat at
// the bin spacing. Same-bin mapping has neither disease (0 st renders
// full level), so transposition happens in the resampler instead, where
// it is exact. High partials transpose rather than vanish: the slow
// read maps every input partial f to r*f (cubic kernel droop ~1-3 dB
// at the very top only); there is no decimation lowpass to delete the
// top octave. Interpolation images sit ~40 dB down (cubic kernel).
//
// Polyphonic strategy (vertical phase coherence): every bin's synthesis
// phase is first propagated at its own measured instantaneous frequency
// (phase-derivative estimate, exact for resolved steady partials); then
// non-peak bins are re-locked to their nearest spectral peak each frame
// (scaled/identity phase locking: peak keeps its propagated phase, the
// skirt inherits it plus the current analysis phase difference). Peaks
// are local magnitude maxima above a per-frame relative floor, regions
// extend to midpoints between peaks. Locking is what keeps chords
// coherent instead of phasey; without it each bin drifts on its own
// noisy frequency estimate and the resynthesis shimmers.
//
// Transient strategy: spectral-flux onset detection with an adaptive
// threshold (median of recent flux x factor + floor). On a transient
// frame, synthesis phases are RESET to the current analysis phases
// instead of propagated, so the local waveform shape (the pick attack)
// is preserved; the stored phase baselines are re-synced so the
// following frame measures post-transient evolution only. Missed onsets
// degrade gracefully to slight attack softening; false positives are
// mostly harmless (a reset preserves shape).
//
// Overlap-add: Hann analysis window, Hann re-applied after the IFFT,
// weighted overlap-add with explicit window-sum normalization
// (accumulated Hann^2 per output sample). Synthesis hop never exceeds
// the analysis hop (downshift-only), so overlap only ever deepens and
// every shift/config reconstructs with flat gain.
//
// Config (default 4096/1024 @ any rate): N sets BOTH the low-frequency
// resolution (bin width = sr/N; low E 82.4 Hz sits at bin ~7 @48k with
// N=4096) AND the latency. Larger N resolves low strings better and
// smears transients more. Hop N/4 (75% analysis overlap) is the
// standard quality/cost point. Set via setConfig() BEFORE reset().
//
// Latency is shift-dependent: (N + 1) + (N - Ha) * (1/r - 1), and 0 at
// exactly 0 st (bit-exact bypass). Offline callers feed latencySamples()
// + tailSamples() extra zeros and drop the first latencySamples() outputs.
//
// Shift changes take effect on reset(): the synthesis hop derives from
// the shift, so mid-stream moves would need a re-derive (not supported;
// the setter just stores). Fixed shifts are the current evaluation
// target; divebomb sweeps are NOT in scope for this iteration.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].
// Output peak is bounded by roughly the input peak (magnitudes are only
// rotated and interpolated, never boosted).

#include <complex>
#include <vector>

#include "dsp/lab/Pitch/LabFft.h"

namespace tdm
{
namespace lab
{
class LabPitchShift
{
public:
  static constexpr float kMinShiftSt = -24.0f;
  static constexpr float kMaxShiftSt = 0.0f;
  static constexpr float kDefaultShiftSt = 0.0f;
  static constexpr int kDefaultFftSize = 4096;
  static constexpr int kDefaultHop = 1024;

  LabPitchShift();

  // Lab config: fftSize must be a power of two in [256, 16384]; hop must
  // divide fftSize and lie in [fftSize/8, fftSize/2]. Takes effect on the
  // next reset(). Throws std::invalid_argument on bad values.
  void setConfig(int fftSize, int hop);
  int fftSize() const { return fftSize_; }
  int hop() const { return hop_; }

  // Off-RT: validates the rate, derives the synthesis hop from the shift,
  // (re)allocates all frame/state buffers, clears state. Deterministic
  // start: zero history, so output fades in from silence over the first
  // window (no burst).
  void reset(double sampleRate);

  // Shift in semitones, clamped to [kMinShiftSt, kMaxShiftSt]; fractional
  // values allowed. Takes effect on the next reset() (see note above).
  // Exactly 0.0f (no epsilon: -0.1 st still runs the PV) becomes a
  // bit-exact zero-latency bypass on reset(); see processBlock().
  void setShiftSt(float semitones);
  void setEnabled(bool enabled);

  float shiftSt() const { return shiftSt_; }
  bool isEnabled() const { return enabled_; }
  // Actual synthesis hop / resample ratio after rounding (valid post-reset).
  int synthHop() const { return synthHop_; }
  double actualRatio() const { return actualRatio_; }
  // Streaming latency in samples (valid post-reset; derives from the
  // shift): (N + 1) + (N - Ha) * (1/r - 1). The window term scales by
  // 1/r on the compressed grid. Residual deep-shift group delay
  // (phase-slope fits suggest up to ~150 samples at -24, within
  // measurement uncertainty) is unauditioned at sample precision and
  // flagged for senior DSP review; it is far below audibility for
  // offline alignment. Exactly 0 st bypasses (see below): latency 0.
  // Offline callers feed latencySamples() + tailSamples() extra zeros
  // and drop the first latencySamples().
  int latencySamples() const { return latency_; }
  // Extra zeros an offline caller must feed past the input end (beyond
  // the latency) so the final outputs are fully computed. 0 when the
  // exact-0-st bypass is active.
  int tailSamples() const { return bypass0_ ? 0 : fftSize_ + hop_; }

  // Mono float processing, in-place safe. Disabled bypass copies exactly,
  // as does the exact-0-st bypass (shiftSt_ == 0.0f at reset, zero
  // latency). Otherwise emits exactly numFrames samples; output lags the
  // input by latencySamples() (see above).
  void processBlock(const float* input, float* output, int numFrames);

private:
  void processFrame(); // consume one hop, run the PV + resampler
  float stretchedAt(long long pos) const; // compressed stream, 0 pre-roll
  void pushOutput(float v); // append to outFifo_

  int fftSize_ = kDefaultFftSize;
  int hop_ = kDefaultHop;
  double sampleRate_ = 0.0;
  float shiftSt_ = kDefaultShiftSt;
  bool enabled_ = false;

  int synthHop_ = kDefaultHop; // Hs = round(r*Ha), derived in reset()
  double actualRatio_ = 1.0; // Hs/Ha, the true pitch ratio
  int latency_ = kDefaultFftSize + 1; // derived in reset(), see above
  bool bypass0_ = false; // exact-0-st bypass, derived in reset()

  LabFft fft_;
  std::vector<float> window_; // Hann, N
  std::vector<double> omega_; // nominal bin freqs 2 pi k / N, k = 0..nb

  std::vector<float> inHist_; // input history, N (linear, shifted per hop)
  std::vector<float> inHop_; // current hop fill, Ha
  int inHopCount_ = 0;

  std::vector<std::complex<float>> spec_; // working spectrum, N
  std::vector<double> mag_; // analysis magnitudes, nb+1
  std::vector<double> phase_; // analysis phases, nb+1
  std::vector<double> prevPhase_; // previous analysis phases, nb+1
  std::vector<double> prevMag_; // previous magnitudes (flux), nb+1
  std::vector<double> synthPhase_; // running synthesis phases, nb+1
  std::vector<int> peakOfBin_; // nearest-peak assignment, nb+1
  std::vector<int> peakBins_; // scratch peak list, nb+1 (reused, no alloc)
  std::vector<double> fluxHist_; // recent spectral flux, kFluxHist
  int fluxCount_ = 0;

  std::vector<float> outAcc_; // OLA accumulator ring, N
  std::vector<float> outSum_; // OLA window-sum ring, N
  long long olaPos_ = 0; // absolute stretched position of next emission

  std::vector<float> compRing_; // compressed-stream ring (cubic lookbehind)
  int compMask_ = 0; // ring size - 1 (power of two)
  long long compEmitted_ = 0; // stretched samples emitted so far
  long long resampOut_ = 0; // absolute output sample index (read = o*r)

  std::vector<float> outFifo_; // completed samples awaiting emission
  int fifoRead_ = 0;
  int fifoWrite_ = 0;
  int fifoCount_ = 0;

  bool firstFrame_ = true;
};
} // namespace lab
} // namespace tdm

// LabPitchShift: same-bin phase-vocoder time compressor + slow
// fractional read. See LabPitchShift.h for the algorithm note.

#include "dsp/lab/Pitch/LabPitchShift.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace lab
{
namespace
{
constexpr double kTwoPi = 2.0 * 3.14159265358979;
constexpr int kFluxHist = 8;
constexpr double kPeakRelFloor = 1e-3; // peaks must clear max/1000 per frame
constexpr double kPeakAbsFloor = 1e-9; // ... and this absolute floor
constexpr double kMagEps = 1e-12; // synth bins at/below this are zeroed
constexpr double kFluxFactor = 2.0; // transient if flux > median*factor+add
constexpr double kFluxAdd = 0.015;
constexpr float kSumEps = 1e-8f; // OLA window-sum guard

double wrapPi(double p)
{
  return p - kTwoPi * std::round(p / kTwoPi);
}
} // namespace

LabPitchShift::LabPitchShift() = default;

void LabPitchShift::setConfig(int fftSize, int hop)
{
  if (fftSize < 256 || fftSize > 16384 || (fftSize & (fftSize - 1)) != 0)
    throw std::invalid_argument("LabPitchShift: fftSize must be a power of two in [256, 16384]");
  if (hop < fftSize / 8 || hop > fftSize / 2 || fftSize % hop != 0)
    throw std::invalid_argument("LabPitchShift: hop must divide fftSize and lie in [N/8, N/2]");
  fftSize_ = fftSize;
  hop_ = hop;
}

void LabPitchShift::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("LabPitchShift: sample rate out of range");
  sampleRate_ = sampleRate;
  // Exact-0-st bypass: float equality, deliberately no epsilon, so small
  // real shifts (e.g. -0.1 st) always run the vocoder. Reset-gated like
  // every other shift change.
  bypass0_ = (shiftSt_ == 0.0f);
  const double ratio = std::exp2(static_cast<double>(shiftSt_) / 12.0);
  synthHop_ = static_cast<int>(ratio * hop_ + 0.5);
  if (synthHop_ < 1)
    synthHop_ = 1; // unreachable at r >= 2^-2, kept as a guard
  if (synthHop_ > hop_)
    synthHop_ = hop_; // unreachable at r <= 1, kept as a guard
  actualRatio_ = static_cast<double>(synthHop_) / hop_;
  latency_ = bypass0_
      ? 0
      : static_cast<int>((fftSize_ + 1) + (fftSize_ - hop_) * (1.0 / actualRatio_ - 1.0) + 0.5);

  fft_.init(fftSize_);
  const int n = fftSize_;
  const int nb = n / 2;

  window_.assign(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; ++i)
    window_[static_cast<size_t>(i)] =
        0.5f * (1.0f - static_cast<float>(std::cos(kTwoPi * static_cast<double>(i) / n)));

  omega_.assign(static_cast<size_t>(nb + 1), 0.0);
  for (int k = 0; k <= nb; ++k)
    omega_[static_cast<size_t>(k)] = kTwoPi * static_cast<double>(k) / n;

  inHist_.assign(static_cast<size_t>(n), 0.0f);
  inHop_.assign(static_cast<size_t>(hop_), 0.0f);
  inHopCount_ = 0;

  spec_.assign(static_cast<size_t>(n), std::complex<float>(0.0f, 0.0f));
  mag_.assign(static_cast<size_t>(nb + 1), 0.0);
  phase_.assign(static_cast<size_t>(nb + 1), 0.0);
  prevPhase_.assign(static_cast<size_t>(nb + 1), 0.0);
  prevMag_.assign(static_cast<size_t>(nb + 1), 0.0);
  synthPhase_.assign(static_cast<size_t>(nb + 1), 0.0);
  peakOfBin_.assign(static_cast<size_t>(nb + 1), -1);
  peakBins_.assign(static_cast<size_t>(nb + 1), 0);
  fluxHist_.assign(kFluxHist, 0.0);
  fluxCount_ = 0;

  outAcc_.assign(static_cast<size_t>(n), 0.0f);
  outSum_.assign(static_cast<size_t>(n), 0.0f);
  olaPos_ = 0;

  // Compressed-stream ring: the live span (oldest cubic lookbehind to
  // newest emission) stays under one synthesis hop plus a few samples;
  // Hs + 16 rounded up to a power of two is comfortable.
  int compSize = 32;
  while (compSize < synthHop_ + 16)
    compSize <<= 1;
  compRing_.assign(static_cast<size_t>(compSize), 0.0f);
  compMask_ = compSize - 1;
  compEmitted_ = 0;
  resampOut_ = 0;

  // Output FIFO: the resampler emits ~Ha samples per analysis hop in one
  // burst while processBlock pops one per input, so at most ~Ha + a few
  // ever wait. 4x Ha is comfortable at any valid config.
  outFifo_.assign(static_cast<size_t>(4 * hop_), 0.0f);
  fifoRead_ = 0;
  fifoWrite_ = 0;
  fifoCount_ = 0;

  firstFrame_ = true;
}

void LabPitchShift::setShiftSt(float semitones)
{
  shiftSt_ = semitones < kMinShiftSt ? kMinShiftSt : (semitones > kMaxShiftSt ? kMaxShiftSt : semitones);
}

void LabPitchShift::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void LabPitchShift::processBlock(const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (!enabled_ || bypass0_)
  {
    // Exact bypass (in-place is a no-op by definition).
    if (output != input)
      std::memcpy(output, input, static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }
  if (sampleRate_ <= 0.0)
  {
    // Enabled but never reset: defined as silence (no rate derived yet).
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f;
    return;
  }
  const int fifoCap = static_cast<int>(outFifo_.size());
  for (int i = 0; i < numFrames; ++i)
  {
    inHop_[static_cast<size_t>(inHopCount_++)] = input[i];
    if (inHopCount_ >= hop_)
      processFrame();
    if (fifoCount_ > 0)
    {
      output[i] = outFifo_[static_cast<size_t>(fifoRead_)];
      fifoRead_ = (fifoRead_ + 1) % fifoCap;
      --fifoCount_;
    }
    else
    {
      output[i] = 0.0f; // first hop only: no completed frame yet
    }
  }
}

float LabPitchShift::stretchedAt(long long pos) const
{
  if (pos < 0)
    return 0.0f; // zero pre-roll before the stream starts
  return compRing_[static_cast<size_t>(pos & compMask_)];
}

void LabPitchShift::pushOutput(float v)
{
  const int fifoCap = static_cast<int>(outFifo_.size());
  outFifo_[static_cast<size_t>(fifoWrite_)] = v;
  fifoWrite_ = (fifoWrite_ + 1) % fifoCap;
  ++fifoCount_;
}

void LabPitchShift::processFrame()
{
  const int n = fftSize_;
  const int hop = hop_;
  const int hs = synthHop_;
  const int nb = n / 2;
  const int mask = n - 1; // n is a power of two

  // Shift history by one analysis hop, append the new hop.
  std::memmove(inHist_.data(), inHist_.data() + hop, static_cast<size_t>(n - hop) * sizeof(float));
  std::memcpy(inHist_.data() + (n - hop), inHop_.data(), static_cast<size_t>(hop) * sizeof(float));
  inHopCount_ = 0;

  // Analysis: Hann window + forward FFT, magnitude/phase split.
  for (int i = 0; i < n; ++i)
    spec_[static_cast<size_t>(i)] =
        std::complex<float>(inHist_[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)], 0.0f);
  fft_.forward(spec_.data());
  double frameMax = 0.0;
  double sumMag = 0.0;
  for (int k = 0; k <= nb; ++k)
  {
    const std::complex<float>& c = spec_[static_cast<size_t>(k)];
    const double m = std::sqrt(static_cast<double>(c.real()) * c.real() + //
                               static_cast<double>(c.imag()) * c.imag());
    mag_[static_cast<size_t>(k)] = m;
    phase_[static_cast<size_t>(k)] = std::atan2(static_cast<double>(c.imag()), //
                                               static_cast<double>(c.real()));
    sumMag += m;
    if (k > 0 && k < nb && m > frameMax)
      frameMax = m;
  }

  // Transient detection: normalized positive spectral flux against an
  // adaptive threshold (median of recent flux). Skipped until two frames
  // of history exist (frame 0 vs zero baselines would false-fire).
  double flux = 0.0;
  for (int k = 0; k <= nb; ++k)
  {
    const double d = mag_[static_cast<size_t>(k)] - prevMag_[static_cast<size_t>(k)];
    if (d > 0.0)
      flux += d;
  }
  flux /= sumMag + 1e-12;
  bool transient = false;
  if (fluxCount_ >= 2 && frameMax > 1e-7)
  {
    double sorted[kFluxHist];
    const int m = fluxCount_ < kFluxHist ? fluxCount_ : kFluxHist;
    for (int i = 0; i < m; ++i)
      sorted[i] = fluxHist_[static_cast<size_t>(i)];
    for (int i = 1; i < m; ++i) // tiny deterministic sort, m <= 8
      for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; --j)
      {
        const double t = sorted[j];
        sorted[j] = sorted[j - 1];
        sorted[j - 1] = t;
      }
    const double median = sorted[m / 2];
    transient = flux > median * kFluxFactor + kFluxAdd;
  }
  if (fluxCount_ < kFluxHist)
    fluxHist_[static_cast<size_t>(fluxCount_++)] = flux;
  else
  {
    for (int i = 1; i < kFluxHist; ++i)
      fluxHist_[static_cast<size_t>(i - 1)] = fluxHist_[static_cast<size_t>(i)];
    fluxHist_[kFluxHist - 1] = flux;
  }

  // Spectral peaks: local maxima above the per-frame floor. Regions
  // extend to midpoints between peaks (two-pointer assignment); with no
  // peaks at all (silence) locking is skipped and propagated phases are
  // used directly (magnitudes are ~0 either way).
  const double floor = frameMax * kPeakRelFloor > kPeakAbsFloor ? frameMax * kPeakRelFloor
                                                                : kPeakAbsFloor;
  int numPeaks = 0;
  for (int k = 1; k < nb; ++k)
  {
    const double m = mag_[static_cast<size_t>(k)];
    if (m > floor && m > mag_[static_cast<size_t>(k - 1)] && m >= mag_[static_cast<size_t>(k + 1)])
      peakBins_[static_cast<size_t>(numPeaks++)] = k;
  }
  int pi = 0;
  for (int k = 1; k < nb; ++k)
  {
    if (numPeaks == 0)
    {
      peakOfBin_[static_cast<size_t>(k)] = -1;
      continue;
    }
    while (pi + 1 < numPeaks &&
           std::abs(k - peakBins_[static_cast<size_t>(pi + 1)]) < std::abs(k - peakBins_[static_cast<size_t>(pi)]))
      ++pi;
    peakOfBin_[static_cast<size_t>(k)] = peakBins_[static_cast<size_t>(pi)];
  }

  if (firstFrame_)
  {
    // Seed running phases from the first analysis frame so frame 1
    // uses clean locked phases instead of zero-baseline garbage.
    synthPhase_ = phase_;
    prevPhase_ = phase_;
    firstFrame_ = false;
  }
  else if (transient)
  {
    // Phase reset: synthesis continues from the current analysis
    // phases (attack shape preserved); baselines re-synced so the next
    // frame measures post-transient evolution only.
    synthPhase_ = phase_;
    prevPhase_ = phase_;
  }
  else
  {
    // Propagate every bin at its own instantaneous frequency over the
    // SYNTHESIS hop (same pitch on the compressed time grid), then
    // re-lock non-peak bins to their region peak (scaled phase
    // locking). Propagating all bins keeps newly-appearing peaks
    // fresh without cross-frame peak-identity tracking.
    for (int k = 1; k < nb; ++k)
    {
      const double dphi = wrapPi(phase_[static_cast<size_t>(k)] - prevPhase_[static_cast<size_t>(k)] -
                                 omega_[static_cast<size_t>(k)] * hop);
      const double trueOmega = omega_[static_cast<size_t>(k)] + dphi / hop;
      synthPhase_[static_cast<size_t>(k)] += hs * trueOmega;
    }
    for (int k = 1; k < nb; ++k)
    {
      const int p = peakOfBin_[static_cast<size_t>(k)];
      if (p >= 0 && p != k)
        synthPhase_[static_cast<size_t>(k)] =
            synthPhase_[static_cast<size_t>(p)] + (phase_[static_cast<size_t>(k)] - phase_[static_cast<size_t>(p)]);
    }
    prevPhase_ = phase_;
  }
  prevMag_ = mag_;

  // Synthesis: SAME-bin mapping (carriers untouched - the resampler
  // below performs the transposition). DC/Nyquist stay real.
  spec_[0] = std::complex<float>(static_cast<float>(mag_[0]), 0.0f);
  for (int m = 1; m < nb; ++m)
  {
    const double mag = mag_[static_cast<size_t>(m)];
    if (mag <= kMagEps)
    {
      spec_[static_cast<size_t>(m)] = std::complex<float>(0.0f, 0.0f);
      continue;
    }
    const double ph = transient ? phase_[static_cast<size_t>(m)] : synthPhase_[static_cast<size_t>(m)];
    spec_[static_cast<size_t>(m)] = std::complex<float>(static_cast<float>(mag * std::cos(ph)),
                                                        static_cast<float>(mag * std::sin(ph)));
  }
  spec_[static_cast<size_t>(nb)] = std::complex<float>(static_cast<float>(mag_[static_cast<size_t>(nb)]), 0.0f);
  for (int m = 1; m < nb; ++m)
    spec_[static_cast<size_t>(n - m)] = std::conj(spec_[static_cast<size_t>(m)]);
  fft_.inverse(spec_.data());

  // Weighted overlap-add on the compressed (synthesis-hop) grid, then
  // emit Hs compressed samples into the resampler ring.
  for (int j = 0; j < n; ++j)
  {
    const size_t idx = static_cast<size_t>((olaPos_ + j) & mask);
    const float w = window_[static_cast<size_t>(j)];
    outAcc_[idx] += spec_[static_cast<size_t>(j)].real() * w;
    outSum_[idx] += w * w;
  }
  for (int j = 0; j < hs; ++j)
  {
    const size_t idx = static_cast<size_t>((olaPos_ + j) & mask);
    const float s = outSum_[idx] > kSumEps ? outAcc_[idx] / outSum_[idx] : 0.0f;
    compRing_[static_cast<size_t>(compEmitted_ & compMask_)] = s;
    ++compEmitted_;
    outAcc_[idx] = 0.0f; // zero before slot reuse one window later
    outSum_[idx] = 0.0f;
  }
  olaPos_ += hs;

  // Slow fractional read: output o samples the compressed stream at
  // o*ratio (cubic Lagrange). Duration is restored, pitch drops by
  // ratio. Emits while the cubic lookahead is available.
  for (;;)
  {
    const double p = resampOut_ * actualRatio_;
    const long long q = static_cast<long long>(std::floor(p));
    if (q + 2 > compEmitted_ - 1)
      break;
    const double f = p - static_cast<double>(q);
    const double ym1 = stretchedAt(q - 1);
    const double y0 = stretchedAt(q);
    const double y1 = stretchedAt(q + 1);
    const double y2 = stretchedAt(q + 2);
    const double c1 = 0.5 * (y1 - ym1);
    const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
    const double c3 = -0.5 * ym1 + 1.5 * y0 - 1.5 * y1 + 0.5 * y2;
    pushOutput(static_cast<float>(y0 + f * (c1 + f * (c2 + f * c3))));
    ++resampOut_;
  }
}
} // namespace lab
} // namespace tdm

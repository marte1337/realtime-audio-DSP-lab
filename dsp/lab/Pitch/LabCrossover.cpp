// LabCrossover: complementary linear-phase FIR/subtract split. See header.

#include "dsp/lab/Pitch/LabCrossover.h"

#include <cmath>
#include <stdexcept>

namespace tdm
{
namespace lab
{
namespace
{
constexpr double kPi = 3.14159265358979;
constexpr double kTwoPi = 2.0 * kPi;
// Hann-windowed-sinc main-lobe width factor: taps ~= factor * sr / transHz.
constexpr double kTapsPerTransHz = 3.3;
} // namespace

void LabCrossover::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("LabCrossover: sample rate out of range");
  sampleRate_ = sampleRate;

  // Constant-TIME transition (~200 Hz wide at any rate): tap count scales
  // with the rate, forced odd so the group delay is an integer.
  int taps = static_cast<int>(kTapsPerTransHz * sampleRate / (kStopHz - kPassHz) + 0.5);
  if (taps < 63)
    taps = 63;
  if ((taps & 1) == 0)
    ++taps;
  delay_ = (taps - 1) / 2;

  // Windowed sinc, -6 dB at kCutoffHz, Hann window, DC gain normalized
  // to exactly 1 (sum of coefficients).
  coeffs_.assign(static_cast<size_t>(taps), 0.0f);
  const double fc = kCutoffHz / sampleRate;
  double sum = 0.0;
  for (int k = 0; k < taps; ++k)
  {
    const double m = static_cast<double>(k - delay_);
    double h;
    if (m == 0.0)
      h = 2.0 * fc;
    else
      h = std::sin(kTwoPi * fc * m) / (kPi * m);
    const double w = 0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(k) / (taps - 1)));
    h *= w;
    coeffs_[static_cast<size_t>(k)] = static_cast<float>(h);
    sum += h;
  }
  const float norm = static_cast<float>(1.0 / sum);
  for (int k = 0; k < taps; ++k)
    coeffs_[static_cast<size_t>(k)] *= norm;

  hist_.assign(static_cast<size_t>(taps), 0.0f);
  dry_.assign(static_cast<size_t>(delay_ + 1), 0.0f);
  pos_ = 0;
}

void LabCrossover::processSample(float x, float* low, float* high)
{
  const int taps = static_cast<int>(coeffs_.size());
  if (taps <= 0)
  {
    *low = 0.0f; // never reset: defined as silence, like LabPitchShift
    *high = 0.0f;
    return;
  }
  hist_[static_cast<size_t>(pos_ % taps)] = x;
  dry_[static_cast<size_t>(pos_ % (delay_ + 1))] = x;

  // Direct-form FIR over the history ring (double accumulator keeps the
  // complementary sum exact to ~1e-7). Indexing walks down from the write
  // head with at most one wrap (no per-tap modulo: 64-bit % per tap cost
  // ~3% core in probes). Taps past the stream start read nothing.
  double acc = 0.0;
  const long long kmax = pos_ < taps ? pos_ : taps - 1;
  long long idx = pos_ % taps;
  for (long long k = 0; k <= kmax; ++k)
  {
    acc += static_cast<double>(coeffs_[static_cast<size_t>(k)]) * hist_[static_cast<size_t>(idx)];
    if (--idx < 0)
      idx += taps;
  }
  const float lo = static_cast<float>(acc);
  const float dryDelayed =
      pos_ < delay_ ? 0.0f : dry_[static_cast<size_t>((pos_ - delay_) % (delay_ + 1))];
  ++pos_;
  *low = lo;
  *high = dryDelayed - lo;
}

void LabCrossover::processBlock(const float* input, float* low, float* high, int numFrames)
{
  for (int i = 0; i < numFrames; ++i)
    processSample(input[i], low + i, high + i);
}
} // namespace lab
} // namespace tdm

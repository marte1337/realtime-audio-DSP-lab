#include "dsp/ToneShape/ToneShape.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace
{
// One-pole smoothing coefficient for time constant tauSec at sampleRate:
// y += (target - y) * coeff converges ~63% per tauSec.
float onePoleCoeff(double tauSec, double sampleRate)
{
  const double c = 1.0 - std::exp(-1.0 / (tauSec * sampleRate));
  return static_cast<float>(c < 1.0 ? c : 1.0);
}

// One-pole lowpass coefficient a for cutoff fcHz: y[n] += (1-a)*(x[n]-y[n]).
float lpfCoeff(double fcHz, double sampleRate)
{
  return static_cast<float>(std::exp(-2.0 * 3.141592653589793 * fcHz / sampleRate));
}

// One-pole highpass coefficient a for cutoff fcHz (analog-RC matched):
// y[n] = a * (y[n-1] + x[n] - x[n-1]).
float hpfCoeff(double fcHz, double sampleRate)
{
  return static_cast<float>(std::exp(-2.0 * 3.141592653589793 * fcHz / sampleRate));
}

float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

float snapZero(float v)
{
  return (v < 1e-12f && v > -1e-12f) ? 0.0f : v;
}
} // namespace

ToneShape::ToneShape()
{
  refreshDerived();
}

void ToneShape::reset(double sampleRate)
{
  if (sampleRate <= 0.0)
    throw std::runtime_error("ToneShape: sample rate must be positive");
  sampleRate_ = sampleRate;
  refreshDerived();
  // Documented deterministic state: filters cleared, smoothed mixes snapped
  // to targets, so neutral settings pass bit-exactly from the first sample.
  lpY_ = 0.0f;
  bpV1_ = 0.0f;
  bpV2_ = 0.0f;
  hpY_ = 0.0f;
  hpXPrev_ = 0.0f;
  weightK_ = weightTarget_;
  contourK_ = contourTarget_;
  presenceK_ = presenceTarget_;
}

void ToneShape::setWeight(float v)
{
  weight_ = clampf(v, kMinWeight, kMaxWeight);
  refreshDerived();
}

void ToneShape::setContour(float v)
{
  contour_ = clampf(v, kMinContour, kMaxContour);
  refreshDerived();
}

void ToneShape::setPresence(float v)
{
  presence_ = clampf(v, kMinPresence, kMaxPresence);
  refreshDerived();
}

void ToneShape::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void ToneShape::refreshDerived()
{
  // Weight/Presence: parallel-shelf mixes, exact by construction (the
  // section response tends to 1 at DC / Nyquist respectively).
  const float weightDb = -kWeightMaxDb + 2.0f * kWeightMaxDb * weight_;
  weightTarget_ = std::pow(10.0f, weightDb / 20.0f) - 1.0f;
  const float presenceDb = -kPresenceMaxDb + 2.0f * kPresenceMaxDb * presence_;
  presenceTarget_ = std::pow(10.0f, presenceDb / 20.0f) - 1.0f;
  // Contour: normalized parallel-bell mix; normalization makes the center
  // gain exact (see below).
  const float contourDb = -kContourMaxDb + 2.0f * kContourMaxDb * contour_;
  contourTarget_ = (std::pow(10.0f, contourDb / 20.0f) - 1.0f) / contourNorm_;
  if (sampleRate_ > 0.0)
  {
    lpA_ = lpfCoeff(kWeightHz, sampleRate_);
    hpA_ = hpfCoeff(kPresenceHz, sampleRate_);
    // Fixed RBJ bandpass (constant 0 dB peak gain form), normalized by a0.
    const double w0 = 2.0 * 3.141592653589793 * kContourHz / sampleRate_;
    const double alpha = std::sin(w0) / (2.0 * kContourQ);
    const double a0 = 1.0 + alpha;
    bpB0_ = static_cast<float>(alpha / a0);
    bpB2_ = static_cast<float>(-alpha / a0);
    bpA1_ = static_cast<float>(-2.0 * std::cos(w0) / a0);
    bpA2_ = static_cast<float>((1.0 - alpha) / a0);
    // Normalize the bell by its realized magnitude at center: evaluates the
    // actual biquad (not the textbook ideal), so the +/-6 dB spec holds
    // exactly at fc on every rate even allowing for float rounding.
    const std::complex<double> z = std::exp(std::complex<double>(0.0, w0));
    const std::complex<double> num =
      static_cast<double>(bpB0_) + static_cast<double>(bpB2_) / (z * z); // b1 == 0
    const std::complex<double> den =
      1.0 + static_cast<double>(bpA1_) / z + static_cast<double>(bpA2_) / (z * z);
    const double mag = std::abs(num / den);
    contourNorm_ = mag > 1e-6 ? static_cast<float>(mag) : 1.0f;
    // Re-derive the contour target against the fresh normalization (cheap,
    // off-RT; keeps setters-before-reset honest once reset() has run).
    contourTarget_ = (std::pow(10.0f, contourDb / 20.0f) - 1.0f) / contourNorm_;
    smoothCoeff_ = onePoleCoeff(kSmoothMs / 1000.0, sampleRate_);
  }
}

void ToneShape::processBlock(const float* input, float* output, int numFrames)
{
  assert(input != nullptr && output != nullptr && numFrames >= 0);
  if (numFrames <= 0)
    return;
  if (!enabled_ || sampleRate_ <= 0.0)
  {
    if (output != input)
      std::memcpy(output, input, sizeof(float) * static_cast<size_t>(numFrames));
    return;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    // Smoothed mixes: moves bend, never step (zipper-free).
    weightK_ += (weightTarget_ - weightK_) * smoothCoeff_;
    contourK_ += (contourTarget_ - contourK_) * smoothCoeff_;
    presenceK_ += (presenceTarget_ - presenceK_) * smoothCoeff_;

    const float x = input[i];
    // Weight low shelf, parallel form: y = x + k*LP(x).
    lpY_ = snapZero(lpY_ + (1.0f - lpA_) * (x - lpY_));
    const float weighted = x + weightK_ * lpY_;
    // Contour bell, parallel form: y = x + k*BP(x), biquad in TDF2.
    // (No finiteness guard needed: fixed stable biquad + finite input is
    // BIBO-stable by construction; the suite proves finite output.)
    const float v = weighted - bpA1_ * bpV1_ - bpA2_ * bpV2_;
    const float bp = bpB0_ * v + bpB2_ * bpV2_; // b1 == 0 by topology
    bpV2_ = snapZero(bpV1_);
    bpV1_ = snapZero(v);
    const float contoured = weighted + contourK_ * bp;
    // Presence high shelf, parallel form: y = x + k*HP(x).
    hpY_ = snapZero(hpA_ * (hpY_ + contoured - hpXPrev_));
    hpXPrev_ = contoured;
    output[i] = contoured + presenceK_ * hpY_;
  }
}
} // namespace tdm

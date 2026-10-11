#pragma once

// AlienShift: TRUE frequency shifter (LAB, dsp/lab/Alien). Adds a fixed Hz
// offset to every partial (100/200/300 + 150 Hz -> 250/350/450), producing
// inharmonic, metallic dissonance. NOT a semitone transpose.
//
// Architecture: single-sideband modulation around a windowed FIR Hilbert
// transformer. Q path: 1023-tap Blackman-windowed ideal-Hilbert FIR (even
// taps are exactly zero and skipped: 512 MACs/sample); I path: matching
// 511-sample pure delay. Wet = I*cos(wt) + Q*sin(wt) (this Hilbert sign
// convention; verified by Goertzel in tests). Quadrature osc is exact
// sin/cos per sample (no drift, ever).
//
// Character notes (measured, 48 kHz): sideband rejection ~90 dB midband,
// ~32 dB at 82 Hz, ~22 dB at 62 Hz, ~13 dB at 41 Hz (Hilbert error grows
// near DC; reads as a detuned ghost on the lowest strings, not
// instability). Latency: 511 samples (~10.6 ms).
//
// Params: shiftHz (-2000..+2000, smoothed ~20 ms so knob moves bend
// instead of zipping) and mix (0 = bit-exact dry). reset(sr) is off-RT;
// process() is RT-safe, deterministic, finite-out.

#include <algorithm>

#include "dsp/lab/Alien/AlienDsp.h"

namespace tdm
{
namespace alien
{

class AlienShift
{
public:
  static constexpr int kTaps = 1023; // odd length; even taps exactly zero
  static constexpr int kLatency = (kTaps - 1) / 2; // 255 samples
  static constexpr int kPairs = (kLatency + 1) / 2; // odd-offset pairs (128)
  static constexpr float kMinShiftHz = -2000.0f;
  static constexpr float kMaxShiftHz = 2000.0f;
  static constexpr float kDefaultShiftHz = 75.0f; // industrial starting point
  static constexpr float kDefaultMix = 1.0f;

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    // Blackman-windowed ideal Hilbert: h[o] = 2/(pi*o) at odd offsets
    // o from -127..127, zero at evens. Coefficients are rate-independent
    // (normalized design); band edges scale with Nyquist.
    const int m = kLatency;
    for (int k = 0; k < kPairs; ++k)
    {
      const int o = 2 * k + 1; // 1, 3, ..., 2*kPairs-1
      const float nPos = static_cast<float>(m + o); // tap index, right side
      const float a = 2.0f * 3.141592653589793f * nPos / (kTaps - 1);
      const float w = 0.42f - 0.5f * std::cos(a) + 0.08f * std::cos(2.0f * a);
      h_[static_cast<size_t>(k)] = (2.0f / (3.141592653589793f * o)) * w;
    }
    ring_.assign(static_cast<size_t>(kTaps), 0.0f);
    w_ = 0;
    osc_.reset(sr, shiftHz_.value());
    shiftHz_.reset(sr, 20.0f, shiftTgt_);
    mix_.reset(sr, 10.0f, mixTgt_);
    shiftHz_.snap(shiftTgt_);
    mix_.snap(mixTgt_);
  }

  void setShiftHz(float hz)
  {
    shiftTgt_ = (hz < kMinShiftHz) ? kMinShiftHz : ((hz > kMaxShiftHz) ? kMaxShiftHz : hz);
    shiftHz_.setTarget(shiftTgt_);
  }
  void setMix(float v)
  {
    mixTgt_ = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    mix_.setTarget(mixTgt_);
  }
  // RT-safe state clear (no allocation): zero delay states, snap
  // smoothers to targets, restart oscillator phase. Call at a dip
  // bottom (inaudible) when (re)selecting this effect.
  void clear()
  {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    w_ = 0;
    osc_.reset(sr_, shiftTgt_);
    shiftHz_.snap(shiftTgt_);
    mix_.snap(mixTgt_);
  }
  float shiftHz() const { return shiftTgt_; }
  float mix() const { return mixTgt_; }
  int latencySamples() const { return kLatency; }

  float process(float x)
  {
    ring_[w_] = x;
    // Q = sum over odd offsets (antisymmetric pair around the M=127
    // center tap): x[n-(M-o)] - x[n-(M+o)], both causal (lags 0..254).
    float q = 0.0f;
    const size_t len = static_cast<size_t>(kTaps);
    const size_t m = static_cast<size_t>(kLatency);
    for (int k = 0; k < kPairs; ++k)
    {
      const size_t o = static_cast<size_t>(2 * k + 1);
      size_t jp = w_ + len - (m - o);
      if (jp >= len)
        jp -= len;
      size_t jn = w_ + len - (m + o);
      if (jn >= len)
        jn -= len;
      q += h_[static_cast<size_t>(k)] * (ring_[jp] - ring_[jn]);
    }
    // I = x[n-127] sample-aligned with the Q FIR center.
    size_t ji = w_ + len - static_cast<size_t>(kLatency);
    if (ji >= len)
      ji -= len;
    const float i = ring_[ji];
    ++w_;
    if (w_ >= len)
      w_ = 0;

    const float hz = shiftHz_.next();
    osc_.setFreq(hz);
    float s, c;
    osc_.next(s, c);
    // Upper sideband for hz > 0 (I*cos + Q*sin with this Hilbert sign
    // convention; negative hz mirrors the shift down. Verified by Goertzel:
    // direction pinned in tests).
    const float wet = i * c + q * s;
    const float mx = mix_.next();
    const float y = x + mx * (alienSnap(wet) - x);
    return std::isfinite(y) ? y : x;
  }

  void processBlock(const float* input, float* output, int n)
  {
    for (int k = 0; k < n; ++k)
      output[k] = process(input[k]);
  }

private:
  double sr_ = 48000.0;
  float h_[kPairs] = {0.0f}; // right-half Hilbert taps (odd offsets)
  std::vector<float> ring_;
  size_t w_ = 0;
  QuadOsc osc_;
  SmoothParam shiftHz_, mix_;
  float shiftTgt_ = kDefaultShiftHz, mixTgt_ = kDefaultMix;
};

} // namespace alien
} // namespace tdm

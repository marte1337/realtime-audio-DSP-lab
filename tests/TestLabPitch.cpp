#include "tests/Assert.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/lab/Pitch/LabFft.h"
#include "dsp/lab/Pitch/LabPitchShift.h"

namespace
{
constexpr double kPi = 3.14159265358979;

// Deterministic LCG in [-1, 1].
float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
}

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / sr));
  return out;
}

// Karplus-Strong plucked string: the lab suite's guitar proxy. Noise-burst
// excitation through a damped averaging loop; damp near 1 = open string
// sustain, lower = darker/faster palm-mute-ish decay. Deterministic.
std::vector<float> ksPluck(float peak, float freqHz, double sr, int n, float damp = 0.996f,
                           uint32_t seed = 0x51ab3u)
{
  const int period = static_cast<int>(sr / freqHz + 0.5f) > 2 ? static_cast<int>(sr / freqHz + 0.5f) : 2;
  std::vector<float> line(static_cast<size_t>(period));
  for (int i = 0; i < period; ++i)
    line[static_cast<size_t>(i)] = detNoise(i, seed);
  // Pick lowpass on the excitation: real picks are not white.
  for (int i = 1; i < period; ++i)
    line[static_cast<size_t>(i)] = 0.5f * (line[static_cast<size_t>(i)] + line[static_cast<size_t>(i - 1)]);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  int idx = 0;
  float prev = 0.0f;
  for (int i = 0; i < n; ++i)
  {
    const float cur = line[static_cast<size_t>(idx)];
    const float v = damp * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = peak * cur;
  }
  return out;
}

void mixInto(std::vector<float>& dst, const std::vector<float>& src, int at = 0)
{
  for (size_t i = 0; i < src.size() && at + static_cast<int>(i) < static_cast<int>(dst.size()); ++i)
    dst[static_cast<size_t>(at) + i] += src[i];
}

// Run the shifter in small blocks, latency-compensated: feed in +
// latency + tail zeros, drop the first latency outputs. Returns aligned
// output (same length as in).
std::vector<float> runLab(double sr, float shiftSt, const std::vector<float>& in, int fft = 4096,
                          int hop = 1024, int block = 256)
{
  tdm::lab::LabPitchShift p;
  p.setConfig(fft, hop);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(sr);
  const int lat = p.latencySamples();
  const int tail = p.tailSamples();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + lat + tail;
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 9.0f);
  for (int off = 0; off < paddedN; off += block)
  {
    const int m = (paddedN - off) < block ? (paddedN - off) : block;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  return out;
}

double rms(const std::vector<float>& v, int from, int to)
{
  double se = 0.0;
  for (int i = from; i < to; ++i)
    se += static_cast<double>(v[static_cast<size_t>(i)]) * v[static_cast<size_t>(i)];
  return std::sqrt(se / (to - from));
}

double goertzel(const float* v, int n, double sr, double f)
{
  const double w = kPi * 2.0 * f / sr;
  const double c = 2.0 * std::cos(w);
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  for (int i = 0; i < n; ++i)
  {
    s0 = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

double framePitch(const float* v, int n, double sr, double lo, double hi, double step = 2.0)
{
  double best = lo, bm = -1.0;
  for (double f = lo; f <= hi; f += step)
  {
    const double m = goertzel(v, n, sr, f);
    if (m > bm)
    {
      bm = m;
      best = f;
    }
  }
  return best;
}

double median(std::vector<double> v)
{
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double maxStep(const float* v, int n)
{
  double m = 0.0;
  for (int i = 1; i < n; ++i)
  {
    const double d = std::fabs(v[i] - v[i - 1]);
    if (d > m)
      m = d;
  }
  return m;
}
} // namespace

void runLabPitchTests()
{
  { // FFT round-trip: forward -> inverse reproduces the input.
    tdm::lab::LabFft fft(1024);
    std::vector<std::complex<float>> v(1024);
    for (int i = 0; i < 1024; ++i)
      v[static_cast<size_t>(i)] = std::complex<float>(detNoise(i), detNoise(i + 9999));
    fft.forward(v.data());
    TDM_CHECK(tdm_test::allFinite(reinterpret_cast<const float*>(v.data()), 2048), "lab fft finite");
    fft.inverse(v.data());
    double worst = 0.0;
    for (int i = 0; i < 1024; ++i)
    {
      worst = std::max(worst, static_cast<double>(std::fabs(v[static_cast<size_t>(i)].real() - detNoise(i))));
      worst = std::max(worst,
                       static_cast<double>(std::fabs(v[static_cast<size_t>(i)].imag() - detNoise(i + 9999))));
    }
    TDM_CHECK(worst < 1e-3f, "lab fft round-trip");
  }
  { // FFT sanity: impulse -> flat spectrum, sine -> single-bin energy.
    tdm::lab::LabFft fft(256);
    std::vector<std::complex<float>> v(256, std::complex<float>(0.0f, 0.0f));
    v[0] = std::complex<float>(1.0f, 0.0f);
    fft.forward(v.data());
    for (int k = 0; k < 256; k += 37)
      TDM_CHECK_CLOSE(std::abs(v[static_cast<size_t>(k)]), 1.0, 1e-4, "lab fft impulse flat");
    for (int i = 0; i < 256; ++i)
      v[static_cast<size_t>(i)] =
          std::complex<float>(static_cast<float>(std::sin(kPi * 2.0 * 8.0 * i / 256.0)), 0.0f);
    fft.forward(v.data());
    TDM_CHECK(std::abs(v[8]) > 100.0f, "lab fft sine bin energy");
    TDM_CHECK(std::abs(v[9]) < 1.0f, "lab fft sine bin leakage");
  }
  { // Config validation.
    tdm::lab::LabPitchShift p;
    bool threw = false;
    try
    {
      p.setConfig(3000, 750);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "lab config rejects non-power-of-two");
    threw = false;
    try
    {
      p.setConfig(4096, 700);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "lab config rejects non-dividing hop");
    threw = false;
    try
    {
      p.reset(1000.0);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "lab reset rejects bad rate");
  }
  { // Disabled bypass is bit-exact (out-of-place and in-place).
    tdm::lab::LabPitchShift p;
    p.reset(48000.0);
    p.setEnabled(false);
    p.setShiftSt(-12.0f);
    std::vector<float> in(4096);
    for (int i = 0; i < 4096; ++i)
      in[static_cast<size_t>(i)] = detNoise(i) * 0.7f;
    std::vector<float> out(4096, 9.0f);
    p.processBlock(in.data(), out.data(), 4096);
    TDM_CHECK(out == in, "lab bypass exact");
    p.processBlock(in.data(), in.data(), 4096);
    for (int i = 0; i < 4096; ++i)
      if (in[static_cast<size_t>(i)] != detNoise(i) * 0.7f)
      {
        TDM_CHECK(false, "lab bypass in-place exact");
        break;
      }
  }
  { // Latency follows (N + 1) + (N - Ha) * (1/r - 1), pinned exactly
    // per shift (regression on the alignment the tool relies on).
    // Exactly 0 st bypasses: latency 0, tail 0.
    tdm::lab::LabPitchShift p;
    p.setEnabled(true);
    for (float st : {-1.0f, -2.0f, -7.0f, -12.0f, -24.0f})
    {
      p.setShiftSt(st);
      p.reset(48000.0);
      const double r = p.actualRatio();
      const int want = static_cast<int>((4096 + 1) + (4096 - 1024) * (1.0 / r - 1.0) + 0.5);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab latency formula st=%.0f", st);
      TDM_CHECK(p.latencySamples() == want, msg);
    }
    p.setShiftSt(0.0f);
    p.reset(48000.0);
    TDM_CHECK(p.latencySamples() == 0, "lab latency 0st bypass == 0");
    TDM_CHECK(p.tailSamples() == 0, "lab tail 0st bypass == 0");
    p.setShiftSt(-12.0f);
    p.reset(48000.0);
    // Probe past latency (7169 @ -12) + one window so DC is steady.
    std::vector<float> in(14000, 0.5f), out(14000, 9.0f);
    p.processBlock(in.data(), out.data(), 14000);
    bool zeros = true;
    for (int i = 0; i + 1 < 1024; ++i)
      if (out[static_cast<size_t>(i)] != 0.0f)
        zeros = false;
    TDM_CHECK(zeros, "lab first hop zeros before first frame");
    // (The very first emitted samples sit under the window edge guard;
    // check past it.)
    TDM_CHECK(out[1500] != 0.0f, "lab content emerges after one hop");
    // Steady-state sine level at -12 (OLA + resample gain is ~flat for AC;
    // measured 0.95-0.97 RMS. DC is NOT a valid probe here: static DC
    // settles 1-30% low depending on shift depth - a DC-only quirk under
    // lab observation, harmless for AC guitar signals.)
    {
      const int sn = static_cast<int>(48000.0 * 1.5);
      std::vector<float> s = sine(0.5f, 220.0f, 48000.0, sn);
      std::vector<float> os = runLab(48000.0, -12.0f, s);
      const double lr = rms(os, sn / 2, sn) / rms(s, sn / 2, sn);
      TDM_CHECK(lr > 0.90 && lr < 1.05, "lab sine steady level -12");
    }
  }
  { // Exact 0-st bypass: output is bit-identical to input (streaming and
    // latency-compensated offline), in-place and out-of-place, at any
    // block size. Small real shifts must NOT bypass (-0.1 st runs the
    // vocoder: nonzero latency, output differs from input).
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detNoise(i) * 0.7f;
    mixInto(in, sine(0.3f, 220.0f, sr, n));
    mixInto(in, ksPluck(0.4f, 82.41f, sr, n));
    std::vector<float> off = runLab(sr, 0.0f, in);
    TDM_CHECK(off == in, "lab 0st offline bit-exact");
    for (int block : {1, 64, 1024, 5000})
    {
      std::vector<float> b = runLab(sr, 0.0f, in, 4096, 1024, block);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "lab 0st bit-exact block=%d", block);
      TDM_CHECK(b == in, msg);
    }
    tdm::lab::LabPitchShift p;
    p.setEnabled(true);
    p.setShiftSt(0.0f);
    p.reset(sr);
    std::vector<float> out(static_cast<size_t>(n), 9.0f);
    p.processBlock(in.data(), out.data(), n);
    TDM_CHECK(out == in, "lab 0st streaming bit-exact");
    std::vector<float> ip = in;
    p.processBlock(ip.data(), ip.data(), n);
    TDM_CHECK(ip == in, "lab 0st in-place bit-exact");
    // Shift changes are reset-gated: storing 0 without reset keeps the PV
    // path (and its latency) until the next reset().
    p.setShiftSt(-7.0f);
    p.reset(sr);
    const int lat7 = p.latencySamples();
    p.setShiftSt(0.0f); // stored, not yet active
    TDM_CHECK(p.latencySamples() == lat7, "lab 0st takes effect on reset");
    std::vector<float> pre(static_cast<size_t>(n), 9.0f);
    p.processBlock(in.data(), pre.data(), n);
    TDM_CHECK(!(pre == in), "lab pre-reset 0st still processes");
    p.reset(sr);
    TDM_CHECK(p.latencySamples() == 0, "lab post-reset 0st latency 0");
    std::vector<float> post(static_cast<size_t>(n), 9.0f);
    p.processBlock(in.data(), post.data(), n);
    TDM_CHECK(post == in, "lab post-reset 0st bit-exact");
    // -0.1 st is a real shift: nonzero latency, vocoder output.
    tdm::lab::LabPitchShift q;
    q.setEnabled(true);
    q.setShiftSt(-0.1f);
    q.reset(sr);
    TDM_CHECK(q.latencySamples() > 0, "lab -0.1st latency nonzero");
    TDM_CHECK(q.tailSamples() > 0, "lab -0.1st tail nonzero");
    std::vector<float> sm = runLab(sr, -0.1f, in);
    TDM_CHECK(tdm_test::allFinite(sm.data(), n), "lab -0.1st finite");
    TDM_CHECK(!(sm == in), "lab -0.1st not bypassed");
  }
  { // Silence in -> silence out (exact), all finite.
    for (double sr : {44100.0, 48000.0, 96000.0})
    {
      std::vector<float> in(static_cast<size_t>(sr * 0.5), 0.0f);
      std::vector<float> out = runLab(sr, -7.0f, in);
      TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "lab silence finite");
      TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(out.size())) == 0.0f, "lab silence exact");
    }
  }
  { // 0 st identity: exactly 0 st is now a bit-exact bypass (pinned by
    // sample comparison in the test above); pitch + level still hold and
    // are kept as a cheap end-to-end sanity check.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> s = sine(0.5f, 220.0f, sr, n);
    std::vector<float> os = runLab(sr, 0.0f, s);
    const int nf = static_cast<int>(sr * 0.04);
    std::vector<double> pitches;
    for (int off = n / 4; off + nf < n; off += nf)
      pitches.push_back(framePitch(os.data() + off, nf, sr, 200.0, 240.0, 1.0));
    TDM_CHECK(std::fabs(median(pitches) - 220.0) / 220.0 < 0.01, "lab identity sine pitch");
    TDM_CHECK(std::fabs(rms(os, n / 4, n) / rms(s, n / 4, n) - 1.0) < 0.02, "lab identity sine level");
    std::vector<float> k = ksPluck(0.5f, 82.41f, sr, n);
    std::vector<float> ok = runLab(sr, 0.0f, k);
    const int nf2 = static_cast<int>(sr * 0.08);
    std::vector<double> pp;
    for (int off = n / 8; off + nf2 < n / 2; off += nf2)
      pp.push_back(framePitch(ok.data() + off, nf2, sr, 70.0, 95.0, 1.0));
    TDM_CHECK(std::fabs(median(pp) - 82.41) / 82.41 < 0.02, "lab identity pluck pitch");
    TDM_CHECK(std::fabs(rms(ok, n / 8, n / 2) / rms(k, n / 8, n / 2) - 1.0) < 0.1, "lab identity pluck level");
  }
  { // Onset coarse anchor: a gated-sine onset reappears within +-3000
    // samples of its index after compensation. Wide by physics, not by
    // sloppiness: onsets smear over ~N(1/r-1) samples and the threshold
    // crossing sits inside the smear (measured early by up to ~2500 at
    // -24). Guards gross breakage; precision lives in group delay above.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.5);
    const int t0 = n / 3;
    for (float st : {-7.0f, -12.0f, -24.0f})
    {
      std::vector<float> in(static_cast<size_t>(n), 0.0f);
      for (int i = t0; i < n; ++i)
      {
        double g = (i - t0) < 64 ? 0.5 * (1.0 - std::cos(kPi * (i - t0) / 64.0)) : 1.0;
        in[static_cast<size_t>(i)] = static_cast<float>(0.5 * g * std::sin(kPi * 2.0 * 220.0 * i / sr));
      }
      std::vector<float> out = runLab(sr, st, in);
      float pk = 0.0f;
      for (int i = t0 + 8000; i < n; ++i)
        pk = std::max(pk, std::fabs(out[static_cast<size_t>(i)]));
      int cross = -1;
      for (int i = t0 - 3000; i < n; ++i)
        if (std::fabs(out[static_cast<size_t>(i)]) > 0.2f * pk)
        {
          cross = i;
          break;
        }
      float ipk = 0.0f;
      for (int i = t0 + 8000; i < n; ++i)
        ipk = std::max(ipk, std::fabs(in[static_cast<size_t>(i)]));
      int icross = -1;
      for (int i = t0 - 3000; i < n; ++i)
        if (std::fabs(in[static_cast<size_t>(i)]) > 0.2f * ipk)
        {
          icross = i;
          break;
        }
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab onset coarse st=%.0f", st);
      TDM_CHECK(cross >= 0 && icross >= 0 && std::abs(cross - icross) <= 3000, msg);
    }
  }
  { // Sustained group delay: four steady tones in SEPARATE renders
    // (shared renders beat inside one bin and muddle PV phases) pin the
    // residual phase slope after latency compensation within +-1500
    // samples of flat. A fitted slope is required (each partial carries
    // its own arbitrary seed phase); adjacent-unwrap is safe because
    // residual steps are << pi (fold margins >= +-5000 samples, guarded
    // by the onset coarse test, so catastrophic breaks still fail).
    // Why +-1500 and not tighter: output phase-vs-frequency is curved
    // (seed-transient + peak-region dispersion makes group delay itself
    // frequency-dependent), so local-slope fits legitimately disagree by
    // hundreds of samples across sub-bands. Measured residuals with the
    // current implementation: -195 (-7), -386 (-12), -1241 (-24);
    // re-measure (don't just widen) if the PV core ever changes. This
    // still pins tool alignment far below audibility for detuned A/B.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1);
    const double tones[4] = {220.0, 227.0, 233.0, 240.0};
    for (float st : {-7.0f, -12.0f, -24.0f})
    {
      tdm::lab::LabPitchShift probe;
      probe.setShiftSt(st);
      probe.reset(sr);
      const double r = probe.actualRatio();
      double ph[4] = {0.0, 0.0, 0.0, 0.0};
      for (int t = 0; t < 4; ++t)
      {
        const double fo = tones[t] * r;
        std::vector<float> out = runLab(sr, st, sine(0.5f, static_cast<float>(tones[t]), sr, n));
        double si = 0.0, co = 0.0;
        const int m0 = n / 2, m1 = n - 2000;
        for (int i = m0; i < m1; ++i)
        {
          si += out[static_cast<size_t>(i)] * std::sin(kPi * 2.0 * fo * i / sr);
          co += out[static_cast<size_t>(i)] * std::cos(kPi * 2.0 * fo * i / sr);
        }
        ph[t] = std::atan2(co, si);
        if (t > 0) // adjacent unwrap: residual steps are << pi
        {
          while (ph[t] - ph[t - 1] > kPi)
            ph[t] -= 2 * kPi;
          while (ph[t] - ph[t - 1] < -kPi)
            ph[t] += 2 * kPi;
        }
      }
      double sf = 0.0, sp = 0.0, sf2 = 0.0, sfp = 0.0;
      for (int t = 0; t < 4; ++t)
      {
        const double fo = tones[t] * r;
        sf += fo;
        sp += ph[t];
        sf2 += fo * fo;
        sfp += fo * ph[t];
      }
      const double slope = (4 * sfp - sf * sp) / (4 * sf2 - sf * sf);
      const double residual = -slope * sr / (kPi * 2.0);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab group delay st=%.0f", st);
      TDM_CHECK(std::fabs(residual) <= 1500.0, msg);
    }
  }
  { // Click boundedness: an isolated impulse stays finite, near unity,
    // and confined (no runaway ring, no far-flung images).
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    for (float st : {-7.0f, -12.0f})
    {
      std::vector<float> in(static_cast<size_t>(n), 0.0f);
      in[static_cast<size_t>(n / 2)] = 1.0f;
      std::vector<float> out = runLab(sr, st, in);
      TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab click finite");
      TDM_CHECK(tdm_test::peakAbs(out.data(), n) < 1.5f, "lab click bounded");
      float far = 0.0f;
      for (int i = 0; i < n / 2 - 8192; ++i)
        far = std::max(far, std::fabs(out[static_cast<size_t>(i)]));
      for (int i = n / 2 + 16384; i < n; ++i)
        far = std::max(far, std::fabs(out[static_cast<size_t>(i)]));
      TDM_CHECK(far < 0.01f, "lab click confined");
    }
  }
  { // Hop rounding: actualRatio tracks the target within 0.1% at every
    // evaluation shift (pitch error from integer Hs is inaudible).
    tdm::lab::LabPitchShift p;
    p.setEnabled(true);
    for (float st : {-1.0f, -2.0f, -7.0f, -12.0f, -24.0f})
    {
      p.setShiftSt(st);
      p.reset(48000.0);
      const double want = std::exp2(st / 12.0);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab actual ratio st=%.0f", st);
      TDM_CHECK(std::fabs(p.actualRatio() - want) / want < 0.001, msg);
    }
  }
  { // Highs transpose, not vanish: 12 kHz at -12 lands on 6 kHz audibly
    // (cubic droop only), with no strong image at the source pitch. This
    // pins the compress-first topology against decimate-style top loss.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> out = runLab(sr, -12.0f, sine(0.3f, 12000.0f, sr, n));
    const int off = n / 4, nf = n / 2;
    const double want = goertzel(out.data() + off, nf, sr, 6000.0);
    TDM_CHECK(want > 0.05, "lab high partial transposed");
    TDM_CHECK(goertzel(out.data() + off, nf, sr, 12000.0) < want * 0.1 + 1e-4, "lab no source image");
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab high finite");
  }
  for (double sr : {44100.0, 48000.0, 96000.0})
  { // Pitch accuracy: sines at -1/-2/-7/-12, median over 40 ms frames.
    for (float st : {-1.0f, -2.0f, -7.0f, -12.0f})
    {
      const double ratio = std::exp2(st / 12.0);
      for (float f : {110.0f, 220.0f, 440.0f})
      {
        const int n = static_cast<int>(sr * 1.2);
        std::vector<float> out = runLab(sr, st, sine(0.5f, f, sr, n));
        const int nf = static_cast<int>(sr * 0.04);
        std::vector<double> pitches;
        for (int off = n / 4; off + nf < n; off += nf)
          pitches.push_back(framePitch(out.data() + off, nf, sr, f * ratio * 0.8, f * ratio * 1.25));
        const double med = median(pitches);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "lab sine acc sr=%.0f st=%.0f f=%.0f", sr, st, f);
        TDM_CHECK(std::fabs(med - f * ratio) / (f * ratio) < 0.02, msg);
        TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab sine finite");
      }
    }
  }
  { // Low-string accuracy on harmonic + KS material at -7/-12 (the v1
    // failure zone): E2/A2 with harmonics and a real decaying pluck.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.5);
    for (float st : {-7.0f, -12.0f})
    {
      const double ratio = std::exp2(st / 12.0);
      std::vector<float> harm(static_cast<size_t>(n), 0.0f);
      mixInto(harm, sine(0.4f, 82.41f, sr, n));
      mixInto(harm, sine(0.2f, 164.81f, sr, n));
      mixInto(harm, sine(0.1f, 247.0f, sr, n));
      mixInto(harm, sine(0.05f, 329.0f, sr, n));
      std::vector<float> out = runLab(sr, st, harm);
      const int nf = static_cast<int>(sr * 0.06);
      std::vector<double> pitches;
      for (int off = n / 4; off + nf < n; off += nf)
        pitches.push_back(framePitch(out.data() + off, nf, sr, 82.41 * ratio * 0.8, 82.41 * ratio * 1.25,
                                     1.0));
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab lowE harmonic st=%.0f", st);
      TDM_CHECK(std::fabs(median(pitches) - 82.41 * ratio) / (82.41 * ratio) < 0.03, msg);

      std::vector<float> pluck = ksPluck(0.5f, 82.41f, sr, n);
      std::vector<float> op = runLab(sr, st, pluck);
      std::vector<double> pp;
      const int nf2 = static_cast<int>(sr * 0.08);
      for (int off = n / 8; off + nf2 < n / 2; off += nf2) // early sustain
        pp.push_back(framePitch(op.data() + off, nf2, sr, 82.41 * ratio * 0.75, 82.41 * ratio * 1.3, 1.0));
      std::snprintf(msg, sizeof(msg), "lab lowE pluck st=%.0f", st);
      TDM_CHECK(std::fabs(median(pp) - 82.41 * ratio) / (82.41 * ratio) < 0.05, msg);
      TDM_CHECK(tdm_test::allFinite(op.data(), n), "lab pluck finite");
    }
  }
  { // Chord coherence + no dry ghost: E2+B2+E3 stack at -7. Each partial
    // lands on its shifted pitch; energy at ORIGINAL pitches that are NOT
    // also shifted pitches must be far below (a dry bleed fails this).
    // Note: -7 st is a fifth, so shifted B2 == original E2 exactly: energy
    // at 82.41 is EXPECTED (positive check), and only 123.47/164.81 are
    // valid ghost probes.
    const double sr = 48000.0;
    const double ratio = std::exp2(-7.0 / 12.0);
    const int n = static_cast<int>(sr * 1.5);
    std::vector<float> chord(static_cast<size_t>(n), 0.0f);
    for (float f : {82.41f, 123.47f, 164.81f})
      mixInto(chord, sine(0.3f, f, sr, n));
    std::vector<float> out = runLab(sr, -7.0f, chord);
    const int off = n / 3, nf = static_cast<int>(sr * 0.3);
    for (float f : {82.41f, 123.47f, 164.81f})
    {
      const double want = goertzel(out.data() + off, nf, sr, f * ratio);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab chord partial f=%.1f shifted present", f);
      TDM_CHECK(want > 0.05, msg);
    }
    for (float f : {123.47f, 164.81f}) // not in the shifted set {55, 82.4, 110}
    {
      const double dry = goertzel(out.data() + off, nf, sr, f);
      const double want = goertzel(out.data() + off, nf, sr, f * ratio);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab chord partial f=%.1f no dry ghost", f);
      TDM_CHECK(dry < want * 0.1 + 1e-4, msg);
    }
    TDM_CHECK(goertzel(out.data() + off, nf, sr, 82.41) > 0.05, "lab fifth overlap present");
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab chord finite");
  }
  { // KS power-chord accuracy: E2+B2+E3 plucks struck together at -7.
    const double sr = 48000.0;
    const double ratio = std::exp2(-7.0 / 12.0);
    const int n = static_cast<int>(sr * 1.5);
    std::vector<float> chord(static_cast<size_t>(n), 0.0f);
    mixInto(chord, ksPluck(0.3f, 82.41f, sr, n, 0.996f, 0x1111u));
    mixInto(chord, ksPluck(0.25f, 123.47f, sr, n, 0.996f, 0x2222u));
    mixInto(chord, ksPluck(0.2f, 164.81f, sr, n, 0.996f, 0x3333u));
    std::vector<float> out = runLab(sr, -7.0f, chord);
    const int off = n / 6, nf = static_cast<int>(sr * 0.25);
    // Lowest partial dominates a power chord: it must read shifted E2.
    const double med = framePitch(out.data() + off, nf, sr, 82.41 * ratio * 0.8, 82.41 * ratio * 1.2, 1.0);
    TDM_CHECK(std::fabs(med - 82.41 * ratio) / (82.41 * ratio) < 0.06, "lab KS power chord root");
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab KS chord finite");
  }
  { // Continuity: no sample-step clicks on sustained material.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    for (float st : {-1.0f, -12.0f})
    {
      std::vector<float> out = runLab(sr, st, sine(0.5f, 220.0f, sr, n));
      char msg[64];
      std::snprintf(msg, sizeof(msg), "lab continuity st=%.0f", st);
      TDM_CHECK(maxStep(out.data() + n / 4, n - n / 4) < 0.1, msg);
    }
  }
  { // -24 finite + bounded: noise burst + low sine, peak must not run away.
    for (double sr : {44100.0, 48000.0, 96000.0})
    {
      const int n = static_cast<int>(sr * 1.0);
      std::vector<float> in(static_cast<size_t>(n), 0.0f);
      for (int i = 0; i < n / 8; ++i)
        in[static_cast<size_t>(n / 4 + i)] = detNoise(i) * 0.7f;
      mixInto(in, sine(0.5f, 110.0f, sr, n));
      std::vector<float> out = runLab(sr, -24.0f, in);
      TDM_CHECK(tdm_test::allFinite(out.data(), n), "lab -24 finite");
      TDM_CHECK(tdm_test::peakAbs(out.data(), n) < 2.0f, "lab -24 bounded");
    }
  }
  { // Alt config (2048/512) runs and shifts accurately; block-size and
    // reset determinism hold bit-exactly.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = sine(0.5f, 440.0f, sr, n);
    std::vector<float> out = runLab(sr, -12.0f, in, 2048, 512);
    const int nf = static_cast<int>(sr * 0.04);
    std::vector<double> pitches;
    for (int off = n / 4; off + nf < n; off += nf)
      pitches.push_back(framePitch(out.data() + off, nf, sr, 180.0, 260.0));
    TDM_CHECK(std::fabs(median(pitches) - 220.0) / 220.0 < 0.02, "lab alt-config accuracy");
    std::vector<float> b64 = runLab(sr, -7.0f, in, 4096, 1024, 64);
    std::vector<float> b2k = runLab(sr, -7.0f, in, 4096, 1024, 2048);
    TDM_CHECK(b64 == b2k, "lab block-size deterministic");
    std::vector<float> r1 = runLab(sr, -2.0f, in);
    std::vector<float> r2 = runLab(sr, -2.0f, in);
    TDM_CHECK(r1 == r2, "lab reset deterministic");
  }
  { // Quality/latency study configs: each supported FFT/hop pair accepts
    // every evaluation shift, stays finite/bounded, keeps 0 st bit-exact,
    // and follows the latency formula at -7 st.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = ksPluck(0.5f, 82.41f, sr, n);
    mixInto(in, sine(0.3f, 440.0f, sr, n));
    const int cfgs[4][2] = {{4096, 1024}, {2048, 512}, {1024, 256}, {2048, 256}};
    for (const auto& c : cfgs)
    {
      const int fft = c[0], hop = c[1];
      for (float st : {0.0f, -1.0f, -2.0f, -7.0f, -12.0f})
      {
        std::vector<float> out = runLab(sr, st, in, fft, hop);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "lab cfg %d/%d st=%.0f finite", fft, hop, st);
        TDM_CHECK(tdm_test::allFinite(out.data(), n), msg);
        if (st == 0.0f)
        {
          std::snprintf(msg, sizeof(msg), "lab cfg %d/%d 0st bit-exact", fft, hop);
          TDM_CHECK(out == in, msg);
        }
        else
        {
          std::snprintf(msg, sizeof(msg), "lab cfg %d/%d st=%.0f bounded", fft, hop, st);
          TDM_CHECK(tdm_test::peakAbs(out.data(), n) < 2.0f, msg);
        }
      }
      tdm::lab::LabPitchShift p;
      p.setConfig(fft, hop);
      p.setEnabled(true);
      p.setShiftSt(-7.0f);
      p.reset(sr);
      const double r = p.actualRatio();
      const int want = static_cast<int>((fft + 1) + (fft - hop) * (1.0 / r - 1.0) + 0.5);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "lab cfg %d/%d latency formula", fft, hop);
      TDM_CHECK(p.latencySamples() == want, msg);
    }
  }
}

#include "tests/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/lab/Pitch/LabCrossover.h"
#include "dsp/lab/Pitch/LabMultiPitch.h"
#include "dsp/lab/Pitch/LabPitchShift.h"

namespace
{
constexpr double kPi = 3.14159265358979;

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

std::vector<float> ksPluck(float peak, float freqHz, double sr, int n, float damp = 0.996f,
                           uint32_t seed = 0x51ab3u)
{
  const int period = static_cast<int>(sr / freqHz + 0.5f) > 2 ? static_cast<int>(sr / freqHz + 0.5f) : 2;
  std::vector<float> line(static_cast<size_t>(period));
  for (int i = 0; i < period; ++i)
    line[static_cast<size_t>(i)] = detNoise(i, seed);
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

// Latency-compensated offline run through LabMultiPitch.
std::vector<float> runMulti(double sr, float shiftSt, const std::vector<float>& in, int block = 256)
{
  tdm::lab::LabMultiPitch p;
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

double scanPitch(const float* v, int n, double sr, double target)
{
  double best = target, bm = -1.0;
  const double step = target * 0.002;
  for (double f = target * 0.88; f <= target * 1.12; f += step)
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

double maxStep(const float* v, int n)
{
  double m = 0.0;
  for (int i = 1; i < n; ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}
} // namespace

void runLabMultiTests()
{
  { // Crossover complementary sum: lo + hi == input delayed by D, to
    // float rounding, from the very first samples (zero pre-roll is exact
    // too). This is what makes holes/buildup structurally impossible.
    for (double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::lab::LabCrossover x;
      x.reset(sr);
      const int d = x.delaySamples();
      TDM_CHECK(d > 0 && (x.taps() & 1) == 1, "xover delay/taps sane");
      const int n = static_cast<int>(sr * 1.0);
      std::vector<float> in(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = detNoise(i) * 0.7f;
      mixInto(in, sine(0.3f, 82.41f, sr, n));
      mixInto(in, sine(0.2f, 2000.0f, sr, n));
      std::vector<float> lo(static_cast<size_t>(n)), hi(static_cast<size_t>(n));
      x.processBlock(in.data(), lo.data(), hi.data(), n);
      double worst = 0.0;
      for (int i = 0; i < n; ++i)
      {
        const double want = i < d ? 0.0 : in[static_cast<size_t>(i - d)];
        worst = std::max(worst, std::fabs(lo[static_cast<size_t>(i)] + hi[static_cast<size_t>(i)] - want));
      }
      char msg[96];
      std::snprintf(msg, sizeof(msg), "xover complementary sum sr=%.0f", sr);
      TDM_CHECK(worst < 1e-5, msg);
    }
  }
  { // Crossover split sanity: DC and 82 Hz live in low, 2 kHz in high.
    // (Rough energy placement, not a filter-spec pin.)
    const double sr = 48000.0;
    tdm::lab::LabCrossover x;
    x.reset(sr);
    const int d = x.delaySamples();
    const int n = static_cast<int>(sr * 2.0);
    std::vector<float> dc(static_cast<size_t>(n), 0.5f), lo(static_cast<size_t>(n)),
        hi(static_cast<size_t>(n));
    x.processBlock(dc.data(), lo.data(), hi.data(), n);
    TDM_CHECK(std::fabs(rms(lo, n / 2, n) - 0.5) < 1e-3, "xover DC to low");
    TDM_CHECK(rms(hi, n / 2, n) < 1e-3, "xover DC killed in high");
    x.reset(sr);
    std::vector<float> lf = sine(0.5f, 82.41f, sr, n);
    x.processBlock(lf.data(), lo.data(), hi.data(), n);
    TDM_CHECK(rms(lo, 2 * d, n) / (rms(hi, 2 * d, n) + 1e-9) > 10.0, "xover 82Hz to low");
    x.reset(sr);
    std::vector<float> hf = sine(0.5f, 2000.0f, sr, n);
    x.processBlock(hf.data(), lo.data(), hi.data(), n);
    TDM_CHECK(rms(hi, 2 * d, n) / (rms(lo, 2 * d, n) + 1e-9) > 10.0, "xover 2kHz to high");
  }
  { // Crossover edge behavior: never-reset silence, bad rate throws,
    // sample/block equivalence, reset determinism.
    tdm::lab::LabCrossover x;
    float lo = 9.0f, hi = 9.0f;
    x.processSample(0.5f, &lo, &hi);
    TDM_CHECK(lo == 0.0f && hi == 0.0f, "xover never-reset silence");
    bool threw = false;
    try
    {
      x.reset(1000.0);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "xover reset rejects bad rate");
    const double sr = 48000.0;
    const int n = 20000;
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detNoise(i) * 0.5f;
    x.reset(sr);
    std::vector<float> loB(static_cast<size_t>(n)), hiB(static_cast<size_t>(n));
    x.processBlock(in.data(), loB.data(), hiB.data(), n);
    x.reset(sr);
    std::vector<float> loS(static_cast<size_t>(n)), hiS(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      x.processSample(in[static_cast<size_t>(i)], &loS[static_cast<size_t>(i)], &hiS[static_cast<size_t>(i)]);
    TDM_CHECK(loB == loS && hiB == hiS, "xover sample/block identical");
    x.reset(sr);
    std::vector<float> lo2(static_cast<size_t>(n)), hi2(static_cast<size_t>(n));
    x.processBlock(in.data(), lo2.data(), hi2.data(), n);
    TDM_CHECK(loB == lo2 && hiB == hi2, "xover reset deterministic");
  }
  { // Multi exact 0-st bypass: bit-identical (whole-processor copy, the
    // crossover is skipped too), latency/tail 0. -0.1 st still processes.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detNoise(i) * 0.6f;
    mixInto(in, ksPluck(0.4f, 82.41f, sr, n));
    TDM_CHECK(runMulti(sr, 0.0f, in) == in, "multi 0st offline bit-exact");
    tdm::lab::LabMultiPitch p;
    p.setEnabled(true);
    p.setShiftSt(0.0f);
    p.reset(sr);
    TDM_CHECK(p.latencySamples() == 0, "multi 0st latency 0");
    TDM_CHECK(p.tailSamples() == 0, "multi 0st tail 0");
    std::vector<float> out(static_cast<size_t>(n), 9.0f);
    p.processBlock(in.data(), out.data(), n);
    TDM_CHECK(out == in, "multi 0st streaming bit-exact");
    std::vector<float> ip = in;
    p.processBlock(ip.data(), ip.data(), n);
    TDM_CHECK(ip == in, "multi 0st in-place bit-exact");
    std::vector<float> sm = runMulti(sr, -0.1f, in);
    TDM_CHECK(tdm_test::allFinite(sm.data(), n), "multi -0.1st finite");
    TDM_CHECK(!(sm == in), "multi -0.1st not bypassed");
  }
  { // Multi latency contract vs INDEPENDENT single-res instances: total
    // == crossoverD + low(4096/1024) latency, align == low - high(2048/256).
    for (float st : {-1.0f, -2.0f, -7.0f, -12.0f})
    {
      tdm::lab::LabMultiPitch m;
      m.setEnabled(true);
      m.setShiftSt(st);
      m.reset(48000.0);
      tdm::lab::LabPitchShift lo, hi;
      lo.setConfig(4096, 1024);
      hi.setConfig(2048, 256);
      lo.setShiftSt(st);
      hi.setShiftSt(st);
      lo.reset(48000.0);
      hi.reset(48000.0);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "multi latency contract st=%.0f", st);
      TDM_CHECK(m.latencySamples() == m.crossoverDelay() + lo.latencySamples(), msg);
      std::snprintf(msg, sizeof(msg), "multi align contract st=%.0f", st);
      TDM_CHECK(m.alignDelay() == lo.latencySamples() - hi.latencySamples(), msg);
      TDM_CHECK(m.tailSamples() == lo.tailSamples(), "multi tail == low tail");
    }
  }
  { // Silence in -> silence out (exact); never-reset silence; bad rate throws.
    std::vector<float> z(48000, 0.0f);
    TDM_CHECK(tdm_test::peakAbs(runMulti(48000.0, -7.0f, z).data(), 48000) == 0.0f,
              "multi silence exact");
    tdm::lab::LabMultiPitch p;
    p.setEnabled(true);
    std::vector<float> out(1024, 9.0f), in(1024, 0.5f);
    p.processBlock(in.data(), out.data(), 1024);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 1024) == 0.0f, "multi never-reset silence");
    bool threw = false;
    try
    {
      p.reset(1000.0);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "multi reset rejects bad rate");
  }
  for (double sr : {44100.0, 48000.0, 96000.0})
  { // Finite/bounded at every shift and rate; deterministic across
    // resets; identical across block sizes.
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = ksPluck(0.5f, 82.41f, sr, n);
    mixInto(in, sine(0.3f, 440.0f, sr, n));
    for (float st : {0.0f, -1.0f, -2.0f, -7.0f, -12.0f})
    {
      std::vector<float> out = runMulti(sr, st, in);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "multi sr=%.0f st=%.0f finite", sr, st);
      TDM_CHECK(tdm_test::allFinite(out.data(), n), msg);
      std::snprintf(msg, sizeof(msg), "multi sr=%.0f st=%.0f bounded", sr, st);
      TDM_CHECK(tdm_test::peakAbs(out.data(), n) < 2.0f, msg);
    }
    std::vector<float> r1 = runMulti(sr, -7.0f, in, 64);
    std::vector<float> r2 = runMulti(sr, -7.0f, in, 2048);
    std::vector<float> r3 = runMulti(sr, -7.0f, in, 64);
    TDM_CHECK(r1 == r2, "multi block-size deterministic");
    TDM_CHECK(r1 == r3, "multi reset deterministic");
  }
  { // Stable reconstruction: sustained sine keeps level and pitch;
    // power-chord root+fifth survive at -7.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.5);
    std::vector<float> s = sine(0.5f, 220.0f, sr, n);
    std::vector<float> os = runMulti(sr, -7.0f, s);
    const double lr = rms(os, n / 2, n) / rms(s, n / 2, n);
    TDM_CHECK(lr > 0.85 && lr < 1.05, "multi sine steady level -7");
    const double r7 = std::exp2(-7.0 / 12.0);
    const double fE = scanPitch(os.data() + n / 2, n / 4, sr, 220.0 * r7);
    TDM_CHECK(std::fabs(fE - 220.0 * r7) / (220.0 * r7) < 0.03, "multi sine pitch -7");
    std::vector<float> e2 = sine(0.5f, 82.41f, sr, n);
    std::vector<float> oe = runMulti(sr, -12.0f, e2);
    const double r12 = 0.5;
    const double fB = scanPitch(oe.data() + n / 2, n / 4, sr, 82.41 * r12);
    TDM_CHECK(std::fabs(fB - 82.41 * r12) / (82.41 * r12) < 0.03, "multi low-E pitch -12");
    std::vector<float> chord(static_cast<size_t>(n), 0.0f);
    for (float f : {82.41f, 123.47f, 164.81f})
      mixInto(chord, sine(0.3f, f, sr, n));
    std::vector<float> oc = runMulti(sr, -7.0f, chord);
    const int off = n / 3, nf = static_cast<int>(sr * 0.3);
    for (float f : {82.41f, 123.47f, 164.81f})
    {
      const double want = goertzel(oc.data() + off, nf, sr, f * r7);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "multi chord partial f=%.1f present", f);
      TDM_CHECK(want > 0.05, msg);
    }
  }
  { // Continuity: no sample-step clicks on sustained material; impulse
    // stays bounded and confined; tail convention leaves no truncation
    // click at the end of a decaying pluck.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    for (float st : {-1.0f, -7.0f, -12.0f})
    {
      std::vector<float> out = runMulti(sr, st, sine(0.5f, 220.0f, sr, n));
      char msg[64];
      std::snprintf(msg, sizeof(msg), "multi continuity st=%.0f", st);
      TDM_CHECK(maxStep(out.data() + n / 4, n - n / 4) < 0.1, msg);
    }
    std::vector<float> imp(static_cast<size_t>(n), 0.0f);
    imp[static_cast<size_t>(n / 2)] = 1.0f;
    std::vector<float> oi = runMulti(sr, -12.0f, imp);
    TDM_CHECK(tdm_test::allFinite(oi.data(), n), "multi click finite");
    TDM_CHECK(tdm_test::peakAbs(oi.data(), n) < 1.5f, "multi click bounded");
    float far = 0.0f;
    for (int i = 0; i < n / 2 - 24576; ++i)
      far = std::max(far, std::fabs(oi[static_cast<size_t>(i)]));
    for (int i = n / 2 + 24576; i < n; ++i)
      far = std::max(far, std::fabs(oi[static_cast<size_t>(i)]));
    TDM_CHECK(far < 0.02f, "multi click confined");
    const int nd = static_cast<int>(sr * 2.0);
    std::vector<float> dec = runMulti(sr, -12.0f, ksPluck(0.6f, 110.0f, sr, nd));
    TDM_CHECK(tdm_test::allFinite(dec.data() + nd - 2000, 2000), "multi tail finite");
    TDM_CHECK(maxStep(dec.data() + nd - 2000, 2000) < 0.15, "multi tail no truncation click");
  }
}

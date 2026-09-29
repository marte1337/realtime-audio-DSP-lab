#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/lab/Pitch/LabWsolaShift.h"
#include "dsp/lab/Pitch/LabWsolaV2.h"

namespace
{
constexpr double kSr = 48000.0;

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
}

std::vector<float> ksPluck(float peak, float freqHz, int n, float damp, uint32_t seed)
{
  const int period = static_cast<int>(kSr / freqHz + 0.5f) > 2 ? static_cast<int>(kSr / freqHz + 0.5f) : 2;
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

template <typename P> std::vector<float> runThrough(P& p, const std::vector<float>& in, int block = 256)
{
  const int lat = p.latencySamples();
  const int tail = p.tailSamples();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + lat + tail;
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 0.0f);
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

std::vector<float> runV2(tdm::lab::LabWsolaV2::Mode mode, float shiftSt, const std::vector<float>& in,
                        int tolM = 0, int tolP = 0, int block = 256)
{
  tdm::lab::LabWsolaV2 p;
  p.setConfig(20.0);
  if (tolM != 0 || tolP != 0)
    p.setSearch(tolM, tolP);
  p.setMode(mode);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  return runThrough(p, in, block);
}

std::vector<float> runRef(float shiftSt, const std::vector<float>& in, int tolM = 0, int tolP = 0)
{
  tdm::lab::LabWsolaShift p;
  p.setConfig(20.0);
  if (tolM != 0 || tolP != 0)
    p.setSearch(tolM, tolP);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  return runThrough(p, in);
}

double goertzel(const float* v, int n, double f)
{
  const double w = 2.0 * 3.14159265358979 * f / kSr;
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

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
  return worst;
}
} // namespace

void runLabWsolaV2Tests()
{
  namespace wv2 = tdm::lab;
  using Mode = wv2::LabWsolaV2::Mode;

  const int nMix = static_cast<int>(kSr * 2.0);
  std::vector<float> mixture = ksPluck(0.4f, 82.41f, nMix, 0.996f, 0x51ab3u);
  {
    std::vector<float> b1 = ksPluck(0.3f, 61.74f, nMix, 0.996f, 0x2222u);
    std::vector<float> e3 = ksPluck(0.25f, 164.81f, nMix, 0.996f, 0x3333u);
    for (int i = 0; i < nMix; ++i)
      mixture[static_cast<size_t>(i)] += b1[static_cast<size_t>(i)] + e3[static_cast<size_t>(i)] +
          0.1f * detNoise(i, 0x77u);
  }

  { // Drift mode is bit-identical to LabWsolaShift: default symmetric,
    // a latency-study geometry, and the E2 boundary geometry (840,120).
    // The (840,120) proof is the audition-transfer proof: study data
    // measured on the fork applies bit-exactly to the accepted class.
    for (float st : {-1.0f, -7.0f})
      for (auto [tm, tp] : {std::make_pair(0, 0), std::make_pair(640, 320), std::make_pair(840, 120)})
      {
        const std::vector<float> a = runV2(Mode::Drift, st, mixture, tm, tp);
        const std::vector<float> ref = runRef(st, mixture, tm, tp);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "e2 Drift bit-identical st=%.0f %d:%d", st, tm, tp);
        TDM_CHECK(maxAbsDiff(a, ref) == 0.0, msg);
        TDM_CHECK(tdm_test::allFinite(a.data(), static_cast<int>(a.size())), "e2 Drift finite");
      }
  }
  { // setSearch validation: (Dm>=1, 0) causal is admitted; half-default
    // (0, Dp>0) and negatives still throw; range faults throw at reset.
    tdm::lab::LabWsolaV2 p;
    p.setConfig(20.0);
    p.setSearch(960, 0); // causal: must not throw
    p.setShiftSt(-1.0f);
    p.reset(kSr);
    TDM_CHECK(p.tolMinus() == 960 && p.tolPlus() == 0, "e2 causal search admitted");
    bool threw = false;
    try
    {
      p.setSearch(0, 320);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "e2 setSearch rejects half-default");
    threw = false;
    try
    {
      p.setSearch(960, -1);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "e2 setSearch rejects negative");
    threw = false;
    try
    {
      p.setSearch(2000, 0);
      p.reset(kSr); // Dm > W: must throw
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "e2 reset rejects Dm > W");
    threw = false;
    try
    {
      p.setSearch(960, 961);
      p.reset(kSr); // Dp > W: must throw
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "e2 reset rejects Dp > W");
  }
  { // Latency pins for the causal + boundary geometries (W + Dp + C).
    struct Pin
    {
      int tm, tp;
      float st;
      int lat;
    };
    const Pin pins[] = {
        {960, 0, -1.0f, 963}, {960, 0, -2.0f, 963}, {960, 0, -7.0f, 964},
        {840, 120, -1.0f, 1083}, {840, 120, -2.0f, 1083}, {840, 120, -7.0f, 1084},
    };
    for (const Pin& q : pins)
    {
      tdm::lab::LabWsolaV2 p;
      p.setConfig(20.0);
      p.setSearch(q.tm, q.tp);
      p.setShiftSt(q.st);
      p.reset(kSr);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "e2 latency pin %d:%d st=%.0f", q.tm, q.tp, q.st);
      TDM_CHECK(p.latencySamples() == q.lat, msg);
    }
    // E2 geometries match LabWsolaShift where both admit them (audition
    // transfer for E; B is fork-only by construction).
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      tdm::lab::LabWsolaV2 p;
      p.setConfig(20.0);
      p.setSearch(840, 120);
      p.setShiftSt(st);
      p.reset(kSr);
      tdm::lab::LabWsolaShift ref;
      ref.setConfig(20.0);
      ref.setSearch(840, 120);
      ref.setShiftSt(st);
      ref.reset(kSr);
      TDM_CHECK(p.latencySamples() == ref.latencySamples(), "e2 E latency matches accepted class");
    }
  }
  { // No-starve: DC must never read near zero past startup at the causal
    // and boundary geometries (the margin proof reads Dp; this pins Dp=0
    // and Dp=120 on the grid).
    for (auto [tm, tp] : {std::make_pair(960, 0), std::make_pair(840, 120)})
      for (float st : {-1.0f, -2.0f, -7.0f})
      {
        const int n = 48000;
        std::vector<float> dc(static_cast<size_t>(n), 0.5f);
        std::vector<float> o = runV2(Mode::Drift, st, dc, tm, tp);
        float worst = 0.5f;
        for (int i = n / 2; i < n; ++i)
          worst = std::min(worst, o[static_cast<size_t>(i)]);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "e2 no-starve %d:%d st=%.0f", tm, tp, st);
        TDM_CHECK(worst > 0.49f, msg);
      }
  }
  { // Silence exact at the causal geometry (Dp=0 schedule guard).
    std::vector<float> z(48000, 0.0f);
    std::vector<float> o = runV2(Mode::Drift, -7.0f, z, 960, 0);
    TDM_CHECK(tdm_test::peakAbs(o.data(), 48000) == 0.0f, "e2 causal silence exact");
  }
  { // SmallSkip low-sine dry-climb (E2 FINDING, pinned to document the
    // rejection): near-peg lags score within band on low pure tones, so
    // minimal-jump landings peg-climb at J~=1 and the output is dry
    // (measured +6.2%, predicted +6.22%). Drift shifts cleanly.
    const int n = static_cast<int>(kSr * 1.0);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      s[static_cast<size_t>(i)] =
          0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 82.41 * i / kSr));
    const std::vector<float> c = runV2(Mode::SmallSkip, -1.0f, s);
    const std::vector<float> a = runV2(Mode::Drift, -1.0f, s);
    const int off = static_cast<int>(kSr * 0.2), nf = static_cast<int>(kSr * 0.7);
    const double dryMag = goertzel(s.data() + off, nf, 82.41);
    const double cDry = goertzel(c.data() + off, nf, 82.41);
    const double aDry = goertzel(a.data() + off, nf, 82.41);
    TDM_CHECK(cDry > 0.9 * dryMag, "e2 smallskip low-sine dry-climb pinned");
    TDM_CHECK(aDry < 0.1 * dryMag, "e2 drift low-sine no dry-through");
  }
  { // SmallSkip engages, stays finite/bounded/deterministic; bypasses exact.
    const std::vector<float> c = runV2(Mode::SmallSkip, -1.0f, mixture);
    const std::vector<float> a = runV2(Mode::Drift, -1.0f, mixture);
    TDM_CHECK(tdm_test::allFinite(c.data(), static_cast<int>(c.size())), "e2 smallskip finite");
    TDM_CHECK(tdm_test::peakAbs(c.data(), static_cast<int>(c.size())) < 2.0f, "e2 smallskip bounded");
    TDM_CHECK(maxAbsDiff(c, a) > 1e-6, "e2 smallskip differs from drift");
    const std::vector<float> c64 = runV2(Mode::SmallSkip, -7.0f, mixture, 960, 0, 64);
    const std::vector<float> c2k = runV2(Mode::SmallSkip, -7.0f, mixture, 960, 0, 2048);
    const std::vector<float> c64b = runV2(Mode::SmallSkip, -7.0f, mixture, 960, 0, 64);
    TDM_CHECK(c64 == c2k, "e2 block-size deterministic");
    TDM_CHECK(c64 == c64b, "e2 reset deterministic");
    tdm::lab::LabWsolaV2 p;
    p.setConfig(20.0);
    p.setSearch(960, 0);
    p.setMode(Mode::SmallSkip);
    p.setEnabled(true);
    p.setShiftSt(0.0f);
    p.reset(kSr);
    TDM_CHECK(p.latencySamples() == 0, "e2 bypass-0 zero latency");
    const std::vector<float> b0 = runThrough(p, mixture);
    TDM_CHECK(maxAbsDiff(b0, mixture) == 0.0, "e2 bypass-0 bit-exact");
  }
}

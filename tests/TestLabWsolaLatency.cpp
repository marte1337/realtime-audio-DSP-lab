#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/lab/Pitch/LabWsolaShift.h"

namespace
{
// Latency-study geometries (samples @48 kHz design rate; valid at any
// rate where Dm/Dp <= W): a = the robust winner (span 960, Dp 320),
// m = the Dp-floor probe (span 960, Dp 280, slips one -2 fund).
struct StudyGeom
{
  const char* id;
  double wms;
  int tolM, tolP;
};
const StudyGeom kGeoms[] = {{"a", 20.0, 640, 320}, {"m", 20.0, 680, 280}};

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
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

// Latency-compensated offline run with optional asymmetric search.
std::vector<float> runGeom(double sr, float shiftSt, const std::vector<float>& in, double wms, int tolM,
                          int tolP, int block = 256)
{
  tdm::lab::LabWsolaShift p;
  p.setConfig(wms);
  if (tolM != 0 || tolP != 0)
    p.setSearch(tolM, tolP);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(sr);
  const int lat = p.latencySamples();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + lat + p.tailSamples();
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
} // namespace

void runLabWsolaLatencyTests()
{
  { // setSearch validation.
    tdm::lab::LabWsolaShift p;
    bool threw = false;
    try
    {
      p.setSearch(0, 320);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "latency setSearch rejects half-default");
    threw = false;
    try
    {
      p.setSearch(-5, 320);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "latency setSearch rejects negative");
    threw = false;
    try
    {
      p.setConfig(20.0);
      p.setSearch(640, 320);
      p.setShiftSt(-1.0f);
      p.reset(48000.0); // study geometry a: must not throw
      p.setSearch(2000, 320);
      p.reset(48000.0); // Dm > W: must throw
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "latency reset rejects Dm > W");
    p.setSearch(0, 0); // back to default: must not throw
    p.reset(48000.0);
    TDM_CHECK(p.tolMinus() == 480 && p.tolPlus() == 480, "latency (0,0) means symmetric W/2");
  }
  { // Explicit symmetric == default, bitwise (setSearch plumbing guard).
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = ksPluck(0.4f, 82.41f, sr, n);
    std::vector<float> a = runGeom(sr, -7.0f, in, 20.0, 0, 0);
    std::vector<float> b = runGeom(sr, -7.0f, in, 20.0, 480, 480);
    TDM_CHECK(a == b, "latency explicit symmetric bitwise-identical");
  }
  { // Candidate latency pins @48 kHz (hand-derived W + Dp + C).
    struct Pin
    {
      const char* id;
      float st;
      int lat;
    };
    const Pin pins[] = {
        {"a", -1.0f, 1283}, {"a", -2.0f, 1283}, {"a", -7.0f, 1284},
        {"m", -1.0f, 1243}, {"m", -2.0f, 1243}, {"m", -7.0f, 1244},
    };
    for (const StudyGeom& g : kGeoms)
      for (const Pin& q : pins)
      {
        if (std::string(g.id) != q.id)
          continue;
        tdm::lab::LabWsolaShift p;
        p.setConfig(g.wms);
        p.setSearch(g.tolM, g.tolP);
        p.setShiftSt(q.st);
        p.reset(48000.0);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "latency pin %s st=%.0f", g.id, q.st);
        TDM_CHECK(p.latencySamples() == q.lat, msg);
      }
    // General shape W + Dp + C across rates (tols valid wherever <= W).
    for (double sr : {44100.0, 48000.0, 96000.0})
      for (const StudyGeom& g : kGeoms)
        for (float st : {-1.0f, -2.0f, -7.0f})
        {
          tdm::lab::LabWsolaShift p;
          p.setConfig(g.wms);
          p.setSearch(g.tolM, g.tolP);
          p.setShiftSt(st);
          p.reset(sr);
          const int want = p.frameLen() + p.tolPlus() +
              static_cast<int>(std::ceil(3.0 / p.actualRatio() - 1.0));
          char msg[128];
          std::snprintf(msg, sizeof(msg), "latency shape %s sr=%.0f st=%.0f", g.id, sr, st);
          TDM_CHECK(p.latencySamples() == want, msg);
        }
  }
  { // No-starve spot grid for the study geometries (DC must never read
    // near zero past startup; the margin proof covers Dp, this pins it).
    for (const StudyGeom& g : kGeoms)
      for (float st : {-1.0f, -2.0f, -7.0f})
      {
        const int n = 48000;
        std::vector<float> dc(static_cast<size_t>(n), 0.5f);
        std::vector<float> o = runGeom(48000.0, st, dc, g.wms, g.tolM, g.tolP);
        float worst = 0.5f;
        for (int i = n / 2; i < n; ++i)
          worst = std::min(worst, o[static_cast<size_t>(i)]);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "latency no-starve %s st=%.0f", g.id, st);
        TDM_CHECK(worst > 0.49f, msg);
      }
  }
  { // Finite/bounded on a guitar-like mixture at every audition shift.
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = ksPluck(0.4f, 82.41f, sr, n);
    for (size_t i = 0; i < in.size(); ++i)
      in[i] += 0.3f * detNoise(static_cast<int>(i), 0x77u);
    for (const StudyGeom& g : kGeoms)
      for (float st : {-1.0f, -2.0f, -7.0f})
      {
        std::vector<float> o = runGeom(sr, st, in, g.wms, g.tolM, g.tolP);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "latency %s st=%.0f finite", g.id, st);
        TDM_CHECK(tdm_test::allFinite(o.data(), n), msg);
        std::snprintf(msg, sizeof(msg), "latency %s st=%.0f bounded", g.id, st);
        TDM_CHECK(tdm_test::peakAbs(o.data(), n) < 2.0f, msg);
      }
  }
  { // Transient-floor regression: with Dm > Lov the unfloored outright
    // max jumps back to attack-excluding sustain lags and deletes pick
    // transients (measured on -1 chugs: first peaks gone, +15 ms late
    // string peaks). Chug-like stimulus: damped 8th re-strikes with a
    // 1 ms pick burst, each emerging from the previous note's decay so
    // the detector flags it (a lone impulse on loud sustain is
    // detector-invisible and would make this test vacuous - probed).
    // The stimulus replicates the DI's chug section (sustained E2s +
    // damped 8th re-strikes with pick bursts at 140 bpm): short toy
    // strikes do NOT reproduce it (the drift-run phase at the strike
    // decides - probed: 1.2 s toys pass either way). Chugs 2-8 (chug 1
    // is always clean) must survive ref-relative. Verified to FAIL
    // with the floor removed (first peaks ~0.1x, +15 ms late).
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 6.0);
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    auto place = [&](const std::vector<float>& note, double at, float gain) {
      const int s0 = static_cast<int>(sr * at);
      for (size_t i = 0; i < note.size() && s0 + static_cast<int>(i) < n; ++i)
        in[static_cast<size_t>(s0) + i] += gain * note[i];
    };
    place(ksPluck(1.0f, 82.41f, sr, static_cast<int>(sr * 1.2), 0.996f, 0x101u), 0.3, 0.55f);
    place(ksPluck(1.0f, 82.41f, sr, static_cast<int>(sr * 1.2), 0.996f, 0x102u), 1.4, 0.55f);
    place(ksPluck(1.0f, 82.41f, sr, static_cast<int>(sr * 1.2), 0.996f, 0x103u), 2.5, 0.55f);
    const double chugStep = 60.0 / 140.0 / 2.0;
    for (int k = 0; k < 8; ++k)
    {
      const double at = 4.0 + k * chugStep;
      place(ksPluck(1.0f, 82.41f, sr, static_cast<int>(sr * 0.3), 0.94f, 0x201u + k), at, 0.6f);
      const int s0 = static_cast<int>(sr * at);
      const int cn = static_cast<int>(sr * 0.0012);
      for (int i = 0; i < cn && s0 + i < n; ++i)
      {
        const float w = 1.0f - static_cast<float>(i) / cn;
        in[static_cast<size_t>(s0) + i] += detNoise(i, 0x301u + k) * 0.7f * w * 0.5f * 0.6f;
      }
    }
    std::vector<float> ref = runGeom(sr, -1.0f, in, 20.0, 0, 0);
    auto chugPeak = [&](const std::vector<float>& v, double at) {
      const int c = static_cast<int>(sr * at);
      const int a = c - static_cast<int>(sr * 0.002);
      const int b = c + static_cast<int>(sr * 0.012);
      double best = 0.0;
      int pos = a;
      for (int i = a + 1; i < b; ++i)
      {
        const double s = std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)]);
        if (s > best)
        {
          best = s;
          pos = i;
        }
      }
      return std::make_pair(best, pos);
    };
    for (const StudyGeom& g : kGeoms)
    {
      std::vector<float> o = runGeom(sr, -1.0f, in, g.wms, g.tolM, g.tolP);
      double worstRatio = 1e300;
      int worstPos = 0;
      for (int k = 1; k < 8; ++k) // chugs 2-8 (chug 1 is always clean)
      {
        const auto rp = chugPeak(ref, 4.0 + k * chugStep);
        const auto cp = chugPeak(o, 4.0 + k * chugStep);
        worstRatio = std::min(worstRatio, cp.first / rp.first);
        worstPos = std::max(worstPos, std::abs(cp.second - rp.second));
      }
      char msg[128];
      std::snprintf(msg, sizeof(msg), "latency %s chug picks preserved", g.id);
      TDM_CHECK(worstRatio >= 0.7, msg);
      std::snprintf(msg, sizeof(msg), "latency %s chug picks on time", g.id);
      TDM_CHECK(worstPos <= static_cast<int>(sr * 0.012), msg);
    }
  }
}

#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/lab/Pitch/LabPitchShift.h"
#include "dsp/lab/Pitch/LabPitchV2.h"

namespace
{
constexpr double kSr = 48000.0;

// Deterministic LCG in [-1, 1].
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

// Pluck struck mid-file over silence: guaranteed flux transient.
std::vector<float> pluckStimulus()
{
  const int n = static_cast<int>(kSr * 2);
  std::vector<float> in(static_cast<size_t>(n), 0.0f);
  const std::vector<float> pluck = ksPluck(0.7f, 82.41f, static_cast<int>(kSr * 1), 0.996f, 0x51ab3u);
  const int at = static_cast<int>(kSr * 0.5);
  for (size_t i = 0; i < pluck.size(); ++i)
    in[static_cast<size_t>(at) + i] = pluck[i];
  return in;
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

std::vector<float> runV2(tdm::lab::LabPitchV2::Mode mode, float shiftSt, const std::vector<float>& in)
{
  tdm::lab::LabPitchV2 p;
  p.setConfig(2048, 256);
  p.setMode(mode);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  return runThrough(p, in);
}

std::vector<float> runPvd(float shiftSt, const std::vector<float>& in)
{
  tdm::lab::LabPitchShift p;
  p.setConfig(2048, 256);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  return runThrough(p, in);
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
  return worst;
}

} // namespace

void runLabPV2Tests()
{
  namespace pv2 = tdm::lab;
  const pv2::LabPitchV2::Mode modes[] = {
      pv2::LabPitchV2::Mode::A_Baseline, pv2::LabPitchV2::Mode::B_FreqReset,
      pv2::LabPitchV2::Mode::C_TimeAnchor, pv2::LabPitchV2::Mode::D_Both,
  };
  const std::vector<float> stim = pluckStimulus();

  { // Mode A is bit-identical to LabPitchShift at PV-D geometry.
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      const std::vector<float> a = runV2(pv2::LabPitchV2::Mode::A_Baseline, st, stim);
      const std::vector<float> ref = runPvd(st, stim);
      TDM_CHECK(maxAbsDiff(a, ref) == 0.0, "pv2 A bit-identical to PV-D");
      TDM_CHECK(tdm_test::allFinite(a.data(), static_cast<int>(a.size())), "pv2 A finite");
    }
  }
  { // Latency identical across modes and equal to PV-D.
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      tdm::lab::LabPitchShift ref;
      ref.setConfig(2048, 256);
      ref.setShiftSt(st);
      ref.reset(kSr);
      for (auto m : modes)
      {
        tdm::lab::LabPitchV2 p;
        p.setConfig(2048, 256);
        p.setMode(m);
        p.setShiftSt(st);
        p.reset(kSr);
        TDM_CHECK(p.latencySamples() == ref.latencySamples(), "pv2 latency matches PV-D");
        TDM_CHECK(p.tailSamples() == ref.tailSamples(), "pv2 tail matches PV-D");
      }
    }
    tdm::lab::LabPitchV2 p;
    p.setConfig(2048, 256);
    p.setShiftSt(-1.0f);
    p.reset(kSr);
    TDM_CHECK(p.latencySamples() == 2153, "pv2 latency -1 exact");
    p.setShiftSt(-2.0f);
    p.reset(kSr);
    TDM_CHECK(p.latencySamples() == 2269, "pv2 latency -2 exact");
    p.setShiftSt(-7.0f);
    p.reset(kSr);
    TDM_CHECK(p.latencySamples() == 2940, "pv2 latency -7 exact");
  }
  { // B/C/D engage on transient material, stay finite and bounded.
    const std::vector<float> a = runV2(pv2::LabPitchV2::Mode::A_Baseline, -1.0f, stim);
    for (auto m : {pv2::LabPitchV2::Mode::B_FreqReset, pv2::LabPitchV2::Mode::C_TimeAnchor,
                   pv2::LabPitchV2::Mode::D_Both})
    {
      const std::vector<float> v = runV2(m, -1.0f, stim);
      TDM_CHECK(tdm_test::allFinite(v.data(), static_cast<int>(v.size())), "pv2 variant finite");
      TDM_CHECK(maxAbsDiff(v, a) > 1e-6, "pv2 variant differs from A on pluck");
      TDM_CHECK(tdm_test::peakAbs(v.data(), static_cast<int>(v.size())) < 4.0f, "pv2 variant bounded");
    }
  }
  { // C touches transient frames only: the steady-state middle of a
    // sustained sine equals A bit-exactly (start fade-in and abrupt
    // file end are genuine detector transients, excluded by windowing
    // the middle 50%).
    const int n = static_cast<int>(kSr * 4);
    std::vector<float> sine(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      sine[static_cast<size_t>(i)] =
          0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * i / kSr));
    const std::vector<float> a = runV2(pv2::LabPitchV2::Mode::A_Baseline, -1.0f, sine);
    const std::vector<float> c = runV2(pv2::LabPitchV2::Mode::C_TimeAnchor, -1.0f, sine);
    double worst = 0.0;
    for (size_t i = a.size() / 4; i < a.size() * 3 / 4; ++i)
      worst = std::max(worst, static_cast<double>(std::fabs(a[i] - c[i])));
    TDM_CHECK(worst == 0.0, "pv2 C steady-state middle equals A");
  }
  { // Determinism, bypass-0, and disabled bypass.
    const std::vector<float> v1 = runV2(pv2::LabPitchV2::Mode::D_Both, -2.0f, stim);
    const std::vector<float> v2 = runV2(pv2::LabPitchV2::Mode::D_Both, -2.0f, stim);
    TDM_CHECK(maxAbsDiff(v1, v2) == 0.0, "pv2 reset determinism");
    tdm::lab::LabPitchV2 p;
    p.setConfig(2048, 256);
    p.setMode(pv2::LabPitchV2::Mode::C_TimeAnchor);
    p.setEnabled(true);
    p.setShiftSt(0.0f);
    p.reset(kSr);
    TDM_CHECK(p.latencySamples() == 0, "pv2 bypass-0 zero latency");
    const std::vector<float> b0 = runThrough(p, stim);
    TDM_CHECK(maxAbsDiff(b0, stim) == 0.0, "pv2 bypass-0 bit-exact");
    p.setShiftSt(-7.0f);
    p.setEnabled(false);
    p.reset(kSr);
    // Disabled bypass copies sample-for-sample with no latency shift
    // (latencySamples still reports the formula value, as in PV-D, so
    // compensate nothing here).
    std::vector<float> dis(stim.size(), 0.0f);
    const int dn = static_cast<int>(stim.size());
    for (int off = 0; off < dn; off += 256)
    {
      const int m = (dn - off) < 256 ? (dn - off) : 256;
      p.processBlock(stim.data() + off, dis.data() + off, m);
    }
    TDM_CHECK(maxAbsDiff(dis, stim) == 0.0, "pv2 disabled bypass bit-exact");
  }
}

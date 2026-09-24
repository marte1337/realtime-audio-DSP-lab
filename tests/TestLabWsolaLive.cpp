#include "tests/Assert.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/lab/Pitch/LabWsolaLive.h"
#include "dsp/lab/Pitch/LabWsolaShift.h"

namespace
{
constexpr double kPi = 3.14159265358979;

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / sr));
  return out;
}

// Deterministic noise-ish stimulus (no RNG Run-to-run drift: pure function
// of the index), loud enough that any hard bypass switch would jump.
float detLoud(int i)
{
  const double t = i * 0.0137;
  return static_cast<float>(0.7 * std::sin(t) + 0.3 * std::sin(t * 2.73 + 1.0));
}

std::vector<float> runLive(double sr, float shiftSt, const std::vector<float>& in, bool startEnabled,
                           int block, int toggleAt = -1)
{
  tdm::lab::LabWsolaLive w;
  w.prepare(sr, shiftSt, startEnabled, 2048);
  const int n = static_cast<int>(in.size());
  std::vector<float> out(static_cast<size_t>(n));
  for (int off = 0; off < n; off += block)
  {
    if (toggleAt >= 0 && off + block > toggleAt && off <= toggleAt)
    {
      // Toggle exactly at toggleAt: split the straddling block so the
      // request lands on the same sample regardless of block size.
      const int m0 = toggleAt - off;
      if (m0 > 0)
        w.processBlock(in.data() + off, out.data() + off, m0);
      w.setEnabled(!startEnabled);
      const int m1 = (n - off - m0) < (block - m0) ? (n - off - m0) : (block - m0);
      if (m1 > 0)
        w.processBlock(in.data() + off + m0, out.data() + off + m0, m1);
      continue;
    }
    const int m = (n - off) < block ? (n - off) : block;
    w.processBlock(in.data() + off, out.data() + off, m);
  }
  return out;
}

std::vector<float> runShifter(double sr, float shiftSt, const std::vector<float>& in, int block)
{
  tdm::lab::LabWsolaShift p;
  p.setConfig(20.0);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(sr);
  const int n = static_cast<int>(in.size());
  std::vector<float> out(static_cast<size_t>(n));
  for (int off = 0; off < n; off += block)
  {
    const int m = (n - off) < block ? (n - off) : block;
    p.processBlock(in.data() + off, out.data() + off, m);
  }
  return out;
}

double maxStep(const float* v, int from, int to)
{
  double m = 0.0;
  for (int i = from + 1; i < to; ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}
} // namespace

void runLabWsolaLiveTests()
{
  const double sr = 48000.0;
  { // prepare() validation.
    tdm::lab::LabWsolaLive w;
    bool threw = false;
    try
    {
      w.prepare(sr, -12.0f, true, 2048);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "labwsolalive reject shift below -7");
    threw = false;
    try
    {
      w.prepare(sr, -2.0f, true, 0);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "labwsolalive reject non-positive maxBlock");
    threw = false;
    try
    {
      w.prepare(1000.0, -2.0f, true, 2048);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "labwsolalive reject bad rate");
    // Never-prepared processing is silence, never garbage/crash.
    std::vector<float> in(512, 0.5f), out(512, 9.0f);
    w.processBlock(in.data(), out.data(), 512);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 512) == 0.0f, "labwsolalive never-prepared silence");
  }
  { // Latency reporting: wrapper exposes exactly the shifter latency
    // (W + D + C) at every audition rate; both paths carry it.
    for (double r : {44100.0, 48000.0, 96000.0})
      for (float st : {0.0f, -1.0f, -2.0f, -7.0f})
      {
        tdm::lab::LabWsolaShift p;
        p.setConfig(20.0);
        p.setEnabled(true);
        p.setShiftSt(st);
        p.reset(r);
        tdm::lab::LabWsolaLive w;
        w.prepare(r, st, true, 2048);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "labwsolalive latency sr=%.0f st=%.0f", r, st);
        TDM_CHECK(w.latencySamples() == p.latencySamples(), msg);
      }
  }
  { // Disabled steady-state == input delayed by exactly L (constant-
    // latency bypass); enabled steady-state == standalone shifter.
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detLoud(i);
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      tdm::lab::LabWsolaLive probe;
      probe.prepare(sr, st, true, 2048);
      const int lat = probe.latencySamples();
      std::vector<float> off = runLive(sr, st, in, false, 256);
      bool exact = true;
      for (int i = lat; i < n; ++i)
        if (off[static_cast<size_t>(i)] != in[static_cast<size_t>(i - lat)])
        {
          exact = false;
          break;
        }
      char msg[128];
      std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f bypass exact delay-L", st);
      TDM_CHECK(exact, msg);
      std::vector<float> on = runLive(sr, st, in, true, 256);
      std::vector<float> ref = runShifter(sr, st, in, 256);
      std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f enabled equals shifter", st);
      TDM_CHECK(on == ref, msg);
    }
  }
  { // Shift-0 transparency: exact dry both paths (L = 0 degenerates cleanly).
    const int n = static_cast<int>(sr * 0.5);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detLoud(i);
    TDM_CHECK(runLive(sr, 0.0f, in, true, 256) == in, "labwsolalive shift0 enabled exact");
    TDM_CHECK(runLive(sr, 0.0f, in, false, 256) == in, "labwsolalive shift0 bypass exact");
  }
  { // Finite output in every state, including mid-toggle transients.
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detLoud(i);
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      std::vector<float> a = runLive(sr, st, in, true, 512, n / 2);
      std::vector<float> b = runLive(sr, st, in, false, 512, n / 2);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f finite across toggles", st);
      TDM_CHECK(tdm_test::allFinite(a.data(), n) && tdm_test::allFinite(b.data(), n), msg);
    }
  }
  { // Block-size determinism, including a toggle at a fixed sample offset
    // (the ramp advances per sample, so block edges cannot matter).
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detLoud(i);
    for (float st : {-1.0f, -2.0f, -7.0f})
    {
      std::vector<float> r64 = runLive(sr, st, in, true, 64, n / 3);
      std::vector<float> r2048 = runLive(sr, st, in, true, 2048, n / 3);
      std::vector<float> r1 = runLive(sr, st, in, false, 130, n / 3);
      std::vector<float> r2 = runLive(sr, st, in, false, 1023, n / 3);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f block deterministic", st);
      TDM_CHECK(r64 == r2048 && r1 == r2, msg);
      // Reset determinism: same prepare => identical stream.
      std::vector<float> s1 = runLive(sr, st, in, true, 256);
      std::vector<float> s2 = runLive(sr, st, in, true, 256);
      std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f reset deterministic", st);
      TDM_CHECK(s1 == s2, msg);
    }
  }
  { // Toggle de-click: ramping bounds the worst sample step across the
    // transition far below any hard-switch jump (wet and dry differ by
    // a full L-sample time skew plus the pitch change, so a hard switch
    // on this loud stimulus would jump ~1.0; the 128-sample ramp holds
    // every step under 0.05 including natural signal slope).
    const int n = static_cast<int>(sr * 1.0);
    std::vector<float> in = sine(0.8f, 220.0f, sr, n);
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] += 0.2f * detLoud(i);
    for (float st : {-1.0f, -2.0f, -7.0f})
      for (bool startOn : {true, false})
      {
        std::vector<float> o = runLive(sr, st, in, startOn, 256, n / 2);
        const double step = maxStep(o.data(), n / 2 - 64, n / 2 + 256);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "labwsolalive st=%.0f on=%d toggle step %.4f", st,
                      startOn ? 1 : 0, step);
        TDM_CHECK(step < 0.05, msg);
      }
  }
  { // In-place safety: input == output buffer renders identically.
    const int n = static_cast<int>(sr * 0.5);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      in[static_cast<size_t>(i)] = detLoud(i);
    tdm::lab::LabWsolaLive a, b;
    a.prepare(sr, -2.0f, true, 2048);
    b.prepare(sr, -2.0f, true, 2048);
    std::vector<float> sep = in, ref(static_cast<size_t>(n));
    a.processBlock(in.data(), ref.data(), n);
    b.processBlock(sep.data(), sep.data(), n);
    TDM_CHECK(sep == ref, "labwsolalive in-place identical");
  }
}

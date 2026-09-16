#include "tests/Assert.h"

#include <cmath>
#include <stdexcept>
#include <vector>

#include "dsp/OutputTrim.h"
#include "dsp/TechDeathRig.h"

namespace
{
float dbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * std::sin(2.0f * 3.14159265f * freqHz * i / (float)sr);
  return out;
}

void processAll(tdm::OutputTrim& t, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  t.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}

// Samples for the one-pole smoother to cover 99% of a full-range jump.
int samplesToSettled(tdm::OutputTrim& t, int cap)
{
  std::vector<float> z(1, 0.0f), o(1);
  const float target = dbToLinear(t.trimDb());
  for (int i = 0; i < cap; ++i)
  {
    t.processBlock(z.data(), o.data(), 1);
    if (std::fabs(t.currentGain() - target) < 0.01f * std::fabs(target))
      return i + 1;
  }
  return -1;
}
} // namespace

void runOutputTrimTests()
{
  // Defaults are documented and stable.
  {
    tdm::OutputTrim t;
    TDM_CHECK_CLOSE(t.trimDb(), 0.0f, 1e-6f, "default trim");
    TDM_CHECK_CLOSE(t.currentGain(), 1.0f, 1e-9f, "default gain unity");
  }
  // Unity gain: bit-exact pass-through, in place too; silence stays silence.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 512);
    std::vector<float> out(512, 9.0f);
    t.processBlock(in.data(), out.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "0 dB copies exactly");
    t.processBlock(out.data(), out.data(), 512); // in-place
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "0 dB in-place copies exactly");
    std::vector<float> z(512, 0.0f);
    t.processBlock(z.data(), out.data(), 512);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 512) == 0.0f, "silence stays silence");
    tdm::OutputTrim fresh; // never reset: pass through rather than guess
    fresh.processBlock(in.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "unreset trim copies exactly");
  }
  // Positive gain: +6/+12 dB track the linear equivalent in the settled tail
  // (the 10 ms smoother needs ~20 time constants; tolerance is honest float
  // one-pole stall, where a wrong factor would still miss by ~1%).
  for (const float db : {6.0f, 12.0f})
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(db);
    std::vector<float> in = sine(0.25f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    const float want = dbToLinear(db);
    float maxErr = 0.0f;
    for (int i = 9600 - 1024; i < 9600; ++i)
      maxErr = std::max(maxErr, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)] * want));
    TDM_CHECK(maxErr < 1e-4f, "+dB matches linear gain");
  }
  // +18 dB: larger absolute stall (ulp scales with gain), looser bound.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(18.0f);
    std::vector<float> in = sine(0.25f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    const float want = dbToLinear(18.0f);
    float maxErr = 0.0f;
    for (int i = 9600 - 1024; i < 9600; ++i)
      maxErr = std::max(maxErr, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)] * want));
    TDM_CHECK(maxErr < 5e-4f, "+18 dB matches linear gain");
  }
  // Negative gain: -6/-12/-24 dB.
  for (const float db : {-6.0f, -12.0f, -24.0f})
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(db);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    const float want = dbToLinear(db);
    float maxErr = 0.0f;
    for (int i = 9600 - 1024; i < 9600; ++i)
      maxErr = std::max(maxErr, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)] * want));
    TDM_CHECK(maxErr < 1e-4f, "-dB matches linear gain");
  }
  // Clamping at both ends, observed through getter and applied gain tail.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(-30.0f);
    TDM_CHECK_CLOSE(t.trimDb(), -24.0f, 1e-6f, "clamps low");
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    constexpr int tail = 9600 - 1024;
    TDM_CHECK_CLOSE(tdm_test::peakAbs(out.data() + tail, 1024) / tdm_test::peakAbs(in.data() + tail, 1024),
                    dbToLinear(-24.0f), 1e-4f, "low clamp gain");
    t.setTrimDb(30.0f);
    TDM_CHECK_CLOSE(t.trimDb(), 24.0f, 1e-6f, "clamps high");
    processAll(t, in, out);
    TDM_CHECK_CLOSE(tdm_test::peakAbs(out.data() + tail, 1024) / tdm_test::peakAbs(in.data() + tail, 1024),
                    dbToLinear(24.0f), 1e-3f, "high clamp gain");
  }
  // No implicit clipping: over-unity results pass through finite, both rails.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(12.0f);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    TDM_CHECK(tdm_test::allFinite(out.data(), 9600), "hot output finite");
    float peak = 0.0f, trough = 0.0f;
    for (int i = 9600 - 1024; i < 9600; ++i)
    {
      peak = std::max(peak, out[static_cast<size_t>(i)]);
      trough = std::min(trough, out[static_cast<size_t>(i)]);
    }
    // Settled 0.5-peak sine at ~x3.98 must swing well past unity, unclipped.
    TDM_CHECK(peak > 1.5f, "positive rail not hard-clipped");
    TDM_CHECK(trough < -1.5f, "negative rail not hard-clipped");
    TDM_CHECK_CLOSE(peak, 0.5f * dbToLinear(12.0f), 1e-3f, "hot peak tracks linear gain");
  }
  // Smoothing: a full-range jump ramps instead of stepping. After setTrimDb
  // the applied gain must still read ~old (not instant), then converge.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    std::vector<float> in = sine(0.5f, 1000.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out); // settled at unity
    t.setTrimDb(24.0f);
    TDM_CHECK_CLOSE(t.currentGain(), 1.0f, 1e-6f, "gain not applied instantaneously");
    processAll(t, in, out);
    // Implied per-sample gain (out/in where the signal is large) starts near
    // unity and ramps: an instantaneous step would read ~15.85x immediately.
    float earlyGain = 1.0f;
    for (int i = 0; i < 48 && earlyGain <= 1.0f; ++i)
      if (std::fabs(in[static_cast<size_t>(i)]) > 0.1f)
        earlyGain = out[static_cast<size_t>(i)] / in[static_cast<size_t>(i)];
    TDM_CHECK(earlyGain < 1.5f, "no discontinuous sample jump");
    TDM_CHECK_CLOSE(t.currentGain(), dbToLinear(24.0f), 1e-3f, "gain converges to target");
    TDM_CHECK(tdm_test::allFinite(out.data(), 9600), "jump stays finite");
  }
  // Reset snaps applied gain exactly to target: deterministic from sample 0.
  {
    tdm::OutputTrim t;
    t.reset(48000.0);
    t.setTrimDb(12.0f);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 256);
    std::vector<float> a, b;
    processAll(t, in, a); // mid-ramp state
    t.reset(48000.0);
    TDM_CHECK_CLOSE(t.currentGain(), dbToLinear(12.0f), 1e-9f, "reset snaps to target");
    processAll(t, in, b);
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(!exact, "reset state differs from mid-ramp (snap is real)");
    tdm::OutputTrim u, v;
    u.reset(44100.0);
    v.reset(44100.0);
    u.setTrimDb(-6.0f);
    v.setTrimDb(-6.0f);
    std::vector<float> x = sine(0.5f, 220.0f, 44100.0, 512), ou, ov;
    processAll(u, x, ou);
    processAll(v, x, ov);
    exact = true;
    for (int i = 0; exact && i < 512; ++i)
      exact = ou[static_cast<size_t>(i)] == ov[static_cast<size_t>(i)];
    TDM_CHECK(exact, "identical instances deterministic");
    bool threw = false;
    try
    {
      t.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad sample rate throws");
  }
  // Sample rates: settled accuracy everywhere; smoother timing scales with
  // the rate (99% of a 0 -> +24 dB jump in ~4.6 time constants).
  for (const double sr : {44100.0, 48000.0, 96000.0})
  {
    tdm::OutputTrim t;
    t.reset(sr);
    t.setTrimDb(12.0f);
    std::vector<float> in = sine(0.25f, 220.0f, sr, static_cast<int>(0.2 * sr));
    std::vector<float> out;
    processAll(t, in, out);
    const int n = static_cast<int>(in.size());
    const int m = static_cast<int>(0.02 * sr);
    const float want = dbToLinear(12.0f);
    float maxErr = 0.0f;
    for (int i = n - m; i < n; ++i)
      maxErr = std::max(maxErr, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)] * want));
    TDM_CHECK(maxErr < 1e-3f, "settled gain accurate at all rates");

    tdm::OutputTrim s;
    s.reset(sr);
    s.setTrimDb(24.0f);
    const int got = samplesToSettled(s, static_cast<int>(2 * sr));
    TDM_CHECK(got > 0, "smoother converges at all rates");
    if (got > 0)
    {
      const double expect = 4.6 * 0.01 * sr;
      TDM_CHECK(got > 0.75 * expect && got < 1.25 * expect, "smoothing timing rate-independent");
    }
  }
  // Numerical stability: hot input at max trim, repeated runs, all finite.
  {
    std::vector<float> in = sine(0.99f, 220.0f, 96000.0, 4096);
    auto runOnce = [&] {
      tdm::OutputTrim t;
      t.reset(96000.0);
      t.setTrimDb(24.0f);
      std::vector<float> out;
      processAll(t, in, out);
      return out;
    };
    const std::vector<float> a = runOnce(), b = runOnce();
    TDM_CHECK(tdm_test::allFinite(a.data(), 4096), "hot max-trim finite");
    TDM_CHECK(tdm_test::peakAbs(a.data(), 4096) < 20.0f, "hot max-trim bounded");
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(exact, "repeated runs deterministic");
  }
  // Rig integration at 0 dB: full chain incl. the new stage preserves
  // previous behavior bit-exactly (no assets, gate/drive off).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    TDM_CHECK_CLOSE(rig.outputTrimDb(), 0.0f, 1e-6f, "rig output trim defaults 0 dB");
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 1024);
    const float* bi[1] = {in.data()};
    std::vector<float> out(1024);
    float* bo[1] = {out.data()};
    rig.processBlock(bi, 1, bo, 1, 1024);
    bool exact = true;
    for (int i = 0; i < 1024; ++i)
      exact = exact && out[static_cast<size_t>(i)] == in[static_cast<size_t>(i)];
    TDM_CHECK(exact, "0 dB output trim preserves rig path bit-exactly");
  }
  // Positive trim scales post-chain level only: with the gate enabled and a
  // decaying tone crossing its threshold, out(+24)/out(0) must equal the
  // linear gain at every audible sample — any disturbance of the gate would
  // show up as ratio deviations through the transitions. A 1 s settle block
  // runs first so the trim smoother (which correctly ramps on setter calls)
  // is converged in both runs before measurement.
  {
    std::vector<float> in(9600);
    for (int i = 0; i < 9600; ++i)
      in[static_cast<size_t>(i)] = 0.5f * std::exp(-6.0f * i / 9600.0f)
                                   * std::sin(2.0f * 3.14159265f * 220.0f * i / 48000.0f);
    std::vector<float> settle = sine(0.5f, 220.0f, 48000.0, 48000);
    auto runRig = [&](float outDb) {
      tdm::TechDeathRig rig;
      rig.reset(48000.0, 512);
      rig.setGateThresholdDb(-35.0f);
      rig.setGateEnabled(true);
      rig.setOutputTrimDb(outDb);
      std::vector<float> tmp(48000), o(9600);
      const float* bs[1] = {settle.data()};
      float* bt[1] = {tmp.data()};
      rig.processBlock(bs, 1, bt, 1, 48000);
      const float* bi[1] = {in.data()};
      float* bo[1] = {o.data()};
      rig.processBlock(bi, 1, bo, 1, 9600);
      return o;
    };
    const std::vector<float> ref = runRig(0.0f), hot = runRig(24.0f);
    const float want = dbToLinear(24.0f);
    bool scaled = true;
    int compared = 0;
    for (int i = 0; i < 9600 && scaled; ++i)
    {
      if (std::fabs(ref[static_cast<size_t>(i)]) > 1e-3f)
      {
        ++compared;
        scaled = std::fabs(hot[static_cast<size_t>(i)] / ref[static_cast<size_t>(i)] - want) < 0.01f * want;
      }
    }
    TDM_CHECK(compared > 1000, "ratio check covers transitions");
    TDM_CHECK(scaled, "output trim is pure post scaling");
  }
}

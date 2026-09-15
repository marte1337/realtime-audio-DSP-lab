#include "tests/Assert.h"

#include <cmath>
#include <stdexcept>
#include <vector>

#include "dsp/InputTrim.h"
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

void processAll(tdm::InputTrim& t, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  t.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}
} // namespace

void runTrimTests()
{
  // Defaults are documented and stable.
  {
    tdm::InputTrim t;
    TDM_CHECK_CLOSE(t.trimDb(), 0.0f, 1e-6f, "default trim");
    TDM_CHECK_CLOSE(t.currentGain(), 1.0f, 1e-9f, "default gain unity");
  }
  // Unity gain: bit-exact pass-through, in place too; silence stays silence.
  {
    tdm::InputTrim t;
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
    tdm::InputTrim fresh; // never reset: pass through rather than guess
    fresh.processBlock(in.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "unreset trim copies exactly");
  }
  // Positive gain: +6/+12 dB track the linear equivalent. The 10 ms
  // smoother needs ~20 time constants to settle, so measure the tail.
  // Tolerance accounts for honest float behavior: a one-pole update stalls
  // once (target - gain) * coeff rounds below half an ulp of the gain, so
  // the settled error is ~1e-4 absolute worst case, not 1e-8. A wrong gain
  // factor would still miss by ~1%, orders of magnitude above this bound.
  for (const float db : {6.0f, 12.0f})
  {
    tdm::InputTrim t;
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
  // Negative gain: -6/-12 dB.
  for (const float db : {-6.0f, -12.0f})
  {
    tdm::InputTrim t;
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
  // Clamping at both ends, observed through the applied gain. Peaks are
  // measured over the settled tail: the full buffer includes pre-ramp
  // cycles whose peaks still carry the old gain.
  {
    tdm::InputTrim t;
    t.reset(48000.0);
    t.setTrimDb(-30.0f);
    TDM_CHECK_CLOSE(t.trimDb(), -12.0f, 1e-6f, "clamps low");
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out);
    constexpr int tail = 9600 - 1024;
    TDM_CHECK_CLOSE(tdm_test::peakAbs(out.data() + tail, 1024) / tdm_test::peakAbs(in.data() + tail, 1024),
                    dbToLinear(-12.0f), 1e-4f, "low clamp gain");
    t.setTrimDb(30.0f);
    TDM_CHECK_CLOSE(t.trimDb(), 18.0f, 1e-6f, "clamps high");
    processAll(t, in, out);
    TDM_CHECK_CLOSE(tdm_test::peakAbs(out.data() + tail, 1024) / tdm_test::peakAbs(in.data() + tail, 1024),
                    dbToLinear(18.0f), 1e-3f, "high clamp gain");
  }
  // Smoothing: a full-range jump ramps instead of stepping. After setTrimDb
  // the applied gain must still read ~old (not instant), then converge.
  {
    tdm::InputTrim t;
    t.reset(48000.0);
    std::vector<float> in = sine(0.5f, 1000.0f, 48000.0, 9600);
    std::vector<float> out;
    processAll(t, in, out); // settled at unity
    t.setTrimDb(18.0f);
    TDM_CHECK_CLOSE(t.currentGain(), 1.0f, 1e-6f, "gain not applied instantaneously");
    processAll(t, in, out);
    // Implied per-sample gain (out/in where the signal is large) starts near
    // unity and ramps: an instantaneous step would read ~7.94x immediately.
    float earlyGain = 1.0f;
    for (int i = 0; i < 48 && earlyGain <= 1.0f; ++i)
      if (std::fabs(in[static_cast<size_t>(i)]) > 0.1f)
        earlyGain = out[static_cast<size_t>(i)] / in[static_cast<size_t>(i)];
    TDM_CHECK(earlyGain < 1.5f, "no discontinuous sample jump");
    TDM_CHECK_CLOSE(t.currentGain(), dbToLinear(18.0f), 1e-3f, "gain converges to target");
    TDM_CHECK(tdm_test::allFinite(out.data(), 9600), "jump stays finite");
  }
  // Reset snaps applied gain exactly to target: deterministic from sample 0.
  {
    tdm::InputTrim t;
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
    tdm::InputTrim u, v;
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
  // Numerical stability: hot input at max trim, repeated runs, all finite.
  // Each run starts from an identical fresh instance (reset snaps, setter
  // ramps), so both trajectories must match bit-exactly.
  {
    std::vector<float> in = sine(0.99f, 220.0f, 96000.0, 4096);
    auto runOnce = [&] {
      tdm::InputTrim t;
      t.reset(96000.0);
      t.setTrimDb(18.0f);
      std::vector<float> out;
      processAll(t, in, out);
      return out;
    };
    const std::vector<float> a = runOnce(), b = runOnce();
    TDM_CHECK(tdm_test::allFinite(a.data(), 4096), "hot max-trim finite");
    TDM_CHECK(tdm_test::peakAbs(a.data(), 4096) < 10.0f, "hot max-trim bounded");
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(exact, "repeated runs deterministic");
  }
  // Rig integration at 0 dB: trim + bypassed gate preserves M0 behavior;
  // +12 dB trim lifts a sub-threshold tone over the gate threshold.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    TDM_CHECK_CLOSE(rig.inputTrimDb(), 0.0f, 1e-6f, "rig trim defaults 0 dB");
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 1024);
    const float* bi[1] = {in.data()};
    std::vector<float> out(1024);
    float* bo[1] = {out.data()};
    rig.processBlock(bi, 1, bo, 1, 1024); // no assets, gate disabled
    bool exact = true;
    for (int i = 0; i < 1024; ++i)
      exact = exact && out[static_cast<size_t>(i)] == in[static_cast<size_t>(i)];
    TDM_CHECK(exact, "0 dB trim preserves M0 path bit-exactly");
    // -30 dB tone vs -20 dB gate threshold: closed without trim...
    std::vector<float> soft = sine(dbToLinear(-30.0f), 220.0f, 48000.0, 9600);
    rig.setGateThresholdDb(-20.0f);
    rig.setGateEnabled(true);
    std::vector<float> closed(9600);
    bo[0] = closed.data();
    bi[0] = soft.data();
    rig.processBlock(bi, 1, bo, 1, 9600);
    const float closedRatio =
      tdm_test::peakAbs(closed.data(), 9600) / tdm_test::peakAbs(soft.data(), 9600);
    TDM_CHECK(closedRatio < 0.001f, "sub-threshold tone gated without trim");
    // ...+12 dB trim lifts it over the threshold: tone passes at +12 dB.
    rig.reset(48000.0, 512);
    rig.setInputTrimDb(12.0f);
    rig.setGateThresholdDb(-20.0f);
    rig.setGateEnabled(true);
    std::vector<float> opened(9600);
    bo[0] = opened.data();
    rig.processBlock(bi, 1, bo, 1, 9600);
    const float want = dbToLinear(-30.0f + 12.0f);
    TDM_CHECK_CLOSE(tdm_test::peakAbs(opened.data(), 9600), want, 1e-3f, "trim lifts tone over gate");
  }
}

#include "tests/Assert.h"

#include <cmath>
#include <stdexcept>
#include <vector>

#include "dsp/TechDeathGate.h"
#include "dsp/TechDeathRig.h"

namespace
{
float dbToPeak(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

// Sine burst at peakDb, freqHz, starting at phase0 radians.
std::vector<float> tone(float peakDb, float freqHz, double sr, float phase0, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  const float peak = dbToPeak(peakDb);
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * std::sin(2.0f * 3.14159265f * freqHz * i / (float)sr + phase0);
  return out;
}

void processAll(tdm::TechDeathGate& g, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  g.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}

// Samples from tone-cut until currentGain() <= -60 dB. -1 on timeout.
int samplesToMinus60dB(tdm::TechDeathGate& g, double sr, int cap)
{
  std::vector<float> z(1, 0.0f), o(1);
  for (int i = 0; i < cap; ++i)
  {
    g.processBlock(z.data(), o.data(), 1);
    if (g.currentGain() <= 0.001f)
      return i + 1;
  }
  return -1;
}
} // namespace

void runGateTests()
{
  // Defaults are documented and stable.
  {
    tdm::TechDeathGate g;
    TDM_CHECK(!g.isEnabled(), "opt-in: starts disabled");
    TDM_CHECK_CLOSE(g.thresholdDb(), -40.0f, 1e-6f, "default threshold");
    TDM_CHECK_CLOSE(g.releaseMs(), 50.0f, 1e-6f, "default release");
  }
  // Bypass (disabled, or enabled without reset) copies exactly, in place too.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    std::vector<float> in = tone(-6.0f, 220.0f, 48000.0, 0.0f, 512);
    std::vector<float> out(512, 9.0f);
    g.processBlock(in.data(), out.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled bypass copies exactly");
    g.processBlock(out.data(), out.data(), 512); // in-place bypass
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled in-place bypass copies exactly");
    tdm::TechDeathGate fresh; // enabled but never reset: safe copy, no coeffs
    fresh.setEnabled(true);
    fresh.processBlock(in.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "unreset gate copies exactly");
  }
  // Silence stays silence; long silence snaps the envelope exactly to zero
  // (denormal guard) and output stays finite.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> loud = tone(-6.0f, 220.0f, 48000.0, 0.0f, 2048);
    std::vector<float> tmp;
    processAll(g, loud, tmp);
    std::vector<float> tenSeconds(480000, 0.0f), out;
    processAll(g, tenSeconds, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 480000) == 0.0f, "long silence is exactly silent");
    TDM_CHECK(tdm_test::allFinite(out.data(), 480000), "long silence stays finite");
    // One-pole release sticks within ~1e-9 of the floor in float; 1e-6 is
    // still -120 dB-scale precision, far beyond anything audible or malign.
    TDM_CHECK_CLOSE(g.currentGain(), dbToPeak(-80.0f), 1e-6f, "settled at floor");
  }
  // Clearly above threshold: opens fast, then passes tone unchanged.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> in = tone(-6.0f, 220.0f, 48000.0, 0.0f, 4800);
    std::vector<float> out;
    processAll(g, in, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 240) > 0.1f, "opens within 5 ms");
    float maxDiff = 0.0f;
    for (int i = 4800 - 512; i < 4800; ++i)
      maxDiff = std::max(maxDiff, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]));
    TDM_CHECK(maxDiff < 1e-4f, "fully open passes tone unchanged");
  }
  // Clearly below threshold: never opens, suppressed ~80 dB (floor).
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> in = tone(-50.0f, 220.0f, 48000.0, 0.0f, 4800);
    std::vector<float> out;
    processAll(g, in, out);
    const float ratio = tdm_test::peakAbs(out.data(), 4800) / tdm_test::peakAbs(in.data(), 4800);
    TDM_CHECK(ratio < 0.001f, "sub-threshold suppressed 60+ dB");
  }
  // Fast transient: 5 ms burst after silence opens promptly, no step at onset.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> silence(4800, 0.0f), tmp;
    processAll(g, silence, tmp);
    // Cosine phase: burst starts at peak, the hardest case for clicks.
    std::vector<float> burst = tone(-6.0f, 1000.0f, 48000.0, 1.5707963f, 240);
    std::vector<float> out;
    processAll(g, burst, out);
    // Gain updates in the same sample the detector fires (no one-sample
    // opening delay): onset output is ~13% of input at 48 k, a smooth ramp
    // point, not a step to full scale (a binary gate would print 0.5 here).
    TDM_CHECK(std::fabs(out[0]) < 0.15f, "no instantaneous jump at onset");
    TDM_CHECK(tdm_test::peakAbs(out.data(), 96) > 0.2f * dbToPeak(-6.0f), "attack preserved within 2 ms");
    TDM_CHECK(tdm_test::allFinite(out.data(), 240), "burst stays finite");
  }
  // Release: no instantaneous close; 60 dB fall time ~ detector + hold + R.
  for (const double sr : {44100.0, 48000.0, 96000.0})
  {
    for (const float rel : {10.0f, 50.0f, 500.0f})
    {
      if ((sr != 48000.0 && rel != 50.0f))
        continue; // extremes at 48 k; timing invariance across rates at 50 ms
      tdm::TechDeathGate g;
      g.reset(sr);
      g.setEnabled(true);
      g.setReleaseMs(rel);
      std::vector<float> loud = tone(-6.0f, 220.0f, sr, 0.0f, 4096);
      std::vector<float> tmp;
      processAll(g, loud, tmp);
      TDM_CHECK(g.isOpen(), "open before cut");
      // 1 ms after the cut the gate must still be essentially open.
      std::vector<float> z1(static_cast<size_t>(sr / 1000), 0.0f);
      processAll(g, z1, tmp);
      TDM_CHECK(g.currentGain() > 0.5f, "does not close instantaneously");
      const int n = samplesToMinus60dB(g, sr, static_cast<int>(sr * 2));
      TDM_CHECK(n > 0, "release reaches -60 dB");
      if (n > 0)
      {
        // detector fall (-6 dB note to close level) + 8 ms hold + R ms ramp.
        const double expectMs = 4.6 + 8.0 + rel;
        const double gotMs = 1000.0 * n / sr;
        TDM_CHECK(gotMs > 0.7 * expectMs && gotMs < 1.3 * expectMs, "release timing follows Release");
      }
    }
  }
  // Hysteresis + hold: low sustained note never flutters (exactly one close).
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> in = tone(-30.0f, 82.41f, 48000.0, 0.0f, 9600); // 200 ms low E
    std::vector<float> out(9600);
    int closes = 0;
    bool wasOpen = false;
    for (int off = 0; off < 9600; off += 64)
    {
      g.processBlock(in.data() + off, out.data() + off, 64);
      if (wasOpen && !g.isOpen())
        ++closes;
      wasOpen = g.isOpen();
    }
    TDM_CHECK(wasOpen, "sustained note holds gate open");
    TDM_CHECK(closes == 0, "no flutter on sustained low note");
    // Decaying note through the threshold closes exactly once (no chatter).
    std::vector<float> decay(14400);
    for (int i = 0; i < 14400; ++i)
    {
      const float env = dbToPeak(-20.0f - 25.0f * i / 14400.0f);
      decay[static_cast<size_t>(i)] = env * std::sin(2.0f * 3.14159265f * 220.0f * i / 48000.0f);
    }
    g.reset(48000.0);
    g.setEnabled(true);
    closes = 0;
    wasOpen = false;
    for (int off = 0; off < 14400; off += 64)
    {
      g.processBlock(decay.data() + off, out.data() + off, 64);
      if (wasOpen && !g.isOpen())
        ++closes;
      wasOpen = g.isOpen();
    }
    TDM_CHECK(closes == 1, "decaying note closes exactly once");
    TDM_CHECK(tdm_test::allFinite(out.data(), 9600), "sustain finite");
  }
  // Repeated open/close transitions: finite and bit-exact run to run.
  {
    auto runOnce = [] {
      tdm::TechDeathGate g;
      g.reset(48000.0);
      g.setEnabled(true);
      std::vector<float> acc;
      for (int b = 0; b < 20; ++b)
      {
        std::vector<float> blk =
          (b % 2 == 0) ? tone(-6.0f, 220.0f, 48000.0, 0.0f, 500) : tone(-60.0f, 220.0f, 48000.0, 0.0f, 500);
        std::vector<float> out;
        processAll(g, blk, out);
        acc.insert(acc.end(), out.begin(), out.end());
      }
      return acc;
    };
    const std::vector<float> a = runOnce(), b = runOnce();
    TDM_CHECK(a.size() == b.size() && tdm_test::allFinite(a.data(), static_cast<int>(a.size())), "transitions finite");
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(exact, "transitions deterministic");
  }
  // Reset returns to documented safe state (closed) and is deterministic.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> loud = tone(-6.0f, 220.0f, 48000.0, 0.0f, 2048);
    std::vector<float> tmp;
    processAll(g, loud, tmp);
    TDM_CHECK(g.isOpen(), "open before reset");
    g.reset(48000.0);
    TDM_CHECK(!g.isOpen(), "reset closes");
    TDM_CHECK_CLOSE(g.currentGain(), dbToPeak(-80.0f), 1e-9f, "reset gain at floor");
    std::vector<float> z(512, 0.0f), out;
    processAll(g, z, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 512) == 0.0f, "post-reset silence exact");
    bool threw = false;
    try
    {
      g.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad sample rate throws");
  }
  // Parameter extremes + clamping.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-60.0f); // very permissive: -55 dB tone passes
    std::vector<float> soft = tone(-55.0f, 220.0f, 48000.0, 0.0f, 9600);
    std::vector<float> out;
    processAll(g, soft, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 9600) > 0.8f * tdm_test::peakAbs(soft.data(), 9600),
              "permissive threshold passes soft tone");
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-20.0f); // extremely tight: -30 dB tone suppressed
    std::vector<float> mid = tone(-30.0f, 220.0f, 48000.0, 0.0f, 9600);
    processAll(g, mid, out);
    const float ratio = tdm_test::peakAbs(out.data(), 9600) / tdm_test::peakAbs(mid.data(), 9600);
    TDM_CHECK(ratio < 0.001f, "tight threshold suppresses mid tone");
    g.setThresholdDb(-100.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -60.0f, 1e-6f, "threshold clamps low");
    g.setThresholdDb(0.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -20.0f, 1e-6f, "threshold clamps high");
    g.setReleaseMs(1.0f);
    TDM_CHECK_CLOSE(g.releaseMs(), 10.0f, 1e-6f, "release clamps low");
    g.setReleaseMs(5000.0f);
    TDM_CHECK_CLOSE(g.releaseMs(), 500.0f, 1e-6f, "release clamps high");
  }
  // Rig integration: default-disabled rig is untouched; enabling with a
  // permissive threshold passes tone (attack ramp proves the gate engaged).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    TDM_CHECK(!rig.isGateEnabled(), "rig gate off by default");
    std::vector<float> in = tone(-6.0f, 220.0f, 48000.0, 1.5707963f, 4800);
    const float* bi[1] = {in.data()};
    std::vector<float> bypassed(4800), gated(4800);
    float* bo[1] = {bypassed.data()};
    rig.processBlock(bi, 1, bo, 1, 4800);
    rig.setGateThresholdDb(-60.0f);
    rig.setGateEnabled(true);
    bo[0] = gated.data();
    rig.processBlock(bi, 1, bo, 1, 4800);
    // Bypass prints full scale (0.5); the gated path ramps from the floor,
    // so the first sample is small but nonzero (same-sample gain update).
    TDM_CHECK(std::fabs(gated[0]) < 0.15f, "gate engages from closed on first sample");
    float maxDiff = 0.0f;
    for (int i = 4800 - 4000; i < 4800; ++i)
      maxDiff = std::max(maxDiff, std::fabs(gated[static_cast<size_t>(i)] - bypassed[static_cast<size_t>(i)]));
    TDM_CHECK(maxDiff < 1e-4f, "open gate matches bypassed rig");
    TDM_CHECK(tdm_test::allFinite(gated.data(), 4800), "gated rig finite");
  }
}

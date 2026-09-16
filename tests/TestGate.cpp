#include "tests/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "dsp/Gate/TechDeathGate.h"
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
    TDM_CHECK_CLOSE(g.thresholdDb(), -55.0f, 1e-6f, "default threshold");
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
  // Explicit -40 dB threshold (the -55 dB default would pass this tone).
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-40.0f);
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
      g.setThresholdDb(-40.0f); // explicit: timing math below assumes this close level
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
  // Explicit -40 dB: the decay probe only falls to -45 dB, which would sit
  // above the new -55 dB default open level and never close.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-40.0f);
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
    g.setThresholdDb(-80.0f); // very permissive: -75 dB tone passes
    std::vector<float> soft = tone(-75.0f, 220.0f, 48000.0, 0.0f, 9600);
    std::vector<float> out;
    processAll(g, soft, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 9600) > 0.8f * tdm_test::peakAbs(soft.data(), 9600),
              "permissive threshold passes soft tone");
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-35.0f); // extremely tight: -45 dB tone suppressed
    std::vector<float> mid = tone(-45.0f, 220.0f, 48000.0, 0.0f, 9600);
    processAll(g, mid, out);
    const float ratio = tdm_test::peakAbs(out.data(), 9600) / tdm_test::peakAbs(mid.data(), 9600);
    TDM_CHECK(ratio < 0.001f, "tight threshold suppresses mid tone");
    g.setThresholdDb(-100.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -80.0f, 1e-6f, "threshold clamps low");
    g.setThresholdDb(0.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -35.0f, 1e-6f, "threshold clamps high");
    g.setThresholdDb(-80.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -80.0f, 1e-6f, "threshold min passes through");
    g.setThresholdDb(-35.0f);
    TDM_CHECK_CLOSE(g.thresholdDb(), -35.0f, 1e-6f, "threshold max passes through");
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
  // Closing shape: S-curve knee. The fade starts gently (no slope step at
  // the hold/release boundary, the heard "hard stop") and steepens
  // mid-release. A single-pole fade drops fastest at its start, so it
  // fails the early-vs-mid slope comparison below.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    std::vector<float> loud = tone(-6.0f, 220.0f, 48000.0, 0.0f, 4096);
    std::vector<float> tmp;
    processAll(g, loud, tmp);
    std::vector<float> z(1, 0.0f), o(1);
    int closeIdx = -1;
    std::vector<float> gains;
    gains.reserve(4800);
    for (int i = 0; i < 4800; ++i)
    {
      g.processBlock(z.data(), o.data(), 1);
      gains.push_back(g.currentGain());
      if (closeIdx < 0 && !g.isOpen())
        closeIdx = i;
    }
    TDM_CHECK(closeIdx > 0, "knee probe closed");
    const int ms = 48; // samples per millisecond at 48 kHz
    const float early = gains[static_cast<size_t>(closeIdx)] - gains[static_cast<size_t>(closeIdx + ms)];
    const float mid = gains[static_cast<size_t>(closeIdx + 8 * ms)] - gains[static_cast<size_t>(closeIdx + 9 * ms)];
    TDM_CHECK(early > 0.0f && mid > 0.0f, "fade progresses");
    TDM_CHECK(early < 0.5f * mid, "release starts gentle, steepens mid-fade");
  }
  // Ragged-tail closing: no fragment train. A pluck with a hard palm-mute
  // stop at 0.5 s (the ~0.510 s close), plus hum, hiss, and a flurry of
  // three faint sympathetic blips (-37 dB, above Threshold) starting
  // 20 ms after the close. Under v1.1's fixed 60 ms window the flurry
  // collapsed to a single clean pair; under v1.2 the CLOSING retrigger bar
  // (Threshold + 12 dB) has no expiry, so the whole flurry stays shut: a
  // sustained above-threshold resonance ghosts at most once per event and
  // rapid cycling is impossible. The pair counting below pins that down
  // (v1.1 produces one pair here; pre-v1.1 code reopened on every blip).
  // NOTE: this test pins the pre-retune -40 dB threshold explicitly, so
  // the bar sits at -28 dB; the -50 dB reference behavior is covered by
  // the v1.2 tests.
  {
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 2.4);
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    std::uint32_t lcg = 0x12345678u;
    const double flurry[3] = {0.530, 0.585, 0.640};
    for (int i = 0; i < n; ++i)
    {
      const double t = i / sr;
      // Main pluck, hard-stopped at 0.5 s like a muted string.
      float x = t < 0.5 ? 0.5f * std::exp(-6.0f * (float)t) *
                            std::sin(2.0f * 3.14159265f * 82.41f * (float)t)
                        : 0.0f;
      for (double start : flurry) // faint sympathetic blip flurry
      {
        if (t >= start)
          x += 0.01413f * std::exp(-25.0f * ((float)t - (float)start)) *
               std::cos(2.0f * 3.14159265f * 110.0f * ((float)t - (float)start));
      }
      if (t >= 1.40 && t < 1.55) // same-level blip long after: must still open
        x += 0.01413f * std::exp(-25.0f * ((float)t - 1.40f)) *
             std::cos(2.0f * 3.14159265f * 110.0f * ((float)t - 1.40f));
      if (t >= 1.90 && t < 2.10) // hot note: attack must survive intact
        x += 0.2512f * std::cos(2.0f * 3.14159265f * 82.41f * ((float)t - 1.90f));
      x += 0.0008f * std::sin(2.0f * 3.14159265f * 50.0f * (float)t); // hum
      lcg = lcg * 1664525u + 1013904223u; // deterministic hiss, no rand()
      x += 0.0008f * ((float)((lcg >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f);
      in[static_cast<size_t>(i)] = x;
    }
    tdm::TechDeathGate g;
    g.reset(sr);
    g.setEnabled(true);
    g.setThresholdDb(-40.0f); // explicit: blip levels are calibrated to this bar
    std::vector<float> out(static_cast<size_t>(n));
    int closesInZone = 0, opensInZone = 0, closesTotal = 0;
    bool wasOpen = false, openAtBlip2 = false;
    float blip3Attack = 0.0f;
    for (int off = 0; off < n; off += 64)
    {
      g.processBlock(in.data() + off, out.data() + off, 64);
      const double t = off / sr;
      if (wasOpen && !g.isOpen())
      {
        ++closesTotal;
        if (t >= 0.40 && t < 0.80)
          ++closesInZone;
      }
      if (!wasOpen && g.isOpen() && t >= 0.40 && t < 0.80)
        ++opensInZone;
      wasOpen = g.isOpen();
      if (t >= 1.40 && t < 1.50 && g.isOpen())
        openAtBlip2 = true;
      if (t >= 1.90 && t < 1.902)
        blip3Attack = std::max(blip3Attack, tdm_test::peakAbs(out.data() + off, 64));
    }
    TDM_CHECK(closesInZone == 1, "stop closes once, flurry stays shut");
    TDM_CHECK(opensInZone == 0, "flurry never reopens during CLOSING");
    TDM_CHECK(openAtBlip2, "faint blip opens once fully CLOSED again");
    TDM_CHECK(closesTotal == 3, "main, blip2, hot note close once each");
    TDM_CHECK(blip3Attack > 0.2f * 0.2512f, "hot attack survives after stops");
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "ragged tail finite");
  }
  // Attack trio around CLOSING: hot transients reopen instantly even
  // while closing, faint residue stays shut while closing, and the same
  // faint level opens again once fully CLOSED (normal Threshold semantics
  // resume after the tail dies).
  {
    const double sr = 48000.0;
    auto closeFresh = [sr] {
      tdm::TechDeathGate g;
      g.reset(sr);
      g.setEnabled(true);
      g.setThresholdDb(-40.0f); // explicit: trio levels are calibrated to this bar
      std::vector<float> loud = tone(-6.0f, 220.0f, sr, 0.0f, 9600);
      std::vector<float> tmp;
      processAll(g, loud, tmp);
      std::vector<float> z(1, 0.0f), o(1);
      int closeIdx = -1;
      for (int i = 0; i < static_cast<int>(sr) && closeIdx < 0; ++i)
      {
        g.processBlock(z.data(), o.data(), 1);
        if (!g.isOpen())
          closeIdx = i;
      }
      return std::make_pair(g, closeIdx);
    };
    // Hot transient 10 ms after the close: opens within 2 ms.
    {
      auto [g, closeIdx] = closeFresh();
      TDM_CHECK(closeIdx > 0, "trio gate closed");
      std::vector<float> z(1, 0.0f), o(1);
      for (int i = 0; i < static_cast<int>(sr * 0.010); ++i)
        g.processBlock(z.data(), o.data(), 1);
      std::vector<float> burst = tone(-6.0f, 1000.0f, sr, 1.5707963f, 240);
      std::vector<float> out;
      processAll(g, burst, out);
      TDM_CHECK(tdm_test::peakAbs(out.data(), 96) > 0.2f * dbToPeak(-6.0f),
                "hot attack fast while CLOSING");
    }
    // Faint blip (-38 dB) 10 ms after the close: never reopens while it
    // plays. State is polled per block because a decaying blip would
    // re-close by the end of the buffer, hiding a transient reopen from an
    // end-of-buffer check.
    {
      auto [g, closeIdx] = closeFresh();
      TDM_CHECK(closeIdx > 0, "faint probe gate closed");
      std::vector<float> z(1, 0.0f), o(1);
      for (int i = 0; i < static_cast<int>(sr * 0.010); ++i)
        g.processBlock(z.data(), o.data(), 1);
      std::vector<float> blip(static_cast<size_t>(sr * 0.30), 0.0f);
      for (size_t i = 0; i < blip.size(); ++i)
      {
        const float t = (float)i / (float)sr;
        if (t < 0.15f)
          blip[i] = 0.0126f * std::exp(-25.0f * t) *
                    std::sin(2.0f * 3.14159265f * 82.41f * t);
      }
      std::vector<float> out(static_cast<size_t>(sr * 0.30));
      bool openedDuring = false;
      for (size_t off = 0; off < blip.size(); off += 64)
      {
        const size_t blk = std::min<size_t>(64, blip.size() - off);
        g.processBlock(blip.data() + off, out.data() + off, static_cast<int>(blk));
        openedDuring = openedDuring || g.isOpen();
      }
      TDM_CHECK(!openedDuring, "faint residue blocked while CLOSING");
      TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(blip.size())) < 0.02f,
                "blocked blip stays whisper-quiet");
    }
    // Same faint level 600 ms after the close: opens (CLOSED again, so
    // normal Threshold semantics apply). Sustained (not decaying) so the
    // gate is still open at the end.
    {
      auto [g, closeIdx] = closeFresh();
      TDM_CHECK(closeIdx > 0, "late probe gate closed");
      std::vector<float> z(1, 0.0f), o(1);
      for (int i = 0; i < static_cast<int>(sr * 0.60); ++i)
        g.processBlock(z.data(), o.data(), 1);
      std::vector<float> soft = tone(-38.0f, 82.41f, sr, 0.0f, static_cast<int>(sr * 0.15));
      std::vector<float> out;
      processAll(g, soft, out);
      TDM_CHECK(g.isOpen(), "faint tone opens once CLOSED");
      TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(soft.size())) > 0.5f * dbToPeak(-38.0f),
                "faint tone passes from CLOSED");
    }
  }
  // Palm-mute proxy: five hard-stopped low chugs stay tight. Every attack
  // survives (the retrigger bar never eats a real note), every gap goes
  // fully silent, and nothing reopens between chugs.
  {
    const double sr = 48000.0;
    const int chugLen = static_cast<int>(sr * 0.06);
    const int period = static_cast<int>(sr * 0.25);
    const int nChugs = 5;
    const int n = period * nChugs + static_cast<int>(sr * 0.30);
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    std::uint32_t lcg = 0xA53A5A53u;
    for (int i = 0; i < n; ++i)
    {
      const int pos = i % period;
      const int chug = i / period;
      float x = 0.0f;
      if (chug < nChugs && pos < chugLen)
      {
        const float t = (float)pos / (float)sr;
        x = 0.5f * std::exp(-30.0f * t) * std::cos(2.0f * 3.14159265f * 82.41f * t);
      }
      lcg = lcg * 1664525u + 1013904223u;
      x += 0.0011f * ((float)((lcg >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f);
      in[static_cast<size_t>(i)] = x;
    }
    tdm::TechDeathGate g;
    g.reset(sr);
    g.setEnabled(true);
    std::vector<float> out(static_cast<size_t>(n));
    processAll(g, in, out);
    for (int c = 0; c < nChugs; ++c)
    {
      const int start = c * period;
      TDM_CHECK(tdm_test::peakAbs(out.data() + start, static_cast<int>(sr * 0.005)) > 0.1f,
                "chug attack survives");
      const int gapTail = start + period - static_cast<int>(sr * 0.10);
      TDM_CHECK(tdm_test::peakAbs(out.data() + gapTail, static_cast<int>(sr * 0.10)) < dbToPeak(-50.0f),
                "chug gap goes silent");
    }
    // No opens deep inside gaps: second pass observes isOpen per block
    // (output levels already verified above).
    bool openedInGap = false;
    {
      tdm::TechDeathGate probe;
      probe.reset(sr);
      probe.setEnabled(true);
      std::vector<float> tmp(64);
      for (int off = 0; off < n; off += 64)
      {
        probe.processBlock(in.data() + off, tmp.data(), 64);
        const double t = off / sr;
        const int chug = off / period;
        const double tInPeriod = t - (double)(chug * period) / sr;
        if (chug < nChugs && tInPeriod > 0.16 && tInPeriod < 0.24 && probe.isOpen())
          openedInGap = true;
      }
    }
    TDM_CHECK(!openedInGap, "no reopens between chugs");
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "chugs finite");
  }
  // v1.2 sustained-note tests at the real-guitar reference (-50 dB
  // threshold, 52 ms release). A single unre-picked decaying note must go
  // OPEN -> CLOSING -> CLOSED exactly once: no stutter, however long the
  // tail beats near the threshold.
  for (const double v12sr : {44100.0, 48000.0, 96000.0})
  {
    // Long beating low-E sustain (4 s): parks the mean envelope in the
    // threshold zone for ~2 s with +/-3 dB beating, hum and hiss. Under
    // v1.1's fixed 60 ms window this produced 11 open/close cycles.
    {
      const int n = static_cast<int>(v12sr * 4.0);
      std::vector<float> in(static_cast<size_t>(n));
      std::uint32_t lcg = 0x12345678u;
      for (int i = 0; i < n; ++i)
      {
        const double t = i / v12sr;
        float level;
        if (t < 0.6)
          level = 0.2512f * std::exp(-3.5f * (float)t);
        else
          level = 0.0056f * std::exp(-0.55f * ((float)t - 0.6f));
        const float beat = 0.72f + 0.28f * std::sin(2.0f * 3.14159265f * 0.8f * (float)t);
        const float ph = 2.0f * 3.14159265f * 82.41f * (float)t;
        float x = level * beat * std::sin(ph);
        x += 0.35f * level * beat * std::sin(2.0f * ph + 0.7f);
        x += 0.18f * level * beat * std::sin(3.01f * ph + 2.1f);
        x += 0.0008f * std::sin(2.0f * 3.14159265f * 50.0f * (float)t);
        lcg = lcg * 1664525u + 1013904223u;
        x += 0.0008f * ((float)((lcg >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f);
        in[static_cast<size_t>(i)] = x;
      }
      tdm::TechDeathGate g;
      g.reset(v12sr);
      g.setEnabled(true);
      g.setThresholdDb(-50.0f);
      g.setReleaseMs(52.0f);
      int opens = 0, closes = 0;
      bool wasOpen = false;
      std::vector<float> tmp(64), out(static_cast<size_t>(n));
      for (int off = 0; off < n; off += 64)
      {
        const int blk = std::min(64, n - off);
        g.processBlock(in.data() + off, out.data() + off, blk);
        if (!wasOpen && g.isOpen())
          ++opens;
        if (wasOpen && !g.isOpen())
          ++closes;
        wasOpen = g.isOpen();
      }
      TDM_CHECK(opens == 1, "sustained note opens once");
      TDM_CHECK(closes == 1, "sustained note closes exactly once, never stutters");
      TDM_CHECK(tdm_test::allFinite(out.data(), n), "sustain finite");
    }
    // Long near-threshold low-B tail (3 s, different beating): residual
    // modulation crosses the ordinary threshold repeatedly but must not
    // create any reopen once closing has begun.
    {
      const int n = static_cast<int>(v12sr * 3.0);
      std::vector<float> in(static_cast<size_t>(n));
      std::uint32_t lcg = 0xABCDEF01u;
      for (int i = 0; i < n; ++i)
      {
        const double t = i / v12sr;
        float level;
        if (t < 0.5)
          level = 0.2512f * std::exp(-4.0f * (float)t);
        else
          level = 0.0063f * std::exp(-0.6f * ((float)t - 0.5f));
        const float beat = 0.7f + 0.3f * std::sin(2.0f * 3.14159265f * 1.1f * (float)t + 1.0f);
        const float ph = 2.0f * 3.14159265f * 61.74f * (float)t;
        float x = level * beat * std::sin(ph);
        x += 0.3f * level * beat * std::sin(2.0f * ph + 1.9f);
        x += 0.0008f * std::sin(2.0f * 3.14159265f * 50.0f * (float)t);
        lcg = lcg * 1664525u + 1013904223u;
        x += 0.0008f * ((float)((lcg >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f);
        in[static_cast<size_t>(i)] = x;
      }
      tdm::TechDeathGate g;
      g.reset(v12sr);
      g.setEnabled(true);
      g.setThresholdDb(-50.0f);
      g.setReleaseMs(52.0f);
      int transitions = 0;
      bool wasOpen = false, reopenedAfterClose = false, closedOnce = false;
      std::vector<float> tmp(static_cast<size_t>(n));
      for (int off = 0; off < n; off += 64)
      {
        const int blk = std::min(64, n - off);
        g.processBlock(in.data() + off, tmp.data(), blk);
        if (wasOpen != g.isOpen())
        {
          ++transitions;
          if (!g.isOpen())
            closedOnce = true;
          if (closedOnce && g.isOpen())
            reopenedAfterClose = true;
        }
        wasOpen = g.isOpen();
      }
      TDM_CHECK(transitions == 2, "low-B tail: one open plus one close, nothing else");
      TDM_CHECK(!reopenedAfterClose, "no fragment bursts after closing begins");
      TDM_CHECK(tdm_test::allFinite(tmp.data(), n), "low-B tail finite");
    }
    // Genuine repicks during CLOSING reopen promptly: strong, medium and
    // soft-but-intentional strengths, injected 30 ms after the close onto
    // a still-live tail. No attack delay is added for any of them.
    for (const float repickDb : {-12.0f, -24.0f, -32.0f, -36.0f})
    {
      tdm::TechDeathGate g;
      g.reset(v12sr);
      g.setEnabled(true);
      g.setThresholdDb(-50.0f);
      g.setReleaseMs(52.0f);
      // Decaying bed (2 s) with absolute-time phase continuity throughout.
      const int bedN = static_cast<int>(v12sr * 2.0);
      std::vector<float> bed(static_cast<size_t>(bedN));
      for (int i = 0; i < bedN; ++i)
      {
        const double t = i / v12sr;
        bed[static_cast<size_t>(i)] =
          0.2512f * std::exp(-3.5f * (float)t) *
          std::sin(2.0f * 3.14159265f * 82.41f * (float)t);
      }
      std::vector<float> tmp(static_cast<size_t>(bedN));
      int closeIdx = -1;
      for (int off = 0; off < bedN; off += 64)
      {
        const int blk = std::min(64, bedN - off);
        g.processBlock(bed.data() + off, tmp.data() + off, blk);
        if (closeIdx < 0 && !g.isOpen())
          closeIdx = off + blk - 1;
      }
      TDM_CHECK(closeIdx > 0, "repick probe gate closed");
      // 30 ms further down the same decaying bed (still a live tail).
      std::vector<float> o(1);
      const int tail30 = static_cast<int>(v12sr * 0.030);
      for (int i = 1; i <= tail30; ++i)
      {
        const double t = (closeIdx + i) / v12sr;
        const float s = 0.2512f * std::exp(-3.5f * (float)t) *
                        std::sin(2.0f * 3.14159265f * 82.41f * (float)t);
        g.processBlock(&s, o.data(), 1);
      }
      std::vector<float> repick = tone(repickDb, 164.81f, v12sr, 1.5707963f,
                                       static_cast<int>(v12sr * 0.20));
      std::vector<float> out;
      processAll(g, repick, out);
      const int twoMs = static_cast<int>(v12sr * 0.002);
      TDM_CHECK(tdm_test::peakAbs(out.data(), twoMs) > 0.2f * dbToPeak(repickDb),
                "repick reopens within 2 ms while CLOSING");
    }
  }
  // v1.2 tradeoff pin: a very soft repick below the retrigger bar that
  // lands while CLOSING (tail recently alive) stays shut; the same level
  // opens normally once fully CLOSED. At -50 dB the bar sits at -38 dB,
  // so a -44 dB repick is blocked during CLOSING but passes from CLOSED.
  {
    const double sr = 48000.0;
    tdm::TechDeathGate g;
    g.reset(sr);
    g.setEnabled(true);
    g.setThresholdDb(-50.0f);
    g.setReleaseMs(52.0f);
    std::vector<float> loud = tone(-6.0f, 220.0f, sr, 0.0f, 9600);
    std::vector<float> tmp;
    processAll(g, loud, tmp);
    // Hard cut, then 150 ms of silence: CLOSING (confirmation needs
    // 250 ms) with a dead tail.
    std::vector<float> z(1, 0.0f), o(1);
    int closeIdx = -1;
    for (int i = 0; i < static_cast<int>(sr) && closeIdx < 0; ++i)
    {
      g.processBlock(z.data(), o.data(), 1);
      if (!g.isOpen())
        closeIdx = i;
    }
    TDM_CHECK(closeIdx > 0, "tradeoff probe gate closed");
    for (int i = 0; i < static_cast<int>(sr * 0.150); ++i)
      g.processBlock(z.data(), o.data(), 1);
    std::vector<float> soft = tone(-44.0f, 164.81f, sr, 1.5707963f, static_cast<int>(sr * 0.30));
    std::vector<float> out(soft.size());
    bool openedDuring = false;
    for (size_t off = 0; off < soft.size(); off += 64)
    {
      const size_t blk = std::min<size_t>(64, soft.size() - off);
      g.processBlock(soft.data() + off, out.data() + off, static_cast<int>(blk));
      openedDuring = openedDuring || g.isOpen();
    }
    TDM_CHECK(!openedDuring, "sub-bar soft repick blocked while CLOSING");
    TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(soft.size())) < 0.001f,
              "blocked soft repick stays inaudible");
    // 400 ms of silence completes confirmation (-> CLOSED); same level now
    // opens with normal Threshold semantics: no permanent penalty.
    for (int i = 0; i < static_cast<int>(sr * 0.40); ++i)
      g.processBlock(z.data(), o.data(), 1);
    processAll(g, soft, out);
    TDM_CHECK(g.isOpen(), "soft repick opens once CLOSED");
    TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(soft.size())) > 0.5f * dbToPeak(-44.0f),
              "soft repick passes from CLOSED");
  }
  // Fully-closed opening at the -50 dB reference: normal initial picks
  // just above Threshold (-48 dB) engage with zero delay from CLOSED.
  {
    tdm::TechDeathGate g;
    g.reset(48000.0);
    g.setEnabled(true);
    g.setThresholdDb(-50.0f);
    g.setReleaseMs(52.0f);
    std::vector<float> silence(4800, 0.0f), tmp;
    processAll(g, silence, tmp);
    std::vector<float> pick = tone(-48.0f, 164.81f, 48000.0, 1.5707963f, 480);
    std::vector<float> out;
    processAll(g, pick, out);
    TDM_CHECK(g.isOpen(), "near-threshold pick opens from CLOSED");
    TDM_CHECK(tdm_test::peakAbs(out.data(), 96) > 0.2f * dbToPeak(-48.0f),
              "near-threshold pick attacks within 2 ms");
  }
  // Fast rhythm at the -50 dB reference: hot chugs every 250 ms all open;
  // gaps go fully silent. CLOSING never eats a real transient.
  for (const double chugSr : {44100.0, 48000.0, 96000.0})
  {
    const int chugLen = static_cast<int>(chugSr * 0.06);
    const int period = static_cast<int>(chugSr * 0.25);
    const int nChugs = 5;
    const int n = period * nChugs + static_cast<int>(chugSr * 0.30);
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    std::uint32_t lcg = 0x5A5A5A5Au;
    for (int i = 0; i < n; ++i)
    {
      const int pos = i % period;
      const int chug = i / period;
      float x = 0.0f;
      if (chug < nChugs && pos < chugLen)
      {
        const float t = (float)pos / (float)chugSr;
        x = 0.5f * std::exp(-30.0f * t) * std::cos(2.0f * 3.14159265f * 82.41f * t);
      }
      lcg = lcg * 1664525u + 1013904223u;
      x += 0.0008f * ((float)((lcg >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f);
      in[static_cast<size_t>(i)] = x;
    }
    tdm::TechDeathGate g;
    g.reset(chugSr);
    g.setEnabled(true);
    g.setThresholdDb(-50.0f);
    g.setReleaseMs(52.0f);
    std::vector<float> out(static_cast<size_t>(n));
    processAll(g, in, out);
    for (int c = 0; c < nChugs; ++c)
    {
      const int start = c * period;
      TDM_CHECK(tdm_test::peakAbs(out.data() + start, static_cast<int>(chugSr * 0.005)) > 0.1f,
                "reference chug attack survives");
      const int gapTail = start + period - static_cast<int>(chugSr * 0.10);
      TDM_CHECK(tdm_test::peakAbs(out.data() + gapTail, static_cast<int>(chugSr * 0.10)) < dbToPeak(-50.0f),
                "reference chug gap goes silent");
    }
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "reference chugs finite");
  }
}

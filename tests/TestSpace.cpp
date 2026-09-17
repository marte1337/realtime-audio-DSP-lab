// Space v1.3 tests: Delay (pair-equalized ping-pong) + Reverb (dual-tank
// cross-coupled plate) + SpaceProcessor routing + rig integration.
// Delay/Reverb expose wet-only outputs, so time/feedback/decay are measured
// on wet directly; routing, mixes, and stereo live at the Space/rig level.

#include "tests/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "dsp/Space/Delay.h"
#include "dsp/Space/Reverb.h"
#include "dsp/Space/SpaceProcessor.h"
#include "dsp/TechDeathRig.h"

namespace
{
constexpr double kPi = 3.141592653589793;

std::vector<float> impulse(int n, float amp = 1.0f)
{
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  if (n > 0)
    out[0] = amp;
  return out;
}

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * std::sin(2.0f * (float)(kPi * freqHz * i / sr));
  return out;
}

float windowPeak(const float* x, int from, int to)
{
  float m = 0.0f;
  for (int i = from; i < to; ++i)
  {
    const float a = std::fabs(x[i]);
    if (a > m)
      m = a;
  }
  return m;
}

float windowMean(const float* x, int from, int to)
{
  double acc = 0.0;
  for (int i = from; i < to; ++i)
    acc += x[i];
  return static_cast<float>(acc / (to - from));
}

float windowEnergy(const float* x, int from, int to)
{
  double acc = 0.0;
  for (int i = from; i < to; ++i)
    acc += (double)x[i] * x[i];
  return static_cast<float>(acc);
}

void runSpace(tdm::SpaceProcessor& s, const std::vector<float>& in, std::vector<float>& l,
              std::vector<float>& r)
{
  l.assign(in.size(), 0.0f);
  r.assign(in.size(), 0.0f);
  s.processBlock(in.data(), l.data(), r.data(), static_cast<int>(in.size()));
}

tdm::SpaceProcessor makeSpace(double sr, bool delayOn, bool reverbOn)
{
  tdm::SpaceProcessor s;
  s.reset(sr);
  s.setDelayEnabled(delayOn);
  s.setReverbEnabled(reverbOn);
  return s;
}
} // namespace

void runSpaceTests()
{
  // Defaults are documented and stable; both units opt-in off.
  {
    tdm::Delay d;
    tdm::Reverb v;
    tdm::SpaceProcessor s;
    TDM_CHECK_CLOSE(d.timeMs(), 220.0f, 1e-6f, "default delay time");
    TDM_CHECK_CLOSE(d.feedback(), 0.35f, 1e-6f, "default feedback");
    TDM_CHECK(!d.isEnabled(), "delay starts disabled");
    TDM_CHECK_CLOSE(v.decay(), 0.4f, 1e-6f, "default decay");
    TDM_CHECK(!v.isEnabled(), "reverb starts disabled");
    TDM_CHECK_CLOSE(s.delayMix(), 0.25f, 1e-6f, "default delay mix");
    TDM_CHECK_CLOSE(s.reverbMix(), 0.20f, 1e-6f, "default reverb mix");
    TDM_CHECK(!s.isDelayEnabled() && !s.isReverbEnabled(), "space starts bypassed");
    TDM_CHECK(s.isBypassed(), "isBypassed true when both off");
  }
  // Extremes clamp; bad rates throw.
  {
    tdm::Delay d;
    tdm::Reverb v;
    tdm::SpaceProcessor s;
    d.reset(48000.0);
    v.reset(48000.0);
    s.reset(48000.0);
    d.setTimeMs(1.0f);
    d.setFeedback(2.0f);
    v.setDecay(-1.0f);
    s.setDelayMix(2.0f);
    s.setReverbMix(-2.0f);
    TDM_CHECK_CLOSE(d.timeMs(), 20.0f, 1e-6f, "time clamps low");
    TDM_CHECK_CLOSE(d.feedback(), 0.85f, 1e-6f, "feedback clamps high");
    TDM_CHECK_CLOSE(v.decay(), 0.0f, 1e-6f, "decay clamps low");
    TDM_CHECK_CLOSE(s.delayMix(), 1.0f, 1e-6f, "delay mix clamps high");
    TDM_CHECK_CLOSE(s.reverbMix(), 0.0f, 1e-6f, "reverb mix clamps low");
    d.setTimeMs(5000.0f);
    v.setDecay(5.0f);
    TDM_CHECK_CLOSE(d.timeMs(), 2000.0f, 1e-6f, "time clamps high");
    TDM_CHECK_CLOSE(v.decay(), 1.0f, 1e-6f, "decay clamps high");
    int threw = 0;
    try
    {
      d.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      ++threw;
    }
    try
    {
      v.reset(-1.0);
    }
    catch (const std::runtime_error&)
    {
      ++threw;
    }
    try
    {
      s.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      ++threw;
    }
    TDM_CHECK(threw == 3, "bad sample rates throw");
  }
  // Bypass: both off writes dry to L and R bit-exactly (stage level).
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, false, false);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 2048);
    for (int i = 0; i < 2048; ++i)
      in[static_cast<size_t>(i)] += 0.2f * std::sin(2.0f * (float)(kPi * 3000.0 * i / 48000.0));
    std::vector<float> l, r;
    runSpace(s, in, l, r);
    bool exact = l.size() == in.size() && r.size() == in.size();
    for (size_t i = 0; exact && i < in.size(); ++i)
      exact = l[i] == in[i] && r[i] == in[i];
    TDM_CHECK(exact, "bypassed space is bit-exact dry on L+R");
  }
  // Unreset units pass dry / silence wet (safe before first reset).
  {
    tdm::SpaceProcessor s; // never reset
    std::vector<float> in = sine(0.4f, 440.0f, 48000.0, 512);
    std::vector<float> l, r;
    runSpace(s, in, l, r);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && l[i] == in[i] && r[i] == in[i];
    TDM_CHECK(exact, "unreset space passes dry");
    tdm::Delay d;
    d.setEnabled(true);
    tdm::Reverb v;
    v.setEnabled(true);
    std::vector<float> wL(512, 9.0f), wR(512, 9.0f);
    d.processBlock(in.data(), wL.data(), wR.data(), 512);
    v.processBlock(in.data(), wL.data(), wR.data(), 512);
    TDM_CHECK(tdm_test::peakAbs(wL.data(), 512) == 0.0f && tdm_test::peakAbs(wR.data(), 512) == 0.0f,
              "unreset units emit silence");
  }
  // Silence in stays silence, everything on.
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, true, true);
    s.setDelayMix(1.0f);
    s.setReverbMix(1.0f);
    s.setDelayFeedback(0.85f);
    s.setReverbDecay(1.0f);
    std::vector<float> z(4096, 0.0f), l, r;
    runSpace(s, z, l, r);
    TDM_CHECK(tdm_test::peakAbs(l.data(), 4096) == 0.0f && tdm_test::peakAbs(r.data(), 4096) == 0.0f,
              "silence stays silence");
  }
  // Reset is deterministic: two fresh units agree; reset restores agreement.
  {
    std::vector<float> in = sine(0.4f, 110.0f, 48000.0, 48000);
    for (int i = 0; i < 48000; ++i)
      in[static_cast<size_t>(i)] += 0.2f * std::sin(2.0f * (float)(kPi * 1300.0 * i / 48000.0));
    tdm::SpaceProcessor a = makeSpace(48000.0, true, true);
    tdm::SpaceProcessor b = makeSpace(48000.0, true, true);
    std::vector<float> lA, rA, lB, rB;
    runSpace(a, in, lA, rA);
    runSpace(b, in, lB, rB);
    bool same = true;
    for (int i = 0; same && i < 48000; ++i)
      same = lA[i] == lB[i] && rA[i] == rB[i];
    TDM_CHECK(same, "fresh units agree exactly");
    a.reset(48000.0);
    std::vector<float> lA2, rA2;
    runSpace(a, in, lA2, rA2);
    same = true;
    for (int i = 0; same && i < 48000; ++i)
      same = lA2[i] == lB[i] && rA2[i] == rB[i];
    TDM_CHECK(same, "reset restores exact agreement");
  }
  // Disabling mid-tail returns to exact dry immediately (wet zeros).
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, true, true);
    s.setDelayMix(1.0f);
    s.setReverbMix(1.0f);
    std::vector<float> in = impulse(48000);
    std::vector<float> l, r;
    runSpace(s, in, l, r); // build a tail
    std::vector<float> dry(512, 0.3f), oL, oR;
    s.setDelayEnabled(false);
    s.setReverbEnabled(false);
    runSpace(s, dry, oL, oR);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && oL[i] == 0.3f && oR[i] == 0.3f;
    TDM_CHECK(exact, "disable mid-tail returns exact dry");
  }
  // Delay time: first echo lands on Time at every rate; fb=0 = single echo.
  // Params go BEFORE reset: reset snaps the smoothed delay length, so the
  // first echo lands sample-exactly. (Setting Time after reset would
  // exercise the intentional ~30 ms tape-glide instead — correct behavior,
  // wrong setup for a landing test.)
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::Delay d;
      d.setTimeMs(375.0f);
      d.setFeedback(0.0f);
      d.setEnabled(true);
      d.reset(sr);
      const int n = static_cast<int>(1.0 * sr);
      std::vector<float> in = impulse(n), wL(n, 0.0f), wR(n, 0.0f);
      d.processBlock(in.data(), wL.data(), wR.data(), n);
      const int want = static_cast<int>(375.0 / 1000.0 * sr);
      int first = -1;
      for (int i = static_cast<int>(0.05 * sr); i < n; ++i)
      {
        if (std::fabs(wL[static_cast<size_t>(i)]) > 0.1f)
        {
          first = i;
          break;
        }
      }
      TDM_CHECK(first >= want - 3 && first <= want + 3, "first echo lands on Time");
      TDM_CHECK(windowPeak(wL.data(), want + static_cast<int>(0.375 * sr), n) == 0.0f,
                "fb=0 leaves a single echo");
    }
  }
  // Delay feedback: pair-equalized decay pins per-side DC-step means.
  // After the input stops, each side steps down by the pair decay F=fb^2
  // exactly once — L first (its seed dried up), R one generation later
  // (its seed is L's taps) — then holds flat. Per-side ratios cancel the
  // common priming-convergence factor, so they are exact.
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::SpaceProcessor s = makeSpace(sr, true, false);
      s.setDelayTimeMs(375.0f);
      s.setDelayFeedback(0.5f);
      s.setDelayMix(1.0f);
      const int t = static_cast<int>(375.0 / 1000.0 * sr);
      const int n = 9 * t;
      std::vector<float> in(static_cast<size_t>(n), 0.0f);
      for (int i = 0; i < 6 * t; ++i)
        in[static_cast<size_t>(i)] = 1.0f;
      std::vector<float> l, r;
      runSpace(s, in, l, r);
      const float l6 = windowMean(l.data(), 6 * t + t / 8, 7 * t - t / 8);
      const float l7 = windowMean(l.data(), 7 * t + t / 8, 8 * t - t / 8);
      const float l8 = windowMean(l.data(), 8 * t + t / 8, 9 * t - t / 8);
      const float r6 = windowMean(r.data(), 6 * t + t / 8, 7 * t - t / 8);
      const float r7 = windowMean(r.data(), 7 * t + t / 8, 8 * t - t / 8);
      const float r8 = windowMean(r.data(), 8 * t + t / 8, 9 * t - t / 8);
      TDM_CHECK_CLOSE(l7 / l6, 0.25f, 0.02f, "L steps down by pair decay");
      TDM_CHECK_CLOSE(l8 / l7, 1.0f, 0.02f, "L then holds flat");
      TDM_CHECK_CLOSE(r7 / r6, 1.0f, 0.02f, "R holds one generation longer");
      TDM_CHECK_CLOSE(r8 / r7, 0.25f, 0.02f, "R then steps by pair decay");
    }
  }
  // Delay stability: max feedback tail stays finite and keeps decaying.
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, true, false);
    s.setDelayTimeMs(375.0f);
    s.setDelayFeedback(0.85f);
    s.setDelayMix(1.0f);
    std::vector<float> in = impulse(static_cast<int>(4.0 * 48000.0));
    std::vector<float> l, r;
    runSpace(s, in, l, r);
    const int n = static_cast<int>(4.0 * 48000.0);
    TDM_CHECK(tdm_test::allFinite(l.data(), n) && tdm_test::allFinite(r.data(), n), "max fb stays finite");
    const float early = windowEnergy(l.data(), static_cast<int>(1.0 * 48000), static_cast<int>(2.0 * 48000));
    const float late = windowEnergy(l.data(), static_cast<int>(3.0 * 48000), static_cast<int>(4.0 * 48000));
    TDM_CHECK(late < early, "max fb tail keeps decaying");
  }
  // Ping-pong: echoes strictly alternate L/R (opposite side exactly silent).
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, true, false);
    s.setDelayTimeMs(200.0f);
    s.setDelayFeedback(0.5f);
    s.setDelayMix(1.0f);
    const int t = static_cast<int>(200.0 / 1000.0 * 48000.0);
    std::vector<float> in = impulse(5 * t), l, r;
    runSpace(s, in, l, r);
    TDM_CHECK(windowPeak(l.data(), t, 2 * t) > 0.1f, "first echo on L");
    TDM_CHECK(windowPeak(r.data(), t, 2 * t) == 0.0f, "R silent during first echo");
    TDM_CHECK(windowPeak(r.data(), 2 * t, 3 * t) > 0.01f, "second echo on R");
    TDM_CHECK(windowPeak(l.data(), 2 * t, 3 * t) == 0.0f, "L silent during second echo");
    TDM_CHECK(windowPeak(l.data(), 3 * t, 4 * t) > 0.001f, "third echo back on L");
    TDM_CHECK(windowPeak(r.data(), 3 * t, 4 * t) == 0.0f, "R silent during third echo");
  }
  // Balance: cumulative wet energy stays centered. Guitar-range content
  // passes the loop damping nearly untouched, so pairs balance ~exactly;
  // a white impulse is the worst case (one extra damping stage bites HF
  // only) and still improved ~8x over per-echo decay (28:1 -> ~3.5:1).
  {
    const double sr = 48000.0;
    const int t = static_cast<int>(220.0 / 1000.0 * sr);
    // Guitar-representative stimulus: decaying 110 Hz harmonic stack.
    const int n = static_cast<int>(2.0 * sr);
    std::vector<float> note(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < static_cast<int>(1.0 * sr); ++i)
    {
      const double tt = i / sr, env = std::exp(-3.0 * tt);
      note[static_cast<size_t>(i)] = static_cast<float>(
          0.25 * env
          * (std::sin(2.0 * kPi * 110.0 * tt) + 0.5 * std::sin(2.0 * kPi * 220.0 * tt)
             + 0.3 * std::sin(2.0 * kPi * 330.0 * tt) + 0.18 * std::sin(2.0 * kPi * 440.0 * tt)
             + 0.1 * std::sin(2.0 * kPi * 660.0 * tt)));
    }
    for (const float fb : {0.35f, 0.5f})
    {
      tdm::Delay d;
      d.reset(sr);
      d.setTimeMs(220.0f);
      d.setFeedback(fb);
      d.setEnabled(true);
      std::vector<float> wL(static_cast<size_t>(n), 0.0f), wR(static_cast<size_t>(n), 0.0f);
      d.processBlock(note.data(), wL.data(), wR.data(), n);
      const float eL = windowEnergy(wL.data(), t, n);
      const float eR = windowEnergy(wR.data(), t, n);
      TDM_CHECK(eL / eR > 0.5f && eL / eR < 2.0f, "wet energy centered on guitar content");
    }
    // Impulse worst case: regression guard, generous physics-bound.
    {
      tdm::Delay d;
      d.reset(sr);
      d.setTimeMs(220.0f);
      d.setFeedback(0.35f);
      d.setEnabled(true);
      std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                          wR(static_cast<size_t>(n), 0.0f);
      d.processBlock(in.data(), wL.data(), wR.data(), n);
      const float eL = windowEnergy(wL.data(), t, n);
      const float eR = windowEnergy(wR.data(), t, n);
      TDM_CHECK(eL / eR < 4.0f, "impulse wet energy no longer leans hard left");
    }
  }
  // Feedback floor: fb=0 is a single L echo (R exactly silent everywhere);
  // just above zero the R answer fades in gracefully, never a full echo.
  {
    const double sr = 48000.0;
    const int t = static_cast<int>(220.0 / 1000.0 * sr);
    const int n = 4 * t;
    {
      // Feedback set before reset: the smoothed gain snaps to 0, so the
      // R-seed gate is exactly closed from the first sample.
      tdm::Delay d;
      d.setTimeMs(220.0f);
      d.setFeedback(0.0f);
      d.setEnabled(true);
      d.reset(sr);
      std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                          wR(static_cast<size_t>(n), 0.0f);
      d.processBlock(in.data(), wL.data(), wR.data(), n);
      TDM_CHECK(windowPeak(wL.data(), t, 2 * t) > 0.1f, "fb=0 first echo on L");
      TDM_CHECK(windowPeak(wR.data(), 0, n) == 0.0f, "fb=0 R silent everywhere");
    }
    tdm::Delay d;
    d.reset(sr);
    d.setTimeMs(220.0f);
    d.setFeedback(0.05f);
    d.setEnabled(true);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    d.processBlock(in.data(), wL.data(), wR.data(), n);
    TDM_CHECK(windowPeak(wR.data(), 2 * t, 3 * t) > 0.0f, "near-zero fb still seeds R");
    TDM_CHECK(windowPeak(wR.data(), 2 * t, 3 * t) < windowPeak(wL.data(), t, 2 * t),
              "near-zero fb R answer stays partial");
  }
  // Delay mix: 0 is bit-exact dry (mix set before reset, so the smoothed
  // gain snaps to 0 with the documented deterministic state); wet energy
  // grows with mix.
  {
    tdm::SpaceProcessor dry;
    dry.setDelayEnabled(true);
    dry.setDelayTimeMs(200.0f);
    dry.setDelayFeedback(0.5f);
    dry.setDelayMix(0.0f);
    dry.reset(48000.0);
    tdm::SpaceProcessor low = makeSpace(48000.0, true, false);
    low.setDelayTimeMs(200.0f);
    low.setDelayFeedback(0.5f);
    low.setDelayMix(0.2f);
    tdm::SpaceProcessor high = makeSpace(48000.0, true, false);
    high.setDelayTimeMs(200.0f);
    high.setDelayFeedback(0.5f);
    high.setDelayMix(0.8f);
    std::vector<float> in = impulse(48000), l, r;
    runSpace(dry, in, l, r);
    bool exact = true;
    for (int i = 0; i < 48000; ++i)
      exact = exact && l[i] == in[i] && r[i] == in[i];
    TDM_CHECK(exact, "delay mix 0 is bit-exact dry");
    std::vector<float> lL, rL, lH, rH;
    runSpace(low, in, lL, rL);
    runSpace(high, in, lH, rH);
    const int t = static_cast<int>(200.0 / 1000.0 * 48000.0);
    float eL = 0.0f, eH = 0.0f;
    for (int i = t; i < 48000; ++i)
    {
      const float a = lL[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
      const float b = lH[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
      eL += a * a;
      eH += b * b;
    }
    TDM_CHECK(eH > 4.0f * eL, "wet energy grows with mix");
  }
  // Dry onset: first 10 ms are pure dry with both units fully on.
  {
    for (const double sr : {48000.0, 96000.0})
    {
      tdm::SpaceProcessor s = makeSpace(sr, true, true);
      s.setDelayMix(1.0f);
      s.setReverbMix(1.0f);
      s.setDelayFeedback(0.85f);
      s.setReverbDecay(1.0f);
      std::vector<float> in = impulse(static_cast<int>(0.1 * sr));
      std::vector<float> l, r;
      runSpace(s, in, l, r);
      const int m = static_cast<int>(0.01 * sr);
      bool exact = true;
      for (int i = 0; i < m; ++i)
        exact = exact && l[i] == in[i] && r[i] == in[i];
      TDM_CHECK(exact, "first 10 ms stay pure dry");
    }
  }
  // Reverb decay: short dies, long rings (energies, not peaks: the damped
  // highs fall faster than the T60 lows by design, so peak oracles would
  // misread a healthy tail).
  {
    tdm::SpaceProcessor room = makeSpace(48000.0, false, true);
    room.setReverbDecay(0.0f);
    room.setReverbMix(1.0f);
    tdm::SpaceProcessor hall = makeSpace(48000.0, false, true);
    hall.setReverbDecay(1.0f);
    hall.setReverbMix(1.0f);
    std::vector<float> in = impulse(static_cast<int>(1.5 * 48000.0)), l, r;
    runSpace(room, in, l, r);
    const float shortMid =
        windowEnergy(l.data(), static_cast<int>(0.5 * 48000), static_cast<int>(1.0 * 48000));
    runSpace(hall, in, l, r);
    const float longMid =
        windowEnergy(l.data(), static_cast<int>(0.5 * 48000), static_cast<int>(1.0 * 48000));
    const float longLate =
        windowEnergy(l.data(), static_cast<int>(1.0 * 48000), static_cast<int>(1.5 * 48000));
    TDM_CHECK(shortMid < 1e-9f, "short decay dies within a second");
    TDM_CHECK(longMid > 1e-3f, "long decay still rings");
    TDM_CHECK(longLate > 1e-4f, "long decay reaches past a second");
    TDM_CHECK(longMid / (shortMid + 1e-30f) > 1e6f, "decay extremes orders apart");
    TDM_CHECK(tdm_test::allFinite(l.data(), static_cast<int>(1.5 * 48000)), "reverb tail finite");
  }
  // Tail density: a dense FDN tail looks noise-like late (low crest over
  // short windows); sparse flutter echoes spike. The v1 Schroeder tail
  // measured crest 10+ here; the FDN measures ~3.9 (near-Gaussian).
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.75f);
    v.setEnabled(true);
    const int n = static_cast<int>(2.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    float crestMax = 0.0f;
    for (int w = 0; w < 4; ++w)
    {
      const int s0 = static_cast<int>(1.0 * 48000.0 + w * 0.1 * 48000.0);
      const int s1 = s0 + static_cast<int>(0.05 * 48000.0);
      const float e = windowEnergy(wL.data(), s0, s1);
      const float c = windowPeak(wL.data(), s0, s1) / (std::sqrt(e / (s1 - s0)) + 1e-30f);
      if (c > crestMax)
        crestMax = c;
    }
    TDM_CHECK(crestMax < 8.0f, "late tail dense, not fluttery");
  }
  // No ringing modes: spectral flatness (geometric/arithmetic mean, dB)
  // of a late-tail slice. White noise is ~-2.5 dB; a few dominating
  // modes would push this far negative. Plate measures ~-1.5 dB.
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.75f);
    v.setEnabled(true);
    const int n = static_cast<int>(2.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    const int stride = 8, N = 512, a = static_cast<int>(1.0 * 48000.0);
    double logSum = 0.0, magSum = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
      double re = 0.0, im = 0.0;
      for (int j = 0; j < N; ++j)
      {
        const float x = wL[static_cast<size_t>(a + j * stride)];
        const double ph = -2.0 * kPi * k * j / N;
        re += x * std::cos(ph);
        im += x * std::sin(ph);
      }
      const double m = std::sqrt(re * re + im * im) + 1e-30;
      logSum += std::log(m);
      magSum += m;
    }
    const double flatDb = 10.0 * (logSum / (N / 2 - 1) - std::log(magSum / (N / 2 - 1))) / std::log(10.0);
    TDM_CHECK(flatDb > -4.0, "late tail spectrally smooth, no dominant modes");
  }
  // No DC buildup: settled wet mean of a DC input is float noise.
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::Reverb v;
      v.reset(sr);
      v.setDecay(0.6f);
      v.setEnabled(true);
      const int n = static_cast<int>(3.0 * sr);
      std::vector<float> dc(static_cast<size_t>(n), 0.25f), wL(static_cast<size_t>(n), 0.0f),
                          wR(static_cast<size_t>(n), 0.0f);
      v.processBlock(dc.data(), wL.data(), wR.data(), n);
      const int s0 = static_cast<int>(2.5 * sr);
      double mean = 0.0;
      for (int i = s0; i < n; ++i)
        mean += wL[static_cast<size_t>(i)] + wR[static_cast<size_t>(i)];
      mean /= 2.0 * (n - s0);
      TDM_CHECK(std::fabs(mean) < 1e-6, "settled DC leaks nothing audible");
    }
  }
  // Controlled LF: the 180 Hz send highpass keeps lows out of the tank.
  // 80 Hz sine builds far less wet energy than 440 Hz at equal drive.
  {
    tdm::Reverb v80, v440;
    v80.reset(48000.0);
    v440.reset(48000.0);
    v80.setDecay(0.6f);
    v440.setDecay(0.6f);
    v80.setEnabled(true);
    v440.setEnabled(true);
    const int n = 48000;
    std::vector<float> s80 = sine(0.4f, 80.0f, 48000.0, n);
    std::vector<float> s440 = sine(0.4f, 440.0f, 48000.0, n);
    std::vector<float> l80(static_cast<size_t>(n), 0.0f), r80(static_cast<size_t>(n), 0.0f);
    std::vector<float> l440(static_cast<size_t>(n), 0.0f), r440(static_cast<size_t>(n), 0.0f);
    v80.processBlock(s80.data(), l80.data(), r80.data(), n);
    v440.processBlock(s440.data(), l440.data(), r440.data(), n);
    const float e80 = windowEnergy(l80.data(), 24000, 48000) + windowEnergy(r80.data(), 24000, 48000);
    const float e440 =
        windowEnergy(l440.data(), 24000, 48000) + windowEnergy(r440.data(), 24000, 48000);
    TDM_CHECK(e80 / e440 < 0.5f, "lows stay out of the room");
  }
  // Extremes: max decay over a long run stays finite and bounded.
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(1.0f);
    v.setEnabled(true);
    const int n = static_cast<int>(4.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    TDM_CHECK(tdm_test::allFinite(wL.data(), n) && tdm_test::allFinite(wR.data(), n),
              "max decay stays finite");
    TDM_CHECK(windowPeak(wL.data(), 0, n) < 1.0f && windowPeak(wR.data(), 0, n) < 1.0f,
              "max decay stays bounded");
  }
  // Tail continuity: the long tail fades smoothly all the way down — no
  // cliff, no abrupt multi-state shutoff above the inaudible floor — and
  // reaches exact digital silence without ever producing subnormal
  // samples (denormal-stall guard). Bounds pin the REQUIREMENT, not the
  // mechanism: any near-zero strategy that truncates audibly, hikes the
  // clamping level, or lets states decay into denormals fails here.
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(1.0f);
    v.setEnabled(true);
    const int n = static_cast<int>(20.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    // (a) Smooth decrease: 100 ms RMS steps never jump (legit slope here
    // is ~-1.5 dB/step; beating/wander allow small rises).
    const int step = static_cast<int>(0.1 * 48000.0);
    double prevDb = 0.0;
    bool first = true, smooth = true;
    for (int a = static_cast<int>(1.0 * 48000.0); a + step <= n; a += step)
    {
      double e = 0.0;
      for (int i = a; i < a + step; ++i)
        e += (double)wL[static_cast<size_t>(i)] * wL[static_cast<size_t>(i)]
             + (double)wR[static_cast<size_t>(i)] * wR[static_cast<size_t>(i)];
      if (e == 0.0)
        break; // exact silence reached; covered by (b)/(c) below
      const double db = 10.0 * std::log10(e / (2 * step));
      if (!first)
      {
        if (db - prevDb > 2.0 || prevDb - db > 10.0)
          smooth = false;
      }
      first = false;
      prevDb = db;
    }
    TDM_CHECK(smooth, "late tail fades smoothly, no abrupt shutoff");
    // (b) If exact silence is reached, the 200 ms before it are already
    // effectively inaudible (< -180 dBFS, far below any converter floor).
    int onset = -1;
    for (int i = 0; i < n; ++i)
    {
      if (wL[static_cast<size_t>(i)] == 0.0f && wR[static_cast<size_t>(i)] == 0.0f)
      {
        bool rest = true;
        for (int j = i; j < n; ++j)
        {
          if (wL[static_cast<size_t>(j)] != 0.0f || wR[static_cast<size_t>(j)] != 0.0f)
          {
            rest = false;
            break;
          }
        }
        if (rest)
        {
          onset = i;
          break;
        }
      }
    }
    if (onset > 0)
    {
      const int pre = std::max(0, onset - static_cast<int>(0.2 * 48000.0));
      double e = 0.0;
      for (int i = pre; i < onset; ++i)
        e += (double)wL[static_cast<size_t>(i)] * wL[static_cast<size_t>(i)]
             + (double)wR[static_cast<size_t>(i)] * wR[static_cast<size_t>(i)];
      const double db = 10.0 * std::log10(e / (2 * (onset - pre)) + 1e-30);
      TDM_CHECK(db < -180.0, "digital silence only after inaudible level");
    }
    // (c) No subnormal output samples anywhere in the 20 s tail:
    // recirculating states must never decay into denormal range (CPU
    // stall risk on x86 without FTZ), whatever the clamping strategy.
    bool subnormal = false;
    for (int i = 0; i < n && !subnormal; ++i)
    {
      if (std::fpclassify(wL[static_cast<size_t>(i)]) == FP_SUBNORMAL
          || std::fpclassify(wR[static_cast<size_t>(i)]) == FP_SUBNORMAL)
        subnormal = true;
    }
    TDM_CHECK(!subnormal, "no denormal samples in the tail");
  }
  // Reverb stereo: L/R decorrelated but same order of energy. The late
  // tail's normalized cross-correlation stays well below mono (+1):
  // disjoint tank sets + diverging modulation phases keep the channels
  // evolving apart on their own (no Delay needed for width).
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, false, true);
    s.setReverbDecay(0.5f);
    s.setReverbMix(1.0f);
    std::vector<float> in = impulse(48000), l, r;
    runSpace(s, in, l, r);
    const int a = static_cast<int>(0.1 * 48000);
    float maxDiff = 0.0f;
    for (int i = a; i < 48000; ++i)
      maxDiff = std::max(maxDiff, std::fabs(l[static_cast<size_t>(i)] - r[static_cast<size_t>(i)]));
    const float eL = windowEnergy(l.data(), a, 48000);
    const float eR = windowEnergy(r.data(), a, 48000);
    TDM_CHECK(maxDiff > 1e-4f, "reverb L/R decorrelated");
    TDM_CHECK(eL / eR > 0.25f && eL / eR < 4.0f, "reverb L/R same energy order");
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.7f);
    v.setEnabled(true);
    const int n = static_cast<int>(2.5 * 48000.0);
    std::vector<float> jin = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(jin.data(), wL.data(), wR.data(), n);
    const int b0 = static_cast<int>(1.0 * 48000.0), b1 = static_cast<int>(2.0 * 48000.0);
    double slr = 0.0, sll = 0.0, srr = 0.0;
    for (int i = b0; i < b1; ++i)
    {
      slr += (double)wL[static_cast<size_t>(i)] * wR[static_cast<size_t>(i)];
      sll += (double)wL[static_cast<size_t>(i)] * wL[static_cast<size_t>(i)];
      srr += (double)wR[static_cast<size_t>(i)] * wR[static_cast<size_t>(i)];
    }
    const double rho = slr / std::sqrt(sll * srr);
    TDM_CHECK(std::fabs(rho) < 0.7, "late tail stays wide, never collapses to mono");
  }
  // Mid/side energy: the tail carries substantial perceptual side energy,
  // not just decorrelation. S/M ~2.3 here; the center-heavy v1.2 field
  // measured 0.36 on this exact metric (mid 3x side).
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.7f);
    v.setEnabled(true);
    const int n = static_cast<int>(3.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    auto sideOverMid = [&](int a, int b) {
      double mE = 0.0, sE = 0.0;
      for (int i = a; i < b; ++i)
      {
        const double m = 0.5 * (wL[static_cast<size_t>(i)] + wR[static_cast<size_t>(i)]);
        const double s = 0.5 * (wL[static_cast<size_t>(i)] - wR[static_cast<size_t>(i)]);
        mE += m * m;
        sE += s * s;
      }
      return sE / (mE + 1e-30);
    };
    const double early = sideOverMid(static_cast<int>(0.10 * 48000.0), static_cast<int>(0.25 * 48000.0));
    const double late = sideOverMid(static_cast<int>(1.20 * 48000.0), static_cast<int>(2.40 * 48000.0));
    TDM_CHECK(late > 1.2 && late < 5.0, "late tail carries real side energy");
    TDM_CHECK(late > 0.5 * early, "width persists, never collapses back to center");
  }
  // No centered regions: energy-averaged triplet bands across the guitar
  // range. Single bins are modulation luck (one bin can read 6+ while its
  // neighbors read 1); a whole REGION collapsing is the real disease —
  // v1.2's 1760 Hz single bin read 0.32. Triplets measure ~1.0-1.7 here.
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.7f);
    v.setEnabled(true);
    const int n = static_cast<int>(3.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    const int a = static_cast<int>(1.0 * 48000.0), b = static_cast<int>(2.0 * 48000.0);
    const double groups[4][3] = {{82.41, 110.0, 138.59},
                                 {164.81, 220.0, 277.18},
                                 {329.63, 440.0, 554.37},
                                 {880.0, 1760.0, 3520.0}};
    double mn = 1e30, mx = 0.0;
    for (const auto& g : groups)
    {
      double sS = 0.0, sM = 0.0;
      for (double f : g)
      {
        double lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0;
        const double w = 2.0 * kPi * f / 48000.0, cw = std::cos(w), sw = std::sin(w);
        for (const std::vector<float>* ch : {&wL, &wR})
        {
          double s0 = 0.0, s1 = 0.0, s2 = 0.0;
          for (int i = a; i < b; ++i)
          {
            s0 = (*ch)[static_cast<size_t>(i)] + 2.0 * cw * s1 - s2;
            s2 = s1;
            s1 = s0;
          }
          if (ch == &wL)
          {
            lr = s1 * cw - s2;
            li = s1 * sw;
          }
          else
          {
            rr = s1 * cw - s2;
            ri = s1 * sw;
          }
        }
        const double mr = lr + rr, mi = li + ri, dr = lr - rr, di = li - ri;
        sS += dr * dr + di * di;
        sM += mr * mr + mi * mi;
      }
      const double ratio = std::sqrt(sS / (sM + 1e-30));
      if (ratio < mn)
        mn = ratio;
      if (ratio > mx)
        mx = ratio;
    }
    TDM_CHECK(mn > 0.5, "no frequency region collapses to center");
    TDM_CHECK(mx / mn < 4.0, "width consistent across regions");
  }
  // Tonal floor: sustained chord tones never collapse fully at any probed
  // pitch. Per-note variance is inherent to modal reverbs (some notes
  // always bloom wider than others), so only the floor is pinned here;
  // regional consistency above is the stronger guard.
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.6f);
    v.setEnabled(true);
    const double freqs[6] = {110.0, 164.81, 220.0, 277.18, 329.63, 440.0};
    const int n = static_cast<int>(4.0 * 48000.0);
    std::vector<float> s(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
      double samp = 0.0;
      for (double f : freqs)
        samp += std::sin(2.0 * kPi * f * i / 48000.0);
      s[static_cast<size_t>(i)] = static_cast<float>(0.12 * samp);
    }
    std::vector<float> wL(static_cast<size_t>(n), 0.0f), wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(s.data(), wL.data(), wR.data(), n);
    const int a = static_cast<int>(3.0 * 48000.0);
    double mn = 1e30;
    for (double f : freqs)
    {
      double lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0;
      const double w = 2.0 * kPi * f / 48000.0, cw = std::cos(w), sw = std::sin(w);
      for (const std::vector<float>* ch : {&wL, &wR})
      {
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (int i = a; i < n; ++i)
        {
          s0 = (*ch)[static_cast<size_t>(i)] + 2.0 * cw * s1 - s2;
          s2 = s1;
          s1 = s0;
        }
        if (ch == &wL)
        {
          lr = s1 * cw - s2;
          li = s1 * sw;
        }
        else
        {
          rr = s1 * cw - s2;
          ri = s1 * sw;
        }
      }
      const double mr = lr + rr, mi = li + ri, dr = lr - rr, di = li - ri;
      const double ratio =
          std::sqrt(dr * dr + di * di) / (std::sqrt(mr * mr + mi * mi) + 1e-30);
      if (ratio < mn)
        mn = ratio;
    }
    TDM_CHECK(mn > 0.5, "no driven pitch collapses fully to center");
  }
  // Mono fold-down: (L+R)/2 keeps substantial energy (no anti-phase
  // collapse) and L/R stay balanced over successive windows (rotation
  // slosh stays gentle, never seasick).
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.7f);
    v.setEnabled(true);
    const int n = static_cast<int>(3.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    const int a = static_cast<int>(0.5 * 48000.0), b = static_cast<int>(2.0 * 48000.0);
    double foldE = 0.0;
    for (int i = a; i < b; ++i)
    {
      const double f = 0.5 * (wL[static_cast<size_t>(i)] + wR[static_cast<size_t>(i)]);
      foldE += f * f;
    }
    const double stereoE = (windowEnergy(wL.data(), a, b) + windowEnergy(wR.data(), a, b)) / 2.0;
    TDM_CHECK(foldE / stereoE > 0.2, "mono fold-down keeps real energy");
    for (double t0 : {0.1, 0.5, 1.0, 1.5})
    {
      const int s0 = static_cast<int>(t0 * 48000.0);
      const int s1 = s0 + static_cast<int>(0.4 * 48000.0);
      const double bal = windowEnergy(wL.data(), s0, s1) / windowEnergy(wR.data(), s0, s1);
      TDM_CHECK(bal > 0.5 && bal < 2.0, "L/R balance steady over time");
    }
  }
  // Modulation alive but subtle: a steady sine's wet level wanders a
  // little (static modes broken up) but never pumps like a chorus.
  // The static v1.1 tank measured 0.001 dB here (lower bound is the
  // regression guard for "someone disables modulation").
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(0.7f);
    v.setEnabled(true);
    const int n = static_cast<int>(6.0 * 48000.0);
    std::vector<float> s = sine(0.4f, 440.0f, 48000.0, n);
    std::vector<float> wL(static_cast<size_t>(n), 0.0f), wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(s.data(), wL.data(), wR.data(), n);
    double mn = 1e30, mx = 0.0;
    for (int w = 0; w < 20; ++w)
    {
      const int s0 = static_cast<int>((3.0 + w * 0.1) * 48000.0);
      const int s1 = s0 + static_cast<int>(0.1 * 48000.0);
      const double e = windowEnergy(wL.data(), s0, s1) / (s1 - s0);
      if (e < mn)
        mn = e;
      if (e > mx)
        mx = e;
    }
    const double wanderDb = 10.0 * std::log10(mx / mn);
    TDM_CHECK(wanderDb > 0.2, "modulation walks the modes (not frozen)");
    TDM_CHECK(wanderDb < 4.0, "modulation stays subtle (not chorus)");
  }
  // Modulation stability: a long max-decay run stays finite, bounded,
  // and keeps decaying (wobble never pumps the loop).
  {
    tdm::Reverb v;
    v.reset(48000.0);
    v.setDecay(1.0f);
    v.setEnabled(true);
    const int n = static_cast<int>(8.0 * 48000.0);
    std::vector<float> in = impulse(n), wL(static_cast<size_t>(n), 0.0f),
                        wR(static_cast<size_t>(n), 0.0f);
    v.processBlock(in.data(), wL.data(), wR.data(), n);
    TDM_CHECK(tdm_test::allFinite(wL.data(), n) && tdm_test::allFinite(wR.data(), n),
              "modulated tank stays finite");
    const float early = windowEnergy(wL.data(), 48000, 96000);
    const float late = windowEnergy(wL.data(), static_cast<int>(6.0 * 48000.0),
                                    static_cast<int>(7.0 * 48000.0));
    TDM_CHECK(late < early, "modulated tail keeps decaying");
  }
  // Reverb mix: 0 is bit-exact dry (mix set before reset, so the smoothed
  // gain snaps to 0 with the documented deterministic state).
  {
    tdm::SpaceProcessor dry;
    dry.setReverbEnabled(true);
    dry.setReverbDecay(0.6f);
    dry.setReverbMix(0.0f);
    dry.reset(48000.0);
    std::vector<float> in = impulse(48000), l, r;
    runSpace(dry, in, l, r);
    bool exact = true;
    for (int i = 0; i < 48000; ++i)
      exact = exact && l[i] == in[i] && r[i] == in[i];
    TDM_CHECK(exact, "reverb mix 0 is bit-exact dry");
  }
  // Mid-stream parameter jumps stay finite and settle back to dry on disable.
  {
    tdm::SpaceProcessor s = makeSpace(48000.0, true, true);
    std::vector<float> in = sine(0.4f, 440.0f, 48000.0, 512);
    std::vector<float> l(512), r(512);
    bool finite = true;
    for (int b = 0; b < 40; ++b)
    {
      s.setDelayTimeMs(b % 2 == 0 ? 1500.0f : 60.0f);
      s.setDelayFeedback(b % 2 == 0 ? 0.85f : 0.0f);
      s.setDelayMix(b % 2 == 0 ? 1.0f : 0.0f);
      s.setReverbDecay(b % 2 == 0 ? 1.0f : 0.0f);
      s.setReverbMix(b % 2 == 0 ? 1.0f : 0.0f);
      s.processBlock(in.data(), l.data(), r.data(), 512);
      finite = finite && tdm_test::allFinite(l.data(), 512) && tdm_test::allFinite(r.data(), 512);
    }
    TDM_CHECK(finite, "mid-stream jumps stay finite");
    s.setDelayEnabled(false);
    s.setReverbEnabled(false);
    s.processBlock(in.data(), l.data(), r.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && l[i] == in[i] && r[i] == in[i];
    TDM_CHECK(exact, "disable after jumps returns exact dry");
  }
  // Rate consistency: echo lands on Time; decay energies agree across rates.
  {
    for (const double sr : {44100.0, 96000.0})
    {
      // Params before reset (snapped landing, not the tape-glide path).
      tdm::Delay d;
      d.setTimeMs(250.0f);
      d.setFeedback(0.0f);
      d.setEnabled(true);
      d.reset(sr);
      const int n = static_cast<int>(0.6 * sr);
      std::vector<float> in = impulse(n), wL(n, 0.0f), wR(n, 0.0f);
      d.processBlock(in.data(), wL.data(), wR.data(), n);
      const int want = static_cast<int>(250.0 / 1000.0 * sr);
      int first = -1;
      for (int i = static_cast<int>(0.05 * sr); i < n; ++i)
      {
        if (std::fabs(wL[static_cast<size_t>(i)]) > 0.1f)
        {
          first = i;
          break;
        }
      }
      TDM_CHECK(first >= want - 3 && first <= want + 3, "echo on Time across rates");
    }
    // Absolute tail energy differs by rate (echo density/timing), but the
    // decay SLOPE (mid/late ratio over the same 0.5 s span) must agree:
    // tank gains are retuned per rate for the same T60 by design.
    float slope[3] = {0.0f, 0.0f, 0.0f};
    const double rates[3] = {44100.0, 48000.0, 96000.0};
    for (int k = 0; k < 3; ++k)
    {
      tdm::SpaceProcessor s = makeSpace(rates[k], false, true);
      s.setReverbDecay(0.6f);
      s.setReverbMix(1.0f);
      const int n = static_cast<int>(1.5 * rates[k]);
      std::vector<float> in = impulse(n), l, r;
      runSpace(s, in, l, r);
      const float mid =
          windowEnergy(l.data(), static_cast<int>(0.5 * rates[k]), static_cast<int>(1.0 * rates[k]));
      const float late =
          windowEnergy(l.data(), static_cast<int>(1.0 * rates[k]), static_cast<int>(1.5 * rates[k]));
      TDM_CHECK(mid > 1e-6f, "rate tail alive before slope check");
      slope[k] = mid / late;
    }
    TDM_CHECK(slope[0] / slope[1] > 0.7f && slope[0] / slope[1] < 1.4f, "decay slope 44.1/48");
    TDM_CHECK(slope[2] / slope[1] > 0.7f && slope[2] / slope[1] < 1.4f, "decay slope 96/48");
  }
  // Rig integration: round-trip + clamps for all seven space params.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    TDM_CHECK(!rig.isDelayEnabled() && !rig.isReverbEnabled(), "rig space off by default");
    tdm::RigParams p;
    p.delayEnabled = true;
    p.delayTimeMs = 420.0f;
    p.delayFeedback = 0.4f;
    p.delayMix = 0.3f;
    p.reverbEnabled = true;
    p.reverbDecay = 0.55f;
    p.reverbMix = 0.22f;
    rig.setParams(p);
    const tdm::RigParams q = rig.params();
    TDM_CHECK(q.delayEnabled && q.delayTimeMs == 420.0f && q.delayFeedback == 0.4f && q.delayMix == 0.3f,
              "delay params round-trip");
    TDM_CHECK(q.reverbEnabled && q.reverbDecay == 0.55f && q.reverbMix == 0.22f, "reverb params round-trip");
    rig.setDelayTimeMs(5.0f);
    rig.setDelayFeedback(9.0f);
    rig.setDelayMix(-1.0f);
    rig.setReverbDecay(9.0f);
    rig.setReverbMix(-1.0f);
    TDM_CHECK_CLOSE(rig.delayTimeMs(), 20.0f, 1e-6f, "delay time clamps low");
    TDM_CHECK_CLOSE(rig.delayFeedback(), 0.85f, 1e-6f, "delay feedback clamps high");
    TDM_CHECK_CLOSE(rig.delayMix(), 0.0f, 1e-6f, "delay mix clamps low");
    TDM_CHECK_CLOSE(rig.reverbDecay(), 1.0f, 1e-6f, "reverb decay clamps high");
    TDM_CHECK_CLOSE(rig.reverbMix(), 0.0f, 1e-6f, "reverb mix clamps low");
  }
  // Rig integration: bypassed rig is bit-exact (legacy path preserved),
  // enabled mix-0 matches it, delay-on moves it, fold-down is exact.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    std::vector<float> in = sine(0.4f, 220.0f, 48000.0, 2048);
    for (int i = 0; i < 2048; ++i)
      in[static_cast<size_t>(i)] += 0.15f * std::sin(2.0f * (float)(kPi * 90.0 * i / 48000.0));
    const float* bi[1] = {in.data()};
    std::vector<float> m1(2048), m2(2048);
    float* b1[1] = {m1.data()};
    float* b2[1] = {m2.data()};
    rig.processBlock(bi, 1, b1, 1, 2048); // bypassed, mono
    rig.setDelayEnabled(true); // mix defaults 0.25... set explicit 0 below
    rig.setDelayMix(0.0f);
    rig.setReverbMix(0.0f);
    rig.processBlock(bi, 1, b2, 1, 2048);
    bool exact = true;
    for (int i = 0; i < 2048; ++i)
      exact = exact && m2[i] == m1[i];
    TDM_CHECK(exact, "enabled mix-0 matches bypassed rig");
    // 200 ms echo at 48 kHz lands at sample 9600: the buffer must extend
    // past it (a 2048-sample window only ever sees dry + silence).
    const int wn = 16384;
    std::vector<float> bigIn = sine(0.4f, 220.0f, 48000.0, wn);
    const float* bbi[1] = {bigIn.data()};
    auto runRigDelay = [&](float mix) {
      rig.reset(48000.0, 512);
      rig.setDelayEnabled(true);
      rig.setDelayTimeMs(200.0f);
      rig.setDelayFeedback(0.5f);
      rig.setDelayMix(mix);
      std::vector<float> out(wn);
      float* bo[1] = {out.data()};
      rig.processBlock(bbi, 1, bo, 1, wn);
      return out;
    };
    const std::vector<float> bigDry = runRigDelay(0.0f);
    const std::vector<float> wet1 = runRigDelay(1.0f);
    float maxDiff = 0.0f;
    for (int i = 0; i < wn; ++i)
      maxDiff = std::max(maxDiff, std::fabs(wet1[static_cast<size_t>(i)] - bigDry[static_cast<size_t>(i)]));
    TDM_CHECK(maxDiff > 0.01f, "delay-on moves the rig output");
    // Stereo: N=2 carries L/R; N=1 is the exact fold-down; N=3 cycles.
    // Each run starts from an identical reset (deterministic DSP), so the
    // three channel maps compare against the same stereo image.
    auto runStereoRig = [&](int numOut, float** outs) {
      rig.reset(48000.0, 512);
      rig.setDelayEnabled(true);
      rig.setDelayMix(1.0f);
      rig.setDelayTimeMs(200.0f);
      rig.setDelayFeedback(0.5f);
      std::vector<float> imp = impulse(2048);
      const float* ii[1] = {imp.data()};
      rig.processBlock(ii, 1, outs, numOut, 2048);
    };
    std::vector<float> s0(2048), s1(2048), f1(2048), c0(2048), c1(2048), c2(2048);
    float* o2[2] = {s0.data(), s1.data()};
    float* o1[1] = {f1.data()};
    float* o3[3] = {c0.data(), c1.data(), c2.data()};
    runStereoRig(2, o2);
    runStereoRig(1, o1);
    runStereoRig(3, o3);
    exact = true;
    for (int i = 0; i < 2048; ++i)
      exact = exact && f1[i] == (s0[i] + s1[i]) * 0.5f;
    TDM_CHECK(exact, "mono out is the exact fold-down");
    exact = true;
    for (int i = 0; i < 2048; ++i)
      exact = exact && c0[i] == s0[i] && c1[i] == s1[i] && c2[i] == s0[i];
    TDM_CHECK(exact, "extra channels cycle the stereo pair");
  }
  // Rig integration: trim still works post-space (DC gains compose).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setDelayEnabled(true); // mix 0 default path stays dry...
    rig.setDelayMix(0.0f);
    rig.setOutputTrimDb(6.0206f);
    std::vector<float> dc(512, 0.25f), o(512);
    const float* di[1] = {dc.data()};
    float* dob[1] = {o.data()};
    for (int b = 0; b < 200; ++b)
      rig.processBlock(di, 1, dob, 1, 512);
    TDM_CHECK_CLOSE(o[511], 0.5f, 1e-3f, "trim post-space still exact");
  }
}

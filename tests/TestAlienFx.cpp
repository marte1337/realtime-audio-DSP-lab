// Alien FX lab tests: frequency shifter (SSB correctness, rejection,
// bypass, stability), laser, warp delay, ring mod. Housed in one file
// while the lab is small; split if any section outgrows it.

#include "tests/Assert.h"

#include <cmath>
#include <string>
#include <vector>

#include "dsp/lab/Alien/AlienDoppler.h"
#include "dsp/lab/Alien/AlienLaser.h"
#include "dsp/lab/Alien/AlienRing.h"
#include "dsp/lab/Alien/AlienShift.h"
#include "dsp/lab/Alien/AlienWarp.h"
#include "dsp/lab/Alien/DevAlien.h"
#include "dsp/TechDeathRig.h"

namespace
{
constexpr double kSr = 48000.0;

double goertzel(const std::vector<float>& x, int a, int b, double f)
{
  const double w = 2.0 * 3.141592653589793 * f / kSr;
  const double c = 2.0 * std::cos(w);
  double s0 = 0, s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i)
  {
    s0 = x[(size_t)i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / ((b - a) / 2.0);
}

std::vector<float> renderShift(float f0, float shiftHz, double seconds)
{
  tdm::alien::AlienShift fx;
  fx.reset(kSr);
  fx.setShiftHz(shiftHz);
  fx.setMix(1.0f);
  const int n = static_cast<int>(seconds * kSr);
  std::vector<float> y(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    y[(size_t)i] = fx.process(0.5f * std::sin(2.0f * 3.14159265f * f0 * i / (float)kSr));
  return y;
}

double rejDb(const std::vector<float>& y, double fWant, double fUnwant)
{
  const int n = static_cast<int>(y.size());
  const int a = n / 2; // settled half
  const double w = goertzel(y, a, n, fWant);
  const double u = goertzel(y, a, n, fUnwant);
  return 20.0 * std::log10(w / (u + 1e-9));
}

double zcFreq(const std::vector<float>& x, int a, int b)
{
  int n = 0, first = -1, last = -1;
  for (int i = a + 1; i < b; ++i)
  {
    if (x[(size_t)(i - 1)] <= 0.0f && x[(size_t)i] > 0.0f)
    {
      if (first < 0)
        first = i;
      last = i;
      ++n;
    }
  }
  if (n < 2 || last <= first)
    return 0.0;
  return (n - 1) * kSr / (last - first);
}

float maxStep(const std::vector<float>& x)
{
  float m = 0.0f;
  for (size_t i = 1; i < x.size(); ++i)
    m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

std::vector<float> sine(float amp, float f, int n)
{
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[(size_t)i] = amp * std::sin(2.0f * 3.14159265f * f * i / (float)kSr);
  return x;
}

bool bitEqual(const float* a, const float* b, int n)
{
  for (int i = 0; i < n; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

void runDev(tdm::alien::DevAlien& d, std::vector<float>& io)
{
  const int n = static_cast<int>(io.size());
  for (int off = 0; off < n; off += 256)
  {
    const int m = (n - off < 256) ? (n - off) : 256;
    float* p = io.data() + off;
    d.process(p, p, m);
  }
}

void rigRun(tdm::TechDeathRig& rig, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  const int n = static_cast<int>(in.size());
  for (int off = 0; off < n; off += 256)
  {
    const int m = (n - off < 256) ? (n - off) : 256;
    const float* bi[1] = {in.data() + off};
    float* bo[1] = {out.data() + off};
    rig.processBlock(bi, 1, bo, 1, m);
  }
}

} // namespace

void runAlienFxTests()
{
  // A. Frequency shifter: true single-sideband shifting.
  {
    // Direction: +shift moves energy UP, -shift moves it DOWN.
    auto up = renderShift(440.0f, 150.0f, 1.0);
    TDM_CHECK(rejDb(up, 590.0, 290.0) > 40.0, "shift +150 lands up, unwanted down 40 dB");
    auto down = renderShift(440.0f, -150.0f, 1.0);
    TDM_CHECK(rejDb(down, 290.0, 590.0) > 40.0, "shift -150 lands down 40 dB");
    // Unity gain on the wanted sideband (oscillator + Hilbert preserve level).
    TDM_CHECK(std::fabs(goertzel(up, 24000, 48000, 590.0) - 0.5) < 0.02, "shift unity level");
    // Small and large offsets behave the same way.
    TDM_CHECK(rejDb(renderShift(440.0f, 30.0f, 1.0), 470.0, 410.0) > 40.0, "shift +30");
    TDM_CHECK(rejDb(renderShift(440.0f, 600.0f, 1.0), 1040.0, 160.0) > 40.0, "shift +600");
    // Guitar low end: wanted sideband still wins (Hilbert degrades near DC).
    TDM_CHECK(rejDb(renderShift(82.0f, 150.0f, 2.0), 232.0, 68.0) > 20.0, "shift 82 Hz usable");
    TDM_CHECK(rejDb(renderShift(62.0f, 150.0f, 2.0), 212.0, 88.0) > 12.0, "shift 62 Hz usable");
  }
  // A2. Shifter house behavior: bypass, extremes, stability, rates.
  {
    // Mix 0 is bit-exact dry once the mix smoother settles (wet fully out).
    tdm::alien::AlienShift fx;
    fx.reset(kSr);
    fx.setShiftHz(600.0f);
    fx.setMix(0.0f);
    for (int i = 0; i < 48000; ++i) // 1 s: smoothers fully settled
      fx.process(0.0f);
    bool exact = true;
    for (int i = 0; i < 5000; ++i)
    {
      const float x = 0.7f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr);
      if (fx.process(x) != x)
        exact = false;
    }
    TDM_CHECK(exact, "shift mix-0 bit-exact dry");
    // Finite output under extremes: max shift, hot input, dense spectrum.
    tdm::alien::AlienShift hot;
    hot.reset(kSr);
    hot.setShiftHz(2000.0f);
    hot.setMix(1.0f);
    std::vector<float> y(48000);
    for (int i = 0; i < 48000; ++i)
    {
      const float t = i / (float)kSr;
      const float x = 0.9f * std::sin(2.0f * 3.14159265f * 55.0f * t)
          * std::sin(2.0f * 3.14159265f * 997.0f * t);
      y[(size_t)i] = hot.process(x);
    }
    TDM_CHECK(tdm_test::allFinite(y.data(), 48000), "shift finite under extremes");
    TDM_CHECK(tdm_test::peakAbs(y.data(), 48000) < 2.0f, "shift bounded under extremes");
    // Silence in -> silence out (no oscillator bleed at mix 1).
    tdm::alien::AlienShift sil;
    sil.reset(kSr);
    sil.setShiftHz(150.0f);
    sil.setMix(1.0f);
    float peak = 0.0f;
    for (int i = 0; i < 4800; ++i)
      peak = std::max(peak, std::fabs(sil.process(0.0f)));
    TDM_CHECK(peak == 0.0f, "shift silent on silence");
    // Second sample rate runs finite.
    tdm::alien::AlienShift fx44;
    fx44.reset(44100.0);
    fx44.setShiftHz(-75.0f);
    fx44.setMix(1.0f);
    std::vector<float> y44(44100);
    for (int i = 0; i < 44100; ++i)
      y44[(size_t)i] = fx44.process(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f));
    TDM_CHECK(tdm_test::allFinite(y44.data(), 44100), "shift finite at 44.1k");
  }

  // B. Doppler laser: triggered chirps move pitch, cyclic wobbles, no clicks.
  {
    using Dir = tdm::alien::AlienLaser::Direction;
    auto chirp = [](Dir d) {
      tdm::alien::AlienLaser fx;
      fx.reset(kSr);
      fx.setDirection(d);
      fx.setSweepMs(120.0f);
      fx.setDepthMs(12.0f);
      fx.setMix(1.0f);
      fx.trigger();
      std::vector<float> y(static_cast<size_t>(0.5 * kSr));
      for (size_t i = 0; i < y.size(); ++i)
        y[i] = fx.process(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / (float)kSr));
      return y;
    };
    // Out-phase window: first 35% of 120 ms, past the base-delay transient.
    auto down = chirp(Dir::Down);
    const double fDown = zcFreq(down, (int)(0.005 * kSr), (int)(0.040 * kSr));
    TDM_CHECK(fDown < 400.0 && fDown > 200.0, "laser down-chirp dives");
    auto up = chirp(Dir::Up);
    const double fUp = zcFreq(up, (int)(0.005 * kSr), (int)(0.040 * kSr));
    TDM_CHECK(fUp > 480.0 && fUp < 700.0, "laser up-chirp rises");
    // Continuous delay => continuous output (sine-slope bound at 600 Hz).
    TDM_CHECK(maxStep(down) < 0.06f, "laser down click-free");
    TDM_CHECK(maxStep(up) < 0.06f, "laser up click-free");
    // Cyclic triangle: rise flank flat, fall flank sharp (aligned windows).
    tdm::alien::AlienLaser cyc;
    cyc.reset(kSr);
    cyc.setDirection(Dir::Cycle);
    cyc.setSweepMs(250.0f);
    cyc.setDepthMs(20.0f);
    cyc.setMix(1.0f);
    std::vector<float> yc(static_cast<size_t>(kSr));
    for (size_t i = 0; i < yc.size(); ++i)
      yc[i] = cyc.process(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / (float)kSr));
    const double fRise = zcFreq(yc, (int)(0.05 * kSr), (int)(0.20 * kSr));
    const double fFall = zcFreq(yc, (int)(0.30 * kSr), (int)(0.45 * kSr));
    TDM_CHECK(fRise < 425.0 && fFall > 455.0, "laser cycle wobbles both ways");
    TDM_CHECK(maxStep(yc) < 0.06f, "laser cycle click-free at turnarounds");
    // Retrigger mid-chirp: finite, bounded, no step.
    tdm::alien::AlienLaser rt;
    rt.reset(kSr);
    rt.setDirection(Dir::Down);
    rt.setSweepMs(250.0f);
    rt.setDepthMs(40.0f);
    rt.setMix(1.0f);
    rt.trigger();
    std::vector<float> yr(static_cast<size_t>(kSr));
    for (size_t i = 0; i < yr.size(); ++i)
    {
      if (i == (size_t)(0.1 * kSr))
        rt.trigger(); // mid-chirp restart
      yr[i] = rt.process(0.5f * std::sin(2.0f * 3.14159265f * 220.0f * i / (float)kSr));
    }
    TDM_CHECK(tdm_test::allFinite(yr.data(), (int)yr.size()), "laser retrigger finite");
    // Continuity under the 4 ms return zip: 220 Hz dry slope (0.0144) times
    // (1 + peak return slope ~16x for 40 ms excursion) ~= 0.25. A true
    // delay jump would step ~0.5+, so 0.3 separates zip from click.
    TDM_CHECK(maxStep(yr) < 0.3f, "laser retrigger click-free");
  }
  // B2. Laser house behavior: bypass, extremes, silence.
  {
    using Dir = tdm::alien::AlienLaser::Direction;
    tdm::alien::AlienLaser fx;
    fx.reset(kSr);
    fx.setDirection(Dir::Cycle);
    fx.setMix(0.0f);
    for (int i = 0; i < 48000; ++i)
      fx.process(0.0f);
    bool exact = true;
    for (int i = 0; i < 5000; ++i)
    {
      const float x = 0.7f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr);
      if (fx.process(x) != x)
        exact = false;
    }
    TDM_CHECK(exact, "laser mix-0 bit-exact dry");
    // Extreme: fastest sweep, max depth, max feedback + resonance.
    tdm::alien::AlienLaser hot;
    hot.reset(kSr);
    hot.setDirection(Dir::Down);
    hot.setSweepMs(30.0f);
    hot.setDepthMs(80.0f);
    hot.setFeedback(0.7f);
    hot.setReso(8.0f);
    hot.setMix(1.0f);
    hot.trigger();
    std::vector<float> yh(48000);
    for (int i = 0; i < 48000; ++i)
      yh[(size_t)i] =
          hot.process(0.9f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr));
    TDM_CHECK(tdm_test::allFinite(yh.data(), 48000), "laser finite at extremes");
    TDM_CHECK(tdm_test::peakAbs(yh.data(), 48000) < 3.0f, "laser bounded at extremes");
    // Silence in -> silence out.
    tdm::alien::AlienLaser sil;
    sil.reset(kSr);
    sil.setDirection(Dir::Down);
    sil.setMix(1.0f);
    sil.trigger();
    float peak = 0.0f;
    for (int i = 0; i < 48000; ++i)
      peak = std::max(peak, std::fabs(sil.process(0.0f)));
    TDM_CHECK(peak == 0.0f, "laser silent on silence");
  }

  // C0. Doppler core: musical ratios within the documented slip budget.
  {
    auto ratioOut = [](float st) {
      tdm::alien::AlienDoppler d;
      d.reset(kSr);
      d.setSemitones(st);
      std::vector<float> y(static_cast<size_t>(kSr));
      for (size_t i = 0; i < y.size(); ++i)
        y[i] = d.process(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / (float)kSr));
      return y;
    };
    // Small intervals land tight (slip << semitone).
    TDM_CHECK(std::fabs(zcFreq(ratioOut(3.0f), 24000, 48000) - 523.25) < 12.0,
              "doppler +3 st");
    TDM_CHECK(std::fabs(zcFreq(ratioOut(-7.0f), 24000, 48000) - 293.66) < 15.0,
              "doppler -7 st");
    // Extremes sag within the documented ~1 st budget (phase slip).
    TDM_CHECK(std::fabs(zcFreq(ratioOut(-12.0f), 24000, 48000) - 220.0) < 25.0,
              "doppler -12 st in budget");
    TDM_CHECK(std::fabs(zcFreq(ratioOut(12.0f), 24000, 48000) - 880.0) < 50.0,
              "doppler +12 st in budget");
    // Unity target settles to bit-exact dry.
    tdm::alien::AlienDoppler u;
    u.reset(kSr);
    u.setSemitones(0.0f);
    for (int i = 0; i < 48000; ++i)
      u.process(0.0f);
    bool exact = true;
    for (int i = 0; i < 5000; ++i)
    {
      const float x = 0.7f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr);
      if (u.process(x) != x)
        exact = false;
    }
    TDM_CHECK(exact, "doppler unity bit-exact dry");
  }
  // C. Warp delay: repeats actually evolve; loop stays bounded.
  {
    using Mode = tdm::alien::AlienWarp::Mode;
    auto blip = [](tdm::alien::AlienWarp& w, double seconds) {
      const int n = static_cast<int>(seconds * kSr);
      std::vector<float> y(static_cast<size_t>(n));
      const int blen = static_cast<int>(0.2 * kSr);
      for (int i = 0; i < n; ++i)
      {
        const float x = (i < blen)
            ? 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / (float)kSr)
            : 0.0f;
        y[(size_t)i] = w.process(x);
      }
      return y;
    };
    // FreqWarp +100 Hz: repeats carry 540/640/740/840 (repeat k lands at
    // k*(150+10.6) ms: 10.6 ms in-loop shifter latency stretches spacing).
    tdm::alien::AlienWarp fw;
    fw.reset(kSr);
    fw.setMode(Mode::FreqWarp);
    fw.setTimeMs(150.0f);
    fw.setFeedback(0.7f);
    fw.setShiftHz(100.0f);
    fw.setMix(1.0f);
    auto yf = blip(fw, 2.5);
    for (int k = 1; k <= 4; ++k)
    {
      const int c = static_cast<int>(k * 0.1606 * kSr);
      const double want = 440.0 + k * 100.0;
      const double got = goertzel(yf, c + (int)(0.02 * kSr), c + (int)(0.12 * kSr), want);
      TDM_CHECK(got > 0.05, "freqwarp repeat carries +100 Hz steps");
      (void)got;
    }
    TDM_CHECK(tdm_test::allFinite(yf.data(), (int)yf.size()), "freqwarp finite");
    // PitchWarp +3 st: repeats ascend ~523/622/740 (repeat k lands at
    // k*(150+35) ms through the in-loop Doppler).
    tdm::alien::AlienWarp pw;
    pw.reset(kSr);
    pw.setMode(Mode::PitchWarp);
    pw.setTimeMs(150.0f);
    pw.setFeedback(0.7f);
    pw.setSemitones(3.0f);
    pw.setMix(1.0f);
    auto yp = blip(pw, 3.0);
    for (int k = 1; k <= 3; ++k)
    {
      const int c = static_cast<int>(k * 0.185 * kSr);
      const double f = zcFreq(yp, c + (int)(0.02 * kSr), c + (int)(0.12 * kSr));
      const double want = 440.0 * std::pow(2.0, 3.0 * k / 12.0);
      TDM_CHECK(std::fabs(f - want) < want * 0.05, "pitchwarp repeat ascends +3 st steps");
    }
    TDM_CHECK(tdm_test::allFinite(yp.data(), (int)yp.size()), "pitchwarp finite");
    TDM_CHECK(tdm_test::peakAbs(yp.data(), (int)yp.size()) < 2.0f, "pitchwarp bounded");
    // Feedback stress: max fb, hot input, long run — bounded, finite.
    tdm::alien::AlienWarp hot;
    hot.reset(kSr);
    hot.setMode(Mode::FreqWarp);
    hot.setTimeMs(100.0f);
    hot.setFeedback(0.9f);
    hot.setShiftHz(-200.0f);
    hot.setMix(1.0f);
    std::vector<float> yh(static_cast<size_t>(5 * kSr));
    for (size_t i = 0; i < yh.size(); ++i)
    {
      const float t = i / (float)kSr;
      yh[i] = hot.process(0.9f * std::sin(2.0f * 3.14159265f * 82.0f * t));
    }
    TDM_CHECK(tdm_test::allFinite(yh.data(), (int)yh.size()), "warp finite at extremes");
    TDM_CHECK(tdm_test::peakAbs(yh.data(), (int)yh.size()) < 3.0f, "warp bounded at extremes");
    // Mix 0 is bit-exact dry once settled.
    tdm::alien::AlienWarp bp;
    bp.reset(kSr);
    bp.setMix(0.0f);
    for (int i = 0; i < 48000; ++i)
      bp.process(0.0f);
    bool exact = true;
    for (int i = 0; i < 5000; ++i)
    {
      const float x = 0.7f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr);
      if (bp.process(x) != x)
        exact = false;
    }
    TDM_CHECK(exact, "warp mix-0 bit-exact dry");
  }
  // D. Ring mod: both sidebands present (vs the shifter's single one).
  {
    tdm::alien::AlienRing r;
    r.reset(kSr);
    r.setCarrierHz(150.0f);
    r.setMix(1.0f);
    const int n = static_cast<int>(kSr);
    std::vector<float> y(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      y[(size_t)i] = r.process(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / (float)kSr));
    // 440 x 150 Hz sine: sum 590 + difference 290 at 0.25 each.
    TDM_CHECK(std::fabs(goertzel(y, 24000, n, 590.0) - 0.25) < 0.02, "ring sum sideband");
    TDM_CHECK(std::fabs(goertzel(y, 24000, n, 290.0) - 0.25) < 0.02, "ring diff sideband");
    TDM_CHECK(goertzel(y, 24000, n, 440.0) < 0.02, "ring suppresses carrier input");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "ring finite");
  }

  // E. DevAlien router: defaults, transparency, adoption, click-free
  // switching, rig-seam install/remove.
  using tdm::alien::DevAlien;
  using AlienEffect = DevAlien::Effect;
  {
    DevAlien d;
    TDM_CHECK(!d.isEnabled(), "alien default off");
    TDM_CHECK(d.effect() == AlienEffect::Frequency, "alien default frequency");
  }
  // Off is transparent (tap-level + full rig with seam installed).
  {
    auto sig = sine(0.5f, 440.0f, static_cast<int>(kSr));
    DevAlien d;
    d.reset(kSr, 256);
    std::vector<float> io = sig;
    runDev(d, io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "alien off transparent");

    tdm::TechDeathRig dry, wet;
    dry.reset(kSr, 256);
    wet.reset(kSr, 256);
    DevAlien ds;
    wet.setAlienPostIr(&ds);
    wet.reset(kSr, 256); // rig forwards reset to the alien tap
    std::vector<float> o1, o2;
    rigRun(dry, sig, o1);
    rigRun(wet, sig, o2);
    TDM_CHECK(bitEqual(o1.data(), o2.data(), (int)sig.size()), "alien off rig transparent");
  }
  // Mix 0 is transparent for every effect (enabled).
  for (AlienEffect e : {AlienEffect::Frequency, AlienEffect::Laser, AlienEffect::Warp})
  {
    auto sig = sine(0.5f, 330.0f, static_cast<int>(kSr));
    DevAlien d;
    d.setEffect(e);
    d.setMix(0.0f);
    if (e == AlienEffect::Laser)
      d.setLaserDir(2); // cycle: worst-case mover, still dry at mix 0
    d.setEnabled(true);
    d.reset(kSr, 256);
    std::vector<float> io = sig;
    runDev(d, io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "alien mix-0 transparent");
  }
  // Param adoption: frequency +150 moves 440 -> 590 through the router.
  {
    auto sig = sine(0.5f, 440.0f, static_cast<int>(2 * kSr));
    DevAlien d;
    d.setEffect(AlienEffect::Frequency);
    d.setShiftHz(150.0f);
    d.setMix(1.0f);
    d.setEnabled(true);
    d.reset(kSr, 256);
    std::vector<float> io = sig;
    runDev(d, io);
    const int n = static_cast<int>(io.size());
    TDM_CHECK(std::fabs(goertzel(io, n / 2, n, 590.0) - 0.5) < 0.05, "alien router shift adopts");
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "alien router shift finite");
  }
  // Each effect renders non-dry output when enabled at mix 1.
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    for (AlienEffect e : {AlienEffect::Frequency, AlienEffect::Laser, AlienEffect::Warp})
    {
      DevAlien d;
      d.setEffect(e);
      d.setShiftHz(150.0f);
      d.setLaserDir(2);
      d.setMix(1.0f);
      d.setEnabled(true);
      d.reset(kSr, 256);
      std::vector<float> io = sig;
      runDev(d, io);
      float dev = 0.0f;
      for (size_t i = 0; i < io.size(); ++i)
        dev = std::max(dev, std::fabs(io[i] - sig[i]));
      TDM_CHECK(dev > 0.01f, "alien effect renders wet");
      TDM_CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "alien effect finite");
    }
  }
  // Effect switching + enable cycling is click-free on steady tone.
  {
    auto sig = sine(0.4f, 440.0f, static_cast<int>(4 * kSr));
    DevAlien d;
    d.setEnabled(true);
    d.reset(kSr, 256);
    std::vector<float> io = sig;
    const int q = static_cast<int>(io.size()) / 4;
    for (int seg = 0; seg < 4; ++seg)
    {
      d.setEffect(static_cast<AlienEffect>(seg % 3));
      std::vector<float> part(io.begin() + seg * q, io.begin() + (seg + 1) * q);
      // router processes in place per block; reassemble below
      runDev(d, part);
      std::copy(part.begin(), part.end(), io.begin() + seg * q);
    }
    TDM_CHECK(maxStep(io) < 0.1f, "alien switching click-free"); // pre-roll pins this
    TDM_CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "alien switching finite");
    // enable off/on cycle: dry at the bottom, wet again after.
    d.setEnabled(false);
    std::vector<float> tail = sine(0.4f, 440.0f, static_cast<int>(kSr));
    std::vector<float> dryRef = tail;
    runDev(d, tail);
    // past the 64-sample dip-out the tail is bit-exact dry
    TDM_CHECK(bitEqual(tail.data() + 256, dryRef.data() + 256, (int)tail.size() - 256),
              "alien disable returns dry");
    d.setEnabled(true);
    runDev(d, tail);
    float dev = 0.0f;
    for (size_t i = 0; i < tail.size(); ++i)
      dev = std::max(dev, std::fabs(tail[i] - dryRef[i]));
    TDM_CHECK(dev > 0.01f, "alien re-enable renders wet");
  }
  // Rig seam: install -> wet; remove (nullptr) -> dry again.
  {
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    std::vector<float> oDry, oWet, oBack;
    rigRun(rig, sig, oDry);
    DevAlien ds;
    ds.setShiftHz(150.0f);
    ds.setEnabled(true);
    rig.setAlienPostIr(&ds);
    rig.reset(kSr, 256);
    rigRun(rig, sig, oWet);
    float dev = 0.0f;
    for (size_t i = 0; i < oWet.size(); ++i)
      dev = std::max(dev, std::fabs(oWet[i] - oDry[i]));
    TDM_CHECK(dev > 0.01f, "alien rig seam renders wet");
    rig.setAlienPostIr(nullptr);
    rig.reset(kSr, 256);
    rigRun(rig, sig, oBack);
    TDM_CHECK(bitEqual(oBack.data(), oDry.data(), (int)sig.size()), "alien rig seam removal dry");
  }
  // Hostile UI chatter: rapid toggles stay finite and click-safe.
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(2 * kSr));
    DevAlien d;
    d.setEnabled(true);
    d.reset(kSr, 256);
    std::vector<float> io = sig;
    const int n = static_cast<int>(io.size());
    for (int off = 0; off < n; off += 128)
    {
      d.setEffect(static_cast<AlienEffect>((off / 128) % 3));
      d.setMix(((off / 128) % 2 == 0) ? 1.0f : 0.2f);
      const int m = (n - off < 128) ? (n - off) : 128;
      float* p = io.data() + off;
      d.process(p, p, m);
    }
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "alien hostile switching finite");
    TDM_CHECK(maxStep(io) < 0.3f, "alien hostile switching click-safe");
  }
  // Mix dropped mid-rise glides down (never snaps m to target). Rise
  // is entered from each dip bottom (effect switch); the drop lands at
  // varied offsets so no single coincidence zero can hide the snap.
  {
    auto sig = sine(0.4f, 440.0f, static_cast<int>(4 * kSr));
    DevAlien d;
    d.setShiftHz(150.0f);
    d.reset(kSr, 256);
    d.setEnabled(true);
    std::vector<float> io = sig;
    int off = 0;
    auto run = [&](int count) {
      for (int k = 0; k < count; k += 32)
      {
        float* p = io.data() + off;
        d.process(p, p, 32);
        off += 32;
      }
    };
    run(2048); // settle Steady at m=1
    for (int k = 0; k < 8; ++k)
    {
      d.setEffect((k % 2 == 0) ? AlienEffect::Laser : AlienEffect::Frequency);
      run(64 + 40 + 16 * k); // dip (64) + varied rise offsets
      d.setMix(0.0f);        // mid-rise drop
      run(400);
      d.setMix(1.0f);
      run(1200); // settle back to Steady m=1
    }
    io.resize(static_cast<size_t>(off)); // measure the processed prefix only
    TDM_CHECK(maxStep(io) < 0.1f, "alien mix-drop mid-rise glides");
    TDM_CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "alien mix-drop mid-rise finite");
  }
}

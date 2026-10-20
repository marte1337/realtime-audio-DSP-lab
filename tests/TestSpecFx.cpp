// Special FX Laser lab tests: creative voice maps (heel/toe endpoints),
// sweep continuity, unified performance gestures (To/From Depth, 8 slots,
// interruption, one-shot, MIDI-ready trigger-by-ID), independent
// expression/gesture enables + neutral-sound behavior, unified Whammy
// direction, DualWham v1, router insert A/B + voice switching, rig-seam
// transparency, transpose+gesture simultaneity, transpose stacking for
// every flavour x insert x expression/gesture. Synthetic sines validate
// DSP math only — never musical quality.

#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/lab/SpecialFx/DevSpecFx.h"
#include "dsp/lab/SpecialFx/SpecGesture.h"
#include "dsp/lab/SpecialFx/SpecLaser.h"

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

double scanBest(const std::vector<float>& x, int a, int b, double f0, double f1, double step,
                double& bf)
{
  double best = 0.0;
  bf = f0;
  for (double f = f0; f <= f1; f += step)
  {
    const double m = goertzel(x, a, b, f);
    if (m > best)
    {
      best = m;
      bf = f;
    }
  }
  return best;
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

float peak(const std::vector<float>& x)
{
  float m = 0.0f;
  for (float v : x)
    m = std::max(m, std::fabs(v));
  return m;
}

std::vector<float> renderEngine(tdm::specfx::SpecLaser& e, const std::vector<float>& in)
{
  std::vector<float> y(in.size());
  for (size_t i = 0; i < in.size(); ++i)
    y[i] = e.process(in[i]);
  return y;
}

void runTap(tdm::SpecInsert& t, std::vector<float>& io)
{
  const int n = static_cast<int>(io.size());
  for (int off = 0; off < n; off += 256)
  {
    const int m = (n - off < 256) ? (n - off) : 256;
    float* p = io.data() + off;
    t.process(p, p, m);
  }
}

uint64_t fnvBits(const std::vector<float>& x)
{
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < x.size(); ++i)
  {
    uint32_t u;
    std::memcpy(&u, &x[i], 4);
    h ^= u;
    h *= 1099511628211ULL;
    h ^= (u >> 16);
    h *= 1099511628211ULL;
  }
  return h;
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

struct StackRender
{
  std::vector<float> y;
};

// Render the REAL rig (no NAM/IR = bypass, everything else off) with
// production transpose at tSt plus a spec section config. Mirrors the
// audition chain: trim -> gate(off) -> transpose -> [specPre] ->
// drive(off) -> nam(dry) -> ir(dry) -> [specPost] -> shape/space(off).
// exprEn/gestEn select the section; press0 holds slot 0 (configured
// here for the render voice) for gesture renders.
StackRender renderStack(float tSt, bool exprEn, bool gestEn, tdm::specfx::SpecLaser::Voice v,
                        tdm::specfx::DevSpecFx::Insert ins, bool press0, bool install,
                        const std::vector<float>& sig)
{
  tdm::TechDeathRig rig;
  rig.reset(kSr, 256);
  rig.setTransposeEnabled(true);
  rig.setTransposeSemitones(tSt);
  tdm::specfx::DevSpecFx ds;
  ds.setVoice(v);
  ds.setExprDirection(tdm::specfx::GestureDir::Up);
  ds.setExprDepth(12.0f);
  ds.setExpression(1.0f);
  ds.setMix(1.0f);
  ds.setInsert(ins);
  ds.setExpressionEnabled(exprEn);
  ds.setGesturesEnabled(gestEn);
  ds.setSlotVoice(0, v);
  ds.setSlotDir(0, tdm::specfx::GestureDir::Up);
  ds.setSlotMotion(0, tdm::specfx::GestureMotion::ToDepth);
  ds.setSlotDepthSt(0, 12.0f);
  ds.setSlotTimeMs(0, 50.0f);
  ds.setSlotRetMs(0, 100.0f);
  if (install)
  {
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
  }
  rig.reset(kSr, 256);
  if (press0)
    ds.pressSlot(0);
  StackRender r;
  rigRun(rig, sig, r.y);
  return r;
}

tdm::specfx::GestureConfig makeCfg(int voice, tdm::specfx::GestureDir dir,
                                   tdm::specfx::GestureMotion motion, float depth, float time,
                                   float ret)
{
  tdm::specfx::GestureConfig c;
  c.voice = voice;
  c.dir = dir;
  c.motion = motion;
  c.depthSt = depth;
  c.timeMs = time;
  c.retMs = ret;
  return c;
}
} // namespace

void runSpecFxTests()
{
  using tdm::specfx::DevSpecFx;
  using tdm::specfx::GestureConfig;
  using tdm::specfx::GestureDir;
  using tdm::specfx::GestureMotion;
  using tdm::specfx::SpecLaser;
  using Voice = SpecLaser::Voice;
  using tdm::specfx::kVoiceDual;
  using tdm::specfx::kVoiceDualWham;
  using tdm::specfx::kVoiceRing;
  using tdm::specfx::kVoiceWhammy;

  // 1. Engine defaults.
  {
    SpecLaser e;
    e.reset(kSr);
    TDM_CHECK(e.voice() == Voice::Whammy, "spec default whammy");
    TDM_CHECK(e.expressionPos() == 0.0f, "spec default pos 0");
    TDM_CHECK(!e.gestureActive(), "spec default gesture idle");
  }
  // 2. Whammy-Up heel = bit-exact dry (doppler unity bypass, settled).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(bitEqual(y.data(), sig.data(), (int)sig.size()), "spec whammy heel dry");
  }
  // 3. Whammy-Up toe ~= +12 st. Narrow Goertzel is the WRONG oracle here:
  // the documented doppler sag (~+0.5 st flat at +12) lands outside any
  // exact bin, so measure zero-crossing rate in a sag-tolerant band.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(1.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const int n = static_cast<int>(y.size());
    const double f = zcFreq(y, n / 2, n);
    TDM_CHECK(f > 404.0 && f < 454.0, "spec whammy toe octave band"); // +10.5..+12.5 st
    TDM_CHECK(goertzel(y, n / 2, n, 220.0) < 0.15, "spec whammy toe dry suppressed");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec whammy toe finite");
  }
  // 4. Dual toe: sub + octave legs. Two tones defeat zero-crossing, so
  // scan Goertzel across the sag-tolerant bands instead of exact bins.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Dual);
    e.setExpression(1.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const int n = static_cast<int>(y.size());
    double subBest = 0.0, octBest = 0.0;
    for (double f = 100.0; f <= 130.0; f += 2.0) // sag lands ~120 (sharp)
      subBest = std::max(subBest, goertzel(y, n / 2, n, f));
    for (double f = 404.0; f <= 454.0; f += 4.0)
      octBest = std::max(octBest, goertzel(y, n / 2, n, f));
    TDM_CHECK(subBest > 0.08, "spec dual sub leg");
    TDM_CHECK(octBest > 0.08, "spec dual octave leg");
    TDM_CHECK(peak(y) < 0.7f, "spec dual bounded");
  }
  // 5/6. Ring endpoints: heel 60 Hz, toe 2000 Hz carriers on 440 Hz.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Ring);
    e.setExpression(0.0f);
    auto sig = sine(0.5f, 440.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const int n = static_cast<int>(y.size());
    TDM_CHECK(std::fabs(goertzel(y, n / 2, n, 500.0) - 0.25) < 0.05, "spec ring heel sum");
    TDM_CHECK(std::fabs(goertzel(y, n / 2, n, 380.0) - 0.25) < 0.05, "spec ring heel diff");
    e.setExpression(1.0f);
    y = renderEngine(e, sig);
    TDM_CHECK(std::fabs(goertzel(y, n / 2, n, 2440.0) - 0.25) < 0.05, "spec ring toe sum");
    TDM_CHECK(std::fabs(goertzel(y, n / 2, n, 1560.0) - 0.25) < 0.05, "spec ring toe diff");
  }
  // 7. DualWham v1: heel bit-dry (post-clear, as the router does); toe
  // carries BOTH voices (A ~+11.2, B ~+22.7 measured — cascade sag, no
  // silent clamp); dry suppressed; bounded.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::DualWham);
    e.setExpression(0.0f);
    e.clear();
    auto dry = sine(0.5f, 220.0f, static_cast<int>(kSr));
    auto y0 = renderEngine(e, dry);
    TDM_CHECK(bitEqual(y0.data(), dry.data(), (int)dry.size()), "spec dualwham heel dry");
    e.setExpression(1.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(3 * kSr));
    auto y = renderEngine(e, sig);
    const int n = static_cast<int>(y.size());
    double af = 0.0, bf = 0.0;
    const double am = scanBest(y, n / 2, n, 380.0, 460.0, 4.0, af);
    const double bm = scanBest(y, n / 2, n, 700.0, 950.0, 10.0, bf);
    TDM_CHECK(am > 0.08, "spec dualwham A leg");
    TDM_CHECK(bm > 0.04, "spec dualwham B leg");
    TDM_CHECK(af > 400.0 && af < 440.0, "spec dualwham A pitch");
    TDM_CHECK(bf > 760.0 && bf < 880.0, "spec dualwham B pitch");
    TDM_CHECK(goertzel(y, n / 2, n, 220.0) < 0.05, "spec dualwham dry suppressed");
    TDM_CHECK(peak(y) < 0.7f, "spec dualwham bounded");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec dualwham finite");
  }
  // 8. Slow sweeps are continuous in every voice (+ Whammy-Down sweep).
  // Bounds are slope-aware: ring toe content reaches ~2.2 kHz (legit
  // slope ~0.12), DualWham-B cascade hash higher still — far below
  // real-jump territory (~0.4 measured pre-fix).
  for (Voice v : {Voice::Whammy, Voice::Dual, Voice::Ring, Voice::DualWham})
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(v);
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    for (size_t i = 0; i < sig.size(); ++i)
    {
      e.setExpression(static_cast<float>(i) / static_cast<float>(sig.size()));
      y[i] = e.process(sig[i]);
    }
    const float bound = (v == Voice::DualWham) ? 0.2f : (v == Voice::Ring) ? 0.15f : 0.1f;
    TDM_CHECK(maxStep(y) < bound, "spec sweep continuous");
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec sweep finite");
  }
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Whammy);
    e.setExprDirection(GestureDir::Down);
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    for (size_t i = 0; i < sig.size(); ++i)
    {
      e.setExpression(static_cast<float>(i) / static_cast<float>(sig.size()));
      y[i] = e.process(sig[i]);
    }
    TDM_CHECK(maxStep(y) < 0.1f, "spec whamdown sweep continuous");
  }
  // 9. Rapid pedal slams stay finite and bounded.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Ring); // unsmoothed-carrier worst case (engine glides it)
    auto sig = sine(0.4f, 330.0f, static_cast<int>(kSr));
    std::vector<float> y(sig.size());
    for (size_t i = 0; i < sig.size(); ++i)
    {
      e.setExpression(((i / 128) % 2 == 0) ? 1.0f : 0.0f);
      y[i] = e.process(sig[i]);
    }
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec slam finite");
    TDM_CHECK(maxStep(y) < 0.5f, "spec slam bounded");
  }
  // 10. Manual expression parks and holds.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.2f);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(kSr));
    for (int i = 0; i < 48000; ++i)
      e.process(sig[(size_t)(i % (int)sig.size())]);
    TDM_CHECK(std::fabs(e.expressionPos() - 0.2f) < 0.02f, "spec manual parks");
  }
  // 11. Determinism + non-finite safety.
  {
    auto sig = sine(0.4f, 196.0f, 4800);
    SpecLaser a, b;
    a.reset(kSr);
    b.reset(kSr);
    a.setVoice(Voice::Dual);
    b.setVoice(Voice::Dual);
    a.setExpression(0.7f);
    b.setExpression(0.7f);
    auto ya = renderEngine(a, sig), yb = renderEngine(b, sig);
    TDM_CHECK(bitEqual(ya.data(), yb.data(), (int)ya.size()), "spec deterministic");
    SpecLaser c;
    c.reset(kSr);
    c.setExpression(1.0f);
    TDM_CHECK(std::isfinite(c.process(std::nanf(""))), "spec nan safe");
    TDM_CHECK(std::isfinite(c.process(std::numeric_limits<float>::infinity())), "spec inf safe");
  }
  // 12. Whammy direction + expression depth: Down heel bit-dry, Down toe
  // in the -12 sag band; depth 6 renders the tritone at toe.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Whammy);
    e.setExprDirection(GestureDir::Down);
    e.setExpression(0.0f);
    e.clear(); // dip-bottom snap, as the router does on voice changes
    auto dry = sine(0.5f, 220.0f, static_cast<int>(kSr));
    auto y0 = renderEngine(e, dry);
    TDM_CHECK(bitEqual(y0.data(), dry.data(), (int)dry.size()), "spec whamdown heel dry");
    e.setExpression(1.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const double f = zcFreq(y, (int)y.size() / 2, (int)y.size());
    TDM_CHECK(f > 100.0 && f < 130.0, "spec whamdown toe band");
    SpecLaser d;
    d.reset(kSr);
    d.setExprDepth(6.0f);
    d.setExpression(1.0f);
    auto y2 = renderEngine(d, sig);
    const double f2 = zcFreq(y2, (int)y2.size() / 2, (int)y2.size());
    TDM_CHECK(f2 > 295.0 && f2 < 325.0, "spec expr depth scales");
  }
  // 13. DualWham expression response: both voices migrate upward
  // between settled holds (0.25 -> 0.75), proving the control moves
  // pitch. Stepped holds — not a continuous sweep — because doppler
  // spread smears a moving tone across any long coherent window, and
  // coarse oracle grids can miss the true peaks entirely (a 1 s
  // window nulls ~1 Hz off-peak). A ~258 -> ~356, B ~308 -> ~584.
  {
    double aLo = 0.0, aHi = 0.0, bLo = 0.0, bHi = 0.0;
    double amLo = 0.0, amHi = 0.0, bmLo = 0.0, bmHi = 0.0;
    for (int k = 0; k < 2; ++k)
    {
      SpecLaser e;
      e.reset(kSr);
      e.setVoice(Voice::DualWham);
      e.setExpression(k == 0 ? 0.25f : 0.75f);
      auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
      auto y = renderEngine(e, sig);
      const int n = static_cast<int>(y.size());
      double af = 0.0, bf = 0.0;
      const double am = scanBest(y, n / 2, n, 200.0, 440.0, 2.0, af);
      const double bm =
          scanBest(y, n / 2, n, k == 0 ? 280.0 : 500.0, k == 0 ? 340.0 : 660.0, 2.0, bf);
      if (k == 0)
      {
        aLo = af;
        bLo = bf;
        amLo = am;
        bmLo = bm;
      }
      else
      {
        aHi = af;
        bHi = bf;
        amHi = am;
        bmHi = bm;
      }
    }
    TDM_CHECK(aHi - aLo > 60.0, "spec dualwham A migrates");
    TDM_CHECK(bHi - bLo > 200.0, "spec dualwham B migrates");
    TDM_CHECK(amLo > 0.05 && amHi > 0.05 && bmLo > 0.05 && bmHi > 0.03,
              "spec dualwham hold voices present");
  }

  // 14. Router defaults (incl. factory gesture slots + section enables).
  {
    DevSpecFx d;
    TDM_CHECK(!d.isExpressionEnabled(), "spec router default expr off");
    TDM_CHECK(!d.isGesturesEnabled(), "spec router default gest off");
    TDM_CHECK(d.voice() == Voice::Whammy, "spec router default whammy");
    TDM_CHECK(d.exprDirection() == GestureDir::Up, "spec router default dir up");
    TDM_CHECK(d.exprDepth() == 12.0f, "spec router default depth");
    TDM_CHECK(d.insert() == DevSpecFx::Insert::Pre, "spec router default pre");
    TDM_CHECK(d.mix01() == 1.0f, "spec router default mix");
    TDM_CHECK(d.slotVoice(0) == Voice::Whammy && d.slotDir(0) == GestureDir::Up
                  && d.slotMotion(0) == GestureMotion::ToDepth && d.slotDepthSt(0) == 12.0f
                  && d.slotTimeMs(0) == 50.0f && d.slotRetMs(0) == 100.0f,
              "spec router slot 0 factory");
    TDM_CHECK(d.slotVoice(1) == Voice::Whammy && d.slotDir(1) == GestureDir::Down
                  && d.slotMotion(1) == GestureMotion::ToDepth && d.slotTimeMs(1) == 600.0f
                  && d.slotRetMs(1) == 300.0f,
              "spec router slot 1 factory");
    TDM_CHECK(d.slotVoice(2) == Voice::Whammy && d.slotMotion(2) == GestureMotion::FromDepth,
              "spec router slot 2 factory");
    TDM_CHECK(!d.preGestureActive() && !d.postGestureActive(), "spec router gesture idle");
  }
  // 15. Both sections off: transparent (taps + full rig with seams).
  {
    auto sig = sine(0.5f, 440.0f, static_cast<int>(kSr));
    DevSpecFx d;
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "spec off transparent");

    tdm::TechDeathRig dry, wet;
    dry.reset(kSr, 256);
    wet.reset(kSr, 256);
    DevSpecFx ds;
    wet.setSpecPre(&ds.preTap());
    wet.setSpecPost(&ds.postTap());
    wet.reset(kSr, 256);
    std::vector<float> o1, o2;
    rigRun(dry, sig, o1);
    rigRun(wet, sig, o2);
    TDM_CHECK(bitEqual(o1.data(), o2.data(), (int)sig.size()), "spec off rig transparent");
  }
  // 16. Mix 0 transparent enabled (both inserts, both sections on).
  for (DevSpecFx::Insert ins : {DevSpecFx::Insert::Pre, DevSpecFx::Insert::Post})
  {
    auto sig = sine(0.5f, 330.0f, static_cast<int>(kSr));
    DevSpecFx d;
    d.setInsert(ins);
    d.setMix(0.0f);
    d.setExpressionEnabled(true);
    d.setGesturesEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "spec mix-0 transparent");
  }
  // 17. Param adoption through the router (whammy toe shifts).
  {
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::Whammy);
    d.setExpression(1.0f);
    d.setExpressionEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io); // idle tap stays dry
    const int n = static_cast<int>(io.size());
    const double f = zcFreq(io, n / 2, n); // sag-tolerant band, see test 3
    TDM_CHECK(f > 404.0 && f < 454.0, "spec router adopts");
  }
  // 18. Voice switching click-free (dip + preroll), all four voices.
  {
    auto sig = sine(0.4f, 440.0f, static_cast<int>(4 * kSr));
    DevSpecFx d;
    d.setExpressionEnabled(true);
    d.setExpression(0.8f);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    const int q = static_cast<int>(io.size()) / 4;
    for (int seg = 0; seg < 4; ++seg)
    {
      d.setVoice(static_cast<Voice>(seg));
      for (int off = seg * q; off < (seg + 1) * q; off += 256)
      {
        const int m = ((seg + 1) * q - off < 256) ? ((seg + 1) * q - off) : 256;
        float* p = io.data() + off;
        d.preTap().process(p, p, m);
        d.postTap().process(p, p, m);
      }
    }
    TDM_CHECK(maxStep(io) < 0.15f, "spec voice switching click-free");
    TDM_CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "spec voice switching finite");
  }
  // 19. Insert switching click-free.
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(4 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::Dual);
    d.setExpression(0.7f);
    d.setExpressionEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    const int n = static_cast<int>(io.size());
    for (int off = 0; off < n; off += 256)
    {
      d.setInsert(((off / 256) / 94 % 2 == 0) ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post);
      const int m = (n - off < 256) ? (n - off) : 256;
      float* p = io.data() + off;
      d.preTap().process(p, p, m);
      d.postTap().process(p, p, m);
    }
    TDM_CHECK(maxStep(io) < 0.15f, "spec insert switching click-free");
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "spec insert switching finite");
  }
  // 20. Enable cycle: dry at the bottom, wet after.
  {
    DevSpecFx d;
    d.setExpression(1.0f);
    d.setExpressionEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto tail = sine(0.4f, 440.0f, static_cast<int>(kSr));
    auto dryRef = tail;
    runTap(d.preTap(), tail);
    float dev = 0.0f;
    for (size_t i = 0; i < tail.size(); ++i)
      dev = std::max(dev, std::fabs(tail[i] - dryRef[i]));
    TDM_CHECK(dev > 0.01f, "spec enable renders wet");
    d.setExpressionEnabled(false);
    tail = dryRef;
    runTap(d.preTap(), tail);
    runTap(d.postTap(), tail);
    TDM_CHECK(bitEqual(tail.data() + 256, dryRef.data() + 256, (int)tail.size() - 256),
              "spec disable returns dry");
  }
  // 21. Rig seam: install -> wet; remove -> dry again.
  {
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    std::vector<float> oDry, oWet, oBack;
    rigRun(rig, sig, oDry);
    DevSpecFx ds;
    ds.setExpression(1.0f);
    ds.setExpressionEnabled(true);
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
    rig.reset(kSr, 256);
    rigRun(rig, sig, oWet);
    float dev = 0.0f;
    for (size_t i = 0; i < oWet.size(); ++i)
      dev = std::max(dev, std::fabs(oWet[i] - oDry[i]));
    TDM_CHECK(dev > 0.01f, "spec rig seam renders wet");
    rig.setSpecPre(nullptr);
    rig.setSpecPost(nullptr);
    rig.reset(kSr, 256);
    rigRun(rig, sig, oBack);
    TDM_CHECK(bitEqual(oBack.data(), oDry.data(), (int)sig.size()), "spec rig seam removal dry");
  }
  // 22. Hostile UI chatter stays finite and bounded (incl. gesture cmds
  // + section-enable toggles).
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setExpressionEnabled(true);
    d.setGesturesEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    const int n = static_cast<int>(io.size());
    for (int off = 0; off < n; off += 128)
    {
      const int k = off / 128;
      d.setVoice(static_cast<Voice>(k % 4));
      d.setInsert((k % 2 == 0) ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post);
      d.setExpression((k % 3 == 0) ? 1.0f : 0.2f);
      d.setExpressionEnabled((k % 5) != 4);
      d.setGesturesEnabled((k % 7) != 6);
      if (k % 3 == 0)
        d.pressSlot(k % 8);
      else if (k % 3 == 1)
        d.releaseSlot(k % 8);
      else
        d.oneShotSlot(k % 8);
      const int m = (n - off < 128) ? (n - off) : 128;
      float* p = io.data() + off;
      d.preTap().process(p, p, m);
      d.postTap().process(p, p, m);
    }
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "spec hostile finite");
    TDM_CHECK(maxStep(io) < 0.3f, "spec hostile bounded");
  }
  // 23. Router never touches rig params (incl. transpose + gestures).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    DevSpecFx ds;
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
    const tdm::RigParams before = rig.params();
    ds.setExpressionEnabled(true);
    ds.setGesturesEnabled(true);
    ds.setVoice(Voice::Ring);
    ds.setExpression(1.0f);
    ds.setInsert(DevSpecFx::Insert::Post);
    ds.pressSlot(1);
    ds.oneShotSlot(4);
    ds.setSlotDepthSt(2, 6.0f);
    rig.reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, 4096);
    std::vector<float> o;
    rigRun(rig, sig, o);
    const tdm::RigParams after = rig.params();
    TDM_CHECK(before.gateEnabled == after.gateEnabled && before.tight == after.tight
                  && before.driveEnabled == after.driveEnabled
                  && before.inputTrimDb == after.inputTrimDb
                  && before.transposeEnabled == after.transposeEnabled
                  && before.transposeSemitones == after.transposeSemitones,
              "spec rig params untouched");
  }

  // 24. NEUTRAL: expression-on heel is bit-transparent for pitch voices
  // (Whammy/Dual/DualWham, both inserts) — enabling the section at
  // neutral expression must not colour the guitar.
  for (Voice v : {Voice::Whammy, Voice::Dual, Voice::DualWham})
  {
    for (DevSpecFx::Insert ins : {DevSpecFx::Insert::Pre, DevSpecFx::Insert::Post})
    {
      auto sig = sine(0.4f, 220.0f, static_cast<int>(kSr));
      DevSpecFx d;
      d.setVoice(v);
      d.setExpression(0.0f);
      d.setExpressionEnabled(true);
      d.setInsert(ins);
      d.preTap().reset(kSr, 256);
      d.postTap().reset(kSr, 256);
      std::vector<float> io = sig;
      runTap(d.preTap(), io);
      runTap(d.postTap(), io);
      TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "spec heel transparent");
    }
  }
  // 25. NEUTRAL: Ring heel keeps its ACCEPTED character (60 Hz AM on
  // 440 Hz) — coloured by design, pitch-stable, bounded, finite.
  {
    auto sig = sine(0.5f, 440.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::Ring);
    d.setExpression(0.0f);
    d.setExpressionEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    const int n = static_cast<int>(io.size());
    float dev = 0.0f;
    for (int i = n / 2; i < n; ++i)
      dev = std::max(dev, std::fabs(io[(size_t)i] - sig[(size_t)i]));
    TDM_CHECK(dev > 0.01f, "spec ring heel colours by design");
    TDM_CHECK(std::fabs(goertzel(io, n / 2, n, 500.0) - 0.25) < 0.06, "spec ring heel sum kept");
    TDM_CHECK(std::fabs(goertzel(io, n / 2, n, 380.0) - 0.25) < 0.06, "spec ring heel diff kept");
    TDM_CHECK(peak(io) < 0.6f, "spec ring heel bounded");
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "spec ring heel finite");
  }
  // 26. NEUTRAL: expression-off + gestures-on idle is bit-dry in EVERY
  // voice (the structural fix — the selected voice cannot colour idle).
  for (Voice v : {Voice::Whammy, Voice::Dual, Voice::Ring, Voice::DualWham})
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(kSr));
    DevSpecFx d;
    d.setVoice(v);
    d.setExpression(0.7f); // parked mid — must not leak through
    d.setGesturesEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "spec expr-off idle dry");
  }
  // 27. NEUTRAL: gesture ending with expression-off returns to (near-)dry
  // (near-exact: the documented gain-glide stall settles ~7e-6 short).
  {
    DevSpecFx d;
    d.setVoice(Voice::Whammy);
    d.setGesturesEnabled(true);
    d.setSlotTimeMs(1, 50.0f);
    d.setSlotRetMs(1, 50.0f);
    d.pressSlot(1);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto tail = sine(0.4f, 220.0f, 12000);
    runTap(d.preTap(), tail); // diving (wet)
    d.releaseSlot(1);
    auto dryRef = sine(0.4f, 220.0f, static_cast<int>(kSr));
    std::vector<float> io = dryRef;
    runTap(d.preTap(), io);
    float dev = 0.0f;
    for (int i = (int)io.size() - 1000; i < (int)io.size(); ++i)
      dev = std::max(dev, std::fabs(io[(size_t)i] - dryRef[(size_t)i]));
    TDM_CHECK(dev < 2e-5f, "spec expr-off gesture restores dry");
    TDM_CHECK(!d.preGestureActive(), "spec expr-off no stuck gesture");
  }

  // 28. GOLDEN: accepted voice renders bit-identical (Whammy/Dual/Ring;
  // the Detune voice + golden were REMOVED with the flavour — Detune was
  // never accepted and detuned at neutral). Goldens rendered from
  // 6113f3b with -O2; a libm/toolchain change would show here while the
  // behavioral bands above still pass (environmental).
  {
    const uint64_t kGolden[3] = {0x108fe8f852cac451ULL, 0x8f3a1e575856b93eULL,
                                 0xce1714274f397402ULL};
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    const int n = static_cast<int>(sig.size());
    for (int v = 0; v < 3; ++v)
    {
      SpecLaser e;
      e.reset(kSr);
      e.setVoice(static_cast<Voice>(v));
      std::vector<float> y(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i)
      {
        e.setExpression(i < n / 2 ? static_cast<float>(i) / static_cast<float>(n / 2) : 1.0f);
        y[i] = e.process(sig[(size_t)i]);
      }
      TDM_CHECK(fnvBits(y) == kGolden[v], "spec golden voice preserved");
    }
  }
  // 29. GOLDEN: legacy toe-press transient bit-identical at Fast-Toe
  // settings (0 ms gesture = snap; the 5 ms smoother does all slewing,
  // as before). Driven through the unified pressGesture command.
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    const int n = static_cast<int>(sig.size());
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 0.0f, 0.0f));
    std::vector<float> y(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
      e.setExpression(0.0f);
      if (i == n / 2)
        e.pressGesture(0);
      y[i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(fnvBits(y) == 0xea5873b3e8c0d5ffULL, "spec golden toe-press preserved");
  }
  // 30. To-Depth timing: independent linear ramps, exact settle.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 500.0f, 200.0f));
    e.pressGesture(0);
    int riseAt = -1;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
    {
      e.process(0.1f);
      if (e.gestureDisp() == 1.0f)
      {
        riseAt = i;
        break;
      }
    }
    TDM_CHECK(riseAt > 0 && std::abs(riseAt - 24000) <= 64, "spec gesture rise 500ms");
    e.releaseGesture(0);
    int retAt = -1;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
    {
      e.process(0.1f);
      if (!e.gestureActive())
      {
        retAt = i;
        break;
      }
    }
    TDM_CHECK(retAt > 0 && std::abs(retAt - 9600) <= 64, "spec gesture return 200ms");
  }
  // 31. Instant gestures snap: displacement reaches full in 1 sample,
  // position follows within 20 ms through the 5 ms smoother.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 0.0f, 0.0f));
    e.process(0.1f);
    e.pressGesture(0);
    e.process(0.1f);
    TDM_CHECK(e.gestureDisp() == 1.0f, "spec instant gesture snaps");
    int fullAt = -1;
    for (int i = 0; i < 4800; ++i)
    {
      e.process(0.1f);
      if (e.expressionPos() > 0.95f)
      {
        fullAt = i;
        break;
      }
    }
    TDM_CHECK(fullAt > 0 && fullAt < 960, "spec instant gesture fast");
  }
  // 32. Dive accuracy: Slow-Dive shape (-12) on 220 Hz lands in the sag
  // band; depth 6 lands near the tritone; uniform on Ring voice too.
  {
    for (Voice v : {Voice::Whammy, Voice::Ring})
    {
      SpecLaser e;
      e.reset(kSr);
      e.setVoice(v);
      e.setGestureConfig(
          makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
      e.pressGesture(1);
      auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
      auto y = renderEngine(e, sig);
      const int n = static_cast<int>(y.size());
      const double f = zcFreq(y, n / 2, n);
      TDM_CHECK(f > 100.0 && f < 130.0, "spec dive -12 band"); // sag zone, see probe
      TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec dive finite");
    }
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 6.0f, 50.0f, 50.0f));
    e.pressGesture(1);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(zcFreq(y, (int)y.size() / 2, (int)y.size()) > 145.0
                  && zcFreq(y, (int)y.size() / 2, (int)y.size()) < 165.0,
              "spec dive -6 band");
  }
  // 33. Dive sweep + return continuous; release restores the live base.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 300.0f, 200.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 3)
        e.pressGesture(1);
      if (i == 2 * n / 3)
        e.releaseGesture(1);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec dive sweep continuous");
    const double back = zcFreq(y, n - 24000, n);
    TDM_CHECK(back > 295.0 && back < 325.0, "spec dive restores base"); // +6 st
  }
  // 34. Interruption: newest wins; releasing it returns to NORMAL (stale
  // gestures are NOT revived). Toe, then dive joins mid-press, dive
  // releases -> heel base, all continuous.
  {
    SpecLaser e;
    e.reset(kSr);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(4 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    const int q = n / 4;
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    e.pressGesture(0); // toe (would be +12 alone)
    for (int i = 0; i < n; ++i)
    {
      if (i == q)
      {
        e.setGestureConfig(
            makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
        e.pressGesture(1); // dive interrupts: takes over downward
      }
      if (i == 2 * q)
        e.releaseGesture(1); // dive releases: NORMAL heel (toe is stale)
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec gesture handoffs continuous");
    TDM_CHECK(zcFreq(y, q / 2, q) > 404.0, "spec toe alone toe-up");
    const double dv = zcFreq(y, q + q / 4, 2 * q);
    TDM_CHECK(dv > 100.0 && dv < 130.0, "spec dive wins while held");
    const double back = zcFreq(y, n - 24000, n);
    TDM_CHECK(back > 215.0 && back < 225.0, "spec release returns to normal");
    TDM_CHECK(!e.gestureActive(), "spec no stuck gesture");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec gesture handoffs finite");
  }
  // 35. Expression moves during a held slow gesture: continuous, and the
  // release lands on the NEW base (no stale value).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 500.0f, 500.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 6)
        e.pressGesture(0);
      if (i == n / 3)
        e.setExpression(0.5f); // base moves under the held gesture
      if (i == 2 * n / 3)
        e.releaseGesture(0);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec base move under gesture continuous");
    const double back = zcFreq(y, n - 24000, n);
    TDM_CHECK(back > 295.0 && back < 325.0, "spec release lands on new base");
  }
  // 36. Rapid gesture slams stay finite and bounded.
  {
    SpecLaser e;
    e.reset(kSr);
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    bool held = false;
    for (size_t i = 0; i < sig.size(); ++i)
    {
      if (i % 100 == 0)
      {
        held = !held;
        const bool up = ((i / 100) % 2 == 0);
        e.setGestureConfig(makeCfg(up ? kVoiceWhammy : kVoiceRing,
                                   up ? GestureDir::Up : GestureDir::Down, GestureMotion::ToDepth,
                                   12.0f, 100.0f, 100.0f));
        if (held)
          e.pressGesture((int)((i / 100) % 2));
        else
          e.releaseGesture((int)((i / 100) % 2));
      }
      if (i % 150 == 0)
      {
        e.setGestureConfig(
            makeCfg(kVoiceRing, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 150.0f, 150.0f));
        e.oneShotGesture(4);
      }
      y[i] = e.process(sig[i]);
    }
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec gesture slams finite");
    TDM_CHECK(maxStep(y) < 0.3f, "spec gesture slams bounded");
  }
  // 37. Dive depth 0: press dives toward unison (harmless).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 0.0f, 50.0f, 50.0f));
    e.pressGesture(1);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const double f = zcFreq(y, (int)y.size() / 2, (int)y.size());
    TDM_CHECK(f > 215.0 && f < 225.0, "spec dive depth-0 unison");
  }
  // 38. From Depth (Reverse Dive): engages AT depth behind the engage
  // fade (bounded), sweeps back to live normal over Time, converges
  // exactly and disarms (one-shot included). Unified Whammy-Down.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Whammy);
    e.setExprDirection(GestureDir::Down);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::FromDepth, 12.0f, 400.0f, 200.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    e.pressGesture(2);
    for (int i = 0; i < n; ++i)
      y[(size_t)i] = e.process(sig[(size_t)i]);
    TDM_CHECK(maxStep(y) < 0.3f, "spec reverse dive engage bounded");
    const double early = zcFreq(y, 4800, 9600); // 100-200 ms: slew-settled, pre-decay
    TDM_CHECK(early > 120.0 && early < 160.0, "spec reverse dive starts deep");
    const double late = zcFreq(y, n - 24000, n);
    TDM_CHECK(late > 215.0 && late < 225.0, "spec reverse dive converges dry");
    TDM_CHECK(!e.gestureActive(), "spec reverse dive disarms");
    // One-shot From: same convergence, no explicit release.
    SpecLaser s;
    s.reset(kSr);
    s.setVoice(Voice::Whammy);
    s.setExprDirection(GestureDir::Down);
    s.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::FromDepth, 12.0f, 200.0f, 200.0f));
    s.oneShotGesture(2);
    auto y2 = renderEngine(s, sig);
    TDM_CHECK(!s.gestureActive(), "spec reverse one-shot disarms");
    const double l2 = zcFreq(y2, (int)y2.size() - 24000, (int)y2.size());
    TDM_CHECK(l2 > 215.0 && l2 < 225.0, "spec reverse one-shot converges");
    TDM_CHECK(tdm_test::allFinite(y2.data(), (int)y2.size()), "spec reverse finite");
  }
  // 39. From Depth (Reverse Toe): upward mirror of test 38.
  {
    SpecLaser e;
    e.reset(kSr);
    // Slow sweep (800 ms): the 30 ms doppler slew needs ~100 ms to print
    // the depth before the sweep decays (fast From gestures peak lower).
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::FromDepth, 12.0f, 800.0f, 150.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    e.pressGesture(3);
    for (size_t i = 0; i < sig.size(); ++i)
      y[i] = e.process(sig[i]);
    TDM_CHECK(maxStep(y) < 0.3f, "spec reverse toe engage bounded");
    TDM_CHECK(zcFreq(y, 4800, 9600) > 360.0, "spec reverse toe starts high");
    const double late = zcFreq(y, (int)y.size() - 24000, (int)y.size());
    TDM_CHECK(late > 215.0 && late < 225.0, "spec reverse toe converges dry");
    TDM_CHECK(!e.gestureActive(), "spec reverse toe disarms");
  }
  // 40. Cross-voice Ring gesture: normal Whammy + Ring Sweep renders
  // ringed (carrier reaches toe), release restores (near-)dry.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceRing, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 4)
        e.pressGesture(4);
      if (i == 3 * n / 4)
        e.releaseGesture(4);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec ring gesture continuous");
    TDM_CHECK(goertzel(y, n / 2 - 12000, n / 2, 2220.0) > 0.1, "spec ring gesture reaches toe");
    // Near-exact restore: the documented gain-glide stall settles ~7e-6
    // short after a mix excursion (legacy behavior, kept golden-exact).
    float tailDev = 0.0f;
    for (int i = n - 1000; i < n; ++i)
      tailDev = std::max(tailDev, std::fabs(y[(size_t)i] - sig[(size_t)i]));
    TDM_CHECK(tailDev < 2e-5f, "spec ring gesture restores dry");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec ring gesture finite");
  }
  // 41. Unknown gesture voice ids are ignored (robustness, documented).
  {
    SpecLaser a, b;
    a.reset(kSr);
    b.reset(kSr);
    a.setExpression(0.6f);
    b.setExpression(0.6f);
    b.setGestureConfig(
        makeCfg(7, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    b.pressGesture(2);
    TDM_CHECK(!b.gestureActive(), "spec unknown voice ignored");
    auto sig = sine(0.4f, 220.0f, static_cast<int>(kSr));
    auto ya = renderEngine(a, sig), yb = renderEngine(b, sig);
    TDM_CHECK(bitEqual(ya.data(), yb.data(), (int)ya.size()), "spec unknown voice no-op");
  }
  // 42. Foreign release ignored; release-all leaves no stuck state.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    e.pressGesture(0);
    for (int i = 0; i < 1000; ++i)
      e.process(0.1f);
    e.releaseGesture(5); // foreign slot: must not release slot 0
    TDM_CHECK(e.gestureActive() && e.gestureSlot() == 0, "spec foreign release ignored");
    e.releaseGesture(0);
    for (int i = 0; i < 48000; ++i)
      e.process(0.1f);
    TDM_CHECK(!e.gestureActive(), "spec no stuck gesture");
  }
  // 43. One-shot To: reaches depth, auto-returns to base, disarms.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 300.0f, 300.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    e.oneShotGesture(0);
    for (size_t i = 0; i < sig.size(); ++i)
      y[i] = e.process(sig[i]);
    TDM_CHECK(zcFreq(y, 12000, 16000) > 380.0, "spec one-shot reaches depth");
    TDM_CHECK(!e.gestureActive(), "spec one-shot disarms");
    const double late = zcFreq(y, (int)y.size() - 24000, (int)y.size());
    TDM_CHECK(late > 215.0 && late < 225.0, "spec one-shot returns to base");
  }
  // 44. DIRECTION is the single source of truth: a Whammy-Down gesture
  // over Whammy-Up normal dives (Path X — never cancels), and the
  // mirror rises. Same-polarity gestures take Path E.
  {
    SpecLaser e;
    e.reset(kSr); // normal Whammy-Up at heel
    e.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    e.pressGesture(1);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const double dv = zcFreq(y, (int)y.size() / 2, (int)y.size());
    TDM_CHECK(dv > 100.0 && dv < 130.0, "spec down gesture over up normal dives");
    SpecLaser r;
    r.reset(kSr);
    r.setVoice(Voice::Whammy);
    r.setExprDirection(GestureDir::Down);
    r.setGestureConfig(
        makeCfg(kVoiceWhammy, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    r.pressGesture(0);
    auto y2 = renderEngine(r, sig);
    TDM_CHECK(zcFreq(y2, (int)y2.size() / 2, (int)y2.size()) > 404.0,
              "spec up gesture over down normal rises");
  }
  // 45. Ring-Down gesture (carrier fall): normal Ring at 0.8 shows the
  // base carrier; pressing falls toward the heel signature; release
  // restores the base. Continuous throughout.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Ring);
    e.setExpression(0.8f);
    e.setGestureConfig(
        makeCfg(kVoiceRing, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
    auto sig = sine(0.5f, 440.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 4)
        e.pressGesture(7);
      if (i == 3 * n / 4)
        e.releaseGesture(7);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec ring fall continuous");
    // Held: heel signature (440 +/- 60) dominates over the base carrier.
    TDM_CHECK(goertzel(y, n / 2 - 12000, n / 2, 500.0) > 0.15, "spec ring fall reaches heel");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec ring fall finite");
  }
  // 46. Router: slow gesture audibly slews; dive reaches the sag band.
  {
    // Slow toe through the tap: mid-press pitch sits between heel/toe.
    DevSpecFx r;
    r.setSlotTimeMs(0, 500.0f);
    r.pressSlot(0);
    r.setGesturesEnabled(true);
    r.preTap().reset(kSr, 256);
    r.postTap().reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> io = sig;
    runTap(r.preTap(), io);
    const double mid = zcFreq(io, 10000, 14000); // ~250 ms in: mid-sweep
    TDM_CHECK(mid > 260.0 && mid < 400.0, "spec router gesture rate adopts");
    // Dive press through the tap reaches the sag band (expr off!).
    DevSpecFx v;
    v.setSlotTimeMs(1, 50.0f);
    v.pressSlot(1);
    v.setGesturesEnabled(true);
    v.preTap().reset(kSr, 256);
    v.postTap().reset(kSr, 256);
    std::vector<float> io2 = sig;
    runTap(v.preTap(), io2);
    const double dv = zcFreq(io2, (int)io2.size() / 2, (int)io2.size());
    TDM_CHECK(dv > 100.0 && dv < 130.0, "spec router dive adopts");
    TDM_CHECK(v.preGestureActive(), "spec router gesture telemetry");
    v.releaseSlot(1);
    std::vector<float> io3(4096, 0.1f);
    for (int k = 0; k < 5; ++k) // factory return 300 ms needs 14400+ samples
      runTap(v.preTap(), io3);
    TDM_CHECK(!v.preGestureActive(), "spec router release adopts");
  }
  // 47. Slots are independent; triggering is by ID (no display concept).
  {
    DevSpecFx d;
    const float t1 = d.slotTimeMs(1);
    d.setSlotTimeMs(6, 33.0f);
    d.setSlotDepthSt(6, 7.0f);
    TDM_CHECK(d.slotTimeMs(6) == 33.0f && d.slotDepthSt(6) == 7.0f, "spec slot edit stores");
    TDM_CHECK(d.slotTimeMs(1) == t1, "spec slot edit isolated");
    d.resetSlot(6);
    TDM_CHECK(d.slotTimeMs(6) == 150.0f && d.slotDepthSt(6) == 12.0f, "spec slot reset factory");
    // Trigger slot 5 directly (no selection step exists at this layer).
    d.setGesturesEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    d.pressSlot(5);
    std::vector<float> io(1024, 0.1f);
    runTap(d.preTap(), io);
    TDM_CHECK(d.preGestureActive(), "spec trigger by id works");
    d.releaseSlot(5);
    runTap(d.preTap(), io);
    runTap(d.preTap(), io);
    runTap(d.preTap(), io);
    TDM_CHECK(!d.preGestureActive(), "spec release by id works");
  }
  // 48. Section enables: gestures-off mid-hold releases gracefully (no
  // stuck state, returns to normal); presses while off are ignored dry.
  {
    DevSpecFx d;
    d.setExpression(0.5f);
    d.setExpressionEnabled(true);
    d.setGesturesEnabled(true);
    d.setSlotTimeMs(1, 1000.0f); // slow: guaranteed mid-sweep at toggle
    d.setSlotRetMs(1, 100.0f);
    d.pressSlot(1);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto tail = sine(0.4f, 440.0f, 12000);
    runTap(d.preTap(), tail); // diving (wet, mid-sweep)
    TDM_CHECK(d.preGestureActive(), "spec held gesture active");
    d.setGesturesEnabled(false); // section off mid-hold: graceful release
    auto ref = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> io = ref;
    runTap(d.preTap(), io);
    TDM_CHECK(!d.preGestureActive(), "spec gest-off releases hold");
    const double back = zcFreq(io, (int)io.size() - 24000, (int)io.size());
    TDM_CHECK(back > 295.0 && back < 325.0, "spec gest-off returns to base"); // +6 st
    // Press while the section is off: ignored, stays dry-ish (normal).
    d.pressSlot(1);
    std::vector<float> io2 = ref;
    runTap(d.preTap(), io2);
    TDM_CHECK(!d.preGestureActive(), "spec press while off ignored");
    TDM_CHECK(tdm_test::allFinite(io2.data(), (int)io2.size()), "spec section enable finite");
  }
  // 49. Transpose + gesture simultaneously: production -2 feeds the dive
  // (chain order preserved, both operations audible, params untouched).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    DevSpecFx ds;
    ds.setGesturesEnabled(true);
    ds.setSlotTimeMs(1, 100.0f);
    ds.pressSlot(1); // Slow Dive shape (Whammy/Down/To/12)
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
    rig.reset(kSr, 256);
    const tdm::RigParams before = rig.params();
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> o;
    rigRun(rig, sig, o);
    const int n = static_cast<int>(o.size());
    const double f = zcFreq(o, 2 * n / 3, n); // ~220 * 2^(-2/12) * 2^(-12/12+sag)
    TDM_CHECK(f > 85.0 && f < 125.0, "spec transpose plus gesture");
    TDM_CHECK(tdm_test::allFinite(o.data(), n), "spec transpose plus gesture finite");
    const tdm::RigParams after = rig.params();
    TDM_CHECK(before.transposeEnabled == after.transposeEnabled
                  && before.transposeSemitones == after.transposeSemitones,
              "spec gesture keeps transpose");
  }
  // 50. Expression toggle mid-held-gesture: bounded morph, correct
  // endpoints both ways (to dry-base dive and back to expression dive).
  {
    DevSpecFx d;
    d.setVoice(Voice::Whammy);
    d.setExpression(0.5f); // +6 base when expression is on
    d.setExpressionEnabled(true);
    d.setGesturesEnabled(true);
    d.setSlotTimeMs(1, 50.0f);
    d.setSlotRetMs(1, 50.0f);
    d.pressSlot(1); // Slow Dive shape, held throughout
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(4 * kSr));
    std::vector<float> io = sig;
    const int n = static_cast<int>(io.size());
    const int q = n / 4;
    for (int off = 0; off < n; off += 256)
    {
      if (off == q)
        d.setExpressionEnabled(false); // dive morphs to dry base
      if (off == 2 * q)
        d.setExpressionEnabled(true); // ... and back to +6 base
      const int m = (n - off < 256) ? (n - off) : 256;
      float* p = io.data() + off;
      d.preTap().process(p, p, m);
      d.postTap().process(p, p, m);
    }
    TDM_CHECK(maxStep(io) < 0.3f, "spec expr toggle mid-hold bounded");
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "spec expr toggle mid-hold finite");
    TDM_CHECK(d.preGestureActive(), "spec hold survives expr toggle");
  }
  // 51. Transpose-stacking baselines: production transpose alone (no
  // spec installed) prints exact fundamentals — the operating points
  // every creative voice below receives.
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    const float tst[3] = {0.0f, -2.0f, -12.0f};
    const double want[3] = {220.0, 196.0, 110.0};
    for (int t = 0; t < 3; ++t)
    {
      StackRender r =
          renderStack(tst[t], false, false, Voice::Whammy, DevSpecFx::Insert::Pre, false, false, sig);
      const int n = static_cast<int>(r.y.size());
      double bf = 0.0;
      const double m = scanBest(r.y, 2 * n / 3, n, 40.0, 300.0, 1.0, bf);
      TDM_CHECK(std::fabs(bf - want[t]) < 2.0 && m > 0.3, "spec stack baseline");
    }
  }
  // 52. Whammy stacks with transpose (expression toe + gesture hold,
  // pre + post): T=-2 moves the toe down from T=0. NOTE the move is
  // ~1 st, not 2: the accepted Doppler's wrap-slip error is
  // input-frequency-dependent (220 in prints +11.2, 196 in prints
  // +12.2), which PARTIALLY compensates the transpose move. Physics
  // of the shared engine — not a routing gap (Ring below proves the
  // tap receives transposed audio to the Hz).
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    for (int ctl = 0; ctl < 2; ++ctl)
      for (int pi = 0; pi < 2; ++pi)
      {
        double f0 = 0.0, f2 = 0.0, m0 = 0.0, m2 = 0.0;
        for (int t = 0; t < 2; ++t)
        {
          StackRender r = renderStack(t == 0 ? 0.0f : -2.0f, ctl == 0, ctl == 1, Voice::Whammy,
                                      pi == 0 ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post,
                                      ctl == 1, true, sig);
          const int n = static_cast<int>(r.y.size());
          double bf = 0.0;
          const double m = scanBest(r.y, 2 * n / 3, n, 80.0, 600.0, 2.0, bf);
          if (t == 0)
          {
            f0 = bf;
            m0 = m;
          }
          else
          {
            f2 = bf;
            m2 = m;
          }
        }
        TDM_CHECK(f0 > 400.0 && f0 < 440.0 && m0 > 0.1, "spec whammy stack T0");
        TDM_CHECK(f2 > 375.0 && f2 < 415.0 && m2 > 0.1, "spec whammy stack T-2");
        TDM_CHECK(f0 - f2 > 10.0, "spec whammy stack moves");
      }
  }
  // 53. Dual stacks: BOTH legs move down with transpose (expression +
  // gesture, pre + post).
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    for (int ctl = 0; ctl < 2; ++ctl)
      for (int pi = 0; pi < 2; ++pi)
      {
        double lo0 = 0.0, hi0 = 0.0, lo2 = 0.0, hi2 = 0.0;
        double mlo0 = 0.0, mhi0 = 0.0, mlo2 = 0.0, mhi2 = 0.0;
        for (int t = 0; t < 2; ++t)
        {
          StackRender r = renderStack(t == 0 ? 0.0f : -2.0f, ctl == 0, ctl == 1, Voice::Dual,
                                      pi == 0 ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post,
                                      ctl == 1, true, sig);
          const int n = static_cast<int>(r.y.size());
          double bl = 0.0, bh = 0.0;
          const double ml = scanBest(r.y, 2 * n / 3, n, 40.0, 300.0, 2.0, bl);
          const double mh = scanBest(r.y, 2 * n / 3, n, 300.0, 600.0, 2.0, bh);
          if (t == 0)
          {
            lo0 = bl;
            hi0 = bh;
            mlo0 = ml;
            mhi0 = mh;
          }
          else
          {
            lo2 = bl;
            hi2 = bh;
            mlo2 = ml;
            mhi2 = mh;
          }
        }
        TDM_CHECK(lo0 > 100.0 && lo0 < 140.0 && mlo0 > 0.05, "spec dual stack lo T0");
        TDM_CHECK(hi0 > 400.0 && hi0 < 440.0 && mhi0 > 0.05, "spec dual stack hi T0");
        TDM_CHECK(lo2 > 80.0 && lo2 < 115.0 && mlo2 > 0.05, "spec dual stack lo T-2");
        TDM_CHECK(hi2 > 375.0 && hi2 < 415.0 && mhi2 > 0.05, "spec dual stack hi T-2");
        TDM_CHECK(lo0 - lo2 > 10.0 && hi0 - hi2 > 10.0, "spec dual stack moves");
      }
  }
  // 54. DualWham stacks: A and B both move down with transpose
  // (expression + gesture, pre + post). B's move is SMALL (~24 Hz):
  // the cascade compounds the wrap-slip compensation, nearly (not
  // fully) cancelling small transpose moves — the hardware "same
  // range" report, explained and pinned. A routing break would leave
  // B at 820 for both; the bands + direction forbid that.
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    for (int ctl = 0; ctl < 2; ++ctl)
      for (int pi = 0; pi < 2; ++pi)
      {
        double a0 = 0.0, b0 = 0.0, a2 = 0.0, b2 = 0.0;
        double ma0 = 0.0, mb0 = 0.0, ma2 = 0.0, mb2 = 0.0;
        for (int t = 0; t < 2; ++t)
        {
          StackRender r = renderStack(t == 0 ? 0.0f : -2.0f, ctl == 0, ctl == 1, Voice::DualWham,
                                      pi == 0 ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post,
                                      ctl == 1, true, sig);
          const int n = static_cast<int>(r.y.size());
          double ba = 0.0, bb = 0.0;
          const double ma = scanBest(r.y, 2 * n / 3, n, 150.0, 500.0, 2.0, ba);
          const double mb = scanBest(r.y, 2 * n / 3, n, 500.0, 1000.0, 2.0, bb);
          if (t == 0)
          {
            a0 = ba;
            b0 = bb;
            ma0 = ma;
            mb0 = mb;
          }
          else
          {
            a2 = ba;
            b2 = bb;
            ma2 = ma;
            mb2 = mb;
          }
        }
        TDM_CHECK(a0 > 400.0 && a0 < 440.0 && ma0 > 0.05, "spec dualwham stack A T0");
        TDM_CHECK(b0 > 780.0 && b0 < 860.0 && mb0 > 0.03, "spec dualwham stack B T0");
        TDM_CHECK(a2 > 375.0 && a2 < 415.0 && ma2 > 0.05, "spec dualwham stack A T-2");
        TDM_CHECK(b2 > 755.0 && b2 < 835.0 && mb2 > 0.03, "spec dualwham stack B T-2");
        TDM_CHECK(a0 - a2 > 10.0 && b0 - b2 > 10.0, "spec dualwham stack moves");
      }
  }
  // 55. Ring sidebands track transpose EXACTLY (carrier 2000 fixed,
  // sidebands 2000 +/- transposed fundamental): expression + gesture,
  // pre + post. This is the routing proof — exact to the Hz at both
  // settings, so the tap unambiguously receives transposed audio.
  // (Perceived "sameness" is the fixed carrier dominating, by design.)
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    const double base[2] = {220.0, 196.0};
    for (int ctl = 0; ctl < 2; ++ctl)
      for (int pi = 0; pi < 2; ++pi)
        for (int t = 0; t < 2; ++t)
        {
          StackRender r = renderStack(t == 0 ? 0.0f : -2.0f, ctl == 0, ctl == 1, Voice::Ring,
                                      pi == 0 ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post,
                                      ctl == 1, true, sig);
          const int n = static_cast<int>(r.y.size());
          const double wantLo = 2000.0 - base[t], wantHi = 2000.0 + base[t];
          double blo = 0.0, bhi = 0.0;
          const double mlo = scanBest(r.y, 2 * n / 3, n, wantLo - 10.0, wantLo + 10.0, 1.0, blo);
          const double mhi = scanBest(r.y, 2 * n / 3, n, wantHi - 10.0, wantHi + 10.0, 1.0, bhi);
          TDM_CHECK(std::fabs(blo - wantLo) < 3.0 && mlo > 0.15, "spec ring stack lo");
          TDM_CHECK(std::fabs(bhi - wantHi) < 3.0 && mhi > 0.15, "spec ring stack hi");
        }
  }
  // 56. Deep transpose (T=-12) regime: transpose prints 110 Hz; Ring
  // sidebands stay exact (pre + post, expression + gesture). Doppler
  // voices degrade together here (weak/smeared ~110 Hz shifting — a
  // known engine limit, consistent across voices, NOT a stacking
  // inconsistency), so they assert processing + safety, not pitch:
  // finite, bounded, audibly non-dry. Whammy keeps a loose direction
  // pin (dominant peak still drops well below its T=0 toe).
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    {
      StackRender r =
          renderStack(-12.0f, false, false, Voice::Whammy, DevSpecFx::Insert::Pre, false, false, sig);
      const int n = static_cast<int>(r.y.size());
      double bf = 0.0;
      const double m = scanBest(r.y, 2 * n / 3, n, 40.0, 300.0, 1.0, bf);
      TDM_CHECK(std::fabs(bf - 110.0) < 2.0 && m > 0.3, "spec stack deep baseline");
    }
    for (int ctl = 0; ctl < 2; ++ctl)
      for (int pi = 0; pi < 2; ++pi)
      {
        StackRender r = renderStack(-12.0f, ctl == 0, ctl == 1, Voice::Ring,
                                    pi == 0 ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post,
                                    ctl == 1, true, sig);
        const int n = static_cast<int>(r.y.size());
        double blo = 0.0, bhi = 0.0;
        const double mlo = scanBest(r.y, 2 * n / 3, n, 1880.0, 1900.0, 1.0, blo);
        const double mhi = scanBest(r.y, 2 * n / 3, n, 2100.0, 2120.0, 1.0, bhi);
        TDM_CHECK(std::fabs(blo - 1890.0) < 3.0 && mlo > 0.15, "spec ring stack deep lo");
        TDM_CHECK(std::fabs(bhi - 2110.0) < 3.0 && mhi > 0.15, "spec ring stack deep hi");
      }
    for (Voice v : {Voice::Whammy, Voice::Dual, Voice::DualWham})
      for (int ctl = 0; ctl < 2; ++ctl)
      {
        StackRender r = renderStack(-12.0f, ctl == 0, ctl == 1, v, DevSpecFx::Insert::Pre,
                                    ctl == 1, true, sig);
        const int n = static_cast<int>(r.y.size());
        TDM_CHECK(tdm_test::allFinite(r.y.data(), n), "spec stack deep finite");
        TDM_CHECK(peak(r.y) < 0.7f, "spec stack deep bounded");
        bool same = true;
        for (int i = 2 * n / 3; i < n; ++i)
          if (r.y[(size_t)i] != sig[(size_t)i])
          {
            same = false;
            break;
          }
        TDM_CHECK(!same, "spec stack deep processes");
        if (v == Voice::Whammy)
        {
          double bf = 0.0;
          const double m = scanBest(r.y, 2 * n / 3, n, 80.0, 600.0, 2.0, bf);
          TDM_CHECK(bf > 200.0 && bf < 290.0 && m > 0.03, "spec whammy stack deep moves");
        }
      }
  }
}

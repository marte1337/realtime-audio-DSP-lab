// Special FX Laser lab tests: creative voice maps (heel/toe endpoints),
// sweep continuity, unified performance gestures (To/From Depth, 8 slots,
// interruption, one-shot, MIDI-ready trigger-by-ID), router insert A/B +
// voice switching, rig-seam transparency, transpose+creative simultaneity.
// Synthetic sines validate DSP math only — never musical quality.

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

  // 1. Engine defaults.
  {
    SpecLaser e;
    e.reset(kSr);
    TDM_CHECK(e.voice() == Voice::WhamUp, "spec default whamup");
    TDM_CHECK(e.expressionPos() == 0.0f, "spec default pos 0");
    TDM_CHECK(!e.gestureActive(), "spec default gesture idle");
  }
  // 2. WhamUp heel = bit-exact dry (doppler unity bypass, settled).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(bitEqual(y.data(), sig.data(), (int)sig.size()), "spec whammy heel dry");
  }
  // 3. WhamUp toe ~= +12 st. Narrow Goertzel is the WRONG oracle here:
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
  // 7. Detune: non-dry, bounded chorus legs.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::Detune);
    e.setExpression(1.0f);
    auto sig = sine(0.5f, 330.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    float dev = 0.0f;
    for (size_t i = 0; i < y.size(); ++i)
      dev = std::max(dev, std::fabs(y[i] - sig[i]));
    TDM_CHECK(dev > 0.01f, "spec detune renders wet");
    TDM_CHECK(peak(y) <= 0.55f, "spec detune bounded");
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec detune finite");
  }
  // 8. WhamDown voice: heel bit-dry, toe in the -12 sag band, sweeps
  // continuous in all five voices. Bounds are slope-aware (see legacy
  // note): ring toe content reaches ~2.2 kHz (legit slope ~0.12), far
  // below real-jump territory (~0.4 measured pre-fix).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::WhamDown);
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
  }
  for (Voice v :
       {Voice::WhamUp, Voice::Dual, Voice::Ring, Voice::Detune, Voice::WhamDown})
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
    const float bound = (v == Voice::Ring) ? 0.15f : (v == Voice::Detune) ? 0.12f : 0.1f;
    TDM_CHECK(maxStep(y) < bound, "spec sweep continuous");
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec sweep finite");
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

  // 12. Router defaults (incl. factory gesture slots).
  {
    DevSpecFx d;
    TDM_CHECK(!d.isEnabled(), "spec router default off");
    TDM_CHECK(d.voice() == Voice::WhamUp, "spec router default whamup");
    TDM_CHECK(d.insert() == DevSpecFx::Insert::Pre, "spec router default pre");
    TDM_CHECK(d.mix01() == 1.0f, "spec router default mix");
    TDM_CHECK(d.slotVoice(0) == Voice::WhamUp && d.slotDir(0) == GestureDir::Up
                  && d.slotMotion(0) == GestureMotion::ToDepth && d.slotDepthSt(0) == 12.0f
                  && d.slotTimeMs(0) == 50.0f && d.slotRetMs(0) == 100.0f,
              "spec router slot 0 factory");
    TDM_CHECK(d.slotVoice(1) == Voice::WhamDown && d.slotDir(1) == GestureDir::Down
                  && d.slotMotion(1) == GestureMotion::ToDepth && d.slotTimeMs(1) == 600.0f
                  && d.slotRetMs(1) == 300.0f,
              "spec router slot 1 factory");
    TDM_CHECK(d.slotVoice(2) == Voice::WhamDown && d.slotMotion(2) == GestureMotion::FromDepth,
              "spec router slot 2 factory");
    TDM_CHECK(!d.preGestureActive() && !d.postGestureActive(), "spec router gesture idle");
  }
  // 13. Off transparent (taps + full rig with seams).
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
  // 14. Mix 0 transparent enabled (both inserts).
  for (DevSpecFx::Insert ins : {DevSpecFx::Insert::Pre, DevSpecFx::Insert::Post})
  {
    auto sig = sine(0.5f, 330.0f, static_cast<int>(kSr));
    DevSpecFx d;
    d.setInsert(ins);
    d.setMix(0.0f);
    d.setEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io);
    TDM_CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "spec mix-0 transparent");
  }
  // 15. Param adoption through the router (whammy toe shifts).
  {
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::WhamUp);
    d.setExpression(1.0f);
    d.setEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    runTap(d.preTap(), io);
    runTap(d.postTap(), io); // idle tap stays dry
    const int n = static_cast<int>(io.size());
    const double f = zcFreq(io, n / 2, n); // sag-tolerant band, see test 3
    TDM_CHECK(f > 404.0 && f < 454.0, "spec router adopts");
  }
  // 16. Voice switching click-free (dip + preroll), all five voices.
  {
    auto sig = sine(0.4f, 440.0f, static_cast<int>(5 * kSr));
    DevSpecFx d;
    d.setEnabled(true);
    d.setExpression(0.8f);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    const int q = static_cast<int>(io.size()) / 5;
    for (int seg = 0; seg < 5; ++seg)
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
  // 17. Insert switching click-free.
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(4 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::Dual);
    d.setExpression(0.7f);
    d.setEnabled(true);
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
  // 18. Enable cycle: dry at the bottom, wet after.
  {
    DevSpecFx d;
    d.setExpression(1.0f);
    d.setEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto tail = sine(0.4f, 440.0f, static_cast<int>(kSr));
    auto dryRef = tail;
    runTap(d.preTap(), tail);
    float dev = 0.0f;
    for (size_t i = 0; i < tail.size(); ++i)
      dev = std::max(dev, std::fabs(tail[i] - dryRef[i]));
    TDM_CHECK(dev > 0.01f, "spec enable renders wet");
    d.setEnabled(false);
    tail = dryRef;
    runTap(d.preTap(), tail);
    runTap(d.postTap(), tail);
    TDM_CHECK(bitEqual(tail.data() + 256, dryRef.data() + 256, (int)tail.size() - 256),
              "spec disable returns dry");
  }
  // 19. Rig seam: install -> wet; remove -> dry again.
  {
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    std::vector<float> oDry, oWet, oBack;
    rigRun(rig, sig, oDry);
    DevSpecFx ds;
    ds.setExpression(1.0f);
    ds.setEnabled(true);
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
  // 20. Hostile UI chatter stays finite and bounded (incl. gesture cmds).
  {
    auto sig = sine(0.4f, 330.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    std::vector<float> io = sig;
    const int n = static_cast<int>(io.size());
    for (int off = 0; off < n; off += 128)
    {
      const int k = off / 128;
      d.setVoice(static_cast<Voice>(k % 5));
      d.setInsert((k % 2 == 0) ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post);
      d.setExpression((k % 3 == 0) ? 1.0f : 0.2f);
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
  // 21. Router never touches rig params.
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    DevSpecFx ds;
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
    const tdm::RigParams before = rig.params();
    ds.setEnabled(true);
    ds.setVoice(Voice::Ring);
    ds.setExpression(1.0f);
    ds.setInsert(DevSpecFx::Insert::Post);
    rig.reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, 4096);
    std::vector<float> o;
    rigRun(rig, sig, o);
    const tdm::RigParams after = rig.params();
    TDM_CHECK(before.gateEnabled == after.gateEnabled && before.tight == after.tight
                  && before.driveEnabled == after.driveEnabled
                  && before.inputTrimDb == after.inputTrimDb,
              "spec rig params untouched");
  }

  // 22. GOLDEN: milestone voice renders bit-identical (accepted Whammy/
  // Ring/others preserved; idle gestures are exact identity). Goldens
  // rendered from 6113f3b with -O2; a libm/toolchain change would show
  // here while the behavioral bands above still pass (environmental).
  {
    const uint64_t kGolden[4] = {0x108fe8f852cac451ULL, 0x8f3a1e575856b93eULL,
                                 0xce1714274f397402ULL, 0x4e39ceb333dab03dULL};
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    const int n = static_cast<int>(sig.size());
    for (int v = 0; v < 4; ++v)
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
  // 23. GOLDEN: legacy toe-press transient bit-identical at Fast-Toe
  // settings (0 ms gesture = snap; the 5 ms smoother does all slewing,
  // as before). Driven through the unified pressGesture command.
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    const int n = static_cast<int>(sig.size());
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 0.0f, 0.0f));
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
  // 24. To-Depth timing: independent linear ramps, exact settle.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 500.0f, 200.0f));
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
  // 25. Instant gestures snap: displacement reaches full in 1 sample,
  // position follows within 20 ms through the 5 ms smoother.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 0.0f, 0.0f));
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
  // 26. Dive accuracy: Slow-Dive shape (-12) on 220 Hz lands in the sag
  // band; depth 6 lands near the tritone; uniform on Ring voice too.
  {
    for (Voice v : {Voice::WhamUp, Voice::Ring})
    {
      SpecLaser e;
      e.reset(kSr);
      e.setVoice(v);
      e.setGestureConfig(makeCfg(4, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
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
    e.setGestureConfig(makeCfg(4, GestureDir::Down, GestureMotion::ToDepth, 6.0f, 50.0f, 50.0f));
    e.pressGesture(1);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(zcFreq(y, (int)y.size() / 2, (int)y.size()) > 145.0
                  && zcFreq(y, (int)y.size() / 2, (int)y.size()) < 165.0,
              "spec dive -6 band");
  }
  // 27. Dive sweep + return continuous; release restores the live base.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setGestureConfig(
        makeCfg(4, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 300.0f, 200.0f));
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
  // 28. Interruption: newest wins; releasing it returns to NORMAL (stale
  // gestures are NOT revived). Toe, then dive joins mid-press, dive
  // releases -> heel base, all continuous.
  {
    SpecLaser e;
    e.reset(kSr);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(4 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    const int q = n / 4;
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    e.pressGesture(0); // toe (would be +12 alone)
    for (int i = 0; i < n; ++i)
    {
      if (i == q)
      {
        e.setGestureConfig(
            makeCfg(4, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
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
  // 29. Expression moves during a held slow gesture: continuous, and the
  // release lands on the NEW base (no stale value).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(
        makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 500.0f, 500.0f));
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
  // 30. Rapid gesture slams stay finite and bounded.
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
        e.setGestureConfig(makeCfg((i / 100) % 2 == 0 ? 0 : 4,
                                   (i / 100) % 2 == 0 ? GestureDir::Up : GestureDir::Down,
                                   GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
        if (held)
          e.pressGesture((int)((i / 100) % 2));
        else
          e.releaseGesture((int)((i / 100) % 2));
      }
      if (i % 150 == 0)
      {
        e.setGestureConfig(
            makeCfg(2, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 150.0f, 150.0f));
        e.oneShotGesture(4);
      }
      y[i] = e.process(sig[i]);
    }
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec gesture slams finite");
    TDM_CHECK(maxStep(y) < 0.3f, "spec gesture slams bounded");
  }
  // 31. Dive depth 0: press dives toward unison (harmless).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setGestureConfig(makeCfg(4, GestureDir::Down, GestureMotion::ToDepth, 0.0f, 50.0f, 50.0f));
    e.pressGesture(1);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const double f = zcFreq(y, (int)y.size() / 2, (int)y.size());
    TDM_CHECK(f > 215.0 && f < 225.0, "spec dive depth-0 unison");
  }
  // 32. From Depth (Reverse Dive): engages AT depth behind the engage
  // fade (bounded), sweeps back to live normal over Time, converges
  // exactly and disarms (one-shot included).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(Voice::WhamDown);
    e.setGestureConfig(
        makeCfg(4, GestureDir::Down, GestureMotion::FromDepth, 12.0f, 400.0f, 200.0f));
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
    s.setVoice(Voice::WhamDown);
    s.setGestureConfig(
        makeCfg(4, GestureDir::Down, GestureMotion::FromDepth, 12.0f, 200.0f, 200.0f));
    s.oneShotGesture(2);
    auto y2 = renderEngine(s, sig);
    TDM_CHECK(!s.gestureActive(), "spec reverse one-shot disarms");
    const double l2 = zcFreq(y2, (int)y2.size() - 24000, (int)y2.size());
    TDM_CHECK(l2 > 215.0 && l2 < 225.0, "spec reverse one-shot converges");
    TDM_CHECK(tdm_test::allFinite(y2.data(), (int)y2.size()), "spec reverse finite");
  }
  // 33. From Depth (Reverse Toe): upward mirror of test 32.
  {
    SpecLaser e;
    e.reset(kSr);
    // Slow sweep (800 ms): the 30 ms doppler slew needs ~100 ms to print
    // the depth before the sweep decays (fast From gestures peak lower).
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::FromDepth, 12.0f, 800.0f, 150.0f));
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
  // 34. Down-direction To gesture on WhamUp: press falls to dry from a
  // raised base, release climbs back.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setGestureConfig(makeCfg(0, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 4)
        e.pressGesture(5);
      if (i == 3 * n / 4)
        e.releaseGesture(5);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec drop gesture continuous");
    const double held = zcFreq(y, n / 2 - 12000, n / 2);
    TDM_CHECK(held > 215.0 && held < 225.0, "spec drop gesture falls dry");
    const double back = zcFreq(y, n - 12000, n);
    TDM_CHECK(back > 295.0 && back < 325.0, "spec drop gesture restores base");
  }
  // 35. Cross-voice Ring gesture: normal WhamUp + Ring Sweep renders
  // ringed (carrier reaches toe), release restores dry bit-exactly.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(2, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
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
  // 36. Detune is not a gesture voice: press ignored, output == normal.
  {
    SpecLaser a, b;
    a.reset(kSr);
    b.reset(kSr);
    a.setExpression(0.6f);
    b.setExpression(0.6f);
    b.setGestureConfig(makeCfg(3, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
    b.pressGesture(2);
    TDM_CHECK(!b.gestureActive(), "spec detune gesture ignored");
    auto sig = sine(0.4f, 220.0f, static_cast<int>(kSr));
    auto ya = renderEngine(a, sig), yb = renderEngine(b, sig);
    TDM_CHECK(bitEqual(ya.data(), yb.data(), (int)ya.size()), "spec detune gesture no-op");
  }
  // 37. Foreign release ignored; release-all leaves no stuck state.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 50.0f, 50.0f));
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
  // 38. One-shot To: reaches depth, auto-returns to base, disarms.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setGestureConfig(makeCfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 300.0f, 300.0f));
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
  // 39. Router: slow gesture audibly slews; dive reaches the sag band.
  {
    // Slow toe through the tap: mid-press pitch sits between heel/toe.
    DevSpecFx r;
    r.setSlotTimeMs(0, 500.0f);
    r.pressSlot(0);
    r.setEnabled(true);
    r.preTap().reset(kSr, 256);
    r.postTap().reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> io = sig;
    runTap(r.preTap(), io);
    const double mid = zcFreq(io, 10000, 14000); // ~250 ms in: mid-sweep
    TDM_CHECK(mid > 260.0 && mid < 400.0, "spec router gesture rate adopts");
    // Dive press through the tap reaches the sag band.
    DevSpecFx v;
    v.setSlotTimeMs(1, 50.0f);
    v.pressSlot(1);
    v.setEnabled(true);
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
  // 40. Slots are independent; triggering is by ID (no display concept).
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
    d.setEnabled(true);
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
  // 41. Disable mid-gesture: dry at the bottom; re-enable restores the
  // held gesture (no new command needed).
  {
    DevSpecFx d;
    d.setSlotTimeMs(1, 1000.0f); // slow: guaranteed mid-sweep at disable
    d.pressSlot(1);
    d.setEnabled(true);
    d.preTap().reset(kSr, 256);
    d.postTap().reset(kSr, 256);
    auto tail = sine(0.4f, 440.0f, 12000);
    auto dryRef = tail;
    runTap(d.preTap(), tail); // diving (wet, mid-sweep)
    d.setEnabled(false);
    tail = dryRef;
    runTap(d.preTap(), tail);
    runTap(d.postTap(), tail);
    TDM_CHECK(bitEqual(tail.data() + 256, dryRef.data() + 256, (int)tail.size() - 256),
              "spec disable mid-gesture dry");
    d.setEnabled(true); // slot 1 still held underneath (no new command)
    tail = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    auto ref2 = tail;
    runTap(d.preTap(), tail);
    float dev = 0.0f;
    for (size_t i = 0; i < tail.size(); ++i)
      dev = std::max(dev, std::fabs(tail[i] - ref2[i]));
    TDM_CHECK(dev > 0.01f, "spec re-enable restores gesture");
    TDM_CHECK(tdm_test::allFinite(tail.data(), (int)tail.size()), "spec gesture enable finite");
  }
  // 42. Transpose + creative simultaneously: production -2 feeds the
  // creative +12 (chain order preserved, both operations audible).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    DevSpecFx ds;
    ds.setExpression(1.0f); // creative toe (+12, sag ~+11.2)
    ds.setEnabled(true);
    rig.setSpecPre(&ds.preTap());
    rig.setSpecPost(&ds.postTap());
    rig.reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> o;
    rigRun(rig, sig, o);
    const int n = static_cast<int>(o.size());
    const double f = zcFreq(o, 2 * n / 3, n); // ~220 * 2^(-2/12) * 2^(11.2/12)
    TDM_CHECK(f > 340.0 && f < 410.0, "spec transpose plus creative");
    TDM_CHECK(tdm_test::allFinite(o.data(), n), "spec transpose plus creative finite");
  }
}

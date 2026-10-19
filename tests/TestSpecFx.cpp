// Special FX Laser lab tests: voice maps (heel/toe endpoints), sweep
// continuity, envelope follower (speed/sensitivity/trigger), router
// insert A/B + voice switching, rig-seam transparency. Synthetic sines
// validate DSP math only — never musical quality.

#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/lab/SpecialFx/DevSpecFx.h"
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
} // namespace

void runSpecFxTests()
{
  using tdm::specfx::DevSpecFx;
  using tdm::specfx::SpecLaser;
  using Voice = SpecLaser::Voice;
  using Control = SpecLaser::Control;

  // 1. Engine defaults.
  {
    SpecLaser e;
    e.reset(kSr);
    TDM_CHECK(e.voice() == Voice::Whammy, "spec default whammy");
    TDM_CHECK(e.expressionPos() == 0.0f, "spec default pos 0");
  }
  // 2. Whammy heel = bit-exact dry (doppler unity bypass, settled).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.0f);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(bitEqual(y.data(), sig.data(), (int)sig.size()), "spec whammy heel dry");
  }
  // 3. Whammy toe ~= +12 st. Narrow Goertzel is the WRONG oracle here:
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
  // 8. Slow sweeps are continuous in every voice. Bounds are
  // slope-aware: ring toe content reaches ~2.2 kHz (legit slope ~0.12),
  // far below real-jump territory (~0.4 measured pre-fix).
  for (Voice v : {Voice::Whammy, Voice::Dual, Voice::Ring, Voice::Detune})
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
  // 10. Envelope tracks attack/decay; sensitivity scales; speed shapes.
  {
    const int n = static_cast<int>(kSr);
    std::vector<float> burst(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      burst[(size_t)i] =
          0.8f * std::sin(2.0f * 3.14159265f * 110.0f * i / (float)kSr) * std::exp(-i / (0.15f * (float)kSr));
    SpecLaser e;
    e.reset(kSr);
    e.setControl(Control::Envelope);
    e.setSpeed(1.0f);
    e.setSensitivity(0.6f);
    float posAttack = 0.0f, posTail = 0.0f;
    for (int i = 0; i < n; ++i)
    {
      e.process(burst[(size_t)i]);
      if (i == 4800)
        posAttack = e.expressionPos();
      if (i == n - 1)
        posTail = e.expressionPos();
    }
    TDM_CHECK(posAttack > 0.5f, "spec envelope attacks");
    TDM_CHECK(posTail < 0.1f, "spec envelope releases");
    // Sensitivity: same burst, low vs high.
    float lo = 0.0f, hi = 0.0f;
    for (int pass = 0; pass < 2; ++pass)
    {
      SpecLaser s;
      s.reset(kSr);
      s.setControl(Control::Envelope);
      s.setSpeed(1.0f);
      s.setSensitivity(pass == 0 ? 0.2f : 0.9f);
      for (int i = 0; i < 4800; ++i)
        s.process(burst[(size_t)i]);
      (pass == 0 ? lo : hi) = s.expressionPos();
    }
    TDM_CHECK(hi > lo + 0.1f, "spec sensitivity scales");
    // Speed: fast reaches higher earlier.
    float slow = 0.0f, fast = 0.0f;
    for (int pass = 0; pass < 2; ++pass)
    {
      SpecLaser s;
      s.reset(kSr);
      s.setControl(Control::Envelope);
      s.setSpeed(pass == 0 ? 0.0f : 1.0f);
      s.setSensitivity(0.6f);
      for (int i = 0; i < 2400; ++i) // 50 ms in
        s.process(burst[(size_t)i]);
      (pass == 0 ? slow : fast) = s.expressionPos();
    }
    TDM_CHECK(fast > slow, "spec speed shapes attack");
  }
  // 11/12. Trigger: momentary toe in Manual, ignored in Envelope.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.2f);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(kSr));
    for (int i = 0; i < 48000; ++i)
      e.process(sig[(size_t)(i % (int)sig.size())]);
    TDM_CHECK(std::fabs(e.expressionPos() - 0.2f) < 0.02f, "spec manual parks");
    e.setTrigger(true);
    for (int i = 0; i < 24000; ++i)
      e.process(0.1f);
    TDM_CHECK(e.expressionPos() > 0.95f, "spec trigger toes");
    e.setTrigger(false);
    for (int i = 0; i < 48000; ++i)
      e.process(0.1f);
    TDM_CHECK(std::fabs(e.expressionPos() - 0.2f) < 0.02f, "spec trigger releases");
    e.setControl(Control::Envelope);
    e.setTrigger(true);
    for (int i = 0; i < 48000; ++i)
      e.process(0.0f); // silence: envelope rules, not the trigger
    TDM_CHECK(e.expressionPos() < 0.05f, "spec trigger ignored in envelope");
  }
  // 13/14. Determinism + non-finite safety.
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

  // 15. Router defaults.
  {
    DevSpecFx d;
    TDM_CHECK(!d.isEnabled(), "spec router default off");
    TDM_CHECK(d.voice() == Voice::Whammy, "spec router default whammy");
    TDM_CHECK(d.control() == Control::Manual, "spec router default manual");
    TDM_CHECK(d.insert() == DevSpecFx::Insert::Pre, "spec router default pre");
    TDM_CHECK(d.mix01() == 1.0f, "spec router default mix");
  }
  // 16. Off transparent (taps + full rig with seams).
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
  // 17. Mix 0 transparent enabled (both inserts).
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
  // 18. Param adoption through the router (whammy toe shifts).
  {
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    DevSpecFx d;
    d.setVoice(Voice::Whammy);
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
  // 19. Voice switching click-free (dip + preroll).
  {
    auto sig = sine(0.4f, 440.0f, static_cast<int>(4 * kSr));
    DevSpecFx d;
    d.setEnabled(true);
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
  // 20. Insert switching click-free.
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
  // 21. Enable cycle: dry at the bottom, wet after.
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
  // 22. Rig seam: install -> wet; remove -> dry again.
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
  // 23. Hostile UI chatter stays finite and bounded.
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
      d.setVoice(static_cast<Voice>((off / 128) % 4));
      d.setInsert(((off / 128) % 2 == 0) ? DevSpecFx::Insert::Pre : DevSpecFx::Insert::Post);
      d.setExpression(((off / 128) % 3 == 0) ? 1.0f : 0.2f);
      const int m = (n - off < 128) ? (n - off) : 128;
      float* p = io.data() + off;
      d.preTap().process(p, p, m);
      d.postTap().process(p, p, m);
    }
    TDM_CHECK(tdm_test::allFinite(io.data(), n), "spec hostile finite");
    TDM_CHECK(maxStep(io) < 0.3f, "spec hostile bounded");
  }
  // 24. Router never touches rig params.
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

  // 25. GOLDEN: milestone voice renders bit-identical (accepted Whammy/
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
  // 26. GOLDEN: legacy toe-press transient bit-identical at defaults
  // (0 ms gesture = snap; the 5 ms smoother does all slewing, as before).
  {
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    const int n = static_cast<int>(sig.size());
    SpecLaser e;
    e.reset(kSr);
    std::vector<float> y(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
      e.setExpression(0.0f);
      if (i == n / 2)
        e.setTrigger(true);
      y[i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(fnvBits(y) == 0xea5873b3e8c0d5ffULL, "spec golden toe-press preserved");
  }
  // 27. Toe rise/return timing: independent linear ramps, exact settle.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setToeRiseMs(500.0f);
    e.setToeReturnMs(200.0f);
    e.setTrigger(true);
    int riseAt = -1;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
    {
      e.process(0.1f);
      if (e.toeGesture() == 1.0f)
      {
        riseAt = i;
        break;
      }
    }
    TDM_CHECK(riseAt > 0 && std::abs(riseAt - 24000) <= 64, "spec toe rise 500ms");
    e.setTrigger(false);
    int retAt = -1;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
    {
      e.process(0.1f);
      if (e.toeGesture() == 0.0f)
      {
        retAt = i;
        break;
      }
    }
    TDM_CHECK(retAt > 0 && std::abs(retAt - 9600) <= 64, "spec toe return 200ms");
  }
  // 28. Default toe is instant (legacy stab): gesture snaps in 1 sample,
  // position reaches full within 20 ms through the 5 ms smoother.
  {
    SpecLaser e;
    e.reset(kSr);
    e.process(0.1f);
    e.setTrigger(true);
    e.process(0.1f);
    TDM_CHECK(e.toeGesture() == 1.0f, "spec default toe snaps");
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
    TDM_CHECK(fullAt > 0 && fullAt < 960, "spec default toe fast");
  }
  // 29. Dive accuracy: -12 target on 220 Hz lands in the sag band;
  // depth 6 lands near the tritone; uniform on Ring voice too.
  {
    for (Voice v : {Voice::Whammy, Voice::Ring})
    {
      SpecLaser e;
      e.reset(kSr);
      e.setVoice(v);
      e.setDiveDepthSt(12.0f);
      e.setDiveTimeMs(50.0f);
      e.setDive(true);
      auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
      auto y = renderEngine(e, sig);
      const int n = static_cast<int>(y.size());
      const double f = zcFreq(y, n / 2, n);
      TDM_CHECK(f > 100.0 && f < 130.0, "spec dive -12 band"); // sag zone, see probe
      TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec dive finite");
    }
    SpecLaser e;
    e.reset(kSr);
    e.setDiveDepthSt(6.0f);
    e.setDiveTimeMs(50.0f);
    e.setDive(true);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    TDM_CHECK(zcFreq(y, (int)y.size() / 2, (int)y.size()) > 145.0
                  && zcFreq(y, (int)y.size() / 2, (int)y.size()) < 165.0,
              "spec dive -6 band");
  }
  // 30. Dive sweep + return continuous; release restores the live base.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setDiveTimeMs(300.0f);
    e.setDiveReturnMs(200.0f);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 3)
        e.setDive(true);
      if (i == 2 * n / 3)
        e.setDive(false);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec dive sweep continuous");
    const double back = zcFreq(y, n - 24000, n);
    TDM_CHECK(back > 295.0 && back < 325.0, "spec dive restores base"); // +6 st
  }
  // 31. Precedence: dive wins over toe while both held; releases reveal
  // the live underneath, all continuous.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setDiveTimeMs(50.0f);
    e.setDiveReturnMs(50.0f);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(4 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    const int q = n / 4;
    e.setTrigger(true); // toe first (would be +12 alone)
    for (int i = 0; i < n; ++i)
    {
      if (i == q)
        e.setDive(true); // dive joins: takes over downward
      if (i == 2 * q)
        e.setDive(false); // dive releases: toe revealed
      if (i == 3 * q)
        e.setTrigger(false); // toe releases: base (heel) revealed
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec gesture handoffs continuous");
    TDM_CHECK(zcFreq(y, q / 2, q) > 404.0, "spec toe alone toe-up");
    const double dv = zcFreq(y, q + q / 4, 2 * q);
    TDM_CHECK(dv > 100.0 && dv < 130.0, "spec dive wins while held");
    TDM_CHECK(zcFreq(y, 2 * q + q / 4, 3 * q) > 404.0, "spec dive release reveals toe");
    TDM_CHECK(tdm_test::allFinite(y.data(), n), "spec gesture handoffs finite");
  }
  // 32. Expression moves during a held slow toe: continuous, and the
  // release lands on the NEW base (no stale value).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setToeRiseMs(500.0f);
    e.setToeReturnMs(500.0f);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> y(sig.size());
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; ++i)
    {
      if (i == n / 6)
        e.setTrigger(true);
      if (i == n / 3)
        e.setExpression(0.5f); // base moves under the held gesture
      if (i == 2 * n / 3)
        e.setTrigger(false);
      y[(size_t)i] = e.process(sig[(size_t)i]);
    }
    TDM_CHECK(maxStep(y) < 0.15f, "spec base move under gesture continuous");
    const double back = zcFreq(y, n - 24000, n);
    TDM_CHECK(back > 295.0 && back < 325.0, "spec release lands on new base");
  }
  // 33. Source switch with toe held: fast glide to the live envelope,
  // bounded, settles; switching back restores toe.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.0f);
    e.setTrigger(true);
    std::vector<float> y;
    y.reserve(96000);
    for (int i = 0; i < 24000; ++i)
      y.push_back(e.process(0.3f)); // toe up in manual
    e.setControl(Control::Envelope);
    for (int i = 0; i < 24000; ++i)
      y.push_back(e.process(0.0f)); // silence: envelope rules now
    TDM_CHECK(maxStep(y) < 0.3f, "spec source switch bounded");
    TDM_CHECK(e.expressionPos() < 0.05f, "spec source switch settles to envelope");
    e.setControl(Control::Manual); // toe still held underneath
    for (int i = 0; i < 24000; ++i)
      y.push_back(e.process(0.0f));
    TDM_CHECK(e.expressionPos() > 0.95f, "spec switch back reveals toe");
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec source switch finite");
  }
  // 34. Rapid gesture slams stay finite and bounded.
  {
    SpecLaser e;
    e.reset(kSr);
    e.setToeRiseMs(100.0f);
    e.setToeReturnMs(100.0f);
    e.setDiveTimeMs(150.0f);
    e.setDiveReturnMs(150.0f);
    auto sig = sine(0.4f, 196.0f, static_cast<int>(2 * kSr));
    std::vector<float> y(sig.size());
    bool trig = false, dive = false;
    for (size_t i = 0; i < sig.size(); ++i)
    {
      if (i % 100 == 0)
      {
        trig = !trig;
        e.setTrigger(trig);
      }
      if (i % 150 == 0)
      {
        dive = !dive;
        e.setDive(dive);
      }
      y[i] = e.process(sig[i]);
    }
    TDM_CHECK(tdm_test::allFinite(y.data(), (int)y.size()), "spec gesture slams finite");
    TDM_CHECK(maxStep(y) < 0.3f, "spec gesture slams bounded");
  }
  // 35. Dive depth 0: press dives toward unison (harmless).
  {
    SpecLaser e;
    e.reset(kSr);
    e.setExpression(0.5f);
    e.setDiveDepthSt(0.0f);
    e.setDiveTimeMs(50.0f);
    e.setDive(true);
    auto sig = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    auto y = renderEngine(e, sig);
    const double f = zcFreq(y, (int)y.size() / 2, (int)y.size());
    TDM_CHECK(f > 215.0 && f < 225.0, "spec dive depth-0 unison");
  }
  // 36. Router: gesture defaults + adoption (slow toe audibly slews).
  {
    DevSpecFx d;
    TDM_CHECK(!d.dive(), "spec router default dive off");
    TDM_CHECK(d.toeRiseMs() == 0.0f && d.toeReturnMs() == 0.0f, "spec router default toe fast");
    TDM_CHECK(d.diveDepthSt() == 12.0f, "spec router default depth");
    TDM_CHECK(d.diveTimeMs() == 300.0f && d.diveReturnMs() == 200.0f,
              "spec router default dive times");
    // Slow toe through the tap: mid-press pitch sits between heel/toe.
    DevSpecFx r;
    r.setToeRiseMs(500.0f);
    r.setTrigger(true);
    r.setEnabled(true);
    r.preTap().reset(kSr, 256);
    r.postTap().reset(kSr, 256);
    auto sig = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> io = sig;
    runTap(r.preTap(), io);
    const double mid = zcFreq(io, 10000, 14000); // ~250 ms in: mid-sweep
    TDM_CHECK(mid > 260.0 && mid < 400.0, "spec router toe rate adopts");
    // Dive press through the tap reaches the sag band.
    DevSpecFx v;
    v.setDiveTimeMs(50.0f);
    v.setDive(true);
    v.setEnabled(true);
    v.preTap().reset(kSr, 256);
    v.postTap().reset(kSr, 256);
    std::vector<float> io2 = sig;
    runTap(v.preTap(), io2);
    const double dv = zcFreq(io2, (int)io2.size() / 2, (int)io2.size());
    TDM_CHECK(dv > 100.0 && dv < 130.0, "spec router dive adopts");
  }
  // 37. Disable mid-gesture: dry at the bottom; re-enable restores dive.
  {
    DevSpecFx d;
    d.setDiveTimeMs(1000.0f); // slow: guaranteed mid-sweep at disable
    d.setDive(true);
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
    d.setEnabled(true); // dive still held underneath
    tail = sine(0.4f, 220.0f, static_cast<int>(2 * kSr));
    auto ref2 = tail;
    runTap(d.preTap(), tail);
    float dev = 0.0f;
    for (size_t i = 0; i < tail.size(); ++i)
      dev = std::max(dev, std::fabs(tail[i] - ref2[i]));
    TDM_CHECK(dev > 0.01f, "spec re-enable restores gesture");
    TDM_CHECK(tdm_test::allFinite(tail.data(), (int)tail.size()), "spec gesture enable finite");
  }
}

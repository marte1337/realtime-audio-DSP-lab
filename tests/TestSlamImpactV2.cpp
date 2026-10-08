// Impact v2 tests: Sub/Thump/Punch voice character, adaptive low-band gain,
// branch limiter, DevSlam voice/sensitivity/boost integration, detector
// refire + sensitivity pins (frozen SlamTrigger), transpose parity, and the
// Mass-vs-Impact event/continuous distinction. Push/Crush/Mass algorithms
// are untouched by v2 (their pins live in TestDevSlam/TestSlam).

#include "tests/Assert.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dsp/Pitch/GuitarTranspose.h"
#include "dsp/lab/Slam/DevSlam.h"
#include "dsp/lab/Slam/SlamCandidates.h"
#include "dsp/lab/Slam/SlamImpactV2.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

std::string sf(const char* fmt, ...)
{
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return std::string(b);
}
#define CHECK(cond, ...) TDM_CHECK(cond, sf(__VA_ARGS__))

bool bitEqual(const float* a, const float* b, int n)
{
  for (int i = 0; i < n; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}
double dbOf(double v) { return 20.0 * std::log10(v + 1e-12); }
float winPeak(const float* x, int n)
{
  float p = 0.0f;
  for (int i = 0; i < n; ++i)
    p = std::max(p, std::fabs(x[i]));
  return p;
}
double winRms(const float* x, int n)
{
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += (double)x[i] * x[i];
  return std::sqrt(s / (n > 0 ? n : 1));
}

// Periodic choked-100Hz pips (matches TestDevSlam attacks()).
std::vector<float> pips(int count, float periodMs, float peak = 0.8f)
{
  const int gap = (int)(periodMs * 0.001 * kSr + 0.5);
  const int n = (int)(kSr / 2) + count * gap + (int)kSr / 5;
  std::vector<float> sig((size_t)n, 0.0f);
  const int at0 = (int)(kSr / 2);
  for (int p = 0; p < count; ++p)
  {
    const int at = at0 + p * gap;
    for (int i = 0; i < (int)(kSr / 10) && at + i < n; ++i)
      sig[(size_t)(at + i)] += peak
          * std::sin(2.0f * 3.141592653589793f * 100.0f * i / (float)kSr)
          * std::exp(-i / (0.020f * (float)kSr));
  }
  return sig;
}

// Rig-like post-IR pip: hot broadband attack over a small low body
// (dry peak ~10x the low-band peak, as measured post-IR on the 5150).
std::vector<float> rigPip(float peak)
{
  const int n = (int)(kSr / 2);
  std::vector<float> sig((size_t)n, 0.0f);
  for (int i = 0; i < n; ++i)
  {
    const float t = i / (float)kSr;
    sig[(size_t)i] = peak
        * (0.10f * std::sin(2.0f * 3.141592653589793f * 100.0f * t) * std::exp(-t / 0.025f)
           + 0.90f * std::sin(2.0f * 3.141592653589793f * 830.0f * t) * std::exp(-t / 0.008f));
  }
  return sig;
}

void runTaps(tdm::lab::DevSlam& d, std::vector<float>& io)
{
  const int n = (int)io.size();
  for (int off = 0; off < n; off += kBlock)
  {
    const int m = (n - off < kBlock) ? (n - off) : kBlock;
    float* p = io.data() + off;
    d.preDrive().process(p, p, m);
    d.postNam().process(p, p, m);
    d.postIr().process(p, p, m);
  }
}

float maxStep(const float* x, int n)
{
  float ms = 0.0f;
  for (int i = 1; i < n; ++i)
    ms = std::max(ms, std::fabs(x[i] - x[i - 1]));
  return ms;
}

// Band RMS of x[a, b) with 200 ms filter settle from the same signal.
double bandRms(const std::vector<float>& x, int a, int b, float hpHz, float lpHz)
{
  using tdm::lab::SlamBiquad;
  SlamBiquad hp, lp;
  hp.reset();
  lp.reset();
  if (hpHz > 0.0f)
    hp.setHighpass(kSr, hpHz, 0.7071f);
  if (lpHz > 0.0f)
    lp.setLowpass(kSr, lpHz, 0.7071f);
  const int pre = (int)(0.2 * kSr), s0 = (a > pre) ? a - pre : 0;
  const int n = (int)x.size();
  for (int i = s0; i < a; ++i)
  {
    float v = x[(size_t)i];
    if (hpHz > 0.0f)
      v = hp.process(v);
    if (lpHz > 0.0f)
      v = lp.process(v);
  }
  double s = 0.0;
  int c = 0;
  for (int i = a; i < b && i < n; ++i)
  {
    float v = x[(size_t)i];
    if (hpHz > 0.0f)
      v = hp.process(v);
    if (lpHz > 0.0f)
      v = lp.process(v);
    s += (double)v * v;
    ++c;
  }
  return std::sqrt(s / (c > 0 ? c : 1));
}

// Sparse rig-pip train (rig-like attacks the detector fires on).
std::vector<float> rigPipTrain(int count, float periodMs, float peak)
{
  const int gap = (int)(periodMs * 0.001 * kSr + 0.5);
  const int n = (int)(kSr / 2) + count * gap + (int)kSr / 5;
  std::vector<float> sig((size_t)n, 0.0f);
  auto one = rigPip(peak);
  const int at0 = (int)(kSr / 2);
  for (int p = 0; p < count; ++p)
  {
    const int at = at0 + p * gap;
    for (size_t i = 0; i < one.size() && at + (int)i < n; ++i)
      sig[(size_t)(at + i)] += one[i];
  }
  return sig;
}

int64_t countFires(const std::vector<float>& sig, float sens)
{
  tdm::lab::SlamTrigger t;
  t.reset(kSr, sens, 90.0f);
  for (float v : sig)
    t.feed(v);
  return t.fireCount();
}

using Voice = tdm::lab::SlamImpactV2::Voice;
using IVoice = tdm::lab::DevSlam::ImpactVoice;
const char* voiceName(Voice v)
{
  return v == Voice::Sub ? "sub" : v == Voice::Thump ? "thump" : "punch";
}

// One scheduled fire on dry at sample `at` (mimics live fire timing):
// returns the wet render.
std::vector<float> fireOnce(Voice v, const std::vector<float>& dry, int at, float s, float amt = 1.0f,
                            tdm::lab::SlamImpactV2::GainLaw law
                            = tdm::lab::SlamImpactV2::GainLaw::LegacyAdaptive,
                            float tdb = -6.0f)
{
  tdm::lab::SlamImpactV2 voice;
  voice.reset(kSr);
  voice.setVoice(v);
  voice.setAmount(amt);
  voice.setGainLaw(law);
  voice.setTargetDb(tdb);
  std::vector<float> wet(dry.size());
  for (size_t i = 0; i < dry.size(); ++i)
    wet[i] = voice.render(dry[i], (int)i == at ? s : 0.0f);
  return wet;
}

// Single decay pip at loHz + a little 830 Hz edge, normalized to `peak`.
std::vector<float> specPip(float loHz, float peak)
{
  const int n = (int)(0.6 * kSr);
  std::vector<float> d((size_t)n, 0.0f);
  for (int i = 0; i < n; ++i)
  {
    const float t = i / (float)kSr;
    d[(size_t)i] = (0.85f * std::sin(2.0f * 3.141592653589793f * loHz * t)
                   + 0.15f * std::sin(2.0f * 3.141592653589793f * 830.0f * t))
        * std::exp(-t / 0.040f);
  }
  float mp = 0.0f;
  for (float v : d)
    mp = std::max(mp, std::fabs(v));
  for (float& v : d)
    v *= peak / mp;
  return d;
}
} // namespace

void runSlamImpactV2Tests()
{
  // A. Voice render basics (direct, scheduled fires).
  for (Voice v : {Voice::Sub, Voice::Thump, Voice::Punch})
  {
    // Amount 0 is dry-exact even THROUGH a fire.
    {
      auto dry = rigPip(0.4f);
      auto wet = fireOnce(v, dry, 384, 0.9f, 0.0f);
      CHECK(bitEqual(wet.data(), dry.data(), (int)dry.size()), "%s amount0 dry-exact",
            voiceName(v));
    }
    // No fires: bit-exact copy-through (states advance, output untouched).
    {
      tdm::lab::SlamImpactV2 voice;
      voice.reset(kSr);
      voice.setVoice(v);
      voice.setAmount(1.0f);
      auto dry = rigPip(0.4f);
      std::vector<float> wet(dry.size());
      for (size_t i = 0; i < dry.size(); ++i)
        wet[i] = voice.render(dry[i], 0.0f);
      CHECK(bitEqual(wet.data(), dry.data(), (int)dry.size()), "%s no-fire dry-exact",
            voiceName(v));
      CHECK(voice.limiterMin() == 1.0f, "%s limiter untouched without fires", voiceName(v));
    }
    // A fire produces a finite, nonzero, decaying event.
    {
      auto dry = rigPip(0.4f);
      auto wet = fireOnce(v, dry, 100, 0.9f);
      CHECK(tdm_test::allFinite(wet.data(), (int)wet.size()), "%s fire finite", voiceName(v));
      std::vector<float> br(wet.size());
      for (size_t i = 0; i < br.size(); ++i)
        br[i] = wet[i] - dry[i];
      CHECK(winPeak(br.data(), (int)br.size()) > 1e-3f, "%s fire adds energy", voiceName(v));
      const int tail = 100 + (int)(0.300 * kSr);
      CHECK(winPeak(br.data() + tail, (int)br.size() - tail) < 1e-4f, "%s decayed by 300 ms",
            voiceName(v));
    }
    // Velocity dynamics: saturated or not, branch scales with s (the
    // saturation normalizes by s first, so dynamics survive saturation).
    {
      auto dry = rigPip(0.3f);
      auto wetHi = fireOnce(v, dry, 100, 1.0f);
      auto wetLo = fireOnce(v, dry, 100, 0.3f);
      std::vector<float> brHi(dry.size()), brLo(dry.size());
      for (size_t i = 0; i < dry.size(); ++i)
      {
        brHi[i] = wetHi[i] - dry[i];
        brLo[i] = wetLo[i] - dry[i];
      }
      const double ratio =
          winPeak(brHi.data(), (int)brHi.size()) / winPeak(brLo.data(), (int)brLo.size());
      CHECK(std::fabs(ratio - 1.0 / 0.3) < (1.0 / 0.3) * 0.05, "%s velocity linear (%.2f)",
            voiceName(v), ratio);
    }
  }
  // Voice spectral character: strictly rising knock-band (HP 500 Hz)
  // energy Sub < Thump < Punch. HP 500 isolates Punch's saturated mid
  // harmonics (3rd of 130-200 Hz) from Thump's milder/higher-order content
  // and Sub's nothing-above-bloom.
  {
    auto dry = rigPip(0.4f);
    double hi[3] = {0, 0, 0};
    double midhi[3] = {0, 0, 0};
    int vi = 0;
    for (Voice v : {Voice::Sub, Voice::Thump, Voice::Punch})
    {
      auto wet = fireOnce(v, dry, 100, 0.9f);
      std::vector<float> br(dry.size());
      for (size_t i = 0; i < dry.size(); ++i)
        br[i] = wet[i] - dry[i];
      hi[vi] = dbOf(bandRms(br, 100, 100 + (int)(0.150 * kSr), 500.0f, 0.0f));
      midhi[vi] = dbOf(bandRms(br, 100, 100 + (int)(0.150 * kSr), 400.0f, 0.0f));
      ++vi;
    }
    CHECK(hi[1] > hi[0] + 6.0 && hi[2] > hi[1] + 6.0, "event band sub<thump<punch (%+.1f/%+.1f/%+.1f)",
          hi[0], hi[1], hi[2]);
    CHECK(midhi[1] > midhi[0] + 6.0, "thump harmonics over sub (%+.1f dB)", midhi[1] - midhi[0]);
    CHECK(midhi[2] > midhi[0] + 10.0, "punch knock harmonics over sub (%+.1f dB)",
          midhi[2] - midhi[0]);
  }

  // B. Adaptive low-band gain.
  {
    // A 9 dB rig-level sweep barely moves the branch/main ratio (fixed
    // gain would drift the full 9 dB).
    auto loud = rigPip(0.4f);
    auto quiet = rigPip(0.4f / 2.818f); // -9 dB
    for (Voice v : {Voice::Sub, Voice::Thump, Voice::Punch})
    {
      auto wetL = fireOnce(v, loud, 100, 0.9f);
      auto wetQ = fireOnce(v, quiet, 100, 0.9f);
      std::vector<float> brL(loud.size()), brQ(quiet.size());
      for (size_t i = 0; i < brL.size(); ++i)
        brL[i] = wetL[i] - loud[i];
      for (size_t i = 0; i < brQ.size(); ++i)
        brQ[i] = wetQ[i] - quiet[i];
      const double rL = dbOf(winPeak(brL.data(), (int)brL.size())) - dbOf(winPeak(loud.data(), 2000));
      const double rQ =
          dbOf(winPeak(brQ.data(), (int)brQ.size())) - dbOf(winPeak(quiet.data(), 2000));
      CHECK(std::fabs(rL - rQ) <= 2.0, "%s adaptive across 9 dB (%.1f vs %.1f)", voiceName(v),
            rL, rQ);
      CHECK(winPeak(brQ.data(), (int)brQ.size()) > 1e-4f, "%s quiet still slams",
            voiceName(v));
    }
    // Gain law itself is linear just above the fade knee (direct pin).
    {
      tdm::lab::SlamImpactV2 a, b;
      a.reset(kSr);
      b.reset(kSr);
      a.setVoice(Voice::Sub);
      b.setVoice(Voice::Sub);
      const int n = (int)(kSr / 4);
      for (int i = 0; i < n; ++i) // settle trackers on lows-only beds
      {
        const float bed =
            0.012f * std::sin(2.0f * 3.141592653589793f * 90.0f * i / (float)kSr);
        a.render(i == n - 1 ? bed : bed, 0.0f);
        b.render(2.0f * bed, 0.0f);
      }
      a.render(0.012f, 0.8f);
      b.render(0.024f, 0.8f);
      const double ratio = b.lastFireGain() / a.lastFireGain();
      CHECK(std::fabs(ratio - 2.0) < 0.1, "gain linear above knee (%.2f)", ratio);
    }
    // Per-hit fresh: a soft hit 300 ms after a loud one gets its own
    // (small) gain, not the loud hit's (70 ms tracker release).
    {
      tdm::lab::SlamImpactV2 voice;
      voice.reset(kSr);
      voice.setVoice(Voice::Thump);
      auto loud = rigPip(0.4f);
      auto soft = rigPip(0.1f); // -12 dB
      const int gap = (int)(0.300 * kSr);
      std::vector<float> sig((size_t)(loud.size() + gap + soft.size()), 0.0f);
      for (size_t i = 0; i < loud.size(); ++i)
        sig[i] = loud[i];
      for (size_t i = 0; i < soft.size(); ++i)
        sig[(size_t)(loud.size() + gap) + i] = soft[i];
      std::vector<float> wet(sig.size());
      const int f1 = 100, f2 = (int)(loud.size() + gap) + 100;
      for (size_t i = 0; i < sig.size(); ++i)
        wet[i] = voice.render(sig[i], ((int)i == f1 || (int)i == f2) ? 0.9f : 0.0f);
      const double b1 = winPeak(wet.data() + f1, 2000) - 0.0; // branch+ (dry tiny here)
      std::vector<float> br(sig.size());
      for (size_t i = 0; i < sig.size(); ++i)
        br[i] = wet[i] - sig[i];
      const double p1 = winPeak(br.data() + f1, (int)(0.150 * kSr));
      const double p2 = winPeak(br.data() + f2, (int)(0.150 * kSr));
      const double expect = dbOf(winPeak(loud.data(), 2000)) - dbOf(winPeak(soft.data(), 2000));
      const double got = dbOf(p1) - dbOf(p2);
      (void)b1;
      CHECK(std::fabs(got - expect) <= 3.0, "per-hit fresh (%.1f vs %.1f dB)", got, expect);
    }
    // Silence fade: firing into digital silence stays silent.
    {
      tdm::lab::SlamImpactV2 voice;
      voice.reset(kSr);
      voice.setVoice(Voice::Punch);
      const int n = (int)(kSr / 2);
      float worst = 0.0f;
      for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(voice.render(0.0f, i == 100 ? 1.0f : 0.0f)));
      CHECK(worst < 1e-6f, "silence fade (%.2e)", (double)worst);
    }
    // Late-peak glide: gain catches a peak arriving after the fire.
    {
      auto gRun = [&](bool latePeak) {
        tdm::lab::SlamImpactV2 voice;
        voice.reset(kSr);
        voice.setVoice(Voice::Thump);
        const int n = (int)(kSr / 4);
        for (int i = 0; i < n; ++i)
        {
          float dry = 0.02f
              * std::sin(2.0f * 3.141592653589793f * 120.0f * i / (float)kSr);
          if (latePeak && i >= 100 + (int)(0.003 * kSr) && i < 100 + (int)(0.010 * kSr))
            dry += 0.30f * std::sin(2.0f * 3.141592653589793f * 120.0f * i / (float)kSr);
          voice.render(dry, i == 100 ? 0.9f : 0.0f);
        }
        return voice.lastFireGain();
      };
      const float gQuiet = gRun(false), gLate = gRun(true);
      CHECK(gLate > gQuiet * 3.0f, "glide catches late peak (%.3f vs %.3f)", (double)gLate,
            (double)gQuiet);
    }
    // Limiter: a hot sustained bed + full fire cannot clip, and the
    // limiter reports engagement; normal levels never engage it.
    {
      tdm::lab::SlamImpactV2 hot;
      hot.reset(kSr);
      hot.setVoice(Voice::Sub); // hottest k
      const int n = (int)(kSr / 2);
      float peak = 0.0f;
      for (int i = 0; i < n; ++i)
      {
        const float dry =
            0.70f * std::sin(2.0f * 3.141592653589793f * 62.0f * i / (float)kSr);
        peak = std::max(peak, std::fabs(hot.render(dry, i == 1000 ? 1.0f : 0.0f)));
      }
      CHECK(peak <= 0.99f, "limiter clip-safe (%.3f)", (double)peak);
      CHECK(hot.limiterMin() < 0.95f, "limiter engaged when needed (%.2f)",
            (double)hot.limiterMin());
      tdm::lab::SlamImpactV2 calm;
      calm.reset(kSr);
      calm.setVoice(Voice::Punch);
      auto pip = rigPip(0.3f);
      for (size_t i = 0; i < pip.size(); ++i)
        calm.render(pip[i], (int)i == 100 ? 0.9f : 0.0f);
      CHECK(calm.limiterMin() == 1.0f, "limiter transparent normally");
    }
  }

  // C. DevSlam voice/sensitivity/boost integration.
  {
    // Amount 0 transparent per v2 voice (through all taps, fires and all).
    for (IVoice v : {IVoice::Sub, IVoice::Thump, IVoice::Punch})
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(v);
      d.setAmount01(0.0f);
      d.preDrive().reset(kSr, kBlock);
      auto atk = pips(3, 400.0f);
      std::vector<float> io = atk;
      runTaps(d, io);
      CHECK(bitEqual(io.data(), atk.data(), (int)atk.size()), "v2 amount0 transparent (%d)",
            (int)v);
      CHECK(d.impactFireCount() == 3, "v2 amount0 still fires (%d)", (int)v);
    }
    // Voice switching mid-burst: finite, dip-click-bounded (house
    // convention: worst step near the switch vs the dry slope), adopts.
    {
      auto atk = rigPipTrain(4, 400.0f, 0.4f);
      const float dryStep = maxStep(atk.data(), (int)atk.size());
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Sub);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> io = atk;
      const int n = (int)io.size();
      // Switch 20 ms after the 2nd pip onset: a Sub burst is active.
      const int swAt = (int)(kSr / 2) + (int)(0.400 * kSr) + (int)(0.020 * kSr);
      const int swBlk = (swAt / kBlock) * kBlock;
      bool switched = false;
      for (int off = 0; off < n; off += kBlock)
      {
        if (!switched && off >= swBlk)
        {
          d.setImpactVoice(IVoice::Punch);
          switched = true;
        }
        float* p = io.data() + off;
        const int m = (n - off < kBlock) ? (n - off) : kBlock;
        d.preDrive().process(p, p, m);
        d.postNam().process(p, p, m);
        d.postIr().process(p, p, m);
      }
      CHECK(tdm_test::allFinite(io.data(), n), "voice switch finite");
      const float sw = maxStep(io.data() + swBlk - 2048, 8192);
      CHECK(sw < dryStep * 4.0f + 0.01f, "voice switch click-bounded (%.5f vs %.5f)", sw,
            dryStep);
      CHECK(d.activeImpactVoice() == IVoice::Punch, "voice switch adopts");
      CHECK(d.impactFireCount() == 4, "voice switch keeps telemetry (%lld)",
            (long long)d.impactFireCount());
    }
    // Staging a voice while Push is active: no dip, Push render intact.
    {
      auto sig = pips(2, 400.0f);
      auto run = [&](bool stage) {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Push);
        d.setAmount01(1.0f);
        d.preDrive().reset(kSr, kBlock);
        std::vector<float> io = sig;
        runTaps(d, io);
        if (stage)
        {
          d.setImpactVoice(IVoice::Punch); // staged for later
          runTaps(d, io);
        }
        return std::make_pair(io, d.activeFlavor());
      };
      auto a = run(false);
      tdm::lab::DevSlam d2;
      d2.setEnabled(true);
      d2.setFlavor(tdm::lab::DevSlam::Flavor::Push);
      d2.setAmount01(1.0f);
      d2.setImpactVoice(IVoice::Punch); // staged BEFORE reset: parks silently
      d2.preDrive().reset(kSr, kBlock);
      std::vector<float> io2 = sig;
      runTaps(d2, io2);
      CHECK(bitEqual(a.first.data(), io2.data(), (int)sig.size()), "staged voice inert");
      CHECK(a.second == tdm::lab::DevSlam::Flavor::Push, "staging keeps flavour");
    }
    // Sensitivity adopted live; router telemetry matches the standalone
    // detector at both ends of the sweep.
    {
      tdm::lab::DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setImpactSensitivity(0.7f);
      auto groove = pips(6, 230.0f);
      std::vector<float> io = groove;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      runTaps(d, io);
      CHECK(d.currentImpactSens() == 0.7f, "sensitivity adopted");
      for (float s : {0.4f, 0.6f})
      {
        tdm::lab::DevSlam dd;
        dd.preDrive().reset(kSr, kBlock);
        dd.setEnabled(true);
        dd.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        dd.setImpactSensitivity(s);
        std::vector<float> io2 = groove;
        runTaps(dd, io2);
        CHECK(dd.impactFireCount() == countFires(groove, s), "router/detector parity @%.1f", s);
      }
    }
    // Test boost: exact 2x branch on v2, ignored by Legacy.
    {
      auto atk = pips(2, 500.0f, 0.05f); // quiet: limiter stays out (guarded)
      for (IVoice v : {IVoice::Sub, IVoice::Thump, IVoice::Punch})
      {
        auto run = [&](float boost) {
          tdm::lab::DevSlam d;
          d.setEnabled(true);
          d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
          d.setImpactVoice(v);
          d.setAmount01(0.5f);
          d.setImpactTestBoost(boost);
          d.preDrive().reset(kSr, kBlock);
          std::vector<float> io = atk;
          runTaps(d, io);
          CHECK(d.impactLimiterMin() == 1.0f, "boost test avoids limiter (%d)", (int)v);
          return io;
        };
        auto w1 = run(1.0f), w2 = run(2.0f);
        // The boost multiplies the branch by exactly 2 (power of two);
        // the wet-minus-dry recovery itself costs ~1 ulp, hence epsilon.
        float worst = 0.0f;
        for (size_t i = 0; i < atk.size(); ++i)
          worst = std::max(worst, std::fabs(2.0f * (w1[i] - atk[i]) - (w2[i] - atk[i])));
        CHECK(worst < 1e-6f, "boost 2x (%.2e, %d)", (double)worst, (int)v);
      }
      auto runLegacy = [&](float boost) {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        d.setImpactVoice(IVoice::Legacy);
        d.setAmount01(1.0f);
        d.setImpactTestBoost(boost);
        d.setImpactSensitivity(0.5f);
        d.preDrive().reset(kSr, kBlock);
        std::vector<float> io = atk;
        runTaps(d, io);
        return io;
      };
      auto l1 = runLegacy(1.0f), l2 = runLegacy(2.0f);
      CHECK(bitEqual(l1.data(), l2.data(), (int)atk.size()), "legacy ignores boost");
    }
    // Determinism: identical runs are bit-identical.
    {
      auto atk = pips(3, 400.0f);
      auto run = [&]() {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        d.setImpactVoice(IVoice::Thump);
        d.setAmount01(1.0f);
        d.preDrive().reset(kSr, kBlock);
        std::vector<float> io = atk;
        runTaps(d, io);
        return io;
      };
      auto a = run(), b = run();
      CHECK(bitEqual(a.data(), b.data(), (int)atk.size()), "v2 deterministic");
    }
  }

  // D. Detector refire + sensitivity pins (SlamTrigger frozen: feed() and
  // the confirm/refractory logic are untouched by v2; only setSensitivity
  // was added).
  {
    CHECK(countFires(pips(6, 800.0f), 0.5f) == 6, "refire 800 ms: all");
    CHECK(countFires(pips(6, 400.0f), 0.5f) == 6, "refire 400 ms: all");
    CHECK(countFires(pips(6, 250.0f), 0.5f) == 6, "refire 250 ms: all");
    CHECK(countFires(pips(6, 125.0f), 0.5f) == 2, "refire 125 ms: gated");
    CHECK(countFires(pips(6, 100.0f), 0.5f) == 1, "refire 100 ms: first-only");
    auto brk = pips(4, 850.0f), grv = pips(6, 230.0f), fst = pips(8, 100.0f);
    for (float s : {0.4f, 0.5f, 0.6f, 0.7f})
      CHECK(countFires(brk, s) == 4, "breakdown robust @%.1f", s);
    const int64_t grvWant[4] = {3, 3, 6, 6};
    const int64_t fstWant[4] = {1, 1, 2, 3};
    int si = 0;
    for (float s : {0.4f, 0.5f, 0.6f, 0.7f})
    {
      CHECK(countFires(grv, s) == grvWant[si], "groove sweep @%.1f", s);
      CHECK(countFires(fst, s) == fstWant[si], "16ths gated @%.1f", s);
      ++si;
    }
    // Safety: silence never fires; noise-from-silence fires exactly once
    // (the onset is real), then the adapted floor holds.
    std::vector<float> sil((size_t)(kSr * 2), 0.0f);
    std::mt19937 rng(1234);
    std::normal_distribution<float> nd(0.0f, 0.01f);
    std::vector<float> noise((size_t)(kSr * 2));
    for (auto& v : noise)
      v = nd(rng);
    for (float s : {0.4f, 0.7f})
    {
      CHECK(countFires(sil, s) == 0, "silence safe @%.1f", s);
      tdm::lab::SlamTrigger t;
      t.reset(kSr, s, 90.0f);
      for (float v : noise)
        t.feed(v);
      CHECK(t.fireCount() == 1, "noise onset once @%.1f", s);
      bool late = false;
      for (int i = 0; i < t.triggerCount(); ++i)
        if (t.triggerAt(i).sample > (int64_t)kSr)
          late = true;
      CHECK(!late, "steady noise rejected @%.1f", s);
    }
    // Live setSensitivity converges to reset-at-sens decisions.
    {
      auto twelve = pips(12, 230.0f);
      tdm::lab::SlamTrigger pure;
      pure.reset(kSr, 0.6f, 90.0f);
      for (float v : twelve)
        pure.feed(v);
      CHECK(pure.fireCount() == 12, "pure 0.6 fires all 12");
      tdm::lab::SlamTrigger sw;
      sw.reset(kSr, 0.4f, 90.0f);
      const size_t half = twelve.size() / 2;
      for (size_t i = 0; i < half; ++i)
        sw.feed(twelve[i]);
      CHECK(sw.fireCount() == 3, "pre-switch count kept");
      sw.setSensitivity(0.6f);
      CHECK(sw.fireCount() == 3, "switch preserves count");
      int tail = 0;
      for (size_t i = half; i < twelve.size(); ++i)
        if (sw.feed(twelve[i]) > 0.0f)
          ++tail;
      CHECK(tail == 6, "post-switch converges (tail %d)", tail);
    }
    // Transpose parity: the detector hears the same attacks at -2.
    {
      auto atk = pips(4, 850.0f);
      tdm::GuitarTranspose gt;
      gt.reset(kSr);
      gt.setEnabled(true);
      gt.setShiftSt(-2.0f);
      std::vector<float> wet(atk.size(), 0.0f);
      gt.processBlock(atk.data(), wet.data(), (int)atk.size());
      std::vector<float> tail(wet.begin() + 768, wet.end());
      CHECK(countFires(atk, 0.5f) == 4, "direct attacks fire");
      CHECK(countFires(tail, 0.5f) == 4, "transposed attacks fire");
    }
  }

  // E. Mass vs Impact: continuous body vs event hit. A sustained bed with
  // no attacks leaves Impact bit-exact (nothing fires) while Mass works it
  // continuously; between sparse pips over the bed, Impact's gap branch is
  // digital silence and Mass's is not.
  {
    const int n = (int)(kSr * 2);
    std::vector<float> bed((size_t)n);
    for (int i = 0; i < n; ++i) // faded-in bed: no onset for the detector
    {
      const float fade = (i < (int)(0.1 * kSr)) ? (float)i / (float)(0.1 * kSr) : 1.0f;
      bed[(size_t)i] =
          fade * 0.10f * std::sin(2.0f * 3.141592653589793f * 65.0f * i / (float)kSr);
    }
    for (IVoice v : {IVoice::Sub, IVoice::Thump, IVoice::Punch})
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(v);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> io = bed;
      runTaps(d, io);
      CHECK(bitEqual(io.data(), bed.data(), n), "impact ignores bed (%d)", (int)v);
      CHECK(d.impactFireCount() == 0, "bed never fires (%d)", (int)v);
    }
    tdm::lab::DevSlam m;
    m.setEnabled(true);
    m.setFlavor(tdm::lab::DevSlam::Flavor::Mass);
    m.setAmount01(1.0f);
    m.preDrive().reset(kSr, kBlock);
    std::vector<float> ioM = bed;
    runTaps(m, ioM);
    CHECK(!bitEqual(ioM.data(), bed.data(), n), "mass works the bed");
    // Sparse pips over the bed: gap-branch contrast.
    auto atk = pips(3, 400.0f);
    std::vector<float> sig = bed;
    for (size_t i = 0; i < atk.size() && i < sig.size(); ++i)
      sig[i] += atk[i];
    const int g0 = (int)(kSr * 1.5); // settled gap, last pip long decayed
    for (IVoice v : {IVoice::Sub, IVoice::Thump, IVoice::Punch})
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(v);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> io = sig;
      runTaps(d, io);
      std::vector<float> br(n);
      for (int i = 0; i < n; ++i)
        br[(size_t)i] = io[(size_t)i] - sig[(size_t)i];
      CHECK(dbOf(winRms(br.data() + g0, n - g0)) < -90.0, "impact gap clean (%d)", (int)v);
    }
    tdm::lab::DevSlam m2;
    m2.setEnabled(true);
    m2.setFlavor(tdm::lab::DevSlam::Flavor::Mass);
    m2.setAmount01(1.0f);
    m2.preDrive().reset(kSr, kBlock);
    std::vector<float> ioM2 = sig;
    runTaps(m2, ioM2);
    std::vector<float> brM(n);
    for (int i = 0; i < n; ++i)
      brM[(size_t)i] = ioM2[(size_t)i] - sig[(size_t)i];
    CHECK(dbOf(winRms(brM.data() + g0, n - g0)) > -60.0, "mass gap live (%+.1f dB)",
          dbOf(winRms(brM.data() + g0, n - g0)));
  }

  // F. Sample-rate smoke: the voice runs finite off-rate.
  {
    tdm::lab::SlamImpactV2 voice;
    voice.reset(44100.0);
    voice.setVoice(Voice::Punch);
    auto dry = rigPip(0.3f);
    bool finite = true;
    for (size_t i = 0; i < dry.size(); ++i)
      if (!std::isfinite(voice.render(dry[i], (int)i == 100 ? 0.9f : 0.0f)))
        finite = false;
    CHECK(finite, "44.1k finite");
  }

  // G. DEV-only diagnostics: branch solo, debug gain, 180 Hz ref, telemetry.
  // Fast-path contract: Normal + 1x + ref-off renders bit-identical whether
  // or not the diagnostic setters were ever touched.
  {
    auto sig = rigPipTrain(3, 800.0f, 0.5f);
    // (a) Fresh vs exercised-then-restored: bit-identical (setters are
    // target-only; resetAudio parks both identically).
    std::vector<float> fresh, restored;
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Thump);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      fresh = sig;
      runTaps(d, fresh);
    }
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Thump);
      d.setAmount01(1.0f);
      d.setImpactDebugSolo(true);
      d.setImpactDebugGain(8.0f);
      d.setImpactTestRef(true);
      d.setImpactDebugSolo(false);
      d.setImpactDebugGain(1.0f);
      d.setImpactTestRef(false);
      d.preDrive().reset(kSr, kBlock);
      restored = sig;
      runTaps(d, restored);
    }
    CHECK(bitEqual(fresh.data(), restored.data(), (int)sig.size()),
          "diagnostics-off bit-identical");
    // (b) Branch solo + debug gain exactness: preset-then-reset snaps the
    // glides to target, so 4x/8x solo renders are exactly 4x/8x the 1x
    // render (power-of-two FP multiply is exact). Legacy solos too.
    for (IVoice v : {IVoice::Legacy, IVoice::Sub, IVoice::Thump, IVoice::Punch})
    {
      std::vector<float> r1, r4, r8;
      for (int g = 0; g < 3; ++g)
      {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        d.setImpactVoice(v);
        d.setAmount01(1.0f);
        d.setImpactDebugSolo(true);
        d.setImpactDebugGain(g == 0 ? 1.0f : g == 1 ? 4.0f : 8.0f);
        d.preDrive().reset(kSr, kBlock);
        auto io = sig;
        runTaps(d, io);
        (g == 0 ? r1 : g == 1 ? r4 : r8) = io;
      }
      CHECK(winPeak(r1.data(), (int)r1.size()) > 1e-4f, "solo %d nonzero", (int)v);
      bool x4 = true, x8 = true, finite = true;
      for (size_t i = 0; i < r1.size(); ++i)
      {
        if (!std::isfinite(r1[i]) || !std::isfinite(r4[i]) || !std::isfinite(r8[i]))
          finite = false;
        if (r4[i] != r1[i] * 4.0f)
          x4 = false;
        if (r8[i] != r1[i] * 8.0f)
          x8 = false;
      }
      CHECK(finite, "solo %d finite", (int)v);
      CHECK(x4, "solo %d 4x exact", (int)v);
      CHECK(x8, "solo %d 8x exact", (int)v);
    }
    // (c) Solo mutes dry: solo + amount 0 renders all zeros.
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Punch);
      d.setAmount01(0.0f);
      d.setImpactDebugSolo(true);
      d.preDrive().reset(kSr, kBlock);
      auto io = sig;
      runTaps(d, io);
      CHECK(winPeak(io.data(), (int)io.size()) == 0.0f, "solo+amount0 zeros");
    }
    // (d) 180 Hz ref: substituted render differs, carries 180 Hz, and is
    // level-independent (fixed trigger, no adaptive scaling) below limiting.
    {
      auto rsig = rigPipTrain(2, 800.0f, 0.3f);
      std::vector<float> voice, ref;
      for (int r = 0; r < 2; ++r)
      {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        d.setImpactVoice(IVoice::Thump);
        d.setAmount01(1.0f);
        d.setImpactTestRef(r == 1);
        d.preDrive().reset(kSr, kBlock);
        auto io = rsig;
        runTaps(d, io);
        (r == 0 ? voice : ref) = io;
      }
      CHECK(!bitEqual(voice.data(), ref.data(), (int)rsig.size()), "ref substituted");
      CHECK(tdm_test::allFinite(ref.data(), (int)ref.size()), "ref finite");
      std::vector<float> br(ref.size());
      for (size_t i = 0; i < br.size(); ++i)
        br[i] = ref[i] - rsig[i];
      const int a0 = (int)(kSr / 2), b0 = a0 + (int)(0.150 * kSr);
      const double lo = dbOf(bandRms(br, a0, b0, 0.0f, 250.0f));
      const double hi = dbOf(bandRms(br, a0, b0, 2000.0f, 0.0f));
      CHECK(lo > hi + 20.0, "ref is 180 Hz (LP250 %+.1f vs HP2k %+.1f)", lo, hi);
      auto loud = rigPipTrain(2, 800.0f, 0.12f);
      auto quiet = rigPipTrain(2, 800.0f, 0.06f);
      std::vector<float> brL(loud.size()), brQ(quiet.size());
      for (int r = 0; r < 2; ++r)
      {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
        d.setImpactVoice(IVoice::Punch);
        d.setAmount01(1.0f);
        d.setImpactTestRef(true);
        d.preDrive().reset(kSr, kBlock);
        auto io = (r == 0 ? loud : quiet);
        runTaps(d, io);
        auto& brX = (r == 0 ? brL : brQ);
        auto& dx = (r == 0 ? loud : quiet);
        for (size_t i = 0; i < brX.size(); ++i)
          brX[i] = io[i] - dx[i];
      }
      const double ratioDb =
          dbOf(winPeak(brL.data(), (int)brL.size())) - dbOf(winPeak(brQ.data(), (int)brQ.size()));
      CHECK(std::fabs(ratioDb) < 0.5, "ref level-independent (%+.2f dB)", ratioDb);
    }
    // (e) Telemetry: branch/gain advance on v2 fires; Legacy reports the
    // branch but honest gain-zero/lim-one; idle Impact reports zeros.
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Thump);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      auto io = sig;
      runTaps(d, io);
      CHECK(d.impactFireCount() > 0, "telemetry run fired");
      CHECK(d.impactTeleBranch() > 1e-4f, "tele branch live (%.4f)", d.impactTeleBranch());
      CHECK(d.impactTeleGain() > 0.0f, "tele gain live (%.3f)", d.impactTeleGain());
      const float lim = d.impactTeleLim();
      CHECK(lim > 0.0f && lim <= 1.0f, "tele lim sane (%.3f)", lim);
    }
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Legacy);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      auto io = sig;
      runTaps(d, io);
      CHECK(d.impactFireCount() > 0, "legacy run fired");
      CHECK(d.impactTeleBranch() > 1e-4f, "legacy tele branch live (%.4f)",
            d.impactTeleBranch());
      CHECK(d.impactTeleGain() == 0.0f, "legacy tele gain zero");
      CHECK(d.impactTeleLim() == 1.0f, "legacy tele lim one");
    }
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Thump);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> sil((size_t)kSr, 0.0f);
      runTaps(d, sil);
      CHECK(d.impactFireCount() == 0, "silence no fires");
      CHECK(d.impactTeleBranch() == 0.0f, "silence tele branch zero");
      CHECK(d.impactTeleGain() == 0.0f, "silence tele gain zero");
      CHECK(d.impactTeleLim() == 1.0f, "silence tele lim one");
    }
    // (f) Diagnostics never touch Push/Crush/Mass: solo + 8x + ref preset
    // renders bit-identical to diagnostics-off.
    for (auto f : {tdm::lab::DevSlam::Flavor::Push, tdm::lab::DevSlam::Flavor::Crush,
                   tdm::lab::DevSlam::Flavor::Mass})
    {
      std::vector<float> plain, diag;
      for (int r = 0; r < 2; ++r)
      {
        tdm::lab::DevSlam d;
        d.setEnabled(true);
        d.setFlavor(f);
        d.setAmount01(1.0f);
        if (r == 1)
        {
          d.setImpactDebugSolo(true);
          d.setImpactDebugGain(8.0f);
          d.setImpactTestRef(true);
        }
        d.preDrive().reset(kSr, kBlock);
        auto io = sig;
        runTaps(d, io);
        (r == 0 ? plain : diag) = io;
      }
      CHECK(bitEqual(plain.data(), diag.data(), (int)sig.size()), "diag ignores flavour %d",
            (int)f);
    }
  }

  // H. Limiter-telemetry audit + target-ratio gain law (live finding: branch
  // faint, teleG 0.85, "lim 0.0" = 0 dB GR = wide open, healthy).
  using GLaw = tdm::lab::SlamImpactV2::GainLaw;
  // (a) Limiter linear-gain invariant: [0.25, 1.0] always, 1.0 at idle.
  for (Voice v : {Voice::Sub, Voice::Thump, Voice::Punch})
  {
    for (GLaw law : {GLaw::LegacyAdaptive, GLaw::TargetRatio})
    {
      tdm::lab::SlamImpactV2 voice;
      voice.reset(kSr);
      voice.setVoice(v);
      voice.setGainLaw(law);
      voice.setTargetDb(-2.0f);
      CHECK(voice.limiterGain() == 1.0f, "%s idle limiter unity", voiceName(v));
      auto dry = rigPip(0.5f);
      for (size_t i = 0; i < dry.size(); ++i) // hot bed + full fire
        dry[i] += 0.45f * std::sin(2.0f * 3.141592653589793f * 82.0f * i / (float)kSr);
      bool inRange = true;
      for (size_t i = 0; i < dry.size(); ++i)
      {
        voice.render(dry[i], (int)i == 100 ? 1.0f : 0.0f);
        const float g = voice.limiterGain();
        if (!(g >= 0.25f && g <= 1.0f) || !std::isfinite(g))
          inRange = false;
      }
      CHECK(inRange, "%s law %d limiter in [0.25, 1]", voiceName(v), (int)law);
      CHECK(voice.limiterMin() < 1.0f, "%s law %d limiter engaged hot", voiceName(v),
            (int)law);
    }
  }
  // (b) Telemetry text convention: "lim 0.0" means wide open (healthy).
  {
    using tdm::lab::slamTeleText;
    auto idle = slamTeleText(0.0f, 0.0f, 1.0f);
    CHECK(idle.branch == "--" && idle.gain == "--" && idle.lim == "0.0", "tele idle text");
    auto live = slamTeleText(0.5f, 0.85f, 1.0f);
    CHECK(live.branch == "-6.0 dB" && live.gain == "0.85" && live.lim == "0.0",
          "tele live-healthy text");
    auto gr = slamTeleText(0.5f, 0.85f, 0.5f);
    CHECK(gr.lim == "-6.0", "tele GR text");
    auto floor = slamTeleText(0.5f, 0.85f, 0.25f);
    CHECK(floor.lim == "-12.0", "tele floor text");
  }
  // (c) Snapshot records: requested-gain math is exact (legacy + target).
  {
    tdm::lab::SlamImpactV2 voice;
    voice.reset(kSr);
    voice.setVoice(Voice::Thump);
    auto dry = rigPip(0.4f);
    for (size_t i = 0; i < dry.size(); ++i)
      voice.render(dry[i], (int)i == 100 ? 0.9f : 0.0f);
    CHECK(voice.lastFireRig() > 0.01f, "snapshot rig above knee (%.3f)", voice.lastFireRig());
    CHECK(voice.lastReqGain() == voice.lastFireRig() * voice.gainK(), "legacy req math exact");
    const float wantG =
        voice.lastReqGain() > 1.6f ? 1.6f : voice.lastReqGain();
    CHECK(voice.lastFireGain() == wantG, "legacy cap math exact");
    tdm::lab::SlamImpactV2 t2;
    t2.reset(kSr);
    t2.setVoice(Voice::Thump);
    t2.setGainLaw(GLaw::TargetRatio);
    t2.setTargetDb(-6.0f);
    for (size_t i = 0; i < dry.size(); ++i)
      t2.render(dry[i], (int)i == 100 ? 0.9f : 0.0f);
    const float ratio = std::pow(10.0f, -6.0f / 20.0f);
    const float wantReq = (t2.lastFireMain() * ratio) / (t2.shapePeak() * 0.9f);
    CHECK(std::fabs(t2.lastReqGain() - wantReq) <= wantReq * 1e-5f, "target req math exact");
  }
  // (d) Target steps land their ratio (limiter-transparent levels prove the
  // LAW, not the limiter, sets the ratio; +/-3 dB: voice bloom jitter).
  for (IVoice v : {IVoice::Sub, IVoice::Thump, IVoice::Punch})
  {
    auto tsig = rigPipTrain(2, 800.0f, 0.3f);
    for (float db : {-12.0f, -6.0f, -2.0f})
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(v);
      d.setAmount01(1.0f);
      d.setImpactGainLaw(tdm::lab::DevSlam::ImpactGainLaw::TargetRatio);
      d.setImpactTargetDb(db);
      d.preDrive().reset(kSr, kBlock);
      auto io = tsig;
      runTaps(d, io);
      tdm::lab::DevSlam s;
      s.setEnabled(true);
      s.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      s.setImpactVoice(v);
      s.setAmount01(1.0f);
      s.setImpactGainLaw(tdm::lab::DevSlam::ImpactGainLaw::TargetRatio);
      s.setImpactTargetDb(db);
      s.setImpactDebugSolo(true);
      s.preDrive().reset(kSr, kBlock);
      auto solo = tsig;
      runTaps(s, solo);
      const double got = 20.0 * std::log10(winPeak(solo.data(), (int)solo.size())
                                           / (d.impactLastFireMain() + 1e-9f));
      CHECK(std::fabs(got - db) <= 3.0, "voice %d target %+.0f lands (%+.1f)", (int)v, db,
            got);
      CHECK(d.impactLimiterMin() == 1.0f, "voice %d target %+.0f limiter out", (int)v, db);
    }
  }
  // (e) Velocity: legacy preserves s (+9 dB spread), target normalizes it.
  {
    auto dry = rigPip(0.15f);
    float leg[2] = {0, 0}, tgt[2] = {0, 0};
    int k = 0;
    for (float s : {0.2f, 0.9f})
    {
      auto wetL = fireOnce(Voice::Thump, dry, 100, s);
      auto wetT = fireOnce(Voice::Thump, dry, 100, s, 1.0f, GLaw::TargetRatio, -6.0f);
      for (size_t i = 0; i < dry.size(); ++i)
      {
        leg[k] = std::max(leg[k], std::fabs(wetL[i] - dry[i]));
        tgt[k] = std::max(tgt[k], std::fabs(wetT[i] - dry[i]));
      }
      ++k;
    }
    CHECK(dbOf(leg[1]) - dbOf(leg[0]) > 8.0, "legacy velocity spread (%+.1f dB)",
          dbOf(leg[1]) - dbOf(leg[0]));
    CHECK(dbOf(tgt[1]) - dbOf(tgt[0]) < 6.0, "target velocity tamed (%+.1f dB)",
          dbOf(tgt[1]) - dbOf(tgt[0]));
  }
  // (f) Target silence: below-knee main gates the fire to ~nothing.
  {
    tdm::lab::SlamImpactV2 voice;
    voice.reset(kSr);
    voice.setVoice(Voice::Thump);
    voice.setGainLaw(GLaw::TargetRatio);
    voice.setTargetDb(-2.0f);
    std::vector<float> sil((size_t)kSr / 10, 0.0f);
    float peak = 0.0f;
    for (size_t i = 0; i < sil.size(); ++i)
      peak = std::max(peak, std::fabs(voice.render(0.0f, (int)i == 100 ? 0.9f : 0.0f)));
    CHECK(voice.lastFireGain() == 0.0f, "target silence gain zero");
    CHECK(peak == 0.0f, "target silence renders zeros");
  }
  // (g) Law switch mid-ring is smooth; future fires use the new law.
  {
    tdm::lab::SlamImpactV2 voice;
    voice.reset(kSr);
    voice.setVoice(Voice::Thump);
    auto dry = rigPip(0.3f);
    const int n = (int)dry.size();
    std::vector<float> wet((size_t)n);
    for (int i = 0; i < n; ++i)
    {
      if (i == 100 + (int)(0.020 * kSr)) // 20 ms into the ring: switch laws
        voice.setGainLaw(GLaw::TargetRatio);
      wet[(size_t)i] = voice.render(dry[(size_t)i], i == 100 ? 0.9f : 0.0f);
    }
    CHECK(tdm_test::allFinite(wet.data(), n), "law switch finite");
    CHECK(maxStep(wet.data(), n) < 0.1f, "law switch smooth (%.4f)", maxStep(wet.data(), n));
    CHECK(voice.gainLaw() == GLaw::TargetRatio, "law adopted");
  }
  // (h) Default law is legacy bit-exact (never-touched vs explicit).
  {
    auto sig = rigPipTrain(2, 800.0f, 0.4f);
    std::vector<float> fresh, expl;
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Punch);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      fresh = sig;
      runTaps(d, fresh);
    }
    {
      tdm::lab::DevSlam d;
      d.setEnabled(true);
      d.setFlavor(tdm::lab::DevSlam::Flavor::Impact);
      d.setImpactVoice(IVoice::Punch);
      d.setAmount01(1.0f);
      d.setImpactGainLaw(tdm::lab::DevSlam::ImpactGainLaw::LegacyAdaptive);
      d.setImpactTargetDb(-2.0f);
      d.preDrive().reset(kSr, kBlock);
      expl = sig;
      runTaps(d, expl);
    }
    CHECK(bitEqual(fresh.data(), expl.data(), (int)sig.size()), "default law bit-identical");
  }
  // (i) Limiter under the target law: hot + High is clip-safe and engaged;
  // moderate + High stays transparent.
  {
    tdm::lab::SlamImpactV2 hot;
    hot.reset(kSr);
    hot.setVoice(Voice::Thump);
    hot.setGainLaw(GLaw::TargetRatio);
    hot.setTargetDb(-2.0f);
    auto dry = rigPip(0.5f);
    for (size_t i = 0; i < dry.size(); ++i)
      dry[i] += 0.45f * std::sin(2.0f * 3.141592653589793f * 82.0f * i / (float)kSr);
    float peak = 0.0f;
    for (size_t i = 0; i < dry.size(); ++i)
      peak = std::max(peak, std::fabs(hot.render(dry[i], (int)i == 100 ? 1.0f : 0.0f)));
    // No clip (< 1.0), but past 0.99: the 0.25 floor deliberately lets a
    // bounded overshoot past the ceiling (0.996 measured) rather than
    // choking the slam to nothing on hot dry. Existing limiter design.
    CHECK(peak < 1.0f, "target hot clip-safe (%.3f)", (double)peak);
    CHECK(hot.limiterMin() < 1.0f, "target hot limiter engaged (%.2f)",
          (double)hot.limiterMin());
    tdm::lab::SlamImpactV2 calm;
    calm.reset(kSr);
    calm.setVoice(Voice::Thump);
    calm.setGainLaw(GLaw::TargetRatio);
    calm.setTargetDb(-2.0f);
    auto dry2 = rigPip(0.3f);
    for (size_t i = 0; i < dry2.size(); ++i)
      calm.render(dry2[i], (int)i == 100 ? 0.9f : 0.0f);
    CHECK(calm.limiterMin() == 1.0f, "target moderate limiter out");
  }
  // (j) Fixed-s spectrum isolation: same peak + same s, lows-rich vs
  // lows-poor dry. Legacy gains WITH the lows (backwards for low-poor
  // rigs); the target gain is spectrum-independent.
  {
    auto dryLo = specPip(98.0f, 0.30f), dryHi = specPip(392.0f, 0.30f);
    float legG[2] = {0, 0}, tgtG[2] = {0, 0}, legB[2] = {0, 0};
    for (int w = 0; w < 2; ++w)
    {
      auto& dx = (w == 0 ? dryLo : dryHi);
      tdm::lab::SlamImpactV2 leg, tgt;
      leg.reset(kSr);
      leg.setVoice(Voice::Thump);
      tgt.reset(kSr);
      tgt.setVoice(Voice::Thump);
      tgt.setGainLaw(GLaw::TargetRatio);
      tgt.setTargetDb(-6.0f);
      for (size_t i = 0; i < dx.size(); ++i)
      {
        const float s = ((int)i == 64) ? 0.5f : 0.0f;
        legB[(size_t)w] = std::max(legB[(size_t)w], std::fabs(leg.render(dx[i], s) - dx[i]));
        tgt.render(dx[i], s);
      }
      legG[w] = leg.lastFireGain();
      tgtG[w] = tgt.lastFireGain();
    }
    CHECK(legG[0] / legG[1] > 1.4f, "legacy gain follows lows (%.2f vs %.2f)", legG[0],
          legG[1]);
    CHECK(legB[0] > legB[1], "legacy branch hotter on lows-rich");
    const double spread = std::fabs(20.0 * std::log10(tgtG[0] / tgtG[1]));
    CHECK(spread < 1.0, "target gain spectrum-flat (%+.2f dB)", spread);
  }
}

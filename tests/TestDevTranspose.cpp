#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/TransposeInsert.h"
#include "dsp/lab/Pitch/DevTranspose.h"
#include "dsp/lab/Pitch/DevTransposeUi.h"
#include "dsp/lab/Pitch/GuitarTransposeV2.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979;

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
}

std::vector<float> ksPluck(float peak, float freqHz, int n, uint32_t seed = 0x51ab3u)
{
  const int period = static_cast<int>(kSr / freqHz + 0.5f) > 2 ? static_cast<int>(kSr / freqHz + 0.5f) : 2;
  std::vector<float> line(static_cast<size_t>(period));
  for (int i = 0; i < period; ++i)
    line[static_cast<size_t>(i)] = detNoise(i, seed);
  for (int i = 1; i < period; ++i)
    line[static_cast<size_t>(i)] = 0.5f * (line[static_cast<size_t>(i)] + line[static_cast<size_t>(i - 1)]);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  int idx = 0;
  float prev = 0.0f;
  for (int i = 0; i < n; ++i)
  {
    const float cur = line[static_cast<size_t>(idx)];
    const float v = 0.9998f * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = peak * cur;
  }
  return out;
}

// Latency-compensated offline run through the raw accepted GT2 baseline.
std::vector<float> runRawGt2(float shiftSt, const std::vector<float>& in, int block)
{
  tdm::lab::GuitarTransposeV2 p; // default config == auditioned baseline
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  const int lat = p.latencySamples();
  const int n = static_cast<int>(in.size());
  std::vector<float> padded(static_cast<size_t>(n + lat), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(n + lat), 0.0f);
  for (int off = 0; off < n + lat; off += block)
  {
    const int m = (n + lat - off) < block ? (n + lat - off) : block;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  return out;
}

// Same stimulus through the DEV wrapper (GT2 mode, known-good baseline).
std::vector<float> runDevGt2(float shiftSt, const std::vector<float>& in, int block)
{
  tdm::lab::DevTranspose s;
  s.resetGt2ToBaseline();
  s.setEngine(tdm::lab::DevTranspose::Engine::Gt2);
  s.setShiftSt(shiftSt);
  s.setEnabled(true);
  s.reset(kSr, 2048);
  const int lat = s.latencySamples();
  const int n = static_cast<int>(in.size());
  std::vector<float> padded(static_cast<size_t>(n + lat), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(n + lat), 0.0f);
  for (int off = 0; off < n + lat; off += block)
  {
    const int m = (n + lat - off) < block ? (n + lat - off) : block;
    s.process(padded.data() + off, raw.data() + off, m);
  }
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  return out;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
  float m = 0.0f;
  for (size_t i = 0; i < a.size(); ++i)
    m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}

// Trivial production-seam probes (no lab DSP inside).
struct PassInsert : tdm::TransposeInsert
{
  void reset(double, int) override {}
  void process(const float* in, float* out, int n) override
  {
    for (int i = 0; i < n; ++i)
      out[i] = in[i];
  }
  int latencySamples() const override { return 0; }
};
struct HalfInsert : tdm::TransposeInsert
{
  void reset(double, int) override {}
  void process(const float* in, float* out, int n) override
  {
    for (int i = 0; i < n; ++i)
      out[i] = in[i] * 0.5f;
  }
  int latencySamples() const override { return 0; }
};

std::vector<float> runRig(tdm::TechDeathRig& rig, const std::vector<float>& in)
{
  std::vector<float> out(in.size());
  const float* bi[1] = {in.data()};
  float* bo[1] = {out.data()};
  rig.processBlock(bi, 1, bo, 1, static_cast<int>(in.size()));
  return out;
}
} // namespace

void runDevTransposeTests()
{
  using Gt2 = tdm::lab::GuitarTransposeV2;
  using Stage = tdm::lab::DevTranspose;

  // 1. The known-good baseline config is exactly the auditioned GT2 default.
  // Every field is pinned: any drift fails loudly here, not in listening.
  {
    const Gt2::Config c = Stage::knownGoodGt2();
    TDM_CHECK(c.windowMs == 30.0, "baseline window 30ms");
    TDM_CHECK(c.floorMs == 2.0, "baseline floor 2ms");
    TDM_CHECK(c.corrMs == 25.0, "baseline corr 25ms");
    TDM_CHECK(c.fadeMinMs == 30.0, "baseline fadeMin 30ms");
    TDM_CHECK(c.fadeMaxMs == 120.0, "baseline fadeMax 120ms");
    TDM_CHECK(c.fadeNccHi == 0.95, "baseline fadeNccHi");
    TDM_CHECK(c.fadeNccLo == 0.60, "baseline fadeNccLo");
    TDM_CHECK(c.onsetFadeMs == 2.0, "baseline onsetFade");
    TDM_CHECK(c.onsetSpanMs == 4.0, "baseline onsetSpan");
    TDM_CHECK(c.searchLeadMs == 4.0, "baseline searchLead");
    TDM_CHECK(c.refractoryMs == 40.0, "baseline refractory");
    TDM_CHECK(c.detectorHpHz == 600.0, "baseline detectorHp");
    TDM_CHECK(c.detectorSmoothMs == 2.0, "baseline detectorSmooth");
    TDM_CHECK(c.onsetOverMinDb == 9.0, "baseline overMin");
    TDM_CHECK(c.onsetOverMaxDb == 6.0, "baseline overMax");
    TDM_CHECK(c.historyCells == 50, "baseline historyCells");
    TDM_CHECK(c.historySkip == 5, "baseline historySkip");
    TDM_CHECK(c.enableResync == true, "baseline resync on");
    // And it matches a hand-built GT2 default (no wrapper skew).
    const Gt2::Config d;
    TDM_CHECK(c.windowMs == d.windowMs && c.floorMs == d.floorMs && c.corrMs == d.corrMs
                  && c.fadeMinMs == d.fadeMinMs && c.fadeMaxMs == d.fadeMaxMs
                  && c.fadeNccHi == d.fadeNccHi && c.fadeNccLo == d.fadeNccLo
                  && c.onsetFadeMs == d.onsetFadeMs && c.onsetSpanMs == d.onsetSpanMs
                  && c.searchLeadMs == d.searchLeadMs && c.refractoryMs == d.refractoryMs
                  && c.detectorHpHz == d.detectorHpHz && c.detectorSmoothMs == d.detectorSmoothMs
                  && c.onsetOverMinDb == d.onsetOverMinDb && c.onsetOverMaxDb == d.onsetOverMaxDb
                  && c.historyCells == d.historyCells && c.historySkip == d.historySkip
                  && c.enableResync == d.enableResync,
              "baseline == GT2 default");
  }

  // 2. DEV wrapper (GT2, baseline, -1/-2) is sample-equivalent to raw GT2.
  for (const float st : {-1.0f, -2.0f})
  {
    auto chord = ksPluck(0.5f, 82.41f, static_cast<int>(3 * kSr));
    const auto third = ksPluck(0.5f, 103.83f, static_cast<int>(3 * kSr), 0x77u);
    const auto fifth = ksPluck(0.5f, 123.47f, static_cast<int>(3 * kSr), 0x99u);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] = (chord[i] + third[i] + fifth[i]) / 3.0f;
    const auto raw = runRawGt2(st, chord, 128);
    const auto dev = runDevGt2(st, chord, 128);
    TDM_CHECK(tdm_test::allFinite(dev.data(), static_cast<int>(dev.size())), "dev finite");
    const float diff = maxAbsDiff(raw, dev);
    TDM_CHECK(diff < 1e-6f, "dev == raw baseline within float tolerance");
    // Block-size determinism through the wrapper.
    const auto dev64 = runDevGt2(st, chord, 64);
    const auto dev1024 = runDevGt2(st, chord, 1024);
    TDM_CHECK(dev == dev64, "dev block-64 identical");
    TDM_CHECK(dev == dev1024, "dev block-1024 identical");
  }

  // 3. Shared control behavior: clamping, validation, engine gating.
  {
    Stage s;
    s.setShiftSt(-20.0f);
    TDM_CHECK(s.shiftSt() == Stage::kMinShiftSt, "shift clamps low");
    s.setShiftSt(20.0f);
    TDM_CHECK(s.shiftSt() == Stage::kMaxShiftSt, "shift clamps high");
    s.setShiftSt(-7.5f);
    TDM_CHECK(s.shiftSt() == -7.5f, "fractional shift stored");
    bool threw = false;
    try
    {
      s.setT3kWindowMs(25);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad T3K window throws");
    s.setT3kTonalityHz(30000.0f);
    TDM_CHECK(s.t3kTonalityHz() == 20000.0f, "tonality clamps high");
    s.setT3kTonalityHz(0.0f);
    TDM_CHECK(s.t3kTonalityHz() == 0.0f, "tonality off sticks");
    threw = false;
    try
    {
      s.setT3kTonalityHz(-5.0f);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "negative tonality throws");
    Gt2::Config bad;
    bad.windowMs = bad.floorMs;
    threw = false;
    try
    {
      s.configureGt2(bad);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad GT2 config rejected");
    // Failed configure leaves the previous config intact.
    TDM_CHECK(s.gt2Config().windowMs == 30.0, "failed configure keeps previous");
    if (!Stage::hasTone3000())
    {
      threw = false;
      try
      {
        s.setEngine(Stage::Engine::Tone3000);
      }
      catch (const std::invalid_argument&)
      {
        threw = true;
      }
      TDM_CHECK(threw, "T3K select throws without TONE3000");
    }
    // Engine names round-trip.
    Stage::Engine e = Stage::Engine::Gt2;
    TDM_CHECK(tdm::lab::parseDevEngine("ours", e) && e == Stage::Engine::Gt2, "parse ours");
    TDM_CHECK(tdm::lab::parseDevEngine("t3k", e) && e == Stage::Engine::Tone3000, "parse t3k");
    TDM_CHECK(!tdm::lab::parseDevEngine("nope", e), "parse rejects junk");
  }

  // 4. Bypass is the latency-matched dry path (finite, sane level).
  {
    Stage s;
    s.setShiftSt(-2.0f);
    s.setEnabled(false);
    s.reset(kSr, 512);
    const auto in = ksPluck(0.6f, 110.0f, static_cast<int>(kSr));
    std::vector<float> out(in.size());
    for (size_t off = 0; off < in.size(); off += 128)
      s.process(in.data() + off, out.data() + off, 128);
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "bypass finite");
    // Bypassed output equals input delayed by the GT2 latency (past the ramp).
    const int lat = s.latencySamples();
    TDM_CHECK(lat > 0, "bypass latency positive");
    float diff = 0.0f;
    for (size_t i = static_cast<size_t>(lat) + 256; i < out.size(); ++i)
      diff = std::max(diff, std::fabs(out[i] - in[i - lat]));
    TDM_CHECK(diff < 1e-6f, "bypass is latency-matched dry");
  }

  // 5. UI adapter field table: every descriptor maps to the right Config
  // member, range-checks, and round-trips (the AppKit panel builds from it).
  {
    using tdm::lab::devGt2UiField;
    using tdm::lab::devGt2UiFieldCount;
    using tdm::lab::devGt2UiGet;
    using tdm::lab::devGt2UiIsBaseline;
    using tdm::lab::devGt2UiSet;
    TDM_CHECK(devGt2UiFieldCount() == 17, "17 numeric adapter rows");
    // Baseline reads back the auditioned defaults through the table.
    const Gt2::Config base = Stage::knownGoodGt2();
    const double expect[17] = {30.0, 2.0, 25.0, 30.0, 120.0, 0.95, 0.60, 2.0, 4.0, 4.0, 40.0,
                               600.0, 2.0, 9.0, 6.0, 50.0, 5.0};
    for (int i = 0; i < 17; ++i)
      TDM_CHECK(devGt2UiGet(base, i) == expect[i], "adapter baseline readback");
    // Each field writes its own member and nothing else.
    for (int i = 0; i < 17; ++i)
    {
      Gt2::Config c = base;
      const auto& f = devGt2UiField(i);
      const double probe = (f.min + f.max) * 0.5;
      TDM_CHECK(devGt2UiSet(c, i, probe), "adapter set accepts mid-range");
      TDM_CHECK(!devGt2UiIsBaseline(c), "edited config is not baseline");
      for (int j = 0; j < 17; ++j)
      {
        const double got = devGt2UiGet(c, j);
        if (j == i)
        {
          // Integer fields round; the probe mid-point may be fractional.
          const double want = (j == 15 || j == 16) ? static_cast<double>(static_cast<int>(probe + 0.5))
                                                   : probe;
          TDM_CHECK(got == want, "adapter set lands on own member");
        }
        else
        {
          TDM_CHECK(got == expect[j], "adapter set spares other members");
        }
      }
      // Out-of-range rejected, config untouched.
      Gt2::Config before = c;
      TDM_CHECK(!devGt2UiSet(c, i, f.min - 1.0), "adapter rejects below min");
      TDM_CHECK(!devGt2UiSet(c, i, f.max + 1.0), "adapter rejects above max");
      TDM_CHECK(devGt2UiIsBaseline(before) == devGt2UiIsBaseline(c), "rejected set is side-effect free");
    }
    Gt2::Config mut = base;
    TDM_CHECK(!devGt2UiSet(mut, -1, 0.0) && !devGt2UiSet(mut, 17, 0.0), "adapter rejects bad index");
    TDM_CHECK(devGt2UiIsBaseline(base), "baseline helper true on baseline");
    Gt2::Config noResync = base;
    noResync.enableResync = false;
    TDM_CHECK(!devGt2UiIsBaseline(noResync), "baseline helper sees resync flip");
    // Descriptor keys match the CLI flag suffixes (same control vocabulary).
    TDM_CHECK(std::string(devGt2UiField(0).key) == "window", "key window");
    TDM_CHECK(std::string(devGt2UiField(5).key) == "fadencc-hi", "key fadencc-hi");
    TDM_CHECK(std::string(devGt2UiField(16).key) == "skip", "key skip");
  }

  // 6. Shared control semantics: shift is one value for both engines,
  // bypass preserves it, restores land exactly.
  {
    Stage s;
    s.setShiftSt(-7.0f);
    s.setT3kWindowMs(40);
    s.setT3kTonalityHz(4000.0f);
    TDM_CHECK(s.shiftSt() == -7.0f, "shared shift stored");
    TDM_CHECK(s.t3kWindowMs() == 40 && s.t3kTonalityHz() == 4000.0f, "T3K config stored");
    // Bypass cycles never touch shift or engine configs.
    s.setEnabled(false);
    s.setEnabled(true);
    s.setEnabled(false);
    TDM_CHECK(s.shiftSt() == -7.0f, "bypass preserves shift");
    TDM_CHECK(s.t3kWindowMs() == 40 && s.t3kTonalityHz() == 4000.0f, "bypass preserves T3K config");
    // GT2 custom config survives bypass + shift moves (applied at reset).
    Gt2::Config custom = Stage::knownGoodGt2();
    custom.windowMs = 40.0;
    custom.enableResync = false;
    s.configureGt2(custom);
    s.setShiftSt(-3.0f);
    s.setEnabled(true);
    TDM_CHECK(s.gt2Config().windowMs == 40.0 && !s.gt2Config().enableResync,
              "GT2 custom config preserved");
    TDM_CHECK(s.shiftSt() == -3.0f, "shift moves under custom config");
    // Restores land exactly on the auditioned references.
    s.resetGt2ToBaseline();
    s.setT3kWindowMs(Stage::kDefaultT3kWindowMs);
    s.setT3kTonalityHz(Stage::kDefaultT3kTonalityHz);
    TDM_CHECK(tdm::lab::devGt2UiIsBaseline(s.gt2Config()), "GT2 baseline restore exact");
    TDM_CHECK(s.t3kWindowMs() == 30 && s.t3kTonalityHz() == 0.0f, "T3K reference restore exact");
    // Shift moves reach the GT2 engine behaviorally (same input, new pitch).
    const auto in = ksPluck(0.5f, 110.0f, static_cast<int>(kSr));
    const auto at1 = runDevGt2(-1.0f, in, 128);
    const auto at2 = runDevGt2(-2.0f, in, 128);
    TDM_CHECK(maxAbsDiff(at1, at2) > 0.01f, "shared shift retunes GT2 render");
  }

  // 7. Production rig seam: null insert is bit-exact; passthrough insert is
  // transparent; the seam sits post-gate pre-drive (gain probe).
  {
    auto stim = ksPluck(0.5f, 82.41f, 8192);
    tdm::TechDeathRig bare;
    bare.reset(kSr, 2048);
    const auto ref = runRig(bare, stim);
    PassInsert pass;
    tdm::TechDeathRig withPass;
    withPass.reset(kSr, 2048);
    withPass.setTransposeInsert(&pass);
    withPass.reset(kSr, 2048); // reinstall path forwards reset
    TDM_CHECK(runRig(withPass, stim) == ref, "passthrough insert transparent");
    HalfInsert half;
    tdm::TechDeathRig withHalf;
    withHalf.reset(kSr, 2048);
    withHalf.setTransposeInsert(&half);
    const auto halved = runRig(withHalf, stim);
    float diff = 0.0f;
    for (size_t i = 0; i < ref.size(); ++i)
      diff = std::max(diff, std::fabs(halved[i] - ref[i] * 0.5f));
    TDM_CHECK(diff < 1e-6f, "seam scales pre-drive signal");
  }
}

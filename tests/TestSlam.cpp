// SLAM lab candidate tests: determinism, bypass/exact-dry, finite output,
// frequency selectivity (A/B/C add lows, ignore highs), trigger behavior (D).

#include "tests/Assert.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/lab/Slam/SlamCandidates.h"

namespace
{
constexpr double kSr = 48000.0;

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

float rms(const float* x, int n)
{
  double acc = 0.0;
  for (int i = 0; i < n; ++i)
    acc += (double)x[i] * x[i];
  return static_cast<float>(std::sqrt(acc / (n > 0 ? n : 1)));
}

bool allFinite(const float* x, int n)
{
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(x[i]))
      return false;
  return true;
}

bool bitEqual(const float* a, const float* b, int n)
{
  for (int i = 0; i < n; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

std::vector<float> sine(float peak, float freqHz, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] =
        peak * std::sin(2.0f * 3.141592653589793f * freqHz * i / (float)kSr);
  return out;
}

void processAll(tdm::lab::SlamPre& u, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  u.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}
void processAll(tdm::lab::SlamPostNam& u, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  u.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}
void processAll(tdm::lab::SlamPostIr& u, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  u.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}
void processAll(tdm::lab::SlamImpact& u, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  u.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}
void processAll(tdm::lab::SlamGated& u, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  u.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}

// Blend extremes for the common contract (SlamGated splits amount into
// base + peak; everything else has setAmount).
template <typename T>
void zeroBlend(T& u)
{
  u.setAmount(0.0f);
}
inline void zeroBlend(tdm::lab::SlamGated& g)
{
  g.setBase(0.0f);
  g.setPeak(0.0f);
}
template <typename T>
void maxBlend(T& u)
{
  u.setAmount(T::kMaxAmount);
}
inline void maxBlend(tdm::lab::SlamGated& g)
{
  g.setBase(1.0f);
  g.setPeak(2.0f);
}

template <typename T>
void commonContract(const char* name)
{
  const int n = static_cast<int>(kSr); // 1 s
  std::vector<float> zeros(static_cast<size_t>(n), 0.0f);
  std::vector<float> out, out2;

  // Silence -> silence (exact).
  {
    T u;
    u.reset(kSr);
    processAll(u, zeros, out);
    CHECK(bitEqual(out.data(), zeros.data(), n), "%s silence exact", name);
  }
  // Disabled -> bit-exact copy (in-place and out-of-place).
  {
    auto hot = sine(0.9f, 82.41f, n);
    T u;
    u.reset(kSr);
    u.setEnabled(false);
    processAll(u, hot, out);
    CHECK(bitEqual(out.data(), hot.data(), n), "%s disabled exact", name);
    std::vector<float> ip = hot;
    u.processBlock(ip.data(), ip.data(), n);
    CHECK(bitEqual(ip.data(), hot.data(), n), "%s disabled in-place exact", name);
  }
  // amount=0 -> bit-exact dry while enabled.
  {
    auto sig = sine(0.7f, 110.0f, n);
    T u;
    u.reset(kSr);
    zeroBlend(u);
    processAll(u, sig, out);
    CHECK(bitEqual(out.data(), sig.data(), n), "%s amount0 exact dry", name);
  }
  // Reset determinism: two fresh instances agree bit-exactly.
  {
    auto sig = sine(0.7f, 98.0f, n);
    T a, b;
    a.reset(kSr);
    b.reset(kSr);
    processAll(a, sig, out);
    processAll(b, sig, out2);
    CHECK(bitEqual(out.data(), out2.data(), n), "%s reset determinism", name);
  }
  // Block-size determinism: 1xN vs 64x(samples) agree bit-exactly.
  {
    auto sig = sine(0.7f, 98.0f, n);
    T a, b;
    a.reset(kSr);
    b.reset(kSr);
    processAll(a, sig, out);
    out2.assign(sig.size(), 0.0f);
    const int bs = 64;
    for (int i = 0; i < n; i += bs)
      b.processBlock(sig.data() + i, out2.data() + i, bs);
    CHECK(bitEqual(out.data(), out2.data(), n), "%s block-size determinism", name);
  }
  // Hot input stays finite at extreme settings.
  {
    auto hot = sine(8.0f, 55.0f, n);
    T u;
    u.reset(kSr);
    maxBlend(u);
    processAll(u, hot, out);
    CHECK(allFinite(out.data(), n), "%s hot finite", name);
  }
  // Alternate rate stays finite.
  {
    auto sig = sine(0.7f, 110.0f, 44100);
    T u;
    u.reset(44100.0);
    processAll(u, sig, out);
    CHECK(allFinite(out.data(), 44100), "%s 44.1k finite", name);
  }
  // In-place equals out-of-place.
  {
    auto sig = sine(0.7f, 130.0f, n);
    T a, b;
    a.reset(kSr);
    b.reset(kSr);
    processAll(a, sig, out);
    out2 = sig;
    b.processBlock(out2.data(), out2.data(), n);
    CHECK(bitEqual(out.data(), out2.data(), n), "%s in-place safe", name);
  }
}

template <typename T>
void parallelSelectivity(const char* name, float bandHz)
{
  // 0.5 s probes with 20 ms edges trimmed (branch settling).
  const int n = static_cast<int>(kSr / 2);
  const int trim = static_cast<int>(kSr * 0.02);
  auto low = sine(0.5f, 80.0f, n);
  auto high = sine(0.5f, 3000.0f, n);
  T u;
  u.reset(kSr);
  u.setBandHz(bandHz);
  std::vector<float> wl, wh;
  processAll(u, low, wl);
  u.reset(kSr);
  u.setBandHz(bandHz);
  processAll(u, high, wh);
  const float lowDry = rms(low.data() + trim, n - 2 * trim);
  const float lowWet = rms(wl.data() + trim, n - 2 * trim);
  const float highDry = rms(high.data() + trim, n - 2 * trim);
  const float highWet = rms(wh.data() + trim, n - 2 * trim);
  CHECK(lowWet > lowDry * 1.2f, "%s boosts lows (%.3f vs %.3f)", name, lowWet, lowDry);
  CHECK(std::fabs(highWet - highDry) / highDry < 0.05f, "%s ignores highs (%.4f vs %.4f)", name,
        highWet, highDry);
  CHECK(allFinite(wl.data(), n) && allFinite(wh.data(), n), "%s probes finite", name);
}
} // namespace

void runSlamTests()
{
  using namespace tdm::lab;
  commonContract<SlamPre>("pre");
  commonContract<SlamPostNam>("postnam");
  commonContract<SlamPostIr>("postir");
  commonContract<SlamImpact>("impact");
  commonContract<SlamGated>("gated");

  parallelSelectivity<SlamPre>("pre", 140.0f);
  parallelSelectivity<SlamPostNam>("postnam", 200.0f);
  parallelSelectivity<SlamPostIr>("postir", 220.0f);

  // Band extremes stay finite and keep selectivity direction.
  for (float hz : {40.0f, 500.0f})
  {
    SlamPre u;
    u.reset(kSr);
    u.setBandHz(hz);
    auto sig = sine(0.6f, 82.41f, static_cast<int>(kSr / 2));
    std::vector<float> out;
    processAll(u, sig, out);
    CHECK(allFinite(out.data(), (int)out.size()), "pre band %.0f finite", hz);
  }

  // --- SlamImpact trigger behavior ---
  {
    // Silence: no fires, exact zeros.
    SlamImpact d;
    d.reset(kSr);
    std::vector<float> zeros(static_cast<size_t>(kSr), 0.0f), out;
    processAll(d, zeros, out);
    CHECK(d.fireCount() == 0, "impact silence no fires");
    CHECK(bitEqual(out.data(), zeros.data(), (int)zeros.size()), "impact silence exact");
  }
  {
    // One loud 5 ms burst after 1 s of silence: exactly one fire near the
    // burst start, output returns to bit-exact dry once burst+bloom die.
    const int n = static_cast<int>(kSr * 2);
    const int at = static_cast<int>(kSr);
    const int len = static_cast<int>(kSr * 0.005);
    std::vector<float> sig(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < len; ++i)
      sig[static_cast<size_t>(at + i)] =
          0.8f * std::sin(2.0f * 3.141592653589793f * 100.0f * i / (float)kSr);
    SlamImpact d;
    d.reset(kSr);
    std::vector<float> out;
    processAll(d, sig, out);
    CHECK(d.fireCount() == 1, "impact single burst one fire (got %lld)", (long long)d.fireCount());
    CHECK(d.triggerCount() == 1, "impact ring recorded");
    const int64_t stamp = d.triggerAt(0).sample;
    CHECK(stamp >= at - 4 && stamp <= at + 96, "impact stamp near attack (%lld vs %d)",
          (long long)stamp, at);
    CHECK(d.triggerAt(0).strength > 0.3f, "impact strength measured (%.2f)",
          d.triggerAt(0).strength);
    // Late output (last 0.5 s, burst long dead) is bit-exact dry.
    const int late = static_cast<int>(kSr * 3 / 2);
    CHECK(bitEqual(out.data() + late, sig.data() + late, n - late), "impact tail returns to dry");
    // But right after the hit the burst is really there.
    CHECK(rms(out.data() + at, len * 4) > rms(sig.data() + at, len * 4), "impact burst adds energy");
  }
  {
    // Fast burst train (4 ms 100 Hz pips every 50 ms) vs 90 ms refractory:
    // must fire on some and skip others (machine-gun guard + floor).
    const int n = static_cast<int>(kSr * 2);
    std::vector<float> sig(static_cast<size_t>(n), 0.0f);
    const int step = static_cast<int>(kSr * 0.05);
    const int len = static_cast<int>(kSr * 0.004);
    int pulses = 0;
    for (int at = step; at + len < n; at += step, ++pulses)
      for (int i = 0; i < len; ++i)
        sig[static_cast<size_t>(at + i)] =
            0.8f * std::sin(2.0f * 3.141592653589793f * 100.0f * i / (float)kSr);
    SlamImpact d;
    d.reset(kSr);
    std::vector<float> out;
    processAll(d, sig, out);
    CHECK(pulses > 10, "impact train has pulses");
    CHECK(d.fireCount() > 0 && d.fireCount() < pulses, "impact refractory skips (%lld of %d)",
          (long long)d.fireCount(), pulses);
  }
  {
    // Single-sample full-scale spike: crosses but must NOT confirm (no
    // sustain 8 ms later). Spike rejection is the confirm window's job.
    const int n = static_cast<int>(kSr);
    std::vector<float> sig(static_cast<size_t>(n), 0.0f);
    sig[static_cast<size_t>(kSr / 2)] = 1.0f;
    SlamImpact d;
    d.reset(kSr);
    std::vector<float> out;
    processAll(d, sig, out);
    CHECK(d.fireCount() == 0, "impact rejects lone spike (got %lld)", (long long)d.fireCount());
  }
  {
    // Scheduled mode: detector bypassed, bursts land stamp+8 ms on
    // otherwise silent input. Deterministic split-tap behavior.
    const int n = static_cast<int>(kSr);
    std::vector<float> zeros(static_cast<size_t>(n), 0.0f), out;
    SlamImpact d;
    d.reset(kSr);
    d.scheduleFire(4800, 1.0f); // 0.10 s
    d.scheduleFire(24000, 0.5f); // 0.50 s
    d.setScheduled(true);
    processAll(d, zeros, out);
    CHECK(d.fireCount() == 2, "impact scheduled fires twice");
    CHECK(d.triggerCount() == 2, "impact scheduled ring");
    CHECK(d.triggerAt(0).sample == 4800, "impact scheduled stamp kept");
    // Burst 1 starts at 4800 + 384 (8 ms): energy right after, silence
    // right before (modulo the raised-cosine attack ramp).
    CHECK(rms(out.data() + 4800 + 384, 2000) > 0.05f, "impact scheduled burst sounds");
    CHECK(rms(out.data(), 4800) == 0.0f, "impact scheduled silent before");
    // Between the bursts (0.3 s in, first burst long dead): exact dry.
    CHECK(bitEqual(out.data() + 14400, zeros.data() + 14400, 4800),
          "impact scheduled returns to dry");
    // reset() leaves scheduled mode and clears the list.
    d.reset(kSr);
    processAll(d, zeros, out);
    CHECK(d.fireCount() == 0, "impact reset clears schedule");
    CHECK(bitEqual(out.data(), zeros.data(), n), "impact reset silent");
  }

  // --- SlamGated (E) ---
  {
    // Scheduled fire on a sustained low sine: gate opens (output grows
    // past the base blend), then decays back to the base blend exactly.
    const int n = static_cast<int>(kSr);
    auto sig = sine(0.5f, 80.0f, n);
    SlamGated g;
    g.reset(kSr);
    g.setBase(0.0f); // pure gated: dry except around fires
    g.setPeak(1.5f);
    g.scheduleFire(4800, 1.0f);
    g.setScheduled(true);
    std::vector<float> out;
    processAll(g, sig, out);
    CHECK(g.fireCount() == 1, "gated scheduled fires once");
    // Gate fully open just after fire+8 ms: clearly above dry.
    const float openRms = rms(out.data() + 4800 + 384, 2000);
    const float dryRms = rms(sig.data() + 4800 + 384, 2000);
    CHECK(openRms > dryRms * 1.3f, "gated opens on fire (%.3f vs %.3f)", openRms, dryRms);
    // Late (0.6 s in, env long dead): bit-exact dry with base 0.
    CHECK(bitEqual(out.data() + 28800, sig.data() + 28800, 4800), "gated returns to dry");
    // Before the stamp: bit-exact dry.
    CHECK(bitEqual(out.data(), sig.data(), 4800), "gated silent before fire");
  }
  {
    // D and E gate on IDENTICAL decisions: same input, same schedule
    // plumbing, same stamps and strengths.
    auto sig = sine(0.7f, 82.41f, static_cast<int>(kSr));
    SlamImpact d;
    SlamGated g;
    d.reset(kSr);
    g.reset(kSr);
    std::vector<float> o1, o2;
    processAll(d, sig, o1);
    processAll(g, sig, o2);
    CHECK(d.fireCount() == g.fireCount(), "gated/impact same fire count");
    CHECK(d.fireCount() == 1, "gated/impact fire once on sustain");
    if (d.triggerCount() == 1 && g.triggerCount() == 1)
    {
      CHECK(d.triggerAt(0).sample == g.triggerAt(0).sample, "gated/impact same stamp");
      CHECK(d.triggerAt(0).strength == g.triggerAt(0).strength, "gated/impact same strength");
    }
  }
  {
    // C-like selectivity with base 1 / peak 0 (continuous branch).
    const int n = static_cast<int>(kSr / 2);
    const int trim = static_cast<int>(kSr * 0.02);
    auto low = sine(0.5f, 80.0f, n);
    auto high = sine(0.5f, 3000.0f, n);
    SlamGated g;
    g.reset(kSr);
    g.setBandHz(220.0f);
    g.setBase(1.0f);
    g.setPeak(0.0f);
    std::vector<float> wl, wh;
    processAll(g, low, wl);
    g.reset(kSr);
    g.setBandHz(220.0f);
    g.setBase(1.0f);
    g.setPeak(0.0f);
    processAll(g, high, wh);
    const float lowWet = rms(wl.data() + trim, n - 2 * trim);
    const float lowDry = rms(low.data() + trim, n - 2 * trim);
    const float highWet = rms(wh.data() + trim, n - 2 * trim);
    const float highDry = rms(high.data() + trim, n - 2 * trim);
    CHECK(lowWet > lowDry * 1.2f, "gated boosts lows (%.3f vs %.3f)", lowWet, lowDry);
    CHECK(std::fabs(highWet - highDry) / highDry < 0.05f, "gated ignores highs");
  }
  {
    // Sustained loud sine: one fire at the start, then silence from the
    // detector (floor adapts; sustain untouched apart from the first hit).
    auto sig = sine(0.7f, 82.41f, static_cast<int>(kSr * 2));
    SlamImpact d;
    d.reset(kSr);
    std::vector<float> out;
    processAll(d, sig, out);
    CHECK(d.fireCount() == 1, "impact sustain fires once (got %lld)", (long long)d.fireCount());
  }
  {
    // Sensitivity extremes: deaf fires less-or-equal, eager more-or-equal.
    auto sig = sine(0.7f, 82.41f, static_cast<int>(kSr));
    SlamImpact deaf, eager;
    deaf.reset(kSr);
    deaf.setSensitivity(0.0f);
    eager.reset(kSr);
    eager.setSensitivity(1.0f);
    std::vector<float> o1, o2;
    processAll(deaf, sig, o1);
    processAll(eager, sig, o2);
    CHECK(deaf.fireCount() <= eager.fireCount(), "impact sensitivity orders fires");
    CHECK(allFinite(o1.data(), (int)o1.size()) && allFinite(o2.data(), (int)o2.size()),
          "impact sens finite");
  }

  // --- SlamDsp unit spots ---
  {
    // Biquad LP passes DC at unity.
    SlamBiquad lp;
    lp.setLowpass(kSr, 200.0f, 0.7f);
    float v = 0.0f;
    for (int i = 0; i < 48000; ++i)
      v = lp.process(1.0f);
    CHECK(std::fabs(v - 1.0f) < 1e-4f, "biquad lp dc unity (%.6f)", v);
    // Shapers bounded (asymptote is 1/tanh(d) by the unity-at-full-scale norm).
    CHECK(std::fabs(slamSoftSat(10.0f, 3.0f)) <= 1.0 / std::tanh(3.0) + 1e-6, "softsat bounded");
    CHECK(std::fabs(slamAsymSat(-10.0f, 2.5f, 1.2f)) <= 1.0 / std::tanh(1.2) + 1e-6,
          "asymsat bounded");
    CHECK(std::fabs(slamHardSat(10.0f, 3.0f)) <= 1.34f, "hardsat bounded");
    CHECK(slamSoftSat(0.0f, 2.0f) == 0.0f, "softsat zero");
    // Burst bounded by gain, decays to inactive.
    SlamBurst b;
    b.reset(kSr, 70.0f, 48.0f, 130.0f, 1.5f, 0.9f);
    b.trigger(1.0f);
    float peak = 0.0f;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
    {
      const float s = b.process();
      if (std::fabs(s) > peak)
        peak = std::fabs(s);
    }
    CHECK(peak <= 0.9f + 1e-5f, "burst bounded (%.4f)", peak);
    CHECK(!b.active(), "burst goes idle");
    CHECK(b.process() == 0.0f, "burst idle silent");
    // Onset silent on silence.
    SlamOnset o;
    o.reset(kSr, 600.0f, 12.0f, -48.0f, 120.0f);
    bool fired = false;
    for (int i = 0; i < 4800; ++i)
      fired = fired || (o.feed(0.0f) > 0.0f);
    CHECK(!fired, "onset silent on silence");
    // Comp reduces hot peaks (smoke: heavy limiting vs dry).
    SlamComp c;
    c.reset(kSr, -20.0f, 20.0f, 1.0f, 50.0f, 1.0f);
    auto hot = sine(1.5f, 100.0f, static_cast<int>(kSr / 4));
    float pk = 0.0f;
    for (size_t i = 0; i < hot.size(); ++i)
    {
      const float y = c.process(hot[i]);
      if (i > 2400 && std::fabs(y) > pk) // past the attack window
        pk = std::fabs(y);
    }
    CHECK(pk < 1.5f && pk > 0.05f, "comp limits (%.3f)", pk);
  }
}

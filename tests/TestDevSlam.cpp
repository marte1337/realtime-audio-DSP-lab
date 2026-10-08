// DevSlam router tests: transparency (off / amount 0), Amount-100 ==
// study candidates (bit-exact), tap routing, single-flavour output,
// rig-param isolation, Impact telemetry, click-free switching, live
// parameter adoption. Rig-level tests run file-free (no-model NAM/IR
// pass through, so the bare rig is a wire).

#include "tests/Assert.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/lab/Slam/DevSlam.h"
#include "dsp/lab/Slam/SlamCandidates.h"

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

std::vector<float> sine(float peak, float freqHz, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] =
        peak * std::sin(2.0f * 3.141592653589793f * freqHz * i / (float)kSr);
  return out;
}

// N decaying-pluck attacks (100 Hz pips with exponential choke), spaced
// `gap` samples apart after 0.5 s of silence. Deterministic Impact fuel.
std::vector<float> attacks(int n, int count, int gap)
{
  std::vector<float> sig(static_cast<size_t>(n), 0.0f);
  const int at0 = static_cast<int>(kSr / 2);
  for (int p = 0; p < count; ++p)
  {
    const int at = at0 + p * gap;
    for (int i = 0; i < static_cast<int>(kSr / 10) && at + i < n; ++i)
      sig[static_cast<size_t>(at + i)] +=
          0.8f * std::sin(2.0f * 3.141592653589793f * 100.0f * i / (float)kSr)
          * std::exp(-i / (0.020f * (float)kSr));
  }
  return sig;
}

// Run all three taps in rig order (pre-drive first: adoption lives there).
void runTaps(tdm::lab::DevSlam& d, std::vector<float>& io)
{
  const int n = static_cast<int>(io.size());
  for (int off = 0; off < n; off += kBlock)
  {
    const int m = (n - off < kBlock) ? (n - off) : kBlock;
    float* p = io.data() + off;
    d.preDrive().process(p, p, m);
    d.postNam().process(p, p, m);
    d.postIr().process(p, p, m);
  }
}

void rigRun(tdm::TechDeathRig& rig, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  const int n = static_cast<int>(in.size());
  for (int off = 0; off < n; off += kBlock)
  {
    const int m = (n - off < kBlock) ? (n - off) : kBlock;
    const float* bi[1] = {in.data() + off};
    float* bo[1] = {out.data() + off};
    rig.processBlock(bi, 1, bo, 1, m);
  }
}

float maxStep(const float* x, int n)
{
  float ms = 0.0f;
  for (int i = 1; i < n; ++i)
    ms = std::max(ms, std::fabs(x[i] - x[i - 1]));
  return ms;
}
} // namespace

void runDevSlamTests()
{
  using tdm::lab::DevSlam;
  using tdm::lab::SlamImpact;
  using tdm::lab::SlamPostIr;
  using tdm::lab::SlamPostNam;
  using tdm::lab::SlamPre;
  using tdm::lab::SlamTrigger;
  using Flavor = DevSlam::Flavor;

  // Defaults: off, Push, 100%, silent telemetry.
  {
    DevSlam d;
    CHECK(!d.isEnabled(), "default off");
    CHECK(d.flavor() == Flavor::Push, "default push");
    CHECK(d.amount01() == 1.0f, "default amount 100");
    CHECK(d.impactFireCount() == 0, "default no fires");
    CHECK(d.activeFlavor() == Flavor::Push, "default active push");
    CHECK(d.macro() == 0.0f, "default macro dry");
  }

  // Off is transparent (taps + full rig with seams installed).
  {
    auto sig = sine(0.6f, 82.41f, static_cast<int>(kSr));
    DevSlam d;
    d.preDrive().reset(kSr, kBlock);
    std::vector<float> io = sig;
    runTaps(d, io);
    CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "off taps transparent");

    tdm::TechDeathRig dry, wet;
    dry.reset(kSr, kBlock);
    wet.reset(kSr, kBlock);
    DevSlam ds;
    wet.setSlamPreDrive(&ds.preDrive());
    wet.setSlamPostNam(&ds.postNam());
    wet.setSlamPostIr(&ds.postIr());
    wet.reset(kSr, kBlock); // rig forwards reset to all three taps
    std::vector<float> o1, o2;
    rigRun(dry, sig, o1);
    rigRun(wet, sig, o2);
    CHECK(bitEqual(o1.data(), o2.data(), (int)sig.size()), "off rig transparent");
  }

  // Amount 0 is transparent for every flavour (enabled). Targets go in
  // before reset (mirrors UI-set-then-Start) so the run is steady-state.
  for (Flavor f : {Flavor::Push, Flavor::Crush, Flavor::Mass, Flavor::Impact})
  {
    auto sig = sine(0.6f, 98.0f, static_cast<int>(kSr));
    DevSlam d;
    d.setEnabled(true);
    d.setFlavor(f);
    d.setAmount01(0.0f);
    d.preDrive().reset(kSr, kBlock);
    std::vector<float> io = sig;
    runTaps(d, io);
    CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "amount0 transparent f=%d", (int)f);
  }

  // Amount 100 reproduces each study candidate bit-exactly (steady state).
  {
    auto sig = sine(0.6f, 82.41f, static_cast<int>(kSr));
    // Push == SlamPre @140 x1.0 (study A1).
    {
      SlamPre ref;
      ref.reset(kSr);
      ref.setBandHz(140.0f);
      ref.setAmount(1.0f);
      std::vector<float> want(sig.size(), 0.0f);
      ref.processBlock(sig.data(), want.data(), (int)sig.size());
      DevSlam d;
      d.setEnabled(true);
      d.setFlavor(Flavor::Push);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> got = sig;
      runTaps(d, got);
      CHECK(bitEqual(got.data(), want.data(), (int)sig.size()), "push100 == study A1");
    }
    // Crush == SlamPostNam @200 x1.0 (study B).
    {
      SlamPostNam ref;
      ref.reset(kSr);
      ref.setBandHz(200.0f);
      ref.setAmount(1.0f);
      std::vector<float> want(sig.size(), 0.0f);
      ref.processBlock(sig.data(), want.data(), (int)sig.size());
      DevSlam d;
      d.setEnabled(true);
      d.setFlavor(Flavor::Crush);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> got = sig;
      runTaps(d, got);
      CHECK(bitEqual(got.data(), want.data(), (int)sig.size()), "crush100 == study B");
    }
    // Mass == SlamPostIr @220 x1.0 (study C).
    {
      SlamPostIr ref;
      ref.reset(kSr);
      ref.setBandHz(220.0f);
      ref.setAmount(1.0f);
      std::vector<float> want(sig.size(), 0.0f);
      ref.processBlock(sig.data(), want.data(), (int)sig.size());
      DevSlam d;
      d.setEnabled(true);
      d.setFlavor(Flavor::Mass);
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> got = sig;
      runTaps(d, got);
      CHECK(bitEqual(got.data(), want.data(), (int)sig.size()), "mass100 == study C");
    }
    // Impact == scheduled SlamImpact dhyb voice on the same fires.
    {
      auto atk = attacks(static_cast<int>(kSr * 2), 3, static_cast<int>(kSr * 0.4));
      // Reference fires: standalone trigger (identical detector code).
      SlamTrigger trig;
      trig.reset(kSr, 0.5f, 90.0f);
      for (float v : atk)
        trig.feed(v);
      SlamImpact ref;
      ref.reset(kSr); // study dhyb voice defaults (70->48, 130 ms)
      for (int i = 0; i < trig.triggerCount(); ++i)
      {
        auto t = trig.triggerAt(i);
        ref.scheduleFire(t.sample, t.strength);
      }
      ref.setScheduled(true);
      std::vector<float> want(atk.size(), 0.0f);
      ref.processBlock(atk.data(), want.data(), (int)atk.size());
      CHECK(ref.fireCount() == trig.fireCount(), "impact ref scheduled all");
      DevSlam d;
      d.setEnabled(true);
      d.setFlavor(Flavor::Impact);
      d.setImpactVoice(DevSlam::ImpactVoice::Legacy); // v1 pin (FROZEN)
      d.setImpactSensitivity(0.5f); // v1 definition (default is now 0.6)
      d.setAmount01(1.0f);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> got = atk;
      runTaps(d, got);
      CHECK(d.impactFireCount() == trig.fireCount(), "impact same fires (%lld)",
            (long long)d.impactFireCount());
      CHECK(bitEqual(got.data(), want.data(), (int)atk.size()), "impact100 == study dhyb");
    }
  }

  // Routing: each flavour touches exactly its own tap; inactive taps are
  // exact no-ops; only one flavour renders at a time (implied by the
  // bit-exact single-candidate matches above, pinned per-tap here).
  {
    auto sig = sine(0.6f, 82.41f, static_cast<int>(kSr));
    // Push: pre-drive wet, post taps dry.
    {
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setEnabled(true);
      d.setFlavor(Flavor::Push);
      d.setAmount01(1.0f);
      std::vector<float> io = sig;
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.preDrive().process(p, p, kBlock);
      }
      CHECK(!bitEqual(io.data(), sig.data(), (int)sig.size()), "push predrive wet");
      std::vector<float> io2 = sig;
      for (int off = 0; off < (int)io2.size(); off += kBlock)
      {
        float* p = io2.data() + off;
        d.postNam().process(p, p, kBlock);
        d.postIr().process(p, p, kBlock);
      }
      CHECK(bitEqual(io2.data(), sig.data(), (int)sig.size()), "push post taps dry");
    }
    // Crush: post-NAM wet only.
    {
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setEnabled(true);
      d.setFlavor(Flavor::Crush);
      d.setAmount01(1.0f);
      std::vector<float> io = sig;
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.preDrive().process(p, p, kBlock); // adoption only (detector idle)
      }
      CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "crush predrive dry");
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.postNam().process(p, p, kBlock);
      }
      CHECK(!bitEqual(io.data(), sig.data(), (int)sig.size()), "crush postnam wet");
      std::vector<float> io2 = sig;
      for (int off = 0; off < (int)io2.size(); off += kBlock)
      {
        float* p = io2.data() + off;
        d.postIr().process(p, p, kBlock);
      }
      CHECK(bitEqual(io2.data(), sig.data(), (int)sig.size()), "crush postir dry");
    }
    // Impact: pre-drive observes (io untouched), post-IR renders.
    {
      auto atk = attacks(static_cast<int>(kSr), 2, static_cast<int>(kSr * 0.3));
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setEnabled(true);
      d.setFlavor(Flavor::Impact);
      d.setAmount01(1.0f);
      std::vector<float> io = atk;
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.preDrive().process(p, p, kBlock);
      }
      CHECK(bitEqual(io.data(), atk.data(), (int)atk.size()), "impact predrive observes");
      CHECK(d.impactFireCount() == 2, "impact telemetry counts (%lld)",
            (long long)d.impactFireCount());
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.postNam().process(p, p, kBlock);
      }
      CHECK(bitEqual(io.data(), atk.data(), (int)atk.size()), "impact postnam dry");
      for (int off = 0; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.postIr().process(p, p, kBlock);
      }
      CHECK(!bitEqual(io.data(), atk.data(), (int)atk.size()), "impact postir wet");
    }
  }

  // Telemetry counts known attacks at any Amount (firing-but-quiet at 0).
  {
    auto atk = attacks(static_cast<int>(kSr * 2), 5, static_cast<int>(kSr * 0.3));
    for (float amt : {0.0f, 0.5f, 1.0f})
    {
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setEnabled(true);
      d.setFlavor(Flavor::Impact);
      d.setAmount01(amt);
      std::vector<float> io = atk;
      runTaps(d, io);
      CHECK(d.impactFireCount() == 5, "telemetry 5 fires @%.1f (%lld)", amt,
            (long long)d.impactFireCount());
      CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "telemetry run finite");
    }
    // Disabled: detector sleeps, counter frozen.
    {
      auto atk2 = attacks(static_cast<int>(kSr), 3, static_cast<int>(kSr * 0.2));
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setFlavor(Flavor::Impact); // armed flavour, but not enabled
      std::vector<float> io = atk2;
      runTaps(d, io);
      CHECK(d.impactFireCount() == 0, "disabled no telemetry");
    }
  }

  // Flavour/enable switching: finite, click-bounded, converges; rig
  // params untouched; SLAM state survives NAM/IR/load changes.
  {
    auto sig = sine(0.5f, 82.41f, static_cast<int>(kSr * 2));
    const float dryStep = maxStep(sig.data(), (int)sig.size());
    // Push -> Mass mid-stream through the taps.
    {
      auto switched = [&](std::vector<float>& io) {
        DevSlam d;
        d.setEnabled(true);
        d.setFlavor(Flavor::Push);
        d.setAmount01(1.0f);
        d.preDrive().reset(kSr, kBlock);
        const int half = 94 * kBlock; // block-aligned switch point
        for (int off = 0; off < half; off += kBlock)
        {
          float* p = io.data() + off;
          d.preDrive().process(p, p, kBlock);
          d.postNam().process(p, p, kBlock);
          d.postIr().process(p, p, kBlock);
        }
        CHECK(d.activeFlavor() == Flavor::Push, "push active pre-switch");
        d.setFlavor(Flavor::Mass); // live switch, audio running
        for (int off = half; off < (int)io.size(); off += kBlock)
        {
          float* p = io.data() + off;
          d.preDrive().process(p, p, kBlock);
          d.postNam().process(p, p, kBlock);
          d.postIr().process(p, p, kBlock);
        }
        CHECK(d.activeFlavor() == Flavor::Mass, "mass active post-switch");
        CHECK(d.macro() == 1.0f, "macro settled");
        return half;
      };
      std::vector<float> io = sig, io2 = sig;
      const int half = switched(io);
      switched(io2);
      CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "switch finite");
      // No click: worst step near the switch stays close to the dry slope.
      const float sw = maxStep(io.data() + half - 2048, 8192);
      CHECK(sw < dryStep * 4.0f + 0.01f, "switch click-bounded (%.5f vs %.5f)", sw, dryStep);
      // Identical switch sequence is bit-deterministic.
      CHECK(bitEqual(io.data(), io2.data(), (int)io.size()), "switch deterministic");
      // Post-switch output renders Mass (clearly wet, settled).
      const int tail0 = (int)io.size() - 48000;
      CHECK(!bitEqual(io.data() + tail0, sig.data() + tail0, 48000), "switch lands wet");
    }
    // Flip-flop (Push->Crush->Push within two blocks): finite, latest wins.
    {
      DevSlam d;
      d.preDrive().reset(kSr, kBlock);
      d.setEnabled(true);
      d.setFlavor(Flavor::Push);
      std::vector<float> io = sig;
      for (int off = 0; off < 2 * kBlock; off += kBlock)
      {
        float* p = io.data() + off;
        d.preDrive().process(p, p, kBlock);
        d.postNam().process(p, p, kBlock);
        d.postIr().process(p, p, kBlock);
      }
      d.setFlavor(Flavor::Crush);
      {
        float* p = io.data() + 2 * kBlock;
        d.preDrive().process(p, p, kBlock);
        d.postNam().process(p, p, kBlock);
        d.postIr().process(p, p, kBlock);
      }
      d.setFlavor(Flavor::Push);
      for (int off = 3 * kBlock; off < (int)io.size(); off += kBlock)
      {
        float* p = io.data() + off;
        d.preDrive().process(p, p, kBlock);
        d.postNam().process(p, p, kBlock);
        d.postIr().process(p, p, kBlock);
      }
      CHECK(d.activeFlavor() == Flavor::Push, "flip-flop latest wins");
      CHECK(d.macro() == 1.0f, "flip-flop settled");
      CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "flip-flop finite");
    }
    // Enable on/off ramps through the taps.
    {
      DevSlam d;
      d.setFlavor(Flavor::Mass);
      d.preDrive().reset(kSr, kBlock);
      std::vector<float> io = sig;
      runTaps(d, io); // off: transparent
      CHECK(bitEqual(io.data(), sig.data(), (int)sig.size()), "starts off transparent");
      d.setEnabled(true);
      io = sig;
      runTaps(d, io);
      CHECK(d.macro() == 1.0f, "enable ramps up");
      CHECK(!bitEqual(io.data(), sig.data(), (int)sig.size()), "enabled wet");
      d.setEnabled(false);
      io = sig; // fresh input: the disable tail must be dry-exact
      runTaps(d, io);
      CHECK(d.macro() == 0.0f, "disable ramps down");
      const int tail0 = (int)io.size() - 48000;
      CHECK(bitEqual(io.data() + tail0, sig.data() + tail0, 48000), "disabled tail dry");
    }
  }

  // Rig isolation: flavour switches never touch rig params; rig/NAM/IR
  // changes never touch SLAM state.
  {
    auto sig = sine(0.5f, 110.0f, static_cast<int>(kSr));
    tdm::TechDeathRig rig;
    rig.reset(kSr, kBlock);
    DevSlam d;
    rig.setSlamPreDrive(&d.preDrive());
    rig.setSlamPostNam(&d.postNam());
    rig.setSlamPostIr(&d.postIr());
    rig.reset(kSr, kBlock);
    rig.setDriveEnabled(true);
    rig.setTight(0.85f);
    rig.setTransposeEnabled(false);
    d.setEnabled(true);
    d.setFlavor(Flavor::Mass);
    d.setAmount01(0.65f);
    std::vector<float> o1;
    rigRun(rig, sig, o1);
    const tdm::RigParams before = rig.params();
    d.setFlavor(Flavor::Impact);
    d.setFlavor(Flavor::Push);
    d.setAmount01(0.25f);
    std::vector<float> o2;
    rigRun(rig, sig, o2);
    const tdm::RigParams after = rig.params();
    CHECK(before.driveEnabled == after.driveEnabled && before.tight == after.tight
              && before.transposeEnabled == after.transposeEnabled
              && before.gateEnabled == after.gateEnabled && before.inputTrimDb == after.inputTrimDb,
          "flavour switches keep rig params");
    // Rig-side changes: SLAM targets + telemetry untouched.
    auto atk = attacks(static_cast<int>(kSr), 2, static_cast<int>(kSr * 0.3));
    d.setFlavor(Flavor::Impact);
    d.setAmount01(1.0f);
    std::vector<float> o3;
    rigRun(rig, atk, o3);
    const int64_t firesBefore = d.impactFireCount();
    CHECK(firesBefore == 2, "rig impact fires (%lld)", (long long)firesBefore);
    const tdm::RigParams rp = rig.params();
    (void)rp;
    rig.clearNam();
    rig.clearIr();
    rig.setDriveEnabled(false);
    rig.setTight(0.2f);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    rig.setWeight(0.7f);
    CHECK(d.flavor() == Flavor::Impact && d.amount01() == 1.0f && d.isEnabled(),
          "rig changes keep slam targets");
    CHECK(d.impactFireCount() == firesBefore, "rig changes keep telemetry");
  }

  // Live amount adoption glides (halving per block) without locks.
  {
    DevSlam d;
    d.preDrive().reset(kSr, kBlock);
    d.setEnabled(true);
    d.setFlavor(Flavor::Mass);
    d.setAmount01(1.0f);
    std::vector<float> io = sine(0.5f, 82.41f, static_cast<int>(kSr));
    runTaps(d, io);
    CHECK(d.currentAmount() == 1.0f, "amount parked at 100");
    d.setAmount01(0.0f);
    for (int b = 0; b < 10; ++b)
    {
      float* p = io.data() + b * kBlock;
      d.preDrive().process(p, p, kBlock);
      d.postNam().process(p, p, kBlock);
      d.postIr().process(p, p, kBlock);
    }
    CHECK(d.currentAmount() < 0.002f, "amount glides down (%.5f)", d.currentAmount());
    CHECK(tdm_test::allFinite(io.data(), (int)io.size()), "amount glide finite");
  }

  // Rig reset() forwards to all three taps (idempotent 3x reset).
  {
    tdm::TechDeathRig rig;
    DevSlam d;
    d.setEnabled(true);
    d.setFlavor(Flavor::Crush);
    rig.setSlamPreDrive(&d.preDrive());
    rig.setSlamPostNam(&d.postNam());
    rig.setSlamPostIr(&d.postIr());
    rig.reset(kSr, kBlock); // resets DevSlam 3x; must stay coherent
    CHECK(d.activeFlavor() == Flavor::Crush && d.macro() == 1.0f, "3x reset parks at targets");
    auto sig = sine(0.5f, 82.41f, static_cast<int>(kSr));
    std::vector<float> o1, o2;
    rigRun(rig, sig, o1);
    rig.reset(kSr, kBlock);
    rigRun(rig, sig, o2);
    CHECK(bitEqual(o1.data(), o2.data(), (int)sig.size()), "reset deterministic");
  }
}

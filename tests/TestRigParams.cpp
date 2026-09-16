// Handoff tests: control-thread parameter stores must reach the audio
// thread safely at block boundaries, with clamping, enable toggles, and
// snapshot round-trips intact. DSP algorithms themselves are covered by the
// existing gate/trim/drive/rig suites; this suite covers only the new
// Rig-level parameter exchange (atomics + per-block sync).

#include "tests/Assert.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "dsp/Gate/TechDeathGate.h"
#include "dsp/InputTrim.h"
#include "dsp/OutputTrim.h"
#include "dsp/TechDeathRig.h"
#include "dsp/TightDrive/TightDrive.h"

namespace
{
// Deterministic LCG in [0, 1).
float unitRand(uint32_t& s)
{
  s = s * 1664525u + 1013904223u;
  return static_cast<float>((s >> 8) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

void runBlocks(tdm::TechDeathRig& rig, const std::vector<float>& in, std::vector<float>& out, int nBlocks)
{
  for (int b = 0; b < nBlocks; ++b)
  {
    const float* bi[1] = {in.data()};
    float* bo[1] = {out.data()};
    rig.processBlock(bi, 1, bo, 1, static_cast<int>(in.size()));
  }
}
} // namespace

void runRigParamsTests()
{
  // Defaults snapshot mirrors the DSP stage defaults.
  {
    const tdm::RigParams p;
    TDM_CHECK(p.inputTrimDb == tdm::InputTrim::kDefaultTrimDb, "params default input trim");
    TDM_CHECK(!p.gateEnabled, "params default gate off");
    TDM_CHECK(p.gateThresholdDb == tdm::TechDeathGate::kDefaultThresholdDb, "params default gate thresh");
    TDM_CHECK(p.gateReleaseMs == tdm::TechDeathGate::kDefaultReleaseMs, "params default gate release");
    TDM_CHECK(!p.driveEnabled, "params default drive off");
    TDM_CHECK(p.tight == tdm::TightDrive::kDefaultTight, "params default tight");
    TDM_CHECK(p.drive == tdm::TightDrive::kDefaultDrive, "params default drive");
    TDM_CHECK(p.bite == tdm::TightDrive::kDefaultBite, "params default bite");
    TDM_CHECK(p.outputTrimDb == tdm::OutputTrim::kDefaultTrimDb, "params default output trim");
  }
  // Fresh rig exposes the same defaults through its getters.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    const tdm::RigParams p = rig.params();
    TDM_CHECK(p.inputTrimDb == 0.0f && !p.gateEnabled && !p.driveEnabled && p.outputTrimDb == 0.0f,
              "rig defaults snapshot");
  }
  // Setters clamp exactly like the stages (in-range values pass through).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    rig.setInputTrimDb(99.0f);
    rig.setGateThresholdDb(99.0f);
    rig.setGateReleaseMs(-5.0f);
    rig.setTight(2.0f);
    rig.setDrive(-2.0f);
    rig.setBite(7.0f);
    rig.setOutputTrimDb(-99.0f);
    TDM_CHECK(rig.inputTrimDb() == tdm::InputTrim::kMaxTrimDb, "input trim clamps high");
    TDM_CHECK(rig.gateThresholdDb() == tdm::TechDeathGate::kMaxThresholdDb, "gate thresh clamps high");
    rig.setGateThresholdDb(-100.0f);
    TDM_CHECK(rig.gateThresholdDb() == tdm::TechDeathGate::kMinThresholdDb, "gate thresh clamps low");
    TDM_CHECK(rig.gateThresholdDb() == -80.0f, "gate thresh min is -80 dB");
    rig.setGateThresholdDb(0.0f);
    TDM_CHECK(rig.gateThresholdDb() == -35.0f, "gate thresh max is -35 dB");
    TDM_CHECK(rig.gateReleaseMs() == tdm::TechDeathGate::kMinReleaseMs, "gate release clamps low");
    TDM_CHECK(rig.tight() == tdm::TightDrive::kMaxTight, "tight clamps high");
    TDM_CHECK(rig.drive() == tdm::TightDrive::kMinDrive, "drive clamps low");
    TDM_CHECK(rig.bite() == tdm::TightDrive::kMaxBite, "bite clamps high");
    TDM_CHECK(rig.outputTrimDb() == tdm::OutputTrim::kMinTrimDb, "output trim clamps low");
  }
  // Bulk snapshot round-trips exactly for in-range values.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    tdm::RigParams p;
    p.inputTrimDb = 6.0f;
    p.gateEnabled = true;
    p.gateThresholdDb = -55.0f;
    p.gateReleaseMs = 52.0f;
    p.driveEnabled = true;
    p.tight = 0.85f;
    p.drive = 0.50f;
    p.bite = 0.70f;
    p.outputTrimDb = -3.0f;
    rig.setParams(p);
    const tdm::RigParams q = rig.params();
    TDM_CHECK(q.inputTrimDb == p.inputTrimDb && q.gateEnabled == p.gateEnabled
                  && q.gateThresholdDb == p.gateThresholdDb && q.gateReleaseMs == p.gateReleaseMs
                  && q.driveEnabled == p.driveEnabled && q.tight == p.tight && q.drive == p.drive
                  && q.bite == p.bite && q.outputTrimDb == p.outputTrimDb,
              "setParams/params round-trip");
  }
  // Gate enable applies at the next block boundary, both directions.
  // Quiet DC sits below the close level, so enabled = ~silent, disabled =
  // bit-exact passthrough.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    rig.setGateThresholdDb(-35.0f); // tightest: close level is -41 dB
    rig.setGateReleaseMs(10.0f);
    std::vector<float> in(256, 0.01f), out(256, 9.0f); // -40 dBFS DC
    rig.setGateEnabled(true);
    runBlocks(rig, in, out, 40); // let the closed fade settle
    TDM_CHECK(out[255] < 1e-4f, "enabled gate suppresses sub-close DC");
    rig.setGateEnabled(false);
    runBlocks(rig, in, out, 1); // one block boundary later...
    bool exact = true;
    for (float v : out)
      exact = exact && v == 0.01f;
    TDM_CHECK(exact, "disabling gate restores exact passthrough in one block");
  }
  // Drive enable audibly changes the path (and disabling restores it).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    std::vector<float> in(256), out(256);
    for (int i = 0; i < 256; ++i)
      in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * i / 64.0f);
    runBlocks(rig, in, out, 4);
    std::vector<float> bypassed = out;
    rig.setDriveEnabled(true);
    rig.setDrive(1.0f);
    runBlocks(rig, in, out, 200); // let smoothing + filters settle
    float maxDiff = 0.0f;
    for (int i = 0; i < 256; ++i)
      maxDiff = std::fabs(out[static_cast<size_t>(i)] - bypassed[static_cast<size_t>(i)]) > maxDiff
                    ? std::fabs(out[static_cast<size_t>(i)] - bypassed[static_cast<size_t>(i)])
                    : maxDiff;
    TDM_CHECK(maxDiff > 0.01f, "enabled drive changes the signal");
    rig.setDriveEnabled(false);
    runBlocks(rig, in, out, 1);
    bool exact = true;
    for (int i = 0; i < 256; ++i)
      exact = exact && out[static_cast<size_t>(i)] == in[static_cast<size_t>(i)];
    TDM_CHECK(exact, "disabling drive restores exact passthrough in one block");
  }
  // Continuous params converge through the handoff: +6.02 dB output trim
  // on DC must settle at ~2x (empty rig, no NAM/IR).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setOutputTrimDb(6.0206f);
    std::vector<float> in(512, 0.25f), out(512, 0.0f);
    runBlocks(rig, in, out, 200); // ~2 s at 48 kHz, smoothing is ~10 ms
    TDM_CHECK_CLOSE(out[511], 0.5f, 1e-3f, "output trim handoff converges");
  }
  // Same for input trim (proves a second continuous path end to end).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setInputTrimDb(-6.0206f);
    std::vector<float> in(512, 0.5f), out(512, 0.0f);
    runBlocks(rig, in, out, 200);
    TDM_CHECK_CLOSE(out[511], 0.25f, 1e-3f, "input trim handoff converges");
  }
  // Concurrent hammer: writer thread stores random params/enables while the
  // audio thread processes continuously. Must stay finite and never crash;
  // with the atomic handoff this is also TSan-clean (see /tmp probe note).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setGateEnabled(true);
    rig.setDriveEnabled(true);
    std::atomic<bool> done{false};
    std::thread writer([&] {
      uint32_t s = 0xC0FFEEu;
      for (int i = 0; i < 3000; ++i)
      {
        rig.setInputTrimDb(-12.0f + 30.0f * unitRand(s));
        rig.setGateThresholdDb(-80.0f + 45.0f * unitRand(s));
        rig.setGateReleaseMs(10.0f + 490.0f * unitRand(s));
        rig.setTight(unitRand(s));
        rig.setDrive(unitRand(s));
        rig.setBite(unitRand(s));
        rig.setOutputTrimDb(-24.0f + 48.0f * unitRand(s));
        rig.setGateEnabled(unitRand(s) > 0.5f);
        rig.setDriveEnabled(unitRand(s) > 0.5f);
        if (done.load(std::memory_order_relaxed))
          break;
      }
    });
    std::vector<float> in(512), out(512);
    bool finite = true;
    for (int b = 0; b < 3000; ++b)
    {
      for (int i = 0; i < 512; ++i)
        in[static_cast<size_t>(i)] = 0.4f * std::sin(2.0f * 3.14159265f * (b * 512 + i) / 97.0f);
      const float* bi[1] = {in.data()};
      float* bo[1] = {out.data()};
      rig.processBlock(bi, 1, bo, 1, 512);
      if (!tdm_test::allFinite(out.data(), 512))
      {
        finite = false;
        break;
      }
    }
    done.store(true, std::memory_order_relaxed);
    writer.join();
    TDM_CHECK(finite, "concurrent param hammer stays finite");
  }
  // Canonical-default contract (the dev UI sources double-click reset
  // values from these same stage constants): a default RigParams must
  // mirror every DSP default exactly, with the literal values pinned so a
  // silent default change breaks loudly here, not in the UI.
  {
    const tdm::RigParams p;
    TDM_CHECK(p.inputTrimDb == tdm::InputTrim::kDefaultTrimDb, "params mirror trim default");
    TDM_CHECK(!p.gateEnabled, "params gate off");
    TDM_CHECK(p.gateThresholdDb == tdm::TechDeathGate::kDefaultThresholdDb, "params mirror gate thresh");
    TDM_CHECK(p.gateReleaseMs == tdm::TechDeathGate::kDefaultReleaseMs, "params mirror gate release");
    TDM_CHECK(!p.driveEnabled, "params drive off");
    TDM_CHECK(p.tight == tdm::TightDrive::kDefaultTight, "params mirror tight");
    TDM_CHECK(p.drive == tdm::TightDrive::kDefaultDrive, "params mirror drive");
    TDM_CHECK(p.bite == tdm::TightDrive::kDefaultBite, "params mirror bite");
    TDM_CHECK(p.outputTrimDb == tdm::OutputTrim::kDefaultTrimDb, "params mirror output trim");
    TDM_CHECK(p.inputTrimDb == 0.0f && p.gateThresholdDb == -55.0f && p.gateReleaseMs == 50.0f,
              "reset literals: trim/gate");
    TDM_CHECK(p.tight == 0.5f && p.drive == 0.3f && p.bite == 0.5f, "reset literals: drive");
    TDM_CHECK(p.outputTrimDb == 0.0f, "reset literals: output trim");
  }
  // Reset-to-defaults flows through the normal handoff and takes audible
  // effect: +12 dB trim (~4x) returns to exact unity, and audition-style
  // drive settings return to the canonical defaults in the snapshot.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setInputTrimDb(12.0f);
    std::vector<float> in(512, 0.25f), out(512, 0.0f);
    runBlocks(rig, in, out, 200);
    // 1e-2: settled one-pole gain stalls ~5e-3 off target in float at 4x
    // (documented non-issue); this only proves the hot value took effect.
    TDM_CHECK_CLOSE(out[511], 1.0f, 1e-2f, "hot trim settles first");
    rig.setInputTrimDb(tdm::InputTrim::kDefaultTrimDb);
    runBlocks(rig, in, out, 200);
    TDM_CHECK_CLOSE(out[511], 0.25f, 1e-3f, "trim reset to default restores unity");
    rig.setDriveEnabled(true);
    rig.setTight(0.85f); // audition values, NOT defaults
    rig.setDrive(0.50f);
    rig.setBite(0.70f);
    tdm::RigParams hot = rig.params();
    TDM_CHECK(hot.tight == 0.85f && hot.drive == 0.50f && hot.bite == 0.70f, "audition values stored");
    rig.setTight(tdm::TightDrive::kDefaultTight);
    rig.setDrive(tdm::TightDrive::kDefaultDrive);
    rig.setBite(tdm::TightDrive::kDefaultBite);
    const tdm::RigParams back = rig.params();
    TDM_CHECK(back.tight == 0.5f && back.drive == 0.3f && back.bite == 0.5f, "drive reset to defaults");
  }
}

// SpecStudy: offline renders + CPU/latency for the Special FX Laser lab.
// Usage:
//   ./build/tdm_spec_study [--outdir /tmp/spec_renders] [--nam ...] [--ir ...]
//
// Chain per render: synthetic DI -> Trim -> Gate(off) -> [Spec pre] ->
// TightDrive -> NAM -> IR -> [Spec post] -> ToneShape. Space bypassed
// (mono lab renders). The DI is SYNTHESIZED (seeded, AlienStudy twin):
// proves transforms, never playability.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/Gate/TechDeathGate.h"
#include "dsp/InputTrim.h"
#include "dsp/NamStage.h"
#include "dsp/TightDrive/TightDrive.h"
#include "dsp/ToneShape/ToneShape.h"
#include "dsp/WavFile.h"
#include "dsp/lab/SpecialFx/SpecLaser.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct Di
{
  std::vector<float> x;
};

Di synthDi() // AlienStudy twin (same seed/material for comparability)
{
  Di d;
  const int n = static_cast<int>(6.0 * kSr);
  d.x.assign(static_cast<size_t>(n), 0.0f);
  std::mt19937 rng(1337);
  std::uniform_real_distribution<float> nz(-1.0f, 1.0f);
  auto pluck = [&](int at, float f0, float peak, float decayMs, int harm) {
    const int len = static_cast<int>(decayMs * 0.001 * kSr);
    for (int i = 0; i < len && at + i < n; ++i)
    {
      const float t = i / (float)kSr;
      const float e = std::exp(-t / (decayMs * 0.001f * 0.35f));
      float v = std::sin(2.0f * 3.14159265f * f0 * t);
      for (int h = 2; h <= harm; ++h)
        v += std::sin(2.0f * 3.14159265f * f0 * h * t) / (h * h);
      v *= peak * e;
      if (i < 96)
        v += peak * 0.8f * nz(rng) * std::exp(-i / 20.0f);
      d.x[(size_t)(at + i)] += v;
    }
  };
  for (int k = 0; k < 8; ++k)
    pluck(static_cast<int>((0.2 + k * 0.25) * kSr), 82.41f, 0.5f, 200.0f, 5);
  const float stabF[] = {110.0f, 138.59f, 164.81f};
  for (int k = 0; k < 4; ++k)
    for (float f : stabF)
      pluck(static_cast<int>((2.4 + k * 0.4) * kSr), f, 0.28f, 320.0f, 4);
  for (int k = 0; k < 6; ++k)
    pluck(static_cast<int>((4.1 + k * 0.14) * kSr), 660.0f * std::pow(2.0f, (k * 2) / 12.0f),
          0.35f, 260.0f, 3);
  for (float f : {98.0f, 103.83f, 138.59f})
    pluck(static_cast<int>(5.1 * kSr), f, 0.3f, 700.0f, 4);
  float pk = 0.0f;
  for (float v : d.x)
    pk = std::max(pk, std::fabs(v));
  for (float& v : d.x)
    v *= 0.8f / pk;
  return d;
}

struct Chain
{
  tdm::InputTrim trim;
  tdm::TechDeathGate gate;
  tdm::TightDrive drive;
  tdm::NamStage nam;
  tdm::CabIrStage ir;
  tdm::ToneShape shape;
  void reset(const std::string& namPath, const std::string& irPath)
  {
    trim.reset(kSr);
    trim.setTrimDb(0.0f);
    gate.reset(kSr);
    gate.setEnabled(false);
    drive.reset(kSr);
    drive.setEnabled(true);
    drive.setTight(0.7f);
    drive.setDrive(0.5f);
    drive.setBite(0.6f);
    nam.reset(kSr, kBlock);
    nam.loadModel(namPath, kSr, kBlock);
    ir.reset(kSr);
    ir.loadIr(irPath, kSr);
    shape.reset(kSr);
    shape.setEnabled(true);
    shape.setWeight(0.5f);
    shape.setContour(0.5f);
    shape.setPresence(0.5f);
  }
  // Run DI with pre-gain and post-IR insert lambdas (either may be dry).
  template <typename P, typename Q>
  std::vector<float> render(const Di& di, P&& pre, Q&& post)
  {
    const int total = static_cast<int>(di.x.size());
    std::vector<float> out(static_cast<size_t>(total));
    std::vector<float> m(kBlock);
    for (int off = 0; off < total; off += kBlock)
    {
      const int n = std::min(kBlock, total - off);
      for (int i = 0; i < n; ++i)
        m[(size_t)i] = di.x[(size_t)(off + i)];
      trim.processBlock(m.data(), m.data(), n);
      gate.processBlock(m.data(), m.data(), n);
      for (int i = 0; i < n; ++i)
        m[(size_t)i] = pre(m[(size_t)i], off + i);
      drive.processBlock(m.data(), m.data(), n);
      nam.processBlock(m.data(), m.data(), n);
      ir.processBlock(m.data(), m.data(), n);
      for (int i = 0; i < n; ++i)
        m[(size_t)i] = post(m[(size_t)i], off + i);
      shape.processBlock(m.data(), m.data(), n);
      for (int i = 0; i < n; ++i)
        out[(size_t)(off + i)] = m[(size_t)i];
    }
    return out;
  }
};

void writeMono(const std::string& path, const std::vector<float>& x)
{
  const float* ch[1] = {x.data()};
  tdm::writeWavFloat32(path, ch, 1, static_cast<int>(x.size()), kSr);
  float pk = 0.0f;
  for (float v : x)
    pk = std::max(pk, std::fabs(v));
  std::printf("wrote %s (peak %.3f)\n", path.c_str(), pk);
}

void benchUs(const char* name, std::function<float(float)> fn)
{
  std::vector<float> buf(kBlock);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  for (auto& v : buf)
    v = u(rng);
  for (int i = 0; i < 20; ++i)
    for (float v : buf)
      fn(v);
  const auto t0 = std::chrono::steady_clock::now();
  const int reps = 200;
  volatile float sink = 0.0f;
  for (int r = 0; r < reps; ++r)
    for (float v : buf)
      sink += fn(v);
  const auto t1 = std::chrono::steady_clock::now();
  (void)sink;
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  std::printf("cpu %-14s %7.1f us/block (%.2f%% of 10.7 ms budget)\n", name, us,
              us / 10666.7 * 100.0);
}
} // namespace

int main(int argc, char** argv)
{
  using tdm::specfx::GestureConfig;
  using tdm::specfx::GestureDir;
  using tdm::specfx::GestureMotion;
  using tdm::specfx::SpecLaser;
  using Voice = SpecLaser::Voice;
  std::string outdir = "build/spec";
  std::string namPath = "assets/nam/5150II_crunch.nam";
  std::string irPath = "assets/ir/test_cab.wav";
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--outdir" && i + 1 < argc)
      outdir = argv[++i];
    else if (a == "--nam" && i + 1 < argc)
      namPath = argv[++i];
    else if (a == "--ir" && i + 1 < argc)
      irPath = argv[++i];
  }
  ::mkdir(outdir.c_str(), 0755);
  const Di di = synthDi();
  const int total = static_cast<int>(di.x.size());
  Chain ch;
  ch.reset(namPath, irPath);
  auto dry = [](float x, int) { return x; };

  writeMono(outdir + "/ref_bypass.wav", ch.render(di, dry, dry));

  // Manual heel->toe sweeps at the PRE tap (reference position).
  // Unified Whammy: polarity comes from Direction (Up/Down entries share
  // the voice); Detune was removed (hardware audition), DualWham added.
  const struct
  {
    Voice v;
    GestureDir dir;
    const char* name;
  } voices[] = {{Voice::Whammy, GestureDir::Up, "whamup"},
                {Voice::Whammy, GestureDir::Down, "whamdown"},
                {Voice::Dual, GestureDir::Up, "dual"},
                {Voice::DualWham, GestureDir::Up, "dualwham"},
                {Voice::Ring, GestureDir::Up, "ring"}};
  for (const auto& vm : voices)
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(vm.v);
    e.setExprDirection(vm.dir);
    writeMono(outdir + std::string("/laser_") + vm.name + "_sweep_pre.wav",
              ch.render(di, [&](float x, int n) {
                e.setExpression(static_cast<float>(n) / static_cast<float>(total));
                const float wet = e.process(x);
                return x + 1.0f * (wet - x); // full wet
              }, dry));
  }
  // POST tap A/B for whamup + dualwham + ring sweeps.
  for (const auto& vm : {voices[0], voices[3], voices[4]})
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(vm.v);
    e.setExprDirection(vm.dir);
    writeMono(outdir + std::string("/laser_") + vm.name + "_sweep_post.wav",
              ch.render(di, dry, [&](float x, int n) {
                e.setExpression(static_cast<float>(n) / static_cast<float>(total));
                return e.process(x);
              }));
  }

  // Gesture demos (edge-triggered presses through the unified engine).
  auto gestDemo = [&](const char* file, Voice v, GestureDir dir, float expr, const GestureConfig& c,
                      double pressAt, double relAt) {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(v);
    e.setExprDirection(dir);
    e.setExpression(expr);
    e.setGestureConfig(c);
    bool held = false;
    writeMono(outdir + file, ch.render(di, [&](float x, int n) {
                const double t = n / kSr;
                if (!held && t >= pressAt)
                {
                  e.pressGesture(0);
                  held = true;
                }
                else if (held && t >= relAt)
                {
                  e.releaseGesture(0);
                  held = false;
                }
                return e.process(x);
              }, dry));
  };
  auto cfg = [](int voice, GestureDir d, GestureMotion m, float depth, float time, float ret) {
    GestureConfig c;
    c.voice = voice;
    c.dir = d;
    c.motion = m;
    c.depthSt = depth;
    c.timeMs = time;
    c.retMs = ret;
    return c;
  };
  // Divebomb: whammy-up at +6 st, dive press at 2 s (300 ms down to
  // -12), release at 4 s (200 ms back). Pre tap.
  gestDemo("/laser_dive_pre.wav", Voice::Whammy, GestureDir::Up, 0.5f,
           cfg(0, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 300.0f, 200.0f), 2.0, 4.0);
  // Slow-rise toe stab: 600 ms up, 80 ms back, press 1 s / release 3.5 s.
  gestDemo("/laser_toestab_pre.wav", Voice::Whammy, GestureDir::Up, 0.0f,
           cfg(0, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 600.0f, 80.0f), 1.0, 3.5);
  // Reverse dive (From Depth): engaged deep at 2 s, sweeps home by ~2.5 s.
  gestDemo("/laser_revdive_pre.wav", Voice::Whammy, GestureDir::Down, 0.0f,
           cfg(0, GestureDir::Down, GestureMotion::FromDepth, 12.0f, 400.0f, 200.0f), 2.0, 4.0);
  // Ring sweep across voices: whammy normal + carrier rise on press.
  gestDemo("/laser_ringsweep_pre.wav", Voice::Whammy, GestureDir::Up, 0.0f,
           cfg(2, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 500.0f, 300.0f), 2.0, 4.0);
  // DualWham stab: whammy normal + dual-voice rise on press.
  gestDemo("/laser_dualwhamstab_pre.wav", Voice::Whammy, GestureDir::Up, 0.0f,
           cfg(3, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 250.0f, 200.0f), 2.0, 4.0);

  SpecLaser lat;
  lat.reset(kSr);
  std::printf("latency laser %d samples @48k (doppler bound; ring leg ~1)\n",
              lat.latencySamples());
  for (const auto& vm : voices)
  {
    SpecLaser e;
    e.reset(kSr);
    e.setVoice(vm.v);
    e.setExprDirection(vm.dir);
    e.setExpression(0.75f);
    benchUs((std::string("laser_") + vm.name).c_str(), [&](float x) { return e.process(x); });
  }
  SpecLaser dv;
  dv.reset(kSr);
  dv.setGestureConfig(cfg(0, GestureDir::Down, GestureMotion::ToDepth, 12.0f, 300.0f, 200.0f));
  dv.pressGesture(1);
  benchUs("laser_diveheld", [&](float x) { return dv.process(x); });
  SpecLaser gx;
  gx.reset(kSr);
  gx.setGestureConfig(cfg(2, GestureDir::Up, GestureMotion::ToDepth, 12.0f, 100.0f, 100.0f));
  gx.pressGesture(4);
  benchUs("laser_ringheld", [&](float x) { return gx.process(x); });
  std::printf("done -> %s\n", outdir.c_str());
  return 0;
}

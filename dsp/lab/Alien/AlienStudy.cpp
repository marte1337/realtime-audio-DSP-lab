// AlienStudy: offline renders + measurements for the Alien FX lab.
//
// Usage:
//   ./build/tdm_alien_study [--outdir build/alien] [--nam assets/nam/5150II_crunch.nam]
//       [--ir assets/ir/test_cab.wav]
//
// Chain per render: synthetic DI -> Trim -> Gate(off) -> TightDrive ->
// NAM -> IR -> [Alien insert, post-IR mono] -> ToneShape -> mono WAV.
// Space is bypassed (mono lab renders). The DI is SYNTHESIZED (seeded,
// deterministic): chugs, power-chord stabs, a lead line, a dissonant
// hold. It exercises the transforms repeatably but proves nothing about
// real-guitar playability - hardware audition decides that.
//
// Also prints per-effect latency + rough CPU (us per 512-block, -O2).

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
#include "dsp/lab/Alien/AlienLaser.h"
#include "dsp/lab/Alien/AlienRing.h"
#include "dsp/lab/Alien/AlienShift.h"
#include "dsp/lab/Alien/AlienWarp.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct Di
{
  std::vector<float> x;
  std::vector<int> hits; // chug/stab onsets (laser trigger schedule)
};

// Deterministic guitar-ish DI: chugs, stabs, lead, dissonant hold.
Di synthDi()
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
      if (i < 96) // pick transient
        v += peak * 0.8f * nz(rng) * std::exp(-i / 20.0f);
      d.x[(size_t)(at + i)] += v;
    }
  };
  for (int k = 0; k < 8; ++k) // chugs: 8ths at 120 BPM, low E
  {
    const int at = static_cast<int>((0.2 + k * 0.25) * kSr);
    d.hits.push_back(at);
    pluck(at, 82.41f, 0.5f, 200.0f, 5);
  }
  const float stabF[] = {110.0f, 138.59f, 164.81f};
  for (int k = 0; k < 4; ++k) // power stabs
  {
    const int at = static_cast<int>((2.4 + k * 0.4) * kSr);
    d.hits.push_back(at);
    for (float f : stabF)
      pluck(at, f, 0.28f, 320.0f, 4);
  }
  for (int k = 0; k < 6; ++k) // lead line 660..990 with vibrato-ish drift
  {
    const int at = static_cast<int>((4.1 + k * 0.14) * kSr);
    const float f = 660.0f * std::pow(2.0f, (k * 2) / 12.0f);
    pluck(at, f, 0.35f, 260.0f, 3);
  }
  { // dissonant hold: tritone cluster
    const int at = static_cast<int>(5.1 * kSr);
    for (float f : {98.0f, 103.83f, 138.59f})
      pluck(at, f, 0.3f, 700.0f, 4);
  }
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
  // Run DI through the chain with an alien insert lambda post-IR.
  template <typename F> std::vector<float> render(const Di& di, F&& alien)
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
      drive.processBlock(m.data(), m.data(), n);
      nam.processBlock(m.data(), m.data(), n);
      ir.processBlock(m.data(), m.data(), n);
      for (int i = 0; i < n; ++i)
        m[(size_t)i] = alien(m[(size_t)i], off + i);
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

double benchUs(const char* name, std::function<float(float)> fn)
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
  const double us =
      std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  std::printf("cpu %-12s %7.1f us/block (%.2f%% of 10.7 ms budget)\n", name, us,
              us / 10666.7 * 100.0);
  return us;
}
} // namespace

int main(int argc, char** argv)
{
  std::string outdir = "build/alien";
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
  Chain ch;
  ch.reset(namPath, irPath);

  // Reference: bypass (alien = wire).
  writeMono(outdir + "/ref_bypass.wav", ch.render(di, [](float x, int) { return x; }));

  // A. Frequency shifter: subtle / industrial / extreme.
  {
    tdm::alien::AlienShift fx;
    fx.reset(kSr);
    fx.setShiftHz(30.0f);
    fx.setMix(0.6f);
    writeMono(outdir + "/shift_subtle_+30.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienShift fx;
    fx.reset(kSr);
    fx.setShiftHz(150.0f);
    fx.setMix(1.0f);
    writeMono(outdir + "/shift_industrial_+150.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienShift fx;
    fx.reset(kSr);
    fx.setShiftHz(-600.0f);
    fx.setMix(1.0f);
    writeMono(outdir + "/shift_extreme_-600.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }

  // B. Laser: zap on every hit, long up-sweep, cyclic wobble.
  {
    tdm::alien::AlienLaser fx;
    fx.reset(kSr);
    fx.setDirection(tdm::alien::AlienLaser::Direction::Down);
    fx.setSweepMs(120.0f);
    fx.setDepthMs(12.0f);
    fx.setFeedback(0.25f);
    fx.setMix(0.8f);
    size_t hi = 0;
    writeMono(outdir + "/laser_zap_down.wav", ch.render(di, [&](float x, int n) {
      while (hi < di.hits.size() && di.hits[hi] <= n)
      {
        fx.trigger();
        ++hi;
      }
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienLaser fx;
    fx.reset(kSr);
    fx.setDirection(tdm::alien::AlienLaser::Direction::Up);
    fx.setSweepMs(250.0f);
    fx.setDepthMs(30.0f);
    fx.setFeedback(0.4f);
    fx.setReso(4.0f);
    fx.setMix(0.8f);
    size_t hi = 0;
    writeMono(outdir + "/laser_rise_up.wav", ch.render(di, [&](float x, int n) {
      while (hi < di.hits.size() && di.hits[hi] <= n)
      {
        fx.trigger();
        ++hi;
      }
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienLaser fx;
    fx.reset(kSr);
    fx.setDirection(tdm::alien::AlienLaser::Direction::Cycle);
    fx.setSweepMs(250.0f);
    fx.setDepthMs(20.0f);
    fx.setFeedback(0.3f);
    fx.setMix(0.7f);
    writeMono(outdir + "/laser_cycle.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }

  // C. Warp: mild evolve, escalating dissonance, pitch up/down.
  {
    tdm::alien::AlienWarp fx;
    fx.reset(kSr);
    fx.setMode(tdm::alien::AlienWarp::Mode::FreqWarp);
    fx.setTimeMs(220.0f);
    fx.setFeedback(0.5f);
    fx.setShiftHz(50.0f);
    fx.setMix(0.6f);
    writeMono(outdir + "/warp_freq_mild.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienWarp fx;
    fx.reset(kSr);
    fx.setMode(tdm::alien::AlienWarp::Mode::FreqWarp);
    fx.setTimeMs(180.0f);
    fx.setFeedback(0.65f);
    fx.setShiftHz(100.0f);
    fx.setMix(0.7f);
    writeMono(outdir + "/warp_freq_alien.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienWarp fx;
    fx.reset(kSr);
    fx.setMode(tdm::alien::AlienWarp::Mode::PitchWarp);
    fx.setTimeMs(200.0f);
    fx.setFeedback(0.6f);
    fx.setSemitones(3.0f);
    fx.setMix(0.7f);
    writeMono(outdir + "/warp_pitch_asc.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }
  {
    tdm::alien::AlienWarp fx;
    fx.reset(kSr);
    fx.setMode(tdm::alien::AlienWarp::Mode::PitchWarp);
    fx.setTimeMs(200.0f);
    fx.setFeedback(0.6f);
    fx.setSemitones(-4.0f);
    fx.setMix(0.7f);
    writeMono(outdir + "/warp_pitch_desc.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }

  // D. Ring mod comparison.
  {
    tdm::alien::AlienRing fx;
    fx.reset(kSr);
    fx.setCarrierHz(150.0f);
    fx.setMix(0.8f);
    writeMono(outdir + "/ring_150.wav", ch.render(di, [&](float x, int) {
      return fx.process(x);
    }));
  }

  // Latency + CPU.
  {
    tdm::alien::AlienShift s;
    s.reset(kSr);
    tdm::alien::AlienLaser l;
    l.reset(kSr);
    tdm::alien::AlienWarp w;
    w.reset(kSr);
    std::printf("latency shift %d / laser %d / warp %d samples @48k\n", s.latencySamples(),
                l.latencySamples(), w.latencySamples());
    benchUs("shift", [&](float x) { return s.process(x); });
    benchUs("laser", [&](float x) { return l.process(x); });
    benchUs("warp", [&](float x) { return w.process(x); });
  }
  std::printf("done -> %s\n", outdir.c_str());
  return 0;
}

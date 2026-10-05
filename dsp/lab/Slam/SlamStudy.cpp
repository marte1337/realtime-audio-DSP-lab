// tdm_slam_study: SLAM architectural study harness (LAB ONLY).
//
// Wires the production STAGE classes directly (TechDeathRig itself is never
// touched) with SLAM candidates inserted at pre-NAM / post-NAM / post-IR /
// impact positions. Produces renders + metrics for the default context sweep
// and (via flags) amp/drive/transpose interaction runs.
//
// Usage:
//   tdm_slam_study --di build/slam_di.wav --map build/slam_di_map.txt \
//       --outdir build/slam [--nam assets/nam/5150II_crunch.nam] \
//       [--ir assets/ir/test_cab.wav] [--nodrive] [--transpose -2] \
//       [--only dry,a1,b] [--norender]
//
// Chain (mono, 1024-frame blocks; Space bypassed, trims 0 dB, gate off):
//   DI -> [A1/Dpre] -> Drive -> [A2] -> NAM -> [B] -> IR -> [C/Dpost] ->
//   ToneShape(neutral) -> Space(dry) -> fold -> out
// The dry path replicates tdm_render bit-exactly (verified with cmp; the
// only deliberate difference is transpose-ON: the study parks the engine in
// steady-wet from sample 0 instead of replaying the rig's engage ramp, so
// transpose runs are self-consistent but differ from tdm_render in the
// first ~ramp ms).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/Gate/TechDeathGate.h"
#include "dsp/InputTrim.h"
#include "dsp/NamStage.h"
#include "dsp/OutputTrim.h"
#include "dsp/Pitch/GuitarTranspose.h"
#include "dsp/Space/SpaceProcessor.h"
#include "dsp/TightDrive/TightDrive.h"
#include "dsp/ToneShape/ToneShape.h"
#include "dsp/WavFile.h"
#include "dsp/lab/Slam/SlamCandidates.h"

namespace
{
constexpr int kBlock = 1024;

double dbOf(double v)
{
  return 20.0 * std::log10(v + 1e-12);
}

// --- DI map ---
struct Section
{
  std::string name;
  double start = 0.0, end = 0.0;
};
struct Event
{
  std::string section;
  double time = 0.0;
};
struct DiMap
{
  std::vector<Section> sections;
  std::vector<Event> events;
};
DiMap loadMap(const std::string& path)
{
  DiMap m;
  std::ifstream f(path);
  std::string kind, name;
  double a, b;
  while (f >> kind)
  {
    if (kind == "section" && (f >> name >> a >> b))
      m.sections.push_back({name, a, b});
    else if (kind == "event" && (f >> name >> a))
      m.events.push_back({name, a});
    else
    {
      std::string skip;
      std::getline(f, skip);
    }
  }
  return m;
}

// --- Metrics ---
struct BandBank
{
  // sub: LP65 | low: HP60+LP160 | lomid: HP150+LP400 | mid: HP380+LP2000 |
  // pres: HP1900. 2nd-order, Q 0.7. Offline only (allocates freely).
  void reset(double sr)
  {
    for (auto& f : lps_)
      f.reset();
    for (auto& f : hps_)
      f.reset();
    lps_[0].setLowpass(sr, 65.0f, 0.7f);
    hps_[0].setHighpass(sr, 60.0f, 0.7f);
    lps_[1].setLowpass(sr, 160.0f, 0.7f);
    hps_[1].setHighpass(sr, 150.0f, 0.7f);
    lps_[2].setLowpass(sr, 400.0f, 0.7f);
    hps_[2].setHighpass(sr, 380.0f, 0.7f);
    lps_[3].setLowpass(sr, 2000.0f, 0.7f);
    hps_[3].setHighpass(sr, 1900.0f, 0.7f);
  }
  // Band RMS over [a, b) (sample indices). Runs 7 filter passes; offline.
  void bandRms(const float* x, int n, int a, int b, double outDb[5])
  {
    if (b > n)
      b = n;
    if (a < 0)
      a = 0;
    double acc[5] = {0, 0, 0, 0, 0};
    // Fresh states per call (calls are windowed slices of one render).
    reset(sr_);
    for (int i = 0; i < b; ++i)
    {
      const float s = x[i];
      const float sub = lps_[0].process(s);
      const float low = lps_[1].process(hps_[0].process(s));
      const float lomid = lps_[2].process(hps_[1].process(s));
      const float mid = lps_[3].process(hps_[2].process(s));
      const float pres = hps_[3].process(s);
      if (i >= a)
      {
        acc[0] += (double)sub * sub;
        acc[1] += (double)low * low;
        acc[2] += (double)lomid * lomid;
        acc[3] += (double)mid * mid;
        acc[4] += (double)pres * pres;
      }
    }
    const int m = (b > a) ? (b - a) : 1;
    for (int k = 0; k < 5; ++k)
      outDb[k] = dbOf(std::sqrt(acc[k] / m));
  }
  void setRate(double sr) { sr_ = sr; }

  double sr_ = 48000.0;
  tdm::lab::SlamBiquad lps_[4], hps_[4];
};

double windowRms(const std::vector<float>& x, int a, int b)
{
  if (b > (int)x.size())
    b = (int)x.size();
  if (a < 0)
    a = 0;
  if (b <= a)
    return 0.0;
  double acc = 0.0;
  for (int i = a; i < b; ++i)
    acc += (double)x[(size_t)i] * x[(size_t)i];
  return std::sqrt(acc / (b - a));
}
double windowPeak(const std::vector<float>& x, int a, int b)
{
  if (b > (int)x.size())
    b = (int)x.size();
  if (a < 0)
    a = 0;
  double pk = 0.0;
  for (int i = a; i < b; ++i)
    pk = std::max(pk, (double)std::fabs(x[(size_t)i]));
  return pk;
}

struct Metrics
{
  double peakDb = -200.0, rmsDb = -200.0, crestDb = 0.0;
  int clips = 0;
  double bandDb[5] = {-200, -200, -200, -200, -200};
  double sparseRmsDb = -200.0; // S1 section RMS (level-match anchor)
  double evtRmsDb = -200.0;    // mean event-window RMS (sparse+breakdown)
  double evtCrestDb = 0.0;
  double recovRmsDb = -200.0; // mean [80,160] ms-post-onset RMS
  double gapRmsDb = -200.0;   // mean pre-onset floor RMS
  double presSparseDb = -200.0;
};

Metrics analyze(const std::vector<float>& x, double sr, const DiMap& map)
{
  Metrics m;
  const int n = (int)x.size();
  m.peakDb = dbOf(windowPeak(x, 0, n));
  m.rmsDb = dbOf(windowRms(x, 0, n));
  m.crestDb = m.peakDb - m.rmsDb;
  for (float v : x)
    if (std::fabs(v) >= 0.99f)
      ++m.clips;
  BandBank bank;
  bank.setRate(sr);
  bank.bandRms(x.data(), n, 0, n, m.bandDb);
  bool foundSparse = false;
  for (const auto& s : map.sections)
    if (s.name == "sparse")
    {
      const int a = (int)(s.start * sr), b = (int)(s.end * sr);
      m.sparseRmsDb = dbOf(windowRms(x, a, b));
      double bb[5];
      bank.bandRms(x.data(), n, a, b, bb);
      m.presSparseDb = bb[4];
      foundSparse = true;
    }
  if (!foundSparse) // no map (e.g. labpitch second opinion): whole-file anchor
  {
    m.sparseRmsDb = m.rmsDb;
    m.presSparseDb = m.bandDb[4];
  }
  // Event stats over sparse + breakdown hits.
  double eRms = 0.0, eCrest = 0.0, eRec = 0.0, eGap = 0.0;
  int cnt = 0;
  for (const auto& e : map.events)
  {
    if (e.section != "sparse" && e.section != "breakdown")
      continue;
    const int t = (int)(e.time * sr);
    const double r = windowRms(x, t, t + (int)(0.250 * sr));
    const double p = windowPeak(x, t, t + (int)(0.250 * sr));
    eRms += r * r;
    eCrest += dbOf(p) - dbOf(r);
    const double rec = windowRms(x, t + (int)(0.080 * sr), t + (int)(0.160 * sr));
    eRec += rec * rec;
    const double gap = windowRms(x, t - (int)(0.150 * sr), t - (int)(0.030 * sr));
    eGap += gap * gap;
    ++cnt;
  }
  if (cnt > 0)
  {
    m.evtRmsDb = dbOf(std::sqrt(eRms / cnt));
    m.evtCrestDb = eCrest / cnt;
    m.recovRmsDb = dbOf(std::sqrt(eRec / cnt));
    m.gapRmsDb = dbOf(std::sqrt(eGap / cnt));
  }
  return m;
}

// --- Study chain: production stages wired by hand (rig untouched) ---
enum class SlamMode
{
  Dry,
  A1, // SlamPre before TightDrive
  A2, // SlamPre after TightDrive, before NAM
  B,  // SlamPostNam between NAM and IR
  C,  // SlamPostIr after IR
  Dpre, // SlamImpact before TightDrive (burst drives the amp)
  Dpost, // SlamImpact after IR (burst on finished tone)
  Dhyb, // detector @ pre-drive tap, scheduled burst after IR (split tap)
  Ehyb // detector @ pre-drive tap, scheduled GATED branch after IR
};

struct Context
{
  std::string namPath = "assets/nam/5150II_crunch.nam";
  std::string irPath = "assets/ir/test_cab.wav";
  bool drive = true;
  double tight = 0.5, driveAmt = 0.3, bite = 0.5;
  bool transpose = false;
  float transposeSt = 0.0f;
};

struct SlamConfig
{
  std::string name;
  SlamMode mode = SlamMode::Dry;
  float bandHz = 140.0f;
  float amount = 1.0f;
  float decayMs = 130.0f; // D only
  float sens = 0.5f;      // D/E trigger
  float f0 = 70.0f, f1 = 48.0f; // D only
  float base = 0.15f;     // E only: continuous floor blend
  float peak = 1.2f;      // E only: fire-posed blend
  float gateDecayMs = 120.0f; // E only
  bool render = true; // write WAV (metrics always computed)
};

struct RenderResult
{
  std::vector<float> audio;
  Metrics met;
  int64_t fires = 0;
  std::vector<std::pair<double, float>> fireTimes; // (seconds, strength), D only
  double cpuUsPerBlock = 0.0; // candidate only, standalone loop
};

class StudyChain
{
public:
  RenderResult run(const std::vector<float>& di, double sr, const DiMap& map, const Context& ctx,
                   const SlamConfig& cfg)
  {
    RenderResult r;
    // Reset everything (rig order: stages, then loads).
    trim_.reset(sr);
    trim_.setTrimDb(0.0f);
    gate_.reset(sr);
    gate_.setEnabled(false);
    if (ctx.transpose)
    {
      gt_.setShiftSt(ctx.transposeSt == 0.0f ? -2.0f : ctx.transposeSt);
      gt_.setEnabled(true);
      gt_.reset(sr);
    }
    drive_.reset(sr);
    drive_.setEnabled(ctx.drive);
    drive_.setTight((float)ctx.tight);
    drive_.setDrive((float)ctx.driveAmt);
    drive_.setBite((float)ctx.bite);
    nam_.reset(sr, kBlock);
    shape_.reset(sr);
    shape_.setEnabled(true);
    shape_.setWeight(0.5f);
    shape_.setContour(0.5f);
    shape_.setPresence(0.5f);
    space_.reset(sr);
    space_.setDelayEnabled(false);
    space_.setReverbEnabled(false);
    outL_.reset(sr);
    outR_.reset(sr);
    outL_.setTrimDb(0.0f);
    outR_.setTrimDb(0.0f);
    nam_.loadModel(ctx.namPath, sr, kBlock);
    ir_.reset(sr);
    ir_.loadIr(ctx.irPath, sr);
    pre_.reset(sr);
    pre_.setBandHz(cfg.bandHz);
    pre_.setAmount(cfg.amount);
    postNam_.reset(sr);
    postNam_.setBandHz(cfg.bandHz);
    postNam_.setAmount(cfg.amount);
    postIr_.reset(sr);
    postIr_.setBandHz(cfg.bandHz);
    postIr_.setAmount(cfg.amount);
    impact_.reset(sr);
    impact_.setBurstHz(cfg.f0, cfg.f1);
    impact_.setDecayMs(cfg.decayMs);
    impact_.setSensitivity(cfg.sens);
    impact_.setAmount(cfg.amount);
    gated_.reset(sr);
    gated_.setBandHz(cfg.bandHz);
    gated_.setBase(cfg.base);
    gated_.setPeak(cfg.peak);
    gated_.setGateDecayMs(cfg.gateDecayMs);
    gated_.setSensitivity(cfg.sens);
    if (cfg.mode == SlamMode::Dhyb || cfg.mode == SlamMode::Ehyb)
    {
      // Pass 1: detect at the pre-drive tap (raw attacks, clear flux).
      tdm::lab::SlamImpact detector;
      detector.reset(sr);
      detector.setSensitivity(cfg.sens);
      std::vector<float> pre(kBlock), wet1(kBlock);
      for (int off = 0; off < (int)di.size(); off += kBlock)
      {
        const int m = std::min(kBlock, (int)di.size() - off);
        for (int i = 0; i < m; ++i)
          pre[(size_t)i] = di[(size_t)(off + i)];
        trim_.processBlock(pre.data(), pre.data(), m);
        gate_.processBlock(pre.data(), pre.data(), m);
        if (ctx.transpose)
        {
          gt_.processBlock(pre.data(), wet1.data(), m);
          for (int i = 0; i < m; ++i)
            pre[(size_t)i] = wet1[(size_t)i];
        }
        detector.processBlock(pre.data(), pre.data(), m);
      }
      // Re-reset the shared stages (pass 1 advanced their state).
      trim_.reset(sr);
      trim_.setTrimDb(0.0f);
      gate_.reset(sr);
      gate_.setEnabled(false);
      if (ctx.transpose)
      {
        gt_.setShiftSt(ctx.transposeSt == 0.0f ? -2.0f : ctx.transposeSt);
        gt_.setEnabled(true);
        gt_.reset(sr);
      }
      const int nt = detector.triggerCount();
      for (int i = 0; i < nt; ++i)
      {
        auto t = detector.triggerAt(i);
        if (cfg.mode == SlamMode::Dhyb)
          impact_.scheduleFire(t.sample, t.strength);
        else
          gated_.scheduleFire(t.sample, t.strength);
      }
      if (cfg.mode == SlamMode::Dhyb)
        impact_.setScheduled(true);
      else
        gated_.setScheduled(true);
    }

    const int total = (int)di.size();
    r.audio.assign(di.size(), 0.0f);
    std::vector<float> mono(kBlock), wet(kBlock), left(kBlock), right(kBlock);
    for (int off = 0; off < total; off += kBlock)
    {
      const int m = std::min(kBlock, total - off);
      for (int i = 0; i < m; ++i)
        mono[(size_t)i] = di[(size_t)(off + i)]; // 1-ch: acc/1 is exact
      trim_.processBlock(mono.data(), mono.data(), m);
      gate_.processBlock(mono.data(), mono.data(), m);
      if (ctx.transpose)
      {
        gt_.processBlock(mono.data(), wet.data(), m);
        for (int i = 0; i < m; ++i)
          mono[(size_t)i] = wet[(size_t)i]; // steady-wet (no engage ramp)
      }
      if (cfg.mode == SlamMode::A1)
        pre_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::Dpre)
        impact_.processBlock(mono.data(), mono.data(), m);
      drive_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::A2)
        pre_.processBlock(mono.data(), mono.data(), m);
      nam_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::B)
        postNam_.processBlock(mono.data(), mono.data(), m);
      ir_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::C)
        postIr_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::Dpost || cfg.mode == SlamMode::Dhyb)
        impact_.processBlock(mono.data(), mono.data(), m);
      if (cfg.mode == SlamMode::Ehyb)
        gated_.processBlock(mono.data(), mono.data(), m);
      shape_.processBlock(mono.data(), mono.data(), m);
      space_.processBlock(mono.data(), left.data(), right.data(), m);
      outL_.processBlock(left.data(), left.data(), m);
      outR_.processBlock(right.data(), right.data(), m);
      for (int i = 0; i < m; ++i)
        r.audio[(size_t)(off + i)] = (left[(size_t)i] + right[(size_t)i]) * 0.5f;
    }
    if (cfg.mode == SlamMode::Dpre || cfg.mode == SlamMode::Dpost || cfg.mode == SlamMode::Dhyb
        || cfg.mode == SlamMode::Ehyb)
    {
      const bool gated = (cfg.mode == SlamMode::Ehyb);
      r.fires = gated ? gated_.fireCount() : impact_.fireCount();
      const int nt = gated ? gated_.triggerCount() : impact_.triggerCount();
      for (int i = 0; i < nt; ++i)
      {
        auto t = gated ? gated_.triggerAt(i) : impact_.triggerAt(i);
        r.fireTimes.emplace_back(t.sample / sr, t.strength);
      }
    }
    r.met = analyze(r.audio, sr, map);
    r.cpuUsPerBlock = measureCpu(di, sr, cfg);
    return r;
  }

private:
  // Candidate-only CPU: standalone loop over the DI, median-of-3.
  double measureCpu(const std::vector<float>& di, double sr, const SlamConfig& cfg)
  {
    if (cfg.mode == SlamMode::Dry)
      return 0.0;
    const int total = (int)di.size();
    std::vector<float> buf(kBlock);
    double best = 1e9;
    for (int rep = 0; rep < 3; ++rep)
    {
      pre_.reset(sr);
      pre_.setBandHz(cfg.bandHz);
      pre_.setAmount(cfg.amount);
      postNam_.reset(sr);
      postNam_.setBandHz(cfg.bandHz);
      postNam_.setAmount(cfg.amount);
      postIr_.reset(sr);
      postIr_.setBandHz(cfg.bandHz);
      postIr_.setAmount(cfg.amount);
      impact_.reset(sr);
      impact_.setBurstHz(cfg.f0, cfg.f1);
      impact_.setDecayMs(cfg.decayMs);
      impact_.setSensitivity(cfg.sens);
      impact_.setAmount(cfg.amount);
      gated_.reset(sr);
      gated_.setBandHz(cfg.bandHz);
      gated_.setBase(cfg.base);
      gated_.setPeak(cfg.peak);
      gated_.setGateDecayMs(cfg.gateDecayMs);
      gated_.setSensitivity(cfg.sens);
      const auto t0 = std::chrono::steady_clock::now();
      for (int off = 0; off < total; off += kBlock)
      {
        const int m = std::min(kBlock, total - off);
        for (int i = 0; i < m; ++i)
          buf[(size_t)i] = di[(size_t)(off + i)];
        switch (cfg.mode)
        {
        case SlamMode::A1:
        case SlamMode::A2:
          pre_.processBlock(buf.data(), buf.data(), m);
          break;
        case SlamMode::B:
          postNam_.processBlock(buf.data(), buf.data(), m);
          break;
        case SlamMode::C:
          postIr_.processBlock(buf.data(), buf.data(), m);
          break;
        case SlamMode::Dpre:
        case SlamMode::Dpost:
        case SlamMode::Dhyb:
          impact_.processBlock(buf.data(), buf.data(), m);
          break;
        case SlamMode::Ehyb:
          gated_.processBlock(buf.data(), buf.data(), m);
          break;
        case SlamMode::Dry:
          break;
        }
      }
      const auto t1 = std::chrono::steady_clock::now();
      const double us =
          std::chrono::duration<double, std::micro>(t1 - t0).count() / (total / (double)kBlock);
      best = std::min(best, us);
    }
    return best;
  }

  tdm::InputTrim trim_;
  tdm::TechDeathGate gate_;
  tdm::GuitarTranspose gt_;
  tdm::TightDrive drive_;
  tdm::NamStage nam_;
  tdm::CabIrStage ir_;
  tdm::ToneShape shape_;
  tdm::SpaceProcessor space_;
  tdm::OutputTrim outL_, outR_;
  tdm::lab::SlamPre pre_;
  tdm::lab::SlamPostNam postNam_;
  tdm::lab::SlamPostIr postIr_;
  tdm::lab::SlamImpact impact_;
  tdm::lab::SlamGated gated_;
};

std::vector<SlamConfig> defaultMatrix()
{
  std::vector<SlamConfig> v;
  v.push_back({"dry", SlamMode::Dry});
  // A1: band sweep + amount sweep @140.
  for (float b : {70.0f, 120.0f, 140.0f, 200.0f, 320.0f})
  {
    SlamConfig c;
    c.name = "a1_" + std::to_string((int)b);
    c.mode = SlamMode::A1;
    c.bandHz = b;
    c.render = (b == 140.0f);
    v.push_back(c);
  }
  for (float a : {0.5f, 1.5f})
  {
    SlamConfig c;
    c.name = "a1_140_x" + std::to_string((int)(a * 10));
    c.mode = SlamMode::A1;
    c.bandHz = 140.0f;
    c.amount = a;
    c.render = false;
    v.push_back(c);
  }
  // A2: spot checks (same engine, post-drive tap).
  for (float b : {70.0f, 140.0f, 320.0f})
  {
    SlamConfig c;
    c.name = "a2_" + std::to_string((int)b);
    c.mode = SlamMode::A2;
    c.bandHz = b;
    c.render = (b == 140.0f);
    v.push_back(c);
  }
  // B.
  for (float b : {100.0f, 200.0f, 320.0f})
  {
    SlamConfig c;
    c.name = "b_" + std::to_string((int)b);
    c.mode = SlamMode::B;
    c.bandHz = b;
    c.render = (b == 200.0f);
    v.push_back(c);
  }
  for (float a : {0.5f, 1.5f})
  {
    SlamConfig c;
    c.name = "b_200_x" + std::to_string((int)(a * 10));
    c.mode = SlamMode::B;
    c.bandHz = 200.0f;
    c.amount = a;
    c.render = false;
    v.push_back(c);
  }
  // C.
  for (float b : {120.0f, 220.0f, 320.0f})
  {
    SlamConfig c;
    c.name = "c_" + std::to_string((int)b);
    c.mode = SlamMode::C;
    c.bandHz = b;
    c.render = (b == 220.0f);
    v.push_back(c);
  }
  for (float a : {0.5f, 1.5f})
  {
    SlamConfig c;
    c.name = "c_220_x" + std::to_string((int)(a * 10));
    c.mode = SlamMode::C;
    c.bandHz = 220.0f;
    c.amount = a;
    c.render = false;
    v.push_back(c);
  }
  // D at both taps + decay/sens spots.
  {
    // Pre-amp burst lives ABOVE the drive HPF (~113 Hz @ tight 0.5): a
    // 70 Hz burst is erased downstream (measured +0.01 dB in v1).
    SlamConfig c;
    c.name = "dpre";
    c.mode = SlamMode::Dpre;
    c.f0 = 160.0f;
    c.f1 = 110.0f;
    v.push_back(c);
  }
  {
    SlamConfig c;
    c.name = "dpost";
    c.mode = SlamMode::Dpost;
    v.push_back(c);
  }
  {
    // Split-tap D: pre-drive detector, post-IR burst. The architecture the
    // v1 data demands (pre detects 8/8 breakdown, post burst hits +5 dB).
    SlamConfig c;
    c.name = "dhyb";
    c.mode = SlamMode::Dhyb;
    v.push_back(c);
  }
  {
    // Split-tap E: pre-drive detector, gated post-IR sat branch.
    SlamConfig c;
    c.name = "ehyb";
    c.mode = SlamMode::Ehyb;
    c.bandHz = 220.0f;
    v.push_back(c);
  }
  {
    // Pure-momentary E: no continuous floor (base 0).
    SlamConfig c;
    c.name = "ehyb_base0";
    c.mode = SlamMode::Ehyb;
    c.bandHz = 220.0f;
    c.base = 0.0f;
    c.render = false;
    v.push_back(c);
  }
  for (float dcy : {80.0f, 200.0f})
  {
    SlamConfig c;
    c.name = "dpost_d" + std::to_string((int)dcy);
    c.mode = SlamMode::Dpost;
    c.decayMs = dcy;
    c.render = false;
    v.push_back(c);
  }
  for (float s : {0.25f, 0.75f})
  {
    SlamConfig c;
    c.name = "dpost_s" + std::to_string((int)(s * 100));
    c.mode = SlamMode::Dpost;
    c.sens = s;
    c.render = false;
    v.push_back(c);
  }
  return v;
}

void writeWav(const std::string& path, const std::vector<float>& x, double sr)
{
  const float* ch[1] = {x.data()};
  tdm::writeWavFloat32(path, ch, 1, (int)x.size(), sr);
}

bool wantName(const std::vector<std::string>& only, const std::string& name)
{
  if (only.empty())
    return true;
  for (const auto& o : only)
    if (name.compare(0, o.size(), o) == 0)
      return true;
  return false;
}
} // namespace

int main(int argc, char** argv)
{
  std::string diPath, mapPath, outdir = "build/slam";
  Context ctx;
  std::vector<std::string> only;
  bool norender = false, nomatch = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto need = [&](const char* flag, std::string& dst) {
      if (i + 1 >= argc)
      {
        std::printf("missing value for %s\n", flag);
        std::exit(2);
      }
      dst = argv[++i];
    };
    if (a == "--di")
      need("--di", diPath);
    else if (a == "--map")
      need("--map", mapPath);
    else if (a == "--outdir")
      need("--outdir", outdir);
    else if (a == "--nam")
      need("--nam", ctx.namPath);
    else if (a == "--ir")
      need("--ir", ctx.irPath);
    else if (a == "--nodrive")
      ctx.drive = false;
    else if (a == "--transpose")
    {
      std::string s;
      need("--transpose", s);
      ctx.transpose = true;
      ctx.transposeSt = std::stof(s);
    }
    else if (a == "--only")
    {
      std::string s;
      need("--only", s);
      std::stringstream ss(s);
      std::string tok;
      while (std::getline(ss, tok, ','))
        only.push_back(tok);
    }
    else if (a == "--norender")
      norender = true;
    else if (a == "--nomatch")
      nomatch = true;
    else
    {
      std::printf("unknown arg %s\n", a.c_str());
      return 2;
    }
  }
  if (diPath.empty())
  {
    std::printf("usage: tdm_slam_study --di di.wav [--map map.txt] [--outdir dir] [--nam x.nam] "
                "[--ir x.wav] [--nodrive] [--transpose ST] [--only a,b] [--norender]\n");
    return 2;
  }

  try
  {
    tdm::MonoWav di = tdm::loadWavMono(diPath);
    DiMap map;
    if (!mapPath.empty())
      map = loadMap(mapPath);
    std::string mk = "mkdir -p " + outdir;
    if (std::system(mk.c_str()) != 0)
      throw std::runtime_error("mkdir failed");
    const double sr = di.sampleRate;

    auto matrix = defaultMatrix();
    // Dry always runs first (delta anchor + level-match reference).
    StudyChain chain;
    SlamConfig dryCfg;
    dryCfg.name = "dry";
    dryCfg.mode = SlamMode::Dry;
    RenderResult dryR = chain.run(di.samples, sr, map, ctx, dryCfg);
    if (!norender)
      writeWav(outdir + "/dry.wav", dryR.audio, sr);
    const double matchAnchor = dryR.met.sparseRmsDb;

    std::FILE* csv = std::fopen((outdir + "/metrics.csv").c_str(), "w");
    std::fprintf(csv, "name,peakDb,rmsDb,crestDb,clips,subDb,lowDb,lomidDb,midDb,presDb,"
                      "sparseRmsDb,evtRmsDb,evtCrestDb,recovRmsDb,gapRmsDb,presSparseDb,"
                      "dLowLomid,dPres,dEvtCrest,dRecov,fires,matchGainDb,cpuUsPerBlock\n");
    auto emit = [&](const SlamConfig& c, const RenderResult& r) {
      const double dLow = (r.met.bandDb[1] + r.met.bandDb[2]) / 2.0
          - (dryR.met.bandDb[1] + dryR.met.bandDb[2]) / 2.0;
      const double dPres = r.met.presSparseDb - dryR.met.presSparseDb;
      const double dCrest = r.met.evtCrestDb - dryR.met.evtCrestDb;
      const double dRecov = r.met.recovRmsDb - dryR.met.recovRmsDb;
      const double mg = matchAnchor - r.met.sparseRmsDb;
      std::fprintf(csv, "%s,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lld,%.2f,%.1f\n",
                   c.name.c_str(), r.met.peakDb, r.met.rmsDb, r.met.crestDb, r.met.clips,
                   r.met.bandDb[0], r.met.bandDb[1], r.met.bandDb[2], r.met.bandDb[3],
                   r.met.bandDb[4], r.met.sparseRmsDb, r.met.evtRmsDb, r.met.evtCrestDb,
                   r.met.recovRmsDb, r.met.gapRmsDb, r.met.presSparseDb, dLow, dPres, dCrest,
                   dRecov, (long long)r.fires, mg, r.cpuUsPerBlock);
      std::printf("%-12s peak %6.2f rms %6.2f crest %5.2f | lowmid %+5.2f pres %+5.2f "
                  "evtCrest %+5.2f recov %+5.2f | clip %4d fires %4lld match %+5.2f cpu "
                  "%6.1fus/blk\n",
                  c.name.c_str(), r.met.peakDb, r.met.rmsDb, r.met.crestDb, dLow, dPres, dCrest,
                  dRecov, r.met.clips, (long long)r.fires, mg, r.cpuUsPerBlock);
    };
    emit(dryCfg, dryR);

    for (const auto& c : matrix)
    {
      if (c.mode == SlamMode::Dry || !wantName(only, c.name))
        continue;
      StudyChain ch;
      RenderResult r = ch.run(di.samples, sr, map, ctx, c);
      if (!norender && c.render)
      {
        writeWav(outdir + "/" + c.name + ".wav", r.audio, sr);
        if (!nomatch)
        {
          // Level-matched twin: single scalar to dry's sparse-section RMS.
          const double g = std::pow(10.0, (matchAnchor - r.met.sparseRmsDb) / 20.0);
          std::vector<float> m = r.audio;
          for (float& v : m)
            v = (float)(v * g);
          writeWav(outdir + "/" + c.name + "_m.wav", m, sr);
        }
      }
      emit(c, r);
      if (!r.fireTimes.empty())
      {
        std::printf("    fires (%lld):", (long long)r.fires);
        for (size_t i = 0; i < r.fireTimes.size() && i < 40; ++i)
          std::printf(" %.2fs(%.2f)", r.fireTimes[i].first, r.fireTimes[i].second);
        if (r.fireTimes.size() > 40)
          std::printf(" ...");
        std::printf("\n");
        // Fire-anchored windows: wet vs dry in [fire, fire+250 ms] for
        // fires landing in the sparse/breakdown sections.
        double wWet = 0.0, wDry = 0.0, pWet = 0.0, pDry = 0.0;
        int fw = 0;
        for (const auto& ft : r.fireTimes)
        {
          bool inSparse = false;
          for (const auto& s : map.sections)
            if ((s.name == "sparse" || s.name == "breakdown") && ft.first >= s.start
                && ft.first < s.end)
              inSparse = true;
          if (!inSparse)
            continue;
          const int t = (int)(ft.first * sr), e = t + (int)(0.250 * sr);
          const double rw = windowRms(r.audio, t, e), rd = windowRms(dryR.audio, t, e);
          wWet += rw * rw;
          wDry += rd * rd;
          pWet = std::max(pWet, windowPeak(r.audio, t, e));
          pDry = std::max(pDry, windowPeak(dryR.audio, t, e));
          ++fw;
        }
        if (fw > 0)
          std::printf("    fireWin x%d: rms %+5.2f dB  peak %+5.2f dB\n", fw,
                      dbOf(std::sqrt(wWet / fw)) - dbOf(std::sqrt(wDry / fw)),
                      dbOf(pWet) - dbOf(pDry));
      }
      if (c.render && !norender)
      {
        std::printf("    sections (wet-dry RMS dB):");
        for (const auto& s : map.sections)
        {
          const int a = (int)(s.start * sr), b = (int)(s.end * sr);
          if (s.name == "fastchug")
          {
            // Mud check: first vs second half (accumulation would grow).
            const int mid = (a + b) / 2;
            std::printf(" fastchug1:%+.1f fastchug2:%+.1f",
                        dbOf(windowRms(r.audio, a, mid)) - dbOf(windowRms(dryR.audio, a, mid)),
                        dbOf(windowRms(r.audio, mid, b)) - dbOf(windowRms(dryR.audio, mid, b)));
          }
          else
            std::printf(" %s:%+.1f", s.name.c_str(),
                        dbOf(windowRms(r.audio, a, b)) - dbOf(windowRms(dryR.audio, a, b)));
        }
        std::printf("\n");
      }
    }
    std::fclose(csv);
    std::printf("wrote %s/metrics.csv\n", outdir.c_str());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_slam_study: error: %s\n", e.what());
    return 1;
  }
}

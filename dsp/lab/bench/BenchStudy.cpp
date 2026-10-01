// tdm_bench_study: external-vs-internal pitch benchmark (LAB ONLY).
//
// Renders external benchmark cells (Rubber Band R2 realtime + Live,
// SoundTouch, Signalsmith, TONE3000 Transpose 20/30/40 ms + 30 ms with
// 2 kHz Tonality) and internal references (accepted W20,
// WSOLA-E 20:840:120, PV-D 2048/256, PV-A 4096/1024) through the lab DI
// and deterministic KS voicings at -1/-2 (primary) and -7 (secondary
// stress), writes latency-compensated WAVs to build/labpitch/bench_<id>/,
// and prints comparison tables: API + observed latency, pitch accuracy,
// LF/chord retention, sustained stability (AM + wander), transient
// preservation, RMS/dropouts, CPU @128-frame blocks, determinism.
//
// External engines reach this harness ONLY through the BenchShifter
// wrappers in dsp/lab/bench/ (our shims; library sources stay outside
// the repo under TDM_BENCH_DEPS). Internal references use thin adapters
// below (same interface, same render path — the comparison is
// apples-to-apples by construction).
//
// Not part of `all`, not linked into any product binary:
//   make build/tdm_bench_study && ./build/tdm_bench_study

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/WavFile.h"
#include "dsp/lab/Pitch/LabPitchShift.h"
#include "dsp/lab/Pitch/LabWsolaShift.h"
#include "dsp/lab/bench/BenchRubberBand.h"
#include "dsp/lab/bench/BenchShifter.h"
#include "dsp/lab/bench/BenchSignalsmith.h"
#include "dsp/lab/bench/BenchSoundTouch.h"
#include "dsp/lab/bench/BenchTone3000.h"

namespace
{
constexpr double kPi = 3.14159265358979;
constexpr double kSr = 48000.0;

// ---- internal-reference adapters (our labs behind BenchShifter) ----

struct W20Adapter : tdm::bench::BenchShifter
{
  const char* id() const override { return "w20"; }
  const char* config() const override { return "LabWsolaShift W20 symmetric (accepted baseline)"; }
  void prepare(double sr, int mb) override
  {
    sr_ = sr;
    mb_ = mb;
    reset();
  }
  void setShiftSemitones(double st) override { st_ = st; }
  void reset() override
  {
    p_.setConfig(20.0);
    p_.setEnabled(true);
    p_.setShiftSt((float)st_);
    p_.reset(sr_);
  }
  void processBlock(const float* in, float* out, int n) override { p_.processBlock(in, out, n); }
  int latencySamples() const override { return p_.latencySamples(); }
  int tailSamples() const override { return p_.tailSamples(); }
  double sr_ = 0;
  int mb_ = 0;
  double st_ = 0;
  tdm::lab::LabWsolaShift p_;
};

struct EAdapter : tdm::bench::BenchShifter
{
  const char* id() const override { return "e20"; }
  const char* config() const override { return "LabWsolaShift 20:840:120 (E2 -1 latency reference)"; }
  void prepare(double sr, int mb) override
  {
    sr_ = sr;
    mb_ = mb;
    reset();
  }
  void setShiftSemitones(double st) override { st_ = st; }
  void reset() override
  {
    p_.setConfig(20.0);
    p_.setSearch(840, 120);
    p_.setEnabled(true);
    p_.setShiftSt((float)st_);
    p_.reset(sr_);
  }
  void processBlock(const float* in, float* out, int n) override { p_.processBlock(in, out, n); }
  int latencySamples() const override { return p_.latencySamples(); }
  int tailSamples() const override { return p_.tailSamples(); }
  double sr_ = 0;
  int mb_ = 0;
  double st_ = 0;
  tdm::lab::LabWsolaShift p_;
};

struct PvdAdapter : tdm::bench::BenchShifter
{
  const char* id() const override { return "pvd"; }
  const char* config() const override { return "LabPitchShift 2048/256 (PV-D)"; }
  void prepare(double sr, int mb) override
  {
    sr_ = sr;
    mb_ = mb;
    reset();
  }
  void setShiftSemitones(double st) override { st_ = st; }
  void reset() override
  {
    p_.setConfig(2048, 256);
    p_.setEnabled(true);
    p_.setShiftSt((float)st_);
    p_.reset(sr_);
  }
  void processBlock(const float* in, float* out, int n) override { p_.processBlock(in, out, n); }
  int latencySamples() const override { return p_.latencySamples(); }
  int tailSamples() const override { return p_.tailSamples(); }
  double sr_ = 0;
  int mb_ = 0;
  double st_ = 0;
  tdm::lab::LabPitchShift p_;
};

struct PvaAdapter : tdm::bench::BenchShifter
{
  const char* id() const override { return "pva"; }
  const char* config() const override { return "LabPitchShift 4096/1024 (PV-A ref)"; }
  void prepare(double sr, int mb) override
  {
    sr_ = sr;
    mb_ = mb;
    reset();
  }
  void setShiftSemitones(double st) override { st_ = st; }
  void reset() override
  {
    p_.setConfig(4096, 1024);
    p_.setEnabled(true);
    p_.setShiftSt((float)st_);
    p_.reset(sr_);
  }
  void processBlock(const float* in, float* out, int n) override { p_.processBlock(in, out, n); }
  int latencySamples() const override { return p_.latencySamples(); }
  int tailSamples() const override { return p_.tailSamples(); }
  double sr_ = 0;
  int mb_ = 0;
  double st_ = 0;
  tdm::lab::LabPitchShift p_;
};

// ---- deterministic stimuli (same recipes as our studies) ----

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
}

std::vector<float> sine(float peak, float freqHz, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / kSr));
  return out;
}

std::vector<float> ksNote(float freqHz, int n, float damp, uint32_t seed)
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
    const float v = damp * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = cur;
  }
  return out;
}

std::vector<float> ksVoicing(const std::vector<double>& freqs, double seconds, uint32_t seedBase)
{
  const int n = static_cast<int>(kSr * seconds);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (size_t v = 0; v < freqs.size(); ++v)
  {
    std::vector<float> note = ksNote(static_cast<float>(freqs[v]), n, 0.9998f, seedBase + v * 0x1001u);
    for (int i = 0; i < n; ++i)
      out[static_cast<size_t>(i)] += note[static_cast<size_t>(i)];
  }
  double peak = 1e-9;
  for (float x : out)
    peak = std::max(peak, static_cast<double>(std::fabs(x)));
  const float g = 0.8f / static_cast<float>(peak);
  const int nf = static_cast<int>(kSr * 0.01);
  for (int i = 0; i < n; ++i)
  {
    float w = 1.0f;
    if (i < nf)
      w = 0.5f - 0.5f * static_cast<float>(std::cos(kPi * i / nf));
    out[static_cast<size_t>(i)] *= g * w;
  }
  return out;
}

// ---- metrics (study-proven) ----

double rms(const std::vector<float>& v, int from, int to)
{
  double se = 0.0;
  for (int i = from; i < to; ++i)
    se += static_cast<double>(v[static_cast<size_t>(i)]) * v[static_cast<size_t>(i)];
  return std::sqrt(se / (to - from));
}

double goertzel(const float* v, int n, double f)
{
  const double w = kPi * 2.0 * f / kSr;
  const double c = 2.0 * std::cos(w);
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  for (int i = 0; i < n; ++i)
  {
    s0 = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

double incoherent(const float* v, int off, int nf, double f)
{
  const int m = nf / 10;
  double acc = 0.0;
  for (int k = 0; k < 10; ++k)
    acc += goertzel(v + off + k * m, m, f);
  return acc / 10;
}

double scanPitch(const float* v, int n, double target)
{
  double best = target, bm = -1.0;
  const double step = target * 0.002;
  for (double f = target * 0.88; f <= target * 1.12; f += step)
  {
    const double m = goertzel(v, n, f);
    if (m > bm)
    {
      bm = m;
      best = f;
    }
  }
  return best;
}

double median(std::vector<double> v)
{
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void pitchStats(const std::vector<float>& v, double t0, double t1, double want, double& errPct,
               double& spreadPct)
{
  const int nf = static_cast<int>(kSr * 0.08);
  std::vector<double> pe;
  for (int off = static_cast<int>(kSr * t0); off + nf < static_cast<int>(kSr * t1); off += nf)
    pe.push_back(scanPitch(v.data() + off, nf, want));
  const double med = median(pe);
  double lo = pe[0], hi = pe[0];
  for (double x : pe)
  {
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  errPct = 100.0 * (med - want) / want;
  spreadPct = 100.0 * (hi - lo) / med;
}

double amDepth(const std::vector<float>& v, double t0, double t1)
{
  const int w = static_cast<int>(kSr * 0.1);
  std::vector<double> e;
  for (int off = static_cast<int>(kSr * t0); off + w < static_cast<int>(kSr * t1); off += w)
    e.push_back(rms(v, off, off + w));
  double mean = 0.0;
  for (double x : e)
    mean += x;
  mean /= e.size();
  double va = 0.0;
  for (double x : e)
    va += (x - mean) * (x - mean);
  return std::sqrt(va / e.size()) / mean;
}

// wanderIdx: coherent/incoherent fund energy (1 = stable tone, ~0.1 =
// incoherent smear). Low wander + retained energy = diffuse phasiness.
double wanderIdx(const float* v, int off, int nf, double f)
{
  const double coh = goertzel(v + off, nf, f);
  const double inc = incoherent(v, off, nf, f);
  return inc > 1e-9 ? coh / inc : 0.0;
}

double maxStep(const float* v, int n)
{
  double m = 0.0;
  for (int i = 1; i < n; ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}

// Attack emergence: HP-peak level ratio + peak-position delay. The output
// search extends past the dry peak so transient-vs-sustain time skew
// (found in rb2/st click probes) is MEASURED as delay rather than misread
// as loss. Tight engines are unaffected (their peak sits at the dry
// position). postMs must stay below the onset IOI (chugs 214 ms, riff
// 100 ms) so each window sees exactly one onset.
struct Emergence
{
  double level = 0.0; // out peak / dry peak
  double delayMs = 0.0; // out peak pos - dry peak pos
};

Emergence attackEmergence(const std::vector<float>& o, const std::vector<float>& d, double onset,
                         double postMs)
{
  const int c = static_cast<int>(kSr * onset);
  const int pre = static_cast<int>(kSr * 0.002);
  auto hpPeak = [&](const std::vector<float>& v, int from, int to) {
    double best = 0.0;
    int pos = from;
    for (int i = std::max(1, from); i < std::min(to, static_cast<int>(v.size())); ++i)
    {
      const double s = std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)]);
      if (s > best)
      {
        best = s;
        pos = i;
      }
    }
    return std::make_pair(best, pos);
  };
  const auto dry = hpPeak(d, c - pre, c + static_cast<int>(kSr * 0.03));
  const auto out = hpPeak(o, c - pre, c + static_cast<int>(kSr * postMs / 1000.0));
  Emergence e;
  e.level = dry.first > 1e-9 ? out.first / dry.first : 0.0;
  e.delayMs = 1000.0 * (out.second - dry.second) / kSr;
  return e;
}

// Crude HF-energy proxy (diff = highpass): out/dry RMS ratio on a
// sustain. >>1 with inharmonic content on listening = metallic/sizzle.
double hfRatio(const std::vector<float>& o, const std::vector<float>& d, double t0, double t1)
{
  const int a = static_cast<int>(kSr * t0), b = static_cast<int>(kSr * t1);
  double so = 0.0, sd = 0.0;
  for (int i = a + 1; i < b; ++i)
  {
    so += (o[static_cast<size_t>(i)] - o[static_cast<size_t>(i - 1)]) *
        (o[static_cast<size_t>(i)] - o[static_cast<size_t>(i - 1)]);
    sd += (d[static_cast<size_t>(i)] - d[static_cast<size_t>(i - 1)]) *
        (d[static_cast<size_t>(i)] - d[static_cast<size_t>(i - 1)]);
  }
  return std::sqrt(so / (sd + 1e-12));
}

struct Render
{
  std::vector<float> out;
  double seconds = 0.0;
};

Render renderBench(tdm::bench::BenchShifter& c, const std::vector<float>& in, int block)
{
  Render r;
  const int lat = c.latencySamples();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + lat + c.tailSamples();
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 0.0f);
  const auto t0 = std::chrono::steady_clock::now();
  for (int off = 0; off < paddedN; off += block)
  {
    const int m = (paddedN - off) < block ? (paddedN - off) : block;
    c.processBlock(padded.data() + off, raw.data() + off, m);
  }
  const auto t1 = std::chrono::steady_clock::now();
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.out.resize(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    r.out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  return r;
}

struct Voicing
{
  const char* id;
  std::vector<double> freqs;
};
} // namespace

int main()
{
  constexpr int kBlock = 128; // live interface condition
  const float shifts[] = {-1.0f, -2.0f, -7.0f};

  tdm::MonoWav di;
  try
  {
    di = tdm::loadWavMono("build/labpitch_di.wav");
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_bench_study: need build/labpitch_di.wav: %s\n", e.what());
    return 1;
  }
  const int nDi = static_cast<int>(di.samples.size());

  const Voicing voices[] = {
      {"lowE", {82.41}}, {"lowB", {61.74}}, {"highE", {329.63}}, {"r5", {82.41, 123.47}},
      {"r4", {82.41, 110.0}}, {"rmaj3", {82.41, 103.83}}, {"maj", {82.41, 103.83, 123.47}},
      {"min", {82.41, 98.0, 123.47}}, {"dense", {82.41, 87.31, 98.0, 103.83}},
  };
  constexpr int kNvoice = sizeof(voices) / sizeof(voices[0]);
  std::vector<std::vector<float>> dryVoice(kNvoice);
  for (int i = 0; i < kNvoice; ++i)
    dryVoice[i] = ksVoicing(voices[i].freqs, 5.0, 0x500u);
  // 4 s sines with LATE oracle windows (2.0-3.5 s): RB2 realtime needs
  // ~2 s to settle a hard-start sine; same window for every engine.
  const int nSine = static_cast<int>(kSr * 4.0);
  const std::vector<float> sE2 = sine(0.5f, 82.41f, nSine);
  const std::vector<float> sB1 = sine(0.5f, 61.74f, nSine);
  const std::vector<float> sE4 = sine(0.5f, 329.63f, nSine);

  struct Chord
  {
    double t0, t1, root;
  };
  const Chord chords[] = {{6.4, 7.0, 82.41}, {7.4, 8.0, 98.0}, {8.4, 9.0, 110.0}};
  const double chugStep = 60.0 / 140.0 / 2.0;
  const double riffStep = 60.0 / 150.0 / 4.0;

  auto makeCells = []() {
    std::vector<std::unique_ptr<tdm::bench::BenchShifter>> v;
    v.emplace_back(new tdm::bench::BenchRubberBand(tdm::bench::BenchRubberBand::Mode::R2Realtime));
    v.emplace_back(new tdm::bench::BenchRubberBand(tdm::bench::BenchRubberBand::Mode::Live));
    v.emplace_back(new tdm::bench::BenchSoundTouch());
    v.emplace_back(new tdm::bench::BenchSignalsmith());
    v.emplace_back(new tdm::bench::BenchTone3000(tdm::bench::BenchTone3000::Window::Ms20));
    v.emplace_back(new tdm::bench::BenchTone3000(tdm::bench::BenchTone3000::Window::Ms30));
    v.emplace_back(new tdm::bench::BenchTone3000(tdm::bench::BenchTone3000::Window::Ms40));
    v.emplace_back(new tdm::bench::BenchTone3000(tdm::bench::BenchTone3000::Window::Ms30, 2000.0f));
    v.emplace_back(new W20Adapter());
    v.emplace_back(new EAdapter());
    v.emplace_back(new PvdAdapter());
    v.emplace_back(new PvaAdapter());
    return v;
  };
  {
    auto cells = makeCells();
    std::printf("cells:\n");
    for (auto& c : cells)
    {
      c->setShiftSemitones(-1.0);
      c->prepare(kSr, kBlock);
      std::printf("  %-6s %s\n", c->id(), c->config());
    }
  }

  for (float st : shifts)
  {
    const double r = std::exp2(st / 12.0);
    const bool secondary = (st == -7.0f);
    std::printf("\n=== shift %.0f (r=%.5f)%s ===\n", st, r, secondary ? " [secondary stress]" : "");
    auto cells = makeCells();
    const int ncell = static_cast<int>(cells.size());
    std::vector<Render> diR(ncell);
    std::vector<std::vector<Render>> voiceR(ncell, std::vector<Render>(kNvoice));
    std::vector<double> cpuSec(ncell, 0.0);
    std::vector<int> obsLat(ncell, 0);
    std::vector<bool> determin(ncell, true);

    for (int ci = 0; ci < ncell; ++ci)
    {
      auto& c = *cells[ci];
      c.setShiftSemitones(st);
      c.prepare(kSr, kBlock);
      c.reset();
      std::filesystem::create_directories("build/labpitch/bench_" + std::string(c.id()));
      // Observed latency: click onset at 1.0 s, first sustained energy.
      {
        const int nn = static_cast<int>(kSr * 2.0);
        std::vector<float> stim(static_cast<size_t>(nn), 0.0f);
        const int at = static_cast<int>(kSr * 1.0);
        for (int i = 0; i < 58; ++i)
          stim[static_cast<size_t>(at) + i] = detNoise(i, 0x301u) * 0.42f * (1.0f - i / 58.0f);
        Render lr = renderBench(c, stim, kBlock);
        c.reset();
        int first = nn;
        for (int i = 0; i + 8 < nn; ++i)
        {
          bool hot = true;
          for (int k = 0; k < 8; ++k)
            if (std::fabs(lr.out[static_cast<size_t>(i) + k]) < 0.02f)
              hot = false;
          if (hot)
          {
            first = i;
            break;
          }
        }
        obsLat[ci] = first - at + c.latencySamples(); // back to raw-output terms
      }
      diR[ci] = renderBench(c, di.samples, kBlock);
      {
        char path[144];
        std::snprintf(path, sizeof(path), "build/labpitch/bench_%s/shift_%.0f_di.wav", c.id(), st);
        const float* ch[1] = {diR[ci].out.data()};
        tdm::writeWavFloat32(path, ch, 1, nDi, kSr);
      }
      for (int vi = 0; vi < kNvoice; ++vi)
      {
        if (secondary && std::string(voices[vi].id) != "r4" && std::string(voices[vi].id) != "maj" &&
            std::string(voices[vi].id) != "lowB" && std::string(voices[vi].id) != "lowE")
          continue;
        c.reset();
        voiceR[ci][vi] = renderBench(c, dryVoice[vi], kBlock);
        char path[144];
        std::snprintf(path, sizeof(path), "build/labpitch/bench_%s/shift_%.0f_%s.wav", c.id(), st,
                      voices[vi].id);
        const float* ch[1] = {voiceR[ci][vi].out.data()};
        tdm::writeWavFloat32(path, ch, 1, static_cast<int>(voiceR[ci][vi].out.size()), kSr);
      }
      // CPU: mean of 2 full-DI renders @128 (live block).
      c.reset();
      cpuSec[ci] = (renderBench(c, di.samples, kBlock).seconds +
                    renderBench(c, di.samples, kBlock).seconds) /
          2.0;
      // Determinism: same render twice must match bit-exactly.
      c.reset();
      Render d1 = renderBench(c, dryVoice[4], kBlock);
      c.reset();
      Render d2 = renderBench(c, dryVoice[4], kBlock);
      determin[ci] = (d1.out == d2.out);
    }

    // Table 1: latency + pitch + LF + CPU + determinism.
    std::printf("%-6s %7s %7s | %7s %7s %7s | %7s %7s %7s %7s %7s | %7s %4s\n", "cell", "latAPI",
                "latObs", "sinE2%", "sinB1%", "sinE4%", "ksE2%", "ksE2sp", "ksB1%", "ksB1sp",
                "lowBen", "cpuSec", "det");
    for (int ci = 0; ci < ncell; ++ci)
    {
      auto& c = *cells[ci];
      c.reset();
      Render rE2 = renderBench(c, sE2, kBlock);
      c.reset();
      Render rB1 = renderBench(c, sB1, kBlock);
      c.reset();
      Render rE4 = renderBench(c, sE4, kBlock);
      double eSine, sSine, eB1, sB1, eE4, sE4, eKE2, sKE2, eKB1, sKB1;
      pitchStats(rE2.out, 2.0, 3.5, 82.41 * r, eSine, sSine);
      pitchStats(rB1.out, 2.0, 3.5, 61.74 * r, eB1, sB1);
      pitchStats(rE4.out, 2.0, 3.5, 329.63 * r, eE4, sE4);
      pitchStats(voiceR[ci][0].out, 1.0, 4.0, 82.41 * r, eKE2, sKE2);
      pitchStats(voiceR[ci][1].out, 1.0, 4.0, 61.74 * r, eKB1, sKB1);
      const int boff = static_cast<int>(kSr * 1.0), bn = static_cast<int>(kSr * 3.0);
      const double lowBen = incoherent(voiceR[ci][1].out.data(), boff, bn, 61.74 * r) /
          incoherent(dryVoice[1].data(), boff, bn, 61.74);
      std::printf("%-6s %7d %7d | %+7.2f %+7.2f %+7.2f | %+7.2f %7.2f %+7.2f %7.2f %7.3f | %7.3f "
                  "%4s\n",
                  c.id(), c.latencySamples(), obsLat[ci], eSine, eB1, eE4, eKE2, sKE2, eKB1, sKB1,
                  lowBen, cpuSec[ci], determin[ci] ? "yes" : "NO");
      (void)sSine;
      (void)sB1;
      (void)sE4;
    }
    // Table 2: voicing retention + stability (wantMin / AM / wander of weakest fund).
    auto useVoice = [&](int i) {
      return !secondary || std::string(voices[i].id) == "r4" || std::string(voices[i].id) == "maj" ||
          std::string(voices[i].id) == "lowB" || std::string(voices[i].id) == "lowE";
    };
    std::printf("%-6s", "cell");
    for (int i = 0; i < kNvoice; ++i)
      if (useVoice(i))
        std::printf(" | %-8s %-6s %-6s %-6s", voices[i].id, "wantMn", "amDep", "wander");
    std::printf("\n");
    for (int ci = 0; ci < ncell; ++ci)
    {
      std::printf("%-6s", cells[ci]->id());
      for (int i = 0; i < kNvoice; ++i)
      {
        if (!useVoice(i))
          continue;
        const std::vector<float>& o = voiceR[ci][i].out;
        double wantMin = 1e300, wandMin = 1e300;
        const int off = static_cast<int>(kSr * 1.0), nf = static_cast<int>(kSr * 2.0);
        for (double f : voices[i].freqs)
        {
          const double dry = incoherent(dryVoice[i].data(), off, nf, f);
          const double want = incoherent(o.data(), off, nf, f * r);
          wantMin = std::min(wantMin, want / dry);
          wandMin = std::min(wandMin, wanderIdx(o.data(), off, nf, f * r));
        }
        std::printf(" | %-8.3f %-6.4f %-6.2f", wantMin, amDepth(o, 1.0, 4.5), wandMin);
      }
      std::printf("\n");
    }
    // Table 3: DI chords/level/artifacts/attacks (absolute vs dry).
    std::printf("%-6s %-8s %-11s %-8s | %-7s %-7s | %-6s %-6s %-6s | %-6s %-6s %-6s %-6s %-7s %-6s\n",
                "cell", "wantMin", "min@", "leakMax", "rmsRat", "pkRat", "amS1", "amS6", "drop",
                "chugMn", "chugDl", "riffMn", "riffDl", "maxStep", "hfRat");
    const double dryRms = rms(di.samples, 0, nDi);
    double dryPeak = 0.0;
    for (float x : di.samples)
      dryPeak = std::max(dryPeak, static_cast<double>(std::fabs(x)));
    for (int ci = 0; ci < ncell; ++ci)
    {
      const std::vector<float>& o = diR[ci].out;
      double wantMin = 1e300, leakMax = 0.0;
      char minAt[16] = "?";
      for (const Chord& chd : chords)
      {
        const double funds[3] = {chd.root, chd.root * 1.4983, chd.root * 2.0};
        const int off = static_cast<int>(kSr * chd.t0);
        const int nf = static_cast<int>(kSr * (chd.t1 - chd.t0));
        for (double f : funds)
        {
          const double dry = incoherent(di.samples.data(), off, nf, f);
          const double want = incoherent(o.data(), off, nf, f * r);
          if (want / dry < wantMin)
          {
            wantMin = want / dry;
            std::snprintf(minAt, sizeof(minAt), "%.0f:%.0f", chd.root, f);
          }
          bool collide = false;
          for (double g : funds)
            for (int k = 1; k <= 6; ++k)
              if (std::fabs(k * g * r - f) < 4.0)
                collide = true;
          if (!collide)
          {
            const double leak = goertzel(o.data() + off, nf, f);
            leakMax = std::max(leakMax, leak / dry);
          }
        }
      }
      double peak = 0.0;
      for (float x : o)
        peak = std::max(peak, static_cast<double>(std::fabs(x)));
      double chugMn = 1e300, riffMn = 1e300, chugDl = 0.0, riffDl = 0.0;
      for (int k = 0; k < 8; ++k)
      {
        const Emergence e = attackEmergence(o, di.samples, 4.0 + k * chugStep, 150.0);
        chugMn = std::min(chugMn, e.level);
        chugDl = std::max(chugDl, e.delayMs);
      }
      for (int k = 0; k < 16; ++k)
      {
        const Emergence e = attackEmergence(o, di.samples, 12.4 + k * riffStep, 80.0);
        riffMn = std::min(riffMn, e.level);
        riffDl = std::max(riffDl, e.delayMs);
      }
      const int w = static_cast<int>(kSr * 0.03);
      int drops = 0;
      for (int off = 0; off + w < nDi; off += w)
      {
        const double rr = rms(di.samples, off, off + w);
        if (rr > 0.03 && rms(o, off, off + w) < 0.1 * rr)
          ++drops;
      }
      const int s1a = static_cast<int>(kSr * 0.6), s1b = static_cast<int>(kSr * 1.2);
      std::printf("%-6s %-8.3f %-11s %-8.3f | %-7.4f %-7.4f | %-6.4f %-6.4f %-6d | %-6.3f %-6.1f "
                  "%-6.3f %-6.1f %-7.4f %-6.3f\n",
                  cells[ci]->id(), wantMin, minAt, leakMax, rms(o, 0, nDi) / dryRms, peak / dryPeak,
                  amDepth(o, 0.6, 1.2), amDepth(o, 14.8, 16.2), drops, chugMn, chugDl, riffMn, riffDl,
                  maxStep(o.data() + s1a, s1b - s1a), hfRatio(o, di.samples, 0.6, 1.2));
    }
  }
  return 0;
}

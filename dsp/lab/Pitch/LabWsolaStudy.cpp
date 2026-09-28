// tdm_wsola_study: WSOLA latency-study measurement harness (LAB ONLY).
//
// Renders the accepted W20 reference plus the latency-study candidates
// through build/labpitch_di.wav (48 kHz float32 mono, deterministic -
// see scripts/labpitch_synth_di.py) at shifts -1/-2/-7, writes
// latency-compensated WAVs to build/labpitch/lat_<id>/, and prints
// comparison tables: exact latency, pitch accuracy (sine oracle + KS
// spots + DI segments), chord presence/leak, level, flutter proxies,
// dropout count, attack preservation, telemetry, and CPU cost.
//
// Not part of `all`, not linked into any product binary:
//   make build/tdm_wsola_study && ./build/tdm_wsola_study
// Sections below mirror the synth script; if the DI is regenerated
// with different sections these windows must move with it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "dsp/WavFile.h"
#include "dsp/lab/Pitch/LabWsolaShift.h"

namespace
{
constexpr double kPi = 3.14159265358979;

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
}

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / sr));
  return out;
}

// House KS pluck (same recipe as the WSOLA tests).
std::vector<float> ksPluck(float peak, float freqHz, double sr, int n, float damp = 0.996f,
                           uint32_t seed = 0x51ab3u)
{
  const int period = static_cast<int>(sr / freqHz + 0.5f) > 2 ? static_cast<int>(sr / freqHz + 0.5f) : 2;
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
    out[static_cast<size_t>(i)] = peak * cur;
  }
  return out;
}

double rms(const std::vector<float>& v, int from, int to)
{
  double se = 0.0;
  for (int i = from; i < to; ++i)
    se += static_cast<double>(v[static_cast<size_t>(i)]) * v[static_cast<size_t>(i)];
  return std::sqrt(se / (to - from));
}

double goertzel(const float* v, int n, double sr, double f)
{
  const double w = kPi * 2.0 * f / sr;
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

double incoherent(const float* v, int off, int nf, double sr, double f)
{
  const int nsub = 10;
  const int m = nf / nsub;
  double acc = 0.0;
  for (int k = 0; k < nsub; ++k)
    acc += goertzel(v + off + k * m, m, sr, f);
  return acc / nsub;
}

double scanPitch(const float* v, int n, double sr, double target)
{
  double best = target, bm = -1.0;
  const double step = target * 0.002;
  for (double f = target * 0.88; f <= target * 1.12; f += step)
  {
    const double m = goertzel(v, n, sr, f);
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

double maxStep(const float* v, int n)
{
  double m = 0.0;
  for (int i = 1; i < n; ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}

struct Config
{
  const char* id; // output dir suffix + table label
  double wms;
  int tolM, tolP; // 0,0 = default symmetric
};

struct Render
{
  std::vector<float> out; // latency-compensated, same length as input
  int latency = 0;
  tdm::lab::LabWsolaShift::Telemetry tm;
  double seconds = 0.0;
};

Render renderThrough(const Config& c, float shiftSt, const std::vector<float>& in, double sr, int block)
{
  Render r;
  tdm::lab::LabWsolaShift p;
  p.setConfig(c.wms);
  if (c.tolM != 0 || c.tolP != 0)
    p.setSearch(c.tolM, c.tolP);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(sr);
  r.latency = p.latencySamples();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + r.latency + p.tailSamples();
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 0.0f);
  const auto t0 = std::chrono::steady_clock::now();
  for (int off = 0; off < paddedN; off += block)
  {
    const int m = (paddedN - off) < block ? (paddedN - off) : block;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  const auto t1 = std::chrono::steady_clock::now();
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.out.resize(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    r.out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + r.latency)];
  r.tm = p.telemetry();
  return r;
}

// Median pitch error (%) + spread (%) of per-window estimates pooled
// over sustain-only ranges (80 ms windows like the WSOLA tests;
// attacks are excluded so the spread reads warble, not luck).
void pitchStats(const std::vector<float>& v, double sr, const std::vector<std::pair<double, double>>& ranges,
               double want, double& errPct, double& spreadPct)
{
  const int nf = static_cast<int>(sr * 0.08);
  std::vector<double> pe;
  for (const auto& rg : ranges)
    for (int off = static_cast<int>(sr * rg.first); off + nf < static_cast<int>(sr * rg.second); off += nf)
      pe.push_back(scanPitch(v.data() + off, nf, sr, want));
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

// Segmental-RMS flutter proxy: std/mean of 30 ms window RMS over a range.
double flutter(const std::vector<float>& v, double sr, double t0, double t1)
{
  const int w = static_cast<int>(sr * 0.03);
  std::vector<double> e;
  for (int off = static_cast<int>(sr * t0); off + w < static_cast<int>(sr * t1); off += w)
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

// Dropout windows: ref is hot (> -30 dBFS) but candidate reads < 1/10.
int dropouts(const std::vector<float>& cand, const std::vector<float>& ref, double sr)
{
  const int w = static_cast<int>(sr * 0.03);
  int n = 0;
  for (int off = 0; off + w < static_cast<int>(ref.size()); off += w)
  {
    const double r = rms(ref, off, off + w);
    if (r > 0.03 && rms(cand, off, off + w) < 0.1 * r)
      ++n;
  }
  return n;
}

// Attack slope per chug onset: max |step| in [onset, onset+20 ms].
double attackSlope(const std::vector<float>& v, double sr, double onset)
{
  const int a = static_cast<int>(sr * onset);
  const int b = a + static_cast<int>(sr * 0.02);
  double m = 0.0;
  for (int i = a + 1; i < b && i < static_cast<int>(v.size()); ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)])));
  return m;
}
} // namespace

int main()
{
  const Config cfgs[] = {
      {"ref", 20.0, 0, 0}, // accepted W20 baseline (default symmetric)
      {"a", 20.0, 640, 320}, // W20, span 960, lookahead W/3
      {"b", 20.0, 480, 240}, // W20, span 720, lookahead W/4
      {"c", 20.0, 480, 120}, // W20, span 600, lookahead W/8 (floor probe)
      {"d", 16.0, 0, 0}, // W16 symmetric (control for e)
      {"e", 16.0, 512, 256}, // W16, span 768, lookahead W/3
      {"f", 14.0, 0, 0}, // W14 symmetric
      {"g", 20.0, 720, 240}, // W20, span 960, lookahead W/4
      {"h", 20.0, 768, 192}, // W20, span 960, lookahead W/5
      {"i", 20.0, 800, 160}, // W20, span 960, lookahead W/6
      {"j", 20.0, 560, 240}, // W20, span 800 (just over low-B 778)
      {"k", 16.0, 704, 256}, // W16, span 960: span-vs-window discriminator
      {"l", 14.0, 672, 288}, // W14, span 960: full span at ~20 ms?
      {"m", 20.0, 680, 280}, // W20, span 960: Dp-floor bisection (240 fail / 320 pass)
  };
  constexpr int kNcfg = sizeof(cfgs) / sizeof(cfgs[0]);
  const float shifts[] = {-1.0f, -2.0f, -7.0f};

  tdm::MonoWav di;
  try
  {
    di = tdm::loadWavMono("build/labpitch_di.wav");
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_wsola_study: need build/labpitch_di.wav: %s\n", e.what());
    return 1;
  }
  const double sr = di.sampleRate;
  const int n = static_cast<int>(di.samples.size());
  std::printf("study: DI %d frames @ %.0f Hz (%.1f s)\n", n, sr, n / sr);

  // Spot stimuli (clean pitch oracles): KS sustains + sine.
  const int nKs = static_cast<int>(sr * 2.0);
  const std::vector<float> ksE2 = ksPluck(0.5f, 82.41f, sr, nKs);
  const std::vector<float> ksB1 = ksPluck(0.5f, 61.74f, sr, nKs);
  const int nSine = static_cast<int>(sr * 1.0);
  const std::vector<float> s220 = sine(0.5f, 220.0f, sr, nSine);

  // Chord section ground truth (synth script section 3).
  struct Chord
  {
    double t0, t1, root;
  };
  const Chord chords[] = {{6.4, 7.0, 82.41}, {7.4, 8.0, 98.0}, {8.4, 9.0, 110.0}};
  const double chugStep = 60.0 / 140.0 / 2.0;

  for (float st : shifts)
  {
    const double r = std::exp2(st / 12.0);
    std::printf("\n=== shift %.0f (r=%.5f) ===\n", st, r);
    std::printf("%-4s %4s %6s %8s %8s | %-7s %-7s %-7s | %-7s %-7s %-7s %-7s %-7s\n", "cfg", "W",
                "Dm:Dp", "L", "ms", "sine%", "ksE2%", "ksB1%", "s1E2%", "s1spr%", "s6B1%", "s6spr%",
                "s6en");
    Render diRenders[kNcfg];
    for (int ci = 0; ci < kNcfg; ++ci)
    {
      const Config& c = cfgs[ci];
      diRenders[ci] = renderThrough(c, st, di.samples, sr, 1024);
      std::filesystem::create_directories("build/labpitch/lat_" + std::string(c.id));
      char path[128];
      std::snprintf(path, sizeof(path), "build/labpitch/lat_%s/shift_%.0f.wav", c.id, st);
      const float* ch[1] = {diRenders[ci].out.data()};
      tdm::writeWavFloat32(path, ch, 1, n, sr);

      const Render ksE = renderThrough(c, st, ksE2, sr, 1024);
      const Render ksB = renderThrough(c, st, ksB1, sr, 1024);
      const Render sn = renderThrough(c, st, s220, sr, 1024);
      double eSine, sSine, eKsE, sKsE, eKsB, sKsB, eS1, sS1, eS6, sS6;
      pitchStats(sn.out, sr, {{0.2, 0.9}}, 220.0 * r, eSine, sSine);
      pitchStats(ksE.out, sr, {{0.3, 1.0}}, 82.41 * r, eKsE, sKsE);
      pitchStats(ksB.out, sr, {{0.3, 1.0}}, 61.74 * r, eKsB, sKsB);
      pitchStats(diRenders[ci].out, sr, {{0.6, 1.2}, {1.7, 2.3}, {2.8, 3.4}}, 82.41 * r, eS1, sS1);
      pitchStats(diRenders[ci].out, sr, {{14.8, 16.2}}, 61.74 * r, eS6, sS6);
      const int s6off = static_cast<int>(sr * 14.8), s6n = static_cast<int>(sr * 1.4);
      const double s6en = incoherent(diRenders[ci].out.data(), s6off, s6n, sr, 61.74 * r) /
          incoherent(di.samples.data(), s6off, s6n, sr, 61.74);
      tdm::lab::LabWsolaShift probe;
      probe.setConfig(c.wms);
      if (c.tolM != 0 || c.tolP != 0)
        probe.setSearch(c.tolM, c.tolP);
      probe.setShiftSt(st);
      probe.reset(sr);
      char tol[16];
      std::snprintf(tol, sizeof(tol), "%d:%d", probe.tolMinus(), probe.tolPlus());
      std::printf("%-4s %4d %6s %8d %8.2f | %+7.2f %+7.2f %+7.2f | %+7.2f %7.2f %+7.2f %7.2f "
                      "%7.3f\n",
                  c.id, probe.frameLen(), tol, probe.latencySamples(),
                  1000.0 * probe.latencySamples() / sr, eSine, eKsE, eKsB, eS1, sS1, eS6, sS6, s6en);
      (void)sSine;
      (void)sKsE;
      (void)sKsB;
      (void)sS6;
    }
    // Table 2: stability / level / artifacts, ref-relative.
    std::printf("%-4s %-8s %-11s %-8s | %-7s %-7s | %-6s %-6s %-6s | %-6s %-7s %-5s\n", "cfg", "wantMin",
                "min@", "leakMax", "rmsRat", "pkRat", "flS1", "flS6", "drop", "attMin", "maxStep",
                "churn");
    const double refRms = rms(diRenders[0].out, 0, n);
    double refPeak = 0.0;
    for (float x : diRenders[0].out)
      refPeak = std::max(refPeak, static_cast<double>(std::fabs(x)));
    for (int ci = 0; ci < kNcfg; ++ci)
    {
      const std::vector<float>& o = diRenders[ci].out;
      double wantMin = 1e300, leakMax = 0.0;
      char minAt[16] = "?";
      for (const Chord& chd : chords)
      {
        const double funds[3] = {chd.root, chd.root * 1.4983, chd.root * 2.0};
        const int off = static_cast<int>(sr * chd.t0);
        const int nf = static_cast<int>(sr * (chd.t1 - chd.t0));
        for (double f : funds)
        {
          const double dry = incoherent(di.samples.data(), off, nf, sr, f);
          const double want = incoherent(o.data(), off, nf, sr, f * r);
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
            const double leak = goertzel(o.data() + off, nf, sr, f);
            leakMax = std::max(leakMax, leak / dry);
          }
        }
      }
      double peak = 0.0;
      for (float x : o)
        peak = std::max(peak, static_cast<double>(std::fabs(x)));
      double attMin = 1e300;
      for (int k = 0; k < 8; ++k)
      {
        const double a = attackSlope(o, sr, 4.0 + k * chugStep);
        const double b = attackSlope(diRenders[0].out, sr, 4.0 + k * chugStep);
        attMin = std::min(attMin, a / b);
      }
      const int s1a = static_cast<int>(sr * 0.6), s1b = static_cast<int>(sr * 1.2);
      std::printf("%-4s %-8.3f %-11s %-8.3f | %-7.4f %-7.4f | %-6.3f %-6.3f %-6d | %-6.3f %-7.4f %-5.1f\n",
                  cfgs[ci].id, wantMin, minAt, leakMax, rms(o, 0, n) / refRms, peak / refPeak,
                  flutter(o, sr, 0.6, 1.2), flutter(o, sr, 14.8, 16.2), dropouts(o, diRenders[0].out, sr),
                  attMin, maxStep(o.data() + s1a, s1b - s1a),
                  static_cast<double>(diRenders[ci].tm.lagChurn) / diRenders[ci].tm.frames);
    }
    // Per-chug attack-slope ratios vs ref (8 chugs @ 140 bpm 8ths).
    std::printf("%-4s %-47s\n", "cfg", "chug attack slope / ref");
    for (int ci = 0; ci < kNcfg; ++ci)
    {
      std::printf("%-4s ", cfgs[ci].id);
      for (int k = 0; k < 8; ++k)
      {
        const double a = attackSlope(diRenders[ci].out, sr, 4.0 + k * chugStep);
        const double b = attackSlope(diRenders[0].out, sr, 4.0 + k * chugStep);
        std::printf("%5.2f ", a / b);
      }
      std::printf("\n");
    }
    // Table 3: CPU at live-like block 128 (mean of 3 full-DI renders).
    std::printf("%-4s %-8s %-6s\n", "cfg", "cpuSec", "cpuX");
    for (int ci = 0; ci < kNcfg; ++ci)
    {
      double acc = 0.0;
      for (int rep = 0; rep < 3; ++rep)
        acc += renderThrough(cfgs[ci], st, di.samples, sr, 128).seconds;
      acc /= 3.0;
      static double refCpu[3] = {0, 0, 0};
      const int si = (st == -1.0f) ? 0 : ((st == -2.0f) ? 1 : 2);
      if (ci == 0)
        refCpu[si] = acc;
      std::printf("%-4s %-8.3f %-6.2f\n", cfgs[ci].id, acc, acc / refCpu[si]);
    }
  }
  return 0;
}

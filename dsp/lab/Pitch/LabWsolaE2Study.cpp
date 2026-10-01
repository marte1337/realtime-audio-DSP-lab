// tdm_e2_study: E2 causal / small-skip WSOLA comparison harness (LAB ONLY).
//
// Renders the four E2 cells through the lab DI and deterministic KS
// voicings at shifts -1/-2 (primary) and -7 (DI + key voicings only,
// secondary stress), writes latency-compensated WAVs to
// build/labpitch/e2_<cell>/, and prints comparison tables: exact
// latency, pitch accuracy, LF/chord retention, skip-size distribution +
// skip rate (from frame traces), predicted vs measured rephasing,
// join quality, attack preservation, dropouts, and CPU.
//
// Cells (W20 fixed; LabWsolaShift accepted baseline untouched):
//   A = Drift + symmetric (480,480): accepted W20 reference.
//   B = Drift + causal (960,0): Q1 (latency 963 = 20.1 ms @-1).
//   C = SmallSkip + symmetric: Q2 isolation.
//   D = SmallSkip + causal: the E2 candidate.
//   E = Drift + (840,120): Q1 boundary probe (span 960, Dp admits the
//      transient landing peaks; latency 1083 = 22.6 ms @-1).
//
// Not part of `all`, not linked into any product binary:
//   make build/tdm_e2_study && ./build/tdm_e2_study

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
#include "dsp/lab/Pitch/LabWsolaV2.h"

namespace
{
constexpr double kPi = 3.14159265358979;
constexpr double kSr = 48000.0;

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

// Sustained voicing (traj recipe): equal KS notes from 0, damp 0.9998,
// peak-normalized to 0.8, 10 ms raised-cosine fade-in.
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
  const int nsub = 10;
  const int m = nf / nsub;
  double acc = 0.0;
  for (int k = 0; k < nsub; ++k)
    acc += goertzel(v + off + k * m, m, f);
  return acc / nsub;
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

// Slow-AM depth: std/mean of 100 ms window RMS (wobble correlate).
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

double maxStep(const float* v, int n)
{
  double m = 0.0;
  for (int i = 1; i < n; ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}

double attackSlope(const std::vector<float>& v, double onset)
{
  const int a = static_cast<int>(kSr * onset);
  const int b = a + static_cast<int>(kSr * 0.02);
  double m = 0.0;
  for (int i = a + 1; i < b && i < static_cast<int>(v.size()); ++i)
    m = std::max(m, static_cast<double>(std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)])));
  return m;
}

struct Cell
{
  const char* id;
  tdm::lab::LabWsolaV2::Mode mode;
  int tolM, tolP; // 0,0 = default symmetric
};

struct Render
{
  std::vector<float> out;
  std::vector<tdm::lab::LabWsolaV2::FrameTrace> tr;
  int latency = 0;
  tdm::lab::LabWsolaV2::Telemetry tm;
  double seconds = 0.0;
};

Render renderE2(const Cell& c, float shiftSt, const std::vector<float>& in, int block, bool trace)
{
  Render r;
  tdm::lab::LabWsolaV2 p;
  p.setConfig(20.0);
  if (c.tolM != 0 || c.tolP != 0)
    p.setSearch(c.tolM, c.tolP);
  p.setMode(c.mode);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  if (trace)
    p.enableTrace(true);
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
  if (trace)
    r.tr = p.trace();
  return r;
}

// Sustain-window skip statistics from a frame trace.
struct SkipStats
{
  long long nFr = 0, nPeg = 0, nJump = 0;
  long long maxJ = 0;
  double medJ = -1, p10J = -1, p90J = -1, skipRate = 0.0;
  double sacMax = 0.0, selMin = 2.0, runMean = 0.0;
};

SkipStats skipStats(const std::vector<tdm::lab::LabWsolaV2::FrameTrace>& tr, double t0, double t1)
{
  SkipStats s;
  std::vector<long long> pegJ;
  std::vector<int> runLens;
  int runLen = 0;
  long long prevBest = 0;
  bool havePrev = false;
  const long long jumpThresh = 480 / 4;
  for (const auto& r : tr)
  {
    if (!r.searched)
      continue;
    const double t = r.nominal / kSr;
    if (t < t0 || t >= t1)
      continue;
    ++s.nFr;
    s.nPeg += r.pegged ? 1 : 0;
    s.sacMax = std::max(s.sacMax, r.topScore[0] - r.selScore);
    s.selMin = std::min(s.selMin, r.selScore);
    if (havePrev)
    {
      const long long d = std::llabs(r.best - prevBest);
      if (d > jumpThresh)
        ++s.nJump;
      s.maxJ = std::max(s.maxJ, d);
      if (r.pegged)
        pegJ.push_back(d);
    }
    prevBest = r.best;
    havePrev = true;
    ++runLen;
    if (r.pegged)
    {
      runLens.push_back(runLen);
      runLen = 0;
    }
  }
  if (runLen > 0)
    runLens.push_back(runLen);
  if (!pegJ.empty())
  {
    std::sort(pegJ.begin(), pegJ.end());
    s.p10J = static_cast<double>(pegJ[pegJ.size() / 10]);
    s.medJ = static_cast<double>(pegJ[pegJ.size() / 2]);
    s.p90J = static_cast<double>(pegJ[pegJ.size() * 9 / 10]);
  }
  s.skipRate = s.nJump / (t1 - t0);
  if (!runLens.empty())
  {
    double m = 0.0;
    for (int x : runLens)
      m += x;
    s.runMean = m / runLens.size();
  }
  return s;
}

struct Voicing
{
  const char* id;
  std::vector<double> freqs;
};

// Mean folded per-skip relative phase step (cycles in [0, 0.5]) for an
// upper partial at ratio q over the root, given measured skip sizes:
// residual = |frac(q*J/P) - round| with P the root period in samples.
// 0 = skip preserves the interval; 0.5 = maximal (sign flip).
double meanResidual(const std::vector<long long>& jumps, double q, double rootHz)
{
  if (jumps.empty())
    return -1.0;
  const double p = kSr / rootHz;
  double acc = 0.0;
  for (long long j : jumps)
  {
    const double f = q * j / p;
    acc += std::fabs(f - std::floor(f + 0.5));
  }
  return acc / jumps.size();
}

std::vector<long long> pegJumps(const std::vector<tdm::lab::LabWsolaV2::FrameTrace>& tr, double t0,
                               double t1)
{
  std::vector<long long> out;
  long long prevBest = 0;
  bool havePrev = false;
  for (const auto& r : tr)
  {
    if (!r.searched)
      continue;
    const double t = r.nominal / kSr;
    if (t < t0 || t >= t1)
      continue;
    if (havePrev && r.pegged)
      out.push_back(std::llabs(r.best - prevBest));
    prevBest = r.best;
    havePrev = true;
  }
  return out;
}
} // namespace

int main()
{
  namespace wv2 = tdm::lab;
  const Cell cells[] = {
      {"A", wv2::LabWsolaV2::Mode::Drift, 0, 0},
      {"B", wv2::LabWsolaV2::Mode::Drift, 960, 0},
      {"C", wv2::LabWsolaV2::Mode::SmallSkip, 0, 0},
      {"D", wv2::LabWsolaV2::Mode::SmallSkip, 960, 0},
      {"E", wv2::LabWsolaV2::Mode::Drift, 840, 120},
  };
  constexpr int kNcell = sizeof(cells) / sizeof(cells[0]);
  const float shifts[] = {-1.0f, -2.0f, -7.0f};

  tdm::MonoWav di;
  try
  {
    di = tdm::loadWavMono("build/labpitch_di.wav");
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_e2_study: need build/labpitch_di.wav: %s\n", e.what());
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
  const int nSine = static_cast<int>(kSr * 1.0);
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

  for (float st : shifts)
  {
    const double r = std::exp2(st / 12.0);
    const bool secondary = (st == -7.0f);
    std::printf("\n=== shift %.0f (r=%.5f)%s ===\n", st, r, secondary ? " [secondary stress]" : "");
    std::printf("%-4s %8s %8s | %-7s %-7s %-7s | %-7s %-7s %-7s %-7s %-7s | %-5s %-6s %-6s %-6s "
                "%-6s %-6s\n",
                "cell", "L", "ms", "sinE2%", "sinB1%", "sinE4%", "ksE2%", "ksE2sp%", "ksB1%",
                "ksB1sp%", "lowBen", "skRt", "medJ", "maxJ", "sacMx", "r4am", "r5am");
    Render diR[kNcell];
    Render r4R[kNcell], r5R[kNcell];
    for (int ci = 0; ci < kNcell; ++ci)
    {
      diR[ci] = renderE2(cells[ci], st, di.samples, 1024, true);
      std::filesystem::create_directories("build/labpitch/e2_" + std::string(cells[ci].id));
      char path[128];
      std::snprintf(path, sizeof(path), "build/labpitch/e2_%s/shift_%.0f_di.wav", cells[ci].id, st);
      const float* ch[1] = {diR[ci].out.data()};
      tdm::writeWavFloat32(path, ch, 1, nDi, kSr);
      const Render rE2 = renderE2(cells[ci], st, sE2, 1024, false);
      const Render rB1 = renderE2(cells[ci], st, sB1, 1024, false);
      const Render rE4 = renderE2(cells[ci], st, sE4, 1024, false);
      const Render kE2 = renderE2(cells[ci], st, dryVoice[0], 1024, false);
      const Render kB1 = renderE2(cells[ci], st, dryVoice[1], 1024, false);
      double eSine, sSine, eB1, sB1, eE4, sE4, eKE2, sKE2, eKB1, sKB1;
      pitchStats(rE2.out, 0.2, 0.9, 82.41 * r, eSine, sSine);
      pitchStats(rB1.out, 0.2, 0.9, 61.74 * r, eB1, sB1);
      pitchStats(rE4.out, 0.2, 0.9, 329.63 * r, eE4, sE4);
      pitchStats(kE2.out, 1.0, 4.0, 82.41 * r, eKE2, sKE2);
      pitchStats(kB1.out, 1.0, 4.0, 61.74 * r, eKB1, sKB1);
      const int boff = static_cast<int>(kSr * 1.0), bn = static_cast<int>(kSr * 3.0);
      const double lowBen = incoherent(kB1.out.data(), boff, bn, 61.74 * r) /
          incoherent(dryVoice[1].data(), boff, bn, 61.74);
      r4R[ci] = renderE2(cells[ci], st, dryVoice[4], 1024, true);
      r5R[ci] = renderE2(cells[ci], st, dryVoice[3], 1024, true);
      {
        char vp[128];
        std::snprintf(vp, sizeof(vp), "build/labpitch/e2_%s/shift_%.0f_r4.wav", cells[ci].id, st);
        const float* vch[1] = {r4R[ci].out.data()};
        tdm::writeWavFloat32(vp, vch, 1, static_cast<int>(r4R[ci].out.size()), kSr);
        std::snprintf(vp, sizeof(vp), "build/labpitch/e2_%s/shift_%.0f_r5.wav", cells[ci].id, st);
        vch[0] = r5R[ci].out.data();
        tdm::writeWavFloat32(vp, vch, 1, static_cast<int>(r5R[ci].out.size()), kSr);
      }
      const SkipStats sk = skipStats(r4R[ci].tr, 1.0, 4.5);
      std::printf("%-4s %8d %8.2f | %+7.2f %+7.2f %+7.2f | %+7.2f %7.2f %+7.2f %7.2f %7.3f | "
                  "%5.1f %6.0f %6lld %6.3f %6.4f %6.4f\n",
                  cells[ci].id, diR[ci].latency, 1000.0 * diR[ci].latency / kSr, eSine, eB1, eE4,
                  eKE2, sKE2, eKB1, sKB1, lowBen, sk.skipRate, sk.medJ, sk.maxJ, sk.sacMax,
                  amDepth(r4R[ci].out, 1.0, 4.5), amDepth(r5R[ci].out, 1.0, 4.5));
      (void)sSine;
      (void)sB1;
      (void)sE4;
    }
    // Table 2: voicing retention + AM depth.
    auto useVoice = [&](int i) {
      return !secondary || std::string(voices[i].id) == "r4" || std::string(voices[i].id) == "maj" ||
          std::string(voices[i].id) == "lowB";
    };
    std::printf("%-4s", "cell");
    for (int i = 0; i < kNvoice; ++i)
      if (useVoice(i))
        std::printf(" | %-10s %-6s %-6s", voices[i].id, "wantMn", "amDep");
    std::printf("\n");
    for (int ci = 0; ci < kNcell; ++ci)
    {
      std::printf("%-4s", cells[ci].id);
      for (int i = 0; i < kNvoice; ++i)
      {
        if (!useVoice(i))
          continue;
        Render rr;
        if (i == 4)
          rr = r4R[ci];
        else if (i == 3)
          rr = r5R[ci];
        else
        {
          rr = renderE2(cells[ci], st, dryVoice[i], 1024, false);
          char vp[128];
          std::snprintf(vp, sizeof(vp), "build/labpitch/e2_%s/shift_%.0f_%s.wav", cells[ci].id, st,
                        voices[i].id);
          const float* vch[1] = {rr.out.data()};
          tdm::writeWavFloat32(vp, vch, 1, static_cast<int>(rr.out.size()), kSr);
        }
        double wantMin = 1e300;
        const int off = static_cast<int>(kSr * 1.0), nf = static_cast<int>(kSr * 3.0);
        for (double f : voices[i].freqs)
        {
          const double dry = incoherent(dryVoice[i].data(), off, nf, f);
          const double want = incoherent(rr.out.data(), off, nf, f * r);
          wantMin = std::min(wantMin, want / dry);
        }
        std::printf(" | %-10.3f %-6.4f %-6s", wantMin, amDepth(rr.out, 1.0, 4.5), "");
      }
      std::printf("\n");
    }
    // Table 3: DI chord/level/artifacts/attacks, A-relative where noted.
    std::printf("%-4s %-8s %-11s %-8s | %-7s %-7s | %-6s %-6s %-6s | %-6s %-6s %-7s %-5s\n", "cell",
                "wantMin", "min@", "leakMax", "rmsRat", "pkRat", "amS1", "amS6", "drop", "chugMn",
                "riffMn", "maxStep", "churn");
    const double refRms = rms(diR[0].out, 0, nDi);
    double refPeak = 0.0;
    for (float x : diR[0].out)
      refPeak = std::max(refPeak, static_cast<double>(std::fabs(x)));
    for (int ci = 0; ci < kNcell; ++ci)
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
      double chugMn = 1e300, riffMn = 1e300;
      for (int k = 0; k < 8; ++k)
        chugMn = std::min(chugMn, attackSlope(o, 4.0 + k * chugStep) /
                                         attackSlope(diR[0].out, 4.0 + k * chugStep));
      for (int k = 0; k < 16; ++k)
        riffMn = std::min(riffMn, attackSlope(o, 12.4 + k * riffStep) /
                                         attackSlope(diR[0].out, 12.4 + k * riffStep));
      const int w = static_cast<int>(kSr * 0.03);
      int drops = 0;
      for (int off = 0; off + w < nDi; off += w)
      {
        const double rr = rms(diR[0].out, off, off + w);
        if (rr > 0.03 && rms(o, off, off + w) < 0.1 * rr)
          ++drops;
      }
      const int s1a = static_cast<int>(kSr * 0.6), s1b = static_cast<int>(kSr * 1.2);
      std::printf("%-4s %-8.3f %-11s %-8.3f | %-7.4f %-7.4f | %-6.4f %-6.4f %-6d | %-6.3f %-6.3f "
                  "%-7.4f %-5.1f\n",
                  cells[ci].id, wantMin, minAt, leakMax, rms(o, 0, nDi) / refRms, peak / refPeak,
                  amDepth(o, 0.6, 1.2), amDepth(o, 14.8, 16.2), drops, chugMn, riffMn,
                  maxStep(o.data() + s1a, s1b - s1a),
                  static_cast<double>(diR[ci].tm.lagChurn) / diR[ci].tm.frames);
    }
    // Table 4: predicted per-skip residuals from measured r4+r5 jumps.
    std::printf("%-4s %-8s %-8s %-8s %-8s\n", "cell", "res5th", "res4th", "resMaj3", "nSkips");
    for (int ci = 0; ci < kNcell; ++ci)
    {
      std::vector<long long> jumps = pegJumps(r4R[ci].tr, 1.0, 4.5);
      const std::vector<long long> j5 = pegJumps(r5R[ci].tr, 1.0, 4.5);
      jumps.insert(jumps.end(), j5.begin(), j5.end());
      std::printf("%-4s %-8.3f %-8.3f %-8.3f %-8zu\n", cells[ci].id, meanResidual(jumps, 1.5, 82.41),
                  meanResidual(jumps, 4.0 / 3.0, 82.41), meanResidual(jumps, 1.2599, 82.41),
                  jumps.size());
    }
    // Table 5: CPU at live-like block 128 (mean of 3 full-DI renders).
    std::printf("%-4s %-8s %-6s\n", "cell", "cpuSec", "cpuX");
    for (int ci = 0; ci < kNcell; ++ci)
    {
      double acc = 0.0;
      for (int rep = 0; rep < 3; ++rep)
        acc += renderE2(cells[ci], st, di.samples, 128, false).seconds;
      acc /= 3.0;
      static double refCpu[3] = {0, 0, 0};
      const int si = (st == -1.0f) ? 0 : ((st == -2.0f) ? 1 : 2);
      if (ci == 0)
        refCpu[si] = acc;
      std::printf("%-4s %-8.3f %-6.2f\n", cells[ci].id, acc, acc / refCpu[si]);
    }
  }
  return 0;
}

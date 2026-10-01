// tdm_wsola_traj: WSOLA wobble-investigation trajectory instrument (LAB ONLY).
//
// Renders deterministic sustained voicings (sine + Karplus-Strong) plus
// DI slices through baseline W20 and candidate (a) with frame tracing,
// and reports per-voicing trajectory statistics: lag mean/std/range,
// jump counts, top-peak margins (ambiguity), distinct-competing-peak
// fraction, ABAB alternations, drift-run lengths, tie-break sacrifice,
// slow-AM depth and F0 spread (wobble correlates). Dumps per-frame CSVs,
// full-score landscapes for key frames, and audition WAVs.
//
//   make build/tdm_wsola_traj && ./build/tdm_wsola_traj
// Output: build/labpitch/traj/ (wavs, csvs, landscapes).

#include <algorithm>
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
constexpr double kSr = 48000.0;

float detNoise(int i, uint32_t seed = 0x12345678u)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u) ^ seed;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 2.0f * static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 1.0f;
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

// Sustained voicing: equal sine or KS notes, mixed, peak-normalized to
// 0.8, 10 ms raised-cosine fade-in (no step edge).
std::vector<float> voicing(const std::vector<double>& freqs, bool ks, double seconds, uint32_t seedBase)
{
  const int n = static_cast<int>(kSr * seconds);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (size_t v = 0; v < freqs.size(); ++v)
  {
    if (ks)
    {
      std::vector<float> note = ksNote(static_cast<float>(freqs[v]), n, 0.9998f, seedBase + v * 0x1001u);
      for (int i = 0; i < n; ++i)
        out[static_cast<size_t>(i)] += note[static_cast<size_t>(i)];
    }
    else
    {
      for (int i = 0; i < n; ++i)
        out[static_cast<size_t>(i)] +=
            static_cast<float>(std::sin(kPi * 2.0 * freqs[v] * i / kSr + v * 1.7));
    }
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

struct TracedRender
{
  std::vector<float> out; // latency-compensated
  std::vector<tdm::lab::LabWsolaShift::FrameTrace> tr;
  int latency = 0;
  int hopA = 0;
};

TracedRender renderTraced(double wms, int tolM, int tolP, float shiftSt, const std::vector<float>& in)
{
  TracedRender r;
  tdm::lab::LabWsolaShift p;
  p.setConfig(wms);
  if (tolM != 0 || tolP != 0)
    p.setSearch(tolM, tolP);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  p.enableTrace(true);
  r.latency = p.latencySamples();
  r.hopA = p.analysisHop();
  const int n = static_cast<int>(in.size());
  const int paddedN = n + r.latency + p.tailSamples();
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 0.0f);
  for (int off = 0; off < paddedN; off += 1024)
  {
    const int m = (paddedN - off) < 1024 ? (paddedN - off) : 1024;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  r.out.resize(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    r.out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + r.latency)];
  r.tr = p.trace();
  return r;
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

// Slow-AM depth: std/mean of 100 ms window RMS over [t0, t1).
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

double median(std::vector<double> v)
{
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

char scoreGlyph(double s)
{
  if (s < 0.3)
    return ' ';
  if (s < 0.5)
    return '.';
  if (s < 0.7)
    return '-';
  if (s < 0.9)
    return '+';
  if (s < 0.99)
    return '#';
  return '@';
}

struct Voicing
{
  const char* id;
  std::vector<double> freqs;
};

struct Summary
{
  int n = 0, trans = 0, peg = 0, jumps = 0, altAB = 0;
  long long maxJump = 0, lagMin = 0, lagMax = 0;
  double lagMean = 0.0, lagStd = 0.0;
  double margMean = 0.0, margMin = 1e300;
  double fracTie = 0.0, fracOutright = 0.0, fracAmbig = 0.0;
  double top1distMed = 0.0;
  double runMean = 0.0;
  int runMin = 0;
  double sacMean = 0.0, sacMax = 0.0;
};

// Sustain-frame statistics (nominal in [t0, t1), searched frames only).
Summary summarize(const std::vector<tdm::lab::LabWsolaShift::FrameTrace>& tr, int hopA, double t0, double t1)
{
  Summary s;
  std::vector<long long> lags;
  std::vector<double> dists;
  std::vector<int> runLens;
  int runLen = 0;
  long long prevBest = 0;
  bool havePrev = false, havePrev2 = false;
  long long prev2Best = 0;
  const long long jumpThresh = hopA / 4;
  for (const auto& r : tr)
  {
    if (!r.searched)
      continue;
    const double t = r.nominal / kSr;
    if (t < t0 || t >= t1)
      continue;
    ++s.n;
    lags.push_back(r.best);
    s.trans += r.isTransient ? 1 : 0;
    s.peg += r.pegged ? 1 : 0;
    const double marg = r.topScore[0] - r.topScore[1];
    s.margMean += marg;
    s.margMin = std::min(s.margMin, marg);
    s.fracTie += (marg < 1e-3) ? 1 : 0;
    s.fracOutright += (marg < 1e-6) ? 1 : 0;
    const double dist = std::fabs(static_cast<double>(r.topLag[1] - r.topLag[0]));
    dists.push_back(dist);
    s.fracAmbig += (marg < 1e-3 && dist > 50.0) ? 1 : 0;
    const double sac = r.topScore[0] - r.selScore;
    s.sacMean += sac;
    s.sacMax = std::max(s.sacMax, sac);
    if (havePrev)
    {
      const long long d = (r.best > prevBest) ? (r.best - prevBest) : (prevBest - r.best);
      if (d > jumpThresh)
        ++s.jumps;
      s.maxJump = std::max(s.maxJump, d);
      if (havePrev2 && r.best == prev2Best && r.best != prevBest)
        ++s.altAB;
    }
    prev2Best = prevBest;
    prevBest = r.best;
    havePrev2 = havePrev;
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
  if (s.n > 0)
  {
    double mean = 0.0;
    s.lagMin = s.lagMax = lags[0];
    for (long long x : lags)
    {
      mean += x;
      s.lagMin = std::min(s.lagMin, x);
      s.lagMax = std::max(s.lagMax, x);
    }
    mean /= s.n;
    s.lagMean = mean;
    double va = 0.0;
    for (long long x : lags)
      va += (x - mean) * (x - mean);
    s.lagStd = std::sqrt(va / s.n);
    s.margMean /= s.n;
    s.fracTie /= s.n;
    s.fracOutright /= s.n;
    s.fracAmbig /= s.n;
    s.sacMean /= s.n;
    s.top1distMed = median(dists);
  }
  if (!runLens.empty())
  {
    double m = 0.0;
    s.runMin = runLens[0];
    for (int x : runLens)
    {
      m += x;
      s.runMin = std::min(s.runMin, x);
    }
    s.runMean = m / runLens.size();
  }
  return s;
}

void writeCsv(const std::string& path, const std::vector<tdm::lab::LabWsolaShift::FrameTrace>& tr)
{
  std::FILE* f = std::fopen(path.c_str(), "w");
  std::fprintf(f, "frame,nominal,best,cont,prev,t0lag,t0,t1lag,t1,t2lag,t2,sel,marg01,searched,transient,"
                  "pegged\n");
  for (const auto& r : tr)
    std::fprintf(f, "%lld,%lld,%lld,%lld,%lld,%lld,%.6f,%lld,%.6f,%lld,%.6f,%.6f,%.3e,%d,%d,%d\n", r.frame,
                 r.nominal, r.best, r.cont, r.prevDelta, r.topLag[0], r.topScore[0], r.topLag[1],
                 r.topScore[1], r.topLag[2], r.topScore[2], r.selScore, r.topScore[0] - r.topScore[1],
                 r.searched ? 1 : 0, r.isTransient ? 1 : 0, r.pegged ? 1 : 0);
  std::fclose(f);
}

// Full-landscape capture for frame k: feed exactly Dp+W+k*Ha samples so
// frame k is the last one placed (deterministic prefix: identical
// decisions through k). Returns (scores ascending from -Dm, selected lag).
std::pair<std::vector<double>, long long> captureLandscape(const std::vector<float>& in, double wms, int tolM,
                                                          int tolP, float shiftSt, long long k)
{
  tdm::lab::LabWsolaShift probe;
  probe.setConfig(wms);
  if (tolM != 0 || tolP != 0)
    probe.setSearch(tolM, tolP);
  probe.setShiftSt(shiftSt);
  probe.reset(kSr);
  const int inLen = probe.tolPlus() + probe.frameLen() + static_cast<int>(k) * probe.analysisHop();
  tdm::lab::LabWsolaShift p;
  p.setConfig(wms);
  if (tolM != 0 || tolP != 0)
    p.setSearch(tolM, tolP);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  p.enableTrace(true);
  std::vector<float> dummy(static_cast<size_t>(inLen), 0.0f);
  p.processBlock(in.data(), dummy.data(), inLen);
  long long best = 0;
  for (const auto& r : p.trace())
    if (r.frame == k)
      best = r.best;
  return {p.landscape(), best};
}

void printLandscape(const std::vector<double>& land, int dm, long long best, const char* title)
{
  std::printf("%s (best=%lld, @>=0.99 #>=0.9 +=0.7 -=0.5 .>=0.3):\n", title, best);
  std::string line;
  for (size_t i = 0; i < land.size(); i += 4)
  {
    const long long lag = static_cast<long long>(i) - dm;
    double m = -2.0;
    for (size_t j = i; j < i + 4 && j < land.size(); ++j)
      m = std::max(m, land[j]);
    const bool hasBest = (best >= lag && best < lag + 4);
    line += hasBest ? '*' : scoreGlyph(m);
  }
  std::printf("  [%s]\n", line.c_str());
}
} // namespace

int main()
{
  std::filesystem::create_directories("build/labpitch/traj");
  const Voicing voices[] = {
      {"s1e2", {82.41}}, {"s1e4", {329.63}}, {"r5", {82.41, 123.47}}, {"r4", {82.41, 110.0}},
      {"maj", {82.41, 103.83, 123.47}}, {"min", {82.41, 98.0, 123.47}},
      {"dense1", {82.41, 87.31, 98.0}}, {"dense2", {164.81, 174.61, 196.0, 246.94}},
  };
  struct Cfg
  {
    const char* id;
    int tolM, tolP;
  };
  const Cfg cfgs[] = {{"ref", 0, 0}, {"a", 640, 320}, {"a800", 800, 320}, {"a960", 960, 320}};
  const float shifts[] = {-1.0f, -2.0f};

  std::printf("%-8s %-4s %-4s %-3s %4s | %-7s %-7s %-9s | %4s %6s %-7s %-7s %-7s | %-5s %-6s %-6s | "
              "%-6s %-6s\n",
              "voice", "mat", "cfg", "st", "nFr", "lagMean", "lagStd", "lagRange", "jump", "maxJmp",
              "margAvg", "tie%", "ambig%", "abab", "runAvg", "sacMax", "amDep", "f0spr");
  for (const Voicing& v : voices)
  {
    for (bool ks : {false, true})
    {
      const std::vector<float> stim = voicing(v.freqs, ks, 5.0, 0x500u);
      {
        char dryPath[128];
        std::snprintf(dryPath, sizeof(dryPath), "build/labpitch/traj/%s_%s_dry.wav", v.id,
                      ks ? "ks" : "sine");
        const float* dch[1] = {stim.data()};
        tdm::writeWavFloat32(dryPath, dch, 1, static_cast<int>(stim.size()), kSr);
      }
      for (float st : shifts)
      {
        // -2 only for the discriminating voicings (pattern-repeat check).
        if (st == -2.0f && std::string(v.id) != "r4" && std::string(v.id) != "r5" &&
            std::string(v.id) != "dense1")
          continue;
        for (const Cfg& c : cfgs)
        {
          // Dm probes only where they discriminate (same latency as a).
          if ((std::string(c.id) == "a800" || std::string(c.id) == "a960") &&
              (st != -1.0f || (std::string(v.id) != "r4" && std::string(v.id) != "r5" &&
                               std::string(v.id) != "dense1" && std::string(v.id) != "maj")))
            continue;
          TracedRender rr = renderTraced(20.0, c.tolM, c.tolP, st, stim);
          char base[128];
          std::snprintf(base, sizeof(base), "build/labpitch/traj/%s_%s_%s_m%.0f", v.id, ks ? "ks" : "sine",
                        c.id, st);
          const float* ch[1] = {rr.out.data()};
          tdm::writeWavFloat32(std::string(base) + ".wav", ch, 1, static_cast<int>(rr.out.size()), kSr);
          writeCsv(std::string(base) + ".csv", rr.tr);
          const Summary s = summarize(rr.tr, rr.hopA, 1.0, 4.5);
          const double r = std::exp2(st / 12.0);
          const double am = amDepth(rr.out, 1.0, 4.5);
          double f0spr = -1.0;
          if (v.freqs.size() == 1)
          {
            std::vector<double> pe;
            const int nf = static_cast<int>(kSr * 0.08);
            for (int off = static_cast<int>(kSr * 1.0); off + nf < static_cast<int>(kSr * 4.5); off += nf)
              pe.push_back(scanPitch(rr.out.data() + off, nf, v.freqs[0] * r));
            double lo = pe[0], hi = pe[0];
            for (double x : pe)
            {
              lo = std::min(lo, x);
              hi = std::max(hi, x);
            }
            f0spr = 100.0 * (hi - lo) / median(pe);
          }
          char range[32];
          std::snprintf(range, sizeof(range), "%lld..%lld", s.lagMin, s.lagMax);
          std::printf("%-8s %-4s %-4s %-3.0f %4d | %+7.1f %7.1f %-9s | %4d %6lld %-7.1e %-7.1f "
                      "%-7.3f | %-5d %-6.1f %-6.1e | %-6.4f %-6s\n",
                      v.id, ks ? "ks" : "sine", c.id, st, s.n, s.lagMean, s.lagStd, range, s.jumps,
                      s.maxJump, s.margMean, 100.0 * s.fracTie, s.fracAmbig, s.altAB, s.runMean, s.sacMax,
                      am, f0spr < 0 ? "-" : (std::to_string(f0spr).substr(0, 5)).c_str());
        }
      }
    }
  }
  // Landscapes: mid-sustain frame (nominal ~2.5 s) for the key contrast
  // (root+fourth vs root+fifth), sine + KS, ref + a, shift -1.
  std::printf("\nlandscapes @-1, nominal~2.5s:\n");
  for (const char* vid : {"r5", "r4", "dense1"})
  {
    for (bool ks : {false, true})
    {
      const Voicing* vp = nullptr;
      for (const Voicing& v : voices)
        if (std::string(v.id) == vid)
          vp = &v;
      const std::vector<float> stim = voicing(vp->freqs, ks, 5.0, 0x500u);
      for (const Cfg& c : cfgs)
      {
        if (std::string(c.id) == "a800" || std::string(c.id) == "a960")
          continue; // landscapes only for the ref-vs-a contrast
        const long long k = static_cast<long long>(2.5 * kSr / 480); // nominal k*Ha
        auto land = captureLandscape(stim, 20.0, c.tolM, c.tolP, -1.0f, k);
        char title[96];
        std::snprintf(title, sizeof(title), "%s %s %s", vid, ks ? "ks" : "sine", c.id);
        printLandscape(land.first, c.tolM == 0 ? 480 : c.tolM, land.second, title);
      }
    }
  }
  // DI confirmation: E5 chord slice + low-B slice with trace @-1.
  try
  {
    tdm::MonoWav di = tdm::loadWavMono("build/labpitch_di.wav");
    struct Slice
    {
      const char* id;
      double t0, t1;
    };
    for (const Slice& sl : {Slice{"diE5", 6.2, 7.1}, Slice{"diLowB", 14.4, 16.6}})
    {
      std::vector<float> seg(di.samples.begin() + static_cast<size_t>(kSr * sl.t0),
                             di.samples.begin() + static_cast<size_t>(kSr * sl.t1));
      for (const Cfg& c : cfgs)
      {
        TracedRender rr = renderTraced(20.0, c.tolM, c.tolP, -1.0f, seg);
        char base[128];
        std::snprintf(base, sizeof(base), "build/labpitch/traj/%s_%s_m1", sl.id, c.id);
        const float* ch[1] = {rr.out.data()};
        tdm::writeWavFloat32(std::string(base) + ".wav", ch, 1, static_cast<int>(rr.out.size()), kSr);
        writeCsv(std::string(base) + ".csv", rr.tr);
        const double dur = sl.t1 - sl.t0;
        const Summary s = summarize(rr.tr, rr.hopA, 0.3, dur - 0.1);
        std::printf("DI %-6s %-4s nFr=%d lagMean=%+.1f lagStd=%.1f jumps=%d maxJmp=%lld margAvg=%.1e "
                    "tie%%=%.1f ambig=%.3f abab=%d runAvg=%.1f amDep=%.4f\n",
                    sl.id, c.id, s.n, s.lagMean, s.lagStd, s.jumps, s.maxJump, s.margMean,
                    100.0 * s.fracTie, s.fracAmbig, s.altAB, s.runMean, amDepth(rr.out, 0.3, dur - 0.1));
      }
    }
  }
  catch (const std::exception& e)
  {
    std::printf("traj: DI slices skipped: %s\n", e.what());
  }
  return 0;
}

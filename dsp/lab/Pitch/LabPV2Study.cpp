// tdm_pv2_study: PV-D v2 A/B/C/D comparison harness (LAB ONLY).
//
// Renders the four research variants (A baseline, B freq-delimited
// reset, C transient dominance, D both) at shifts -1/-2 (primary) and
// -7 (DI + key voicings only, secondary stress) through the lab DI and
// deterministic KS stress voicings, writes latency-compensated WAVs to
// build/labpitch/pv2_<var>/, and prints comparison tables: exact
// latency, pitch accuracy, fund/partial retention, chord stability,
// attack preservation, level, dropouts, and CPU.
//
// Not part of `all`, not linked into any product binary:
//   make build/tdm_pv2_study && ./build/tdm_pv2_study

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
#include "dsp/lab/Pitch/LabPitchV2.h"

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

// Sustained KS voicing: equal notes struck at 0.05 s, peak-normalized
// to 0.8, 10 ms raised-cosine fade-in.
std::vector<float> ksVoicing(const std::vector<double>& freqs, double seconds, uint32_t seedBase)
{
  const int n = static_cast<int>(kSr * seconds);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  const int s0 = static_cast<int>(kSr * 0.05);
  for (size_t v = 0; v < freqs.size(); ++v)
  {
    std::vector<float> note = ksNote(static_cast<float>(freqs[v]), n - s0, 0.9995f, seedBase + v * 0x1001u);
    for (size_t i = 0; i < note.size(); ++i)
      out[static_cast<size_t>(s0) + i] += note[i];
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

struct Render
{
  std::vector<float> out;
  int latency = 0;
  double seconds = 0.0;
};

Render renderPV2(tdm::lab::LabPitchV2::Mode mode, float shiftSt, const std::vector<float>& in, int block)
{
  Render r;
  tdm::lab::LabPitchV2 p;
  p.setConfig(2048, 256);
  p.setMode(mode);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
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
  namespace pv2 = tdm::lab;
  const pv2::LabPitchV2::Mode modes[] = {
      pv2::LabPitchV2::Mode::A_Baseline, pv2::LabPitchV2::Mode::B_FreqReset,
      pv2::LabPitchV2::Mode::C_TimeAnchor, pv2::LabPitchV2::Mode::D_Both,
  };
  const char* modeIds[] = {"A", "B", "C", "D"};
  const float shifts[] = {-1.0f, -2.0f, -7.0f};

  tdm::MonoWav di;
  try
  {
    di = tdm::loadWavMono("build/labpitch_di.wav");
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_pv2_study: need build/labpitch_di.wav: %s\n", e.what());
    return 1;
  }
  const int nDi = static_cast<int>(di.samples.size());

  const Voicing voices[] = {
      {"lowE", {82.41}}, {"lowB", {61.74}}, {"r5", {82.41, 123.47}}, {"r4", {82.41, 110.0}},
      {"rmaj3", {82.41, 103.83}}, {"rmin3", {82.41, 98.0}}, {"maj", {82.41, 103.83, 123.47}},
      {"min", {82.41, 98.0, 123.47}}, {"dense", {82.41, 87.31, 98.0, 103.83}},
  };
  constexpr int kNvoice = sizeof(voices) / sizeof(voices[0]);
  std::vector<std::vector<float>> dryVoice(kNvoice);
  for (int i = 0; i < kNvoice; ++i)
    dryVoice[i] = ksVoicing(voices[i].freqs, 4.0, 0x600u + i);
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

  for (float st : shifts)
  {
    const double r = std::exp2(st / 12.0);
    const bool secondary = (st == -7.0f);
    std::printf("\n=== shift %.0f (r=%.5f)%s ===\n", st, r, secondary ? " [secondary stress]" : "");
    // Table 1: latency + pitch oracles + low-string retention.
    std::printf("%-4s %8s %8s | %-7s %-7s %-7s | %-7s %-7s %-7s %-7s %-7s\n", "var", "L", "ms", "sinE2%",
                "sinB1%", "sinE4%", "ksE2%", "ksE2sp%", "ksB1%", "ksB1sp%", "lowBen");
    Render diR[4];
    for (int vi = 0; vi < 4; ++vi)
    {
      diR[vi] = renderPV2(modes[vi], st, di.samples, 1024);
      std::filesystem::create_directories("build/labpitch/pv2_" + std::string(modeIds[vi]));
      char path[128];
      std::snprintf(path, sizeof(path), "build/labpitch/pv2_%s/shift_%.0f_di.wav", modeIds[vi], st);
      const float* ch[1] = {diR[vi].out.data()};
      tdm::writeWavFloat32(path, ch, 1, nDi, kSr);
      const Render rE2 = renderPV2(modes[vi], st, sE2, 1024);
      const Render rB1 = renderPV2(modes[vi], st, sB1, 1024);
      const Render rE4 = renderPV2(modes[vi], st, sE4, 1024);
      const Render kE2 = renderPV2(modes[vi], st, dryVoice[0], 1024);
      const Render kB1 = renderPV2(modes[vi], st, dryVoice[1], 1024);
      double eSine, sSine, eB1, sB1, eE4, sE4, eKE2, sKE2, eKB1, sKB1;
      pitchStats(rE2.out, 0.2, 0.9, 82.41 * r, eSine, sSine);
      pitchStats(rB1.out, 0.2, 0.9, 61.74 * r, eB1, sB1);
      pitchStats(rE4.out, 0.2, 0.9, 329.63 * r, eE4, sE4);
      pitchStats(kE2.out, 0.5, 3.5, 82.41 * r, eKE2, sKE2);
      pitchStats(kB1.out, 0.5, 3.5, 61.74 * r, eKB1, sKB1);
      const int boff = static_cast<int>(kSr * 1.0), bn = static_cast<int>(kSr * 2.0);
      const double lowBen = incoherent(kB1.out.data(), boff, bn, 61.74 * r) /
          incoherent(dryVoice[1].data(), boff, bn, 61.74);
      std::printf("%-4s %8d %8.2f | %+7.2f %+7.2f %+7.2f | %+7.2f %7.2f %+7.2f %7.2f %7.3f\n",
                  modeIds[vi], diR[vi].latency, 1000.0 * diR[vi].latency / kSr, eSine, eB1, eE4, eKE2,
                  sKE2, eKB1, sKB1, lowBen);
      (void)sSine;
      (void)sB1;
      (void)sE4;
    }
    // Table 2: voicing chord retention + stability + wander.
    auto useVoice = [&](int i) {
      return !secondary || std::string(voices[i].id) == "r4" || std::string(voices[i].id) == "maj";
    };
    std::printf("%-4s", "var");
    for (int i = 2; i < kNvoice; ++i)
      if (useVoice(i))
        std::printf(" | %-10s %-6s %-6s", voices[i].id, "wantMn", "amDep");
    std::printf(" | %-6s\n", "wandB");
    for (int vi = 0; vi < 4; ++vi)
    {
      std::printf("%-4s", modeIds[vi]);
      for (int i = 2; i < kNvoice; ++i)
      {
        if (!useVoice(i))
          continue;
        const Render rr = renderPV2(modes[vi], st, dryVoice[i], 1024);
        {
          char path[128];
          std::snprintf(path, sizeof(path), "build/labpitch/pv2_%s/shift_%.0f_%s.wav", modeIds[vi], st,
                        voices[i].id);
          const float* ch[1] = {rr.out.data()};
          tdm::writeWavFloat32(path, ch, 1, static_cast<int>(rr.out.size()), kSr);
        }
        double wantMin = 1e300;
        const int off = static_cast<int>(kSr * 1.0), nf = static_cast<int>(kSr * 2.0);
        for (double f : voices[i].freqs)
          wantMin = std::min(wantMin, incoherent(rr.out.data(), off, nf, f * r) /
                                           incoherent(dryVoice[i].data(), off, nf, f));
        std::printf(" | %-10.3f %-6.4f", wantMin, amDepth(rr.out, 1.0, 3.5));
      }
      // Low-B wander index (coherent/incoherent at shifted B1).
      const Render kB1 = renderPV2(modes[vi], st, dryVoice[1], 1024);
      const int boff = static_cast<int>(kSr * 1.0), bn = static_cast<int>(kSr * 2.0);
      const double wand = goertzel(kB1.out.data() + boff, bn, 61.74 * r) /
          incoherent(kB1.out.data(), boff, bn, 61.74 * r);
      std::printf(" | %-6.3f\n", wand);
    }
    // Table 3: DI chords + attacks + level + dropouts + clicks.
    std::printf("%-4s %-8s %-8s | %-7s %-7s | %-6s %-6s %-6s | %-7s %-7s %-7s\n", "var", "wantMin",
                "leakMax", "rmsRat", "pkRat", "chugPk", "chugTm", "riffPk", "drop", "maxStep", "finite");
    const double refRms = rms(diR[0].out, 0, nDi);
    double refPeak = 0.0;
    for (float x : diR[0].out)
      refPeak = std::max(refPeak, static_cast<double>(std::fabs(x)));
    for (int vi = 0; vi < 4; ++vi)
    {
      const std::vector<float>& o = diR[vi].out;
      double wantMin = 1e300, leakMax = 0.0;
      for (const Chord& chd : chords)
      {
        const double funds[3] = {chd.root, chd.root * 1.4983, chd.root * 2.0};
        const int off = static_cast<int>(kSr * chd.t0);
        const int nf = static_cast<int>(kSr * (chd.t1 - chd.t0));
        for (double f : funds)
        {
          const double dry = incoherent(di.samples.data(), off, nf, f);
          wantMin = std::min(wantMin, incoherent(o.data(), off, nf, f * r) / dry);
          bool collide = false;
          for (double g : funds)
            for (int k = 1; k <= 6; ++k)
              if (std::fabs(k * g * r - f) < 4.0)
                collide = true;
          if (!collide)
            leakMax = std::max(leakMax, goertzel(o.data() + off, nf, f) / dry);
        }
      }
      double peak = 0.0;
      for (float x : o)
        peak = std::max(peak, static_cast<double>(std::fabs(x)));
      // Chug attack peaks: max |HP| in [onset, onset+20ms], ratio vs A.
      double chugPk = 0.0;
      int nCh = 0;
      for (int k = 0; k < 8; ++k)
      {
        const int a = static_cast<int>(kSr * (4.0 + k * chugStep));
        const int b = a + static_cast<int>(kSr * 0.02);
        double mo = 0.0, ma = 0.0;
        for (int i = a + 1; i < b; ++i)
        {
          mo = std::max(mo, static_cast<double>(std::fabs(o[static_cast<size_t>(i)] - o[i - 1])));
          ma = std::max(ma, static_cast<double>(std::fabs(diR[0].out[i] - diR[0].out[i - 1])));
        }
        if (ma > 1e-6)
        {
          chugPk += mo / ma;
          ++nCh;
        }
      }
      chugPk /= nCh;
      // Chug timing: main HP peak displacement vs A (mean |ms|).
      double chugTm = 0.0;
      for (int k = 0; k < 8; ++k)
      {
        const int c = static_cast<int>(kSr * (4.0 + k * chugStep));
        auto argpeak = [&](const std::vector<float>& v) {
          double best = 0.0;
          int at = c;
          for (int i = c; i < c + static_cast<int>(kSr * 0.03); ++i)
          {
            const double s = std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)]);
            if (s > best)
            {
              best = s;
              at = i;
            }
          }
          return at;
        };
        chugTm += std::fabs(argpeak(o) - argpeak(diR[0].out)) / kSr * 1000.0;
      }
      chugTm /= 8.0;
      // Riff note peaks: mean ratio vs A over 16 notes.
      double riffPk = 0.0;
      for (int k = 0; k < 16; ++k)
      {
        const int c = static_cast<int>(kSr * (12.4 + k * 0.1));
        double mo = 0.0, ma = 0.0;
        for (int i = c; i < c + static_cast<int>(kSr * 0.03); ++i)
        {
          mo = std::max(mo, static_cast<double>(std::fabs(o[static_cast<size_t>(i)])));
          ma = std::max(ma, static_cast<double>(std::fabs(diR[0].out[i])));
        }
        riffPk += mo / (ma + 1e-9);
      }
      riffPk /= 16.0;
      // Dropouts vs A: A hot (> -30 dBFS) but variant < 1/10.
      int drop = 0;
      const int w = static_cast<int>(kSr * 0.03);
      for (int off = 0; off + w < nDi; off += w)
      {
        const double ra = rms(diR[0].out, off, off + w);
        if (ra > 0.03 && rms(o, off, off + w) < 0.1 * ra)
          ++drop;
      }
      double ms = 0.0;
      bool fin = true;
      for (int i = 1; i < nDi; ++i)
      {
        const float x = o[static_cast<size_t>(i)];
        if (!std::isfinite(x))
          fin = false;
        ms = std::max(ms, static_cast<double>(std::fabs(x - o[static_cast<size_t>(i - 1)])));
      }
      std::printf("%-4s %-8.3f %-8.3f | %-7.4f %-7.4f | %-6.3f %-6.2f %-6.3f | %-7d %-7.4f %-7s\n",
                  modeIds[vi], wantMin, leakMax, rms(o, 0, nDi) / refRms, peak / refPeak, chugPk, chugTm,
                  riffPk, drop, ms, fin ? "yes" : "NO");
    }
    // Table 4: CPU at live-like block 128 (mean of 3 full-DI renders).
    std::printf("%-4s %-8s %-6s\n", "var", "cpuSec", "cpuX");
    double refCpu = 0.0;
    for (int vi = 0; vi < 4; ++vi)
    {
      double acc = 0.0;
      for (int rep = 0; rep < 3; ++rep)
        acc += renderPV2(modes[vi], st, di.samples, 128).seconds;
      acc /= 3.0;
      if (vi == 0)
        refCpu = acc;
      std::printf("%-4s %-8.3f %-6.2f\n", modeIds[vi], acc, acc / refCpu);
    }
  }
  return 0;
}

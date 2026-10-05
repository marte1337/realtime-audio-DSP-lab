#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "dsp/Pitch/GuitarTranspose.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979;

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

std::vector<float> ksPluck(float peak, float freqHz, int n, uint32_t seed = 0x51ab3u)
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
    const float v = 0.9998f * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = peak * cur;
  }
  return out;
}

double goertzelPower(const float* v, int n, double f)
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
  const double m = std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
  return m * m;
}

// Latency-compensated offline run (pad lat+tail, drop lat).
std::vector<float> runGt2(float shiftSt, const std::vector<float>& in, int block = 256,
                           const tdm::GuitarTranspose::Config* cfg = nullptr)
{
  tdm::GuitarTranspose p;
  if (cfg)
    p.setConfig(*cfg);
  p.setEnabled(true);
  p.setShiftSt(shiftSt);
  p.reset(kSr);
  const int lat = p.latencySamples();
  const int n = static_cast<int>(in.size());
  std::vector<float> padded(static_cast<size_t>(n + lat), 0.0f);
  for (int i = 0; i < n; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(n + lat), 0.0f);
  for (int off = 0; off < n + lat; off += block)
  {
    const int m = (n + lat - off) < block ? (n + lat - off) : block;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  return out;
}

double peakNear(const std::vector<float>& v, int from, int n, double target)
{
  double best = target, bm = -1.0;
  const double step = target * 0.001;
  for (double f = target * 0.97; f <= target * 1.03; f += step)
  {
    const double m = goertzelPower(v.data() + from, n, f);
    if (m > bm)
    {
      bm = m;
      best = f;
    }
  }
  return best;
}

float maxStep(const std::vector<float>& v, int from, int to)
{
  float m = 0.0f;
  for (int i = from + 1; i < to; ++i)
  {
    const float s = std::fabs(v[static_cast<size_t>(i)] - v[static_cast<size_t>(i - 1)]);
    if (s > m)
      m = s;
  }
  return m;
}

double rms(const std::vector<float>& v, int from, int to)
{
  double se = 0.0;
  for (int i = from; i < to; ++i)
    se += static_cast<double>(v[static_cast<size_t>(i)]) * v[static_cast<size_t>(i)];
  return std::sqrt(se / (to - from));
}

// ---- Golden regression inputs (lab->production promotion guard) ----
// Fixed synthesis shared with the one-shot generator (/tmp/gt2golden.cpp,
// run against the accepted pre-move baseline). Do not change these without
// regenerating the hashes from the accepted engine.
uint32_t goldenLcg(uint32_t& s)
{
  s = s * 1664525u + 1013904223u;
  return s;
}

std::vector<float> goldenNote(int n)
{
  const int period = static_cast<int>(kSr / 82.41 + 0.5);
  uint32_t s = 0x51ab3u;
  std::vector<float> line(static_cast<size_t>(period));
  for (int i = 0; i < period; ++i)
    line[static_cast<size_t>(i)] = 2.0f * (goldenLcg(s) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f;
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  int idx = 0;
  float prev = 0.0f;
  for (int i = 0; i < n; ++i)
  {
    const float cur = line[static_cast<size_t>(idx)];
    const float v = 0.9998f * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = 0.8f * cur;
  }
  return out;
}

std::vector<float> goldenChord(int n)
{
  const double freqs[3] = {82.41, 110.0, 146.83};
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (int v = 0; v < 3; ++v)
  {
    const int period = static_cast<int>(kSr / freqs[v] + 0.5);
    uint32_t s = 0x1000u + uint32_t(v * 77 + 1);
    std::vector<float> line(static_cast<size_t>(period));
    for (int i = 0; i < period; ++i)
      line[static_cast<size_t>(i)] = 2.0f * (goldenLcg(s) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f;
    int idx = 0;
    float prev = 0.0f;
    for (int i = 0; i < n; ++i)
    {
      const float cur = line[static_cast<size_t>(idx)];
      const float decay = 0.9998f - 0.00002f * v;
      const float nv = decay * 0.5f * (cur + prev);
      prev = cur;
      line[static_cast<size_t>(idx)] = nv;
      idx = (idx + 1) % period;
      out[static_cast<size_t>(i)] += 0.3f * cur;
    }
  }
  return out;
}

std::vector<float> goldenRiff(int n)
{
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  const int period = static_cast<int>(kSr / 82.41 + 0.5);
  const int chugLen = static_cast<int>(kSr * 0.125);
  uint32_t s = 0x77aa1u;
  for (int c = 0; c * chugLen < n; ++c)
  {
    std::vector<float> line(static_cast<size_t>(period));
    for (int i = 0; i < period; ++i)
      line[static_cast<size_t>(i)] = 2.0f * (goldenLcg(s) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f;
    int idx = 0;
    float prev = 0.0f;
    const float amp = (c % 4 == 3) ? 0.9f : 0.6f;
    for (int i = 0; i < chugLen && c * chugLen + i < n; ++i)
    {
      const float cur = line[static_cast<size_t>(idx)];
      const float nv = 0.996f * 0.5f * (cur + prev);
      prev = cur;
      line[static_cast<size_t>(idx)] = nv;
      idx = (idx + 1) % period;
      const float env = float(i < 96 ? i : 96) / 96.0f;
      out[static_cast<size_t>(c * chugLen + i)] += amp * env * cur;
    }
  }
  for (int i = 0; i < 480 && n - 960 - 480 + i < n; ++i)
    out[static_cast<size_t>(n - 960 - 480 + i)] +=
        0.5f * (2.0f * (goldenLcg(s) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f);
  return out;
}

uint64_t goldenHash(const std::vector<float>& v)
{
  uint64_t h = 1469598103934665603ULL;
  const auto* p = reinterpret_cast<const unsigned char*>(v.data());
  for (size_t i = 0; i < v.size() * sizeof(float); ++i)
  {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
} // namespace

void runGuitarTransposeTests()
{
  using Gt2 = tdm::GuitarTranspose;
  // Config validation at reset.
  {
    Gt2 p;
    Gt2::Config c;
    c.windowMs = c.floorMs;
    p.setConfig(c);
    bool threw = false;
    try
    {
      p.reset(kSr);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "window==floor must throw");
    c = Gt2::Config{};
    c.fadeMaxMs = c.fadeMinMs - 1.0;
    p.setConfig(c);
    threw = false;
    try
    {
      p.reset(kSr);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "fadeMax<fadeMin must throw");
    p.setConfig(Gt2::Config{});
    threw = false;
    try
    {
      p.reset(1000.0);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad rate must throw");
  }
  // Silence: finite zeros, no crash, no runaway telemetry.
  {
    const std::vector<float> zeros(static_cast<size_t>(kSr), 0.0f);
    const auto out = runGt2(-2.0f, zeros);
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "silence finite");
    TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(out.size())) < 1e-6f, "silence stays silent");
  }
  // Disabled + exact-0 bypass copy bit-exactly.
  {
    Gt2 p;
    p.reset(kSr); // never enabled
    const auto in = sine(0.5f, 220.0f, 2048);
    std::vector<float> out(2048);
    p.processBlock(in.data(), out.data(), 2048);
    TDM_CHECK(in == out, "disabled copies exactly");
    Gt2 q;
    q.setEnabled(true);
    q.setShiftSt(0.0f);
    q.reset(kSr);
    TDM_CHECK(q.latencySamples() == 0, "0st latency 0");
    q.processBlock(in.data(), out.data(), 2048);
    TDM_CHECK(in == out, "0st copies exactly");
  }
  // Determinism: same render twice bit-identical; block size irrelevant.
  {
    auto chord = ksPluck(0.5f, 82.41f, static_cast<int>(2 * kSr));
    const auto third = ksPluck(0.5f, 103.83f, static_cast<int>(2 * kSr), 0x77u);
    const auto fifth = ksPluck(0.5f, 123.47f, static_cast<int>(2 * kSr), 0x99u);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] = (chord[i] + third[i] + fifth[i]) / 3.0f;
    const auto a = runGt2(-2.0f, chord, 256);
    const auto b = runGt2(-2.0f, chord, 256);
    TDM_CHECK(a == b, "repeat renders bit-identical");
    const auto c = runGt2(-2.0f, chord, 64);
    const auto d = runGt2(-2.0f, chord, 1024);
    TDM_CHECK(a == c, "block-64 identical");
    TDM_CHECK(a == d, "block-1024 identical");
  }
  // Pitch: -12/-2/-1 land exactly (peak scan within 0.3%).
  for (const float st : {-12.0f, -2.0f, -1.0f})
  {
    const auto in = sine(0.5f, 440.0f, static_cast<int>(3 * kSr));
    const auto out = runGt2(st, in);
    const double want = 440.0 * std::pow(2.0, st / 12.0);
    const double got = peakNear(out, 48000, 65536, want);
    TDM_CHECK_CLOSE(got, want, want * 0.003, "pitch exact at " + std::to_string(st));
    const double atWant = goertzelPower(out.data() + 48000, 65536, want);
    const double atDry = goertzelPower(out.data() + 48000, 65536, 440.0);
    TDM_CHECK(atWant > atDry * 100.0, "energy moved at " + std::to_string(st));
  }
  // Unity gain on a shifted steady tone.
  {
    const auto in = sine(0.5f, 220.0f, static_cast<int>(3 * kSr));
    const auto out = runGt2(-2.0f, in);
    const double want = 220.0 * std::pow(2.0, -2.0 / 12.0);
    const double gOut = goertzelPower(out.data() + 48000, 65536, want);
    const double gIn = goertzelPower(in.data() + 48000, 65536, 220.0);
    TDM_CHECK_CLOSE(10.0 * std::log10(gOut / gIn), 0.0, 1.0, "unity gain -2");
  }
  // Sustained single note: no dropouts, rare large matched splices.
  {
    const auto in = ksPluck(0.8f, 82.41f, static_cast<int>(5 * kSr));
    Gt2 p;
    p.setEnabled(true);
    p.setShiftSt(-2.0f);
    p.reset(kSr);
    const int lat = p.latencySamples();
    const int n = static_cast<int>(in.size());
    std::vector<float> padded(static_cast<size_t>(n + lat), 0.0f);
    for (int i = 0; i < n; ++i)
      padded[static_cast<size_t>(i)] = in[i];
    std::vector<float> raw(static_cast<size_t>(n + lat), 0.0f);
    for (int off = 0; off < n + lat; off += 256)
    {
      const int m = (n + lat - off) < 256 ? (n + lat - off) : 256;
      p.processBlock(padded.data() + off, raw.data() + off, m);
    }
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "sustain finite");
    for (int end = 48000; end <= n; end += 960)
    {
      const double rr = rms(in, end - 960, end);
      TDM_CHECK(rms(out, end - 960, end) > 0.05 * rr, "no dropout window");
    }
    const auto t = p.telemetry();
    TDM_CHECK(t.driftSplicesDown >= 5, "sustains splice");
    TDM_CHECK(t.driftSplicesDown <= 60, "splices stay rare");
    const double meanJump = t.jumpSum / std::max(1LL, t.jumpCount);
    TDM_CHECK(meanJump > 500.0, "jumps are large");
    TDM_CHECK(t.nccSum / std::max(1LL, t.jumpCount) > 0.85, "single-note matches strong");
    TDM_CHECK(t.droppedFades == 0, "no dropped fades on sustain");
    TDM_CHECK(t.syncFallbacks == 0, "no sync fallback on steady run");
  }
  // Chord render: no clicks (step bounded vs dry), finite.
  {
    auto chord = ksPluck(0.5f, 82.41f, static_cast<int>(4 * kSr));
    const auto third = ksPluck(0.5f, 103.83f, static_cast<int>(4 * kSr), 0x77u);
    const auto fifth = ksPluck(0.5f, 123.47f, static_cast<int>(4 * kSr), 0x99u);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] = (chord[i] + third[i] + fifth[i]) / 3.0f;
    const auto out = runGt2(-2.0f, chord);
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "chord finite");
    TDM_CHECK(maxStep(out, 48000, static_cast<int>(out.size())) < 3.0f * maxStep(chord, 48000, static_cast<int>(chord.size())),
              "no clicks on chord");
  }
  // Onset re-sync: burst after a drifting sustain emerges near the floor.
  // Raw (uncompensated) render: lag IS the emergence latency here.
  {
    Gt2 probe;
    probe.setEnabled(true);
    probe.setShiftSt(-2.0f);
    probe.reset(kSr);
    const int floor = probe.floorSamples();
    const int oLat = probe.onsetLatencySamples();
    for (const int holdMs : {700, 1600})
    {
      const int hold = holdMs * 48;
      auto in = sine(0.1f, 110.0f, hold + 24000);
      for (int i = 0; i < 24000; ++i)
        in[static_cast<size_t>(hold + i)] = 0.9f * detNoise(i, 11u);
      Gt2 p;
      p.setEnabled(true);
      p.setShiftSt(-2.0f);
      p.reset(kSr);
      std::vector<float> out(in.size());
      for (size_t off = 0; off < in.size(); off += 256)
      {
        const int m = (in.size() - off) < 256 ? static_cast<int>(in.size() - off) : 256;
        p.processBlock(in.data() + off, out.data() + off, m);
      }
      const auto winRms = [&](int end) {
        double acc = 0.0;
        for (int i = end - 48; i < end; ++i)
          acc += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
        return std::sqrt(acc / 48.0);
      };
      int arrival = -1;
      for (int end = hold + 48; end < hold + 24000 && arrival < 0; ++end)
        if (winRms(end) > 0.3)
          arrival = end;
      TDM_CHECK(arrival > 0, "burst arrives");
      if (arrival > 0)
      {
        const int lag = arrival - hold;
        TDM_CHECK(lag >= floor - 48, "re-sync not impossibly early");
        TDM_CHECK(lag <= oLat + 192, "re-sync near floor");
      }
      TDM_CHECK(p.telemetry().resyncs >= 1, "burst re-synced");
    }
  }
  // Detector discipline: steady lows and beating dyads barely fire.
  {
    const auto lowE = ksPluck(0.8f, 82.41f, static_cast<int>(4 * kSr));
    const auto lowB = ksPluck(0.8f, 61.74f, static_cast<int>(4 * kSr), 0x33u);
    auto dyad = ksPluck(0.5f, 82.41f, static_cast<int>(4 * kSr), 0x44u);
    const auto dyad2 = ksPluck(0.5f, 123.47f, static_cast<int>(4 * kSr), 0x55u);
    for (size_t i = 0; i < dyad.size(); ++i)
      dyad[i] += dyad2[i];
    long long edges = 0;
    const std::vector<float>* stims[] = {&lowE, &lowB, &dyad};
    for (const auto* stim : stims)
    {
      Gt2 p;
      p.setEnabled(true);
      p.setShiftSt(-2.0f);
      p.reset(kSr);
      std::vector<float> out(stim->size());
      for (size_t off = 0; off < stim->size(); off += 256)
      {
        const int m = (stim->size() - off) < 256 ? static_cast<int>(stim->size() - off) : 256;
        p.processBlock(stim->data() + off, out.data() + off, m);
      }
      edges += p.telemetry().detectorEdges;
    }
    TDM_CHECK(edges <= 12, "detector quiet on sustains");
  }
  // Upshift mirror smoke: +4/+12 land on pitch.
  for (const float st : {4.0f, 12.0f})
  {
    const auto in = sine(0.5f, 440.0f, static_cast<int>(3 * kSr));
    const auto out = runGt2(st, in);
    const double want = 440.0 * std::pow(2.0, st / 12.0);
    const double got = peakNear(out, 48000, 65536, want);
    TDM_CHECK_CLOSE(got, want, want * 0.005, "upshift on pitch at " + std::to_string(st));
  }
  // Extremes stay finite and bounded.
  for (const float st : {-24.0f, 12.0f})
  {
    std::vector<float> noise(static_cast<size_t>(kSr));
    for (size_t i = 0; i < noise.size(); ++i)
      noise[i] = 0.5f * detNoise(static_cast<int>(i), 0x5eed);
    const auto out = runGt2(st, noise);
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "extreme finite");
    TDM_CHECK(tdm_test::peakAbs(out.data(), static_cast<int>(out.size())) < 10.0f, "extreme bounded");
  }
  // Live shift change mid-stream: finite, no throw, second half retunes.
  {
    Gt2 p;
    p.setEnabled(true);
    p.setShiftSt(-1.0f);
    p.reset(kSr);
    const auto in = sine(0.5f, 220.0f, static_cast<int>(3 * kSr));
    std::vector<float> out(in.size());
    const int half = static_cast<int>(in.size()) / 2;
    for (int off = 0; off < half; off += 256)
    {
      const int m = (half - off) < 256 ? (half - off) : 256;
      p.processBlock(in.data() + off, out.data() + off, m);
    }
    p.setShiftSt(-2.0f); // live change, no reset
    for (int off = half; off < static_cast<int>(in.size()); off += 256)
    {
      const int m = (static_cast<int>(in.size()) - off) < 256 ? (static_cast<int>(in.size()) - off) : 256;
      p.processBlock(in.data() + off, out.data() + off, m);
    }
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "sweep finite");
    const double want2 = 220.0 * std::pow(2.0, -2.0 / 12.0);
    const double got2 = peakNear(out, half + 24000, 32768, want2);
    TDM_CHECK_CLOSE(got2, want2, want2 * 0.02, "second half retunes to -2");
  }
  // Golden regression: bit-exact output hashes from the accepted baseline
  // (single note / chord / transient riff at -1, -2, -7). The lab ->
  // production promotion must not change one bit of these renders.
  {
    struct GoldenCase
    {
      const char* name;
      std::vector<float> in;
      uint64_t wantMinus1;
      uint64_t wantMinus2;
      uint64_t wantMinus7;
    };
    const int n = 96000;
    std::vector<GoldenCase> cases;
    cases.push_back({"note", goldenNote(n), 0x7ca5dc941a2adb04ULL, 0x702b62a772194333ULL, 0xf342427a059ea649ULL});
    cases.push_back({"chord", goldenChord(n), 0xa65cd4e9b8199f96ULL, 0xac0d253940f5f9d0ULL, 0x0d9794e0dd310a97ULL});
    cases.push_back({"riff", goldenRiff(n), 0x440ca1e93ed492c9ULL, 0xcae6e2d90fdb7995ULL, 0xcae52ee449b74be1ULL});
    const float shifts[3] = {-1.0f, -2.0f, -7.0f};
    for (const auto& c : cases)
    {
      const uint64_t wants[3] = {c.wantMinus1, c.wantMinus2, c.wantMinus7};
      for (int s = 0; s < 3; ++s)
      {
        Gt2 p;
        p.setEnabled(true);
        p.setShiftSt(shifts[s]);
        p.reset(kSr);
        TDM_CHECK(p.latencySamples() == 768, "golden nominal latency 768");
        std::vector<float> out(static_cast<size_t>(n));
        for (int off = 0; off < n; off += 256)
          p.processBlock(c.in.data() + off, out.data() + off, 256);
        TDM_CHECK(goldenHash(out) == wants[s],
                  std::string("golden bit-exact ") + c.name + " " + std::to_string(int(shifts[s])));
      }
    }
  }
}

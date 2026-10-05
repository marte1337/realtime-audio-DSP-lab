#include "tests/Assert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "dsp/Pitch/GuitarTranspose.h"
#include "dsp/TechDeathRig.h"
#include "dsp/TightDrive/TightDrive.h"
#include "dsp/TransposeInsert.h"
#include "dsp/WavFile.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979;
constexpr int kNominalLat = 768; // baseline (2 ms + 30 ms) / 2 @ 48 kHz

std::vector<float> sine(float peak, float freqHz, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / kSr));
  return out;
}

std::vector<float> noise(float peak, int n, uint32_t seed = 0x12345678u)
{
  std::vector<float> out(static_cast<size_t>(n));
  uint32_t x = seed;
  for (int i = 0; i < n; ++i)
  {
    x = x * 1664525u + 1013904223u;
    out[static_cast<size_t>(i)] = peak * (2.0f * float((x >> 8) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f);
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

// Mono rig run in fixed blocks; everything else default unless set on rig.
std::vector<float> runRig(tdm::TechDeathRig& rig, const std::vector<float>& in, int block)
{
  std::vector<float> out(in.size(), 0.0f);
  for (size_t off = 0; off < in.size(); off += static_cast<size_t>(block))
  {
    const int m = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - off));
    const float* bi[1] = {in.data() + off};
    float* bo[1] = {out.data() + off};
    rig.processBlock(bi, 1, bo, 1, m);
  }
  return out;
}

// Test-only DEV insert substitute (proves seam precedence, not DSP).
class ZeroInsert : public tdm::TransposeInsert
{
public:
  void reset(double, int) override {}
  void process(const float*, float* output, int numFrames) override
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f;
  }
  int latencySamples() const override { return 0; }
};
} // namespace

void runTransposeTests()
{
  using Gt = tdm::GuitarTranspose;
  // Production range constants.
  TDM_CHECK(Gt::kProductionMinShiftSt == -12.0f && Gt::kProductionMaxShiftSt == 12.0f,
            "production range is -12..+12");
  // Defaults + latency reporting.
  {
    tdm::TechDeathRig rig;
    TDM_CHECK(!rig.isTransposeEnabled() && rig.transposeSemitones() == 0.0f, "transpose defaults off/0");
    TDM_CHECK(rig.transposeLatencySamples() == 0, "latency 0 pre-reset");
    tdm::RigParams p = rig.params();
    TDM_CHECK(!p.transposeEnabled && p.transposeSemitones == 0.0f, "params mirror defaults");
    rig.reset(kSr, 512);
    TDM_CHECK(rig.transposeLatencySamples() == kNominalLat, "nominal latency 768 @ 48 kHz");
  }
  // Clamping + non-finite params (stored values only; no audio needed).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.setTransposeSemitones(100.0f);
    TDM_CHECK(rig.transposeSemitones() == 12.0f, "clamps to +12");
    rig.setTransposeSemitones(-100.0f);
    TDM_CHECK(rig.transposeSemitones() == -12.0f, "clamps to -12");
    rig.setTransposeSemitones(std::numeric_limits<float>::quiet_NaN());
    TDM_CHECK(rig.transposeSemitones() == 0.0f, "NaN parks at 0");
    rig.setTransposeSemitones(std::numeric_limits<float>::infinity());
    TDM_CHECK(rig.transposeSemitones() == 12.0f, "+Inf clamps to +12");
    rig.setTransposeSemitones(-std::numeric_limits<float>::infinity());
    TDM_CHECK(rig.transposeSemitones() == -12.0f, "-Inf clamps to -12");
    rig.setTransposeSemitones(-2.5f);
    TDM_CHECK(rig.transposeSemitones() == -2.5f, "fractional stored exactly");
    tdm::RigParams p;
    p.transposeEnabled = true;
    p.transposeSemitones = -7.0f;
    rig.setParams(p);
    TDM_CHECK(rig.isTransposeEnabled() && rig.transposeSemitones() == -7.0f, "setParams carries transpose");
  }
  // Disengaged transpose is an exact wire (even with a shift dialed in).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    rig.setTransposeSemitones(-7.0f); // set but not enabled
    const auto in = noise(0.7f, 48000);
    const auto out = runRig(rig, in, 256);
    TDM_CHECK(in == out, "disengaged is bit-exact wire");
  }
  // Integer shifts land on pitch through the production chain.
  for (const float st : {-1.0f, -2.0f, -7.0f})
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(st);
    const auto in = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    const auto out = runRig(rig, in, 512);
    TDM_CHECK(tdm_test::allFinite(out.data(), static_cast<int>(out.size())), "shifted finite");
    const double want = 220.0 * std::pow(2.0, st / 12.0);
    const double got = peakNear(out, 48000, 32768, want);
    TDM_CHECK_CLOSE(got, want, want * 0.005, "integer shift on pitch");
  }
  // Fractional shift renders on pitch.
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.5f);
    const auto in = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    const auto out = runRig(rig, in, 512);
    const double want = 220.0 * std::pow(2.0, -2.5 / 12.0);
    TDM_CHECK_CLOSE(peakNear(out, 48000, 32768, want), want, want * 0.01, "fractional on pitch");
  }
  // Enabled at shift 0: latency-matched dry (exact delay, never shift-as-bypass).
  {
    tdm::TechDeathRig rig;
    rig.setTransposeEnabled(true); // configured pre-reset: ramps park, no transition
    rig.setTransposeSemitones(0.0f);
    rig.reset(kSr, 512);
    const auto in = noise(0.7f, 8192);
    const auto out = runRig(rig, in, 512);
    bool exact = true;
    for (int i = 0; i < kNominalLat && exact; ++i)
      exact = out[static_cast<size_t>(i)] == 0.0f; // delay priming
    for (int i = kNominalLat; i < 8192 && exact; ++i)
      exact = out[static_cast<size_t>(i)] == in[static_cast<size_t>(i - kNominalLat)];
    TDM_CHECK(exact, "shift-0 engaged is exact 768-sample delay");
  }
  // Reset-at-0 still transposes after a live shift (priming rule).
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(0.0f);
    const auto in = sine(0.5f, 220.0f, static_cast<int>(2 * kSr));
    std::vector<float> firstHalf(in.begin(), in.begin() + 48000);
    (void)runRig(rig, firstHalf, 512);
    rig.setTransposeSemitones(-2.0f); // live, no reset
    std::vector<float> secondHalf(in.begin() + 48000, in.end());
    // Continue on the SAME rig (state carries).
    std::vector<float> out2(secondHalf.size(), 0.0f);
    for (size_t off = 0; off < secondHalf.size(); off += 512)
    {
      const float* bi[1] = {secondHalf.data() + off};
      float* bo[1] = {out2.data() + off};
      rig.processBlock(bi, 1, bo, 1, 512);
    }
    const double want = 220.0 * std::pow(2.0, -2.0 / 12.0);
    TDM_CHECK_CLOSE(peakNear(out2, 24000, 16384, want), want, want * 0.02, "live 0 -> -2 retunes");
  }
  // Reset determinism + block-size determinism (bit-exact).
  {
    const auto in = noise(0.6f, 48000);
    auto runOnce = [&](int maxBlock) {
      tdm::TechDeathRig rig;
      rig.reset(kSr, maxBlock);
      rig.setTransposeEnabled(true);
      rig.setTransposeSemitones(-2.0f);
      return runRig(rig, in, maxBlock);
    };
    TDM_CHECK(runOnce(512) == runOnce(512), "reset deterministic");
    TDM_CHECK(runOnce(64) == runOnce(512), "block-size deterministic");
  }
  // Live enable/disable: finite, click-free, returns to exact wire.
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    rig.setTransposeSemitones(-2.0f);
    const int n = 48384; // 3 x 16128, block-aligned segments
    const int third = 16128;
    const auto in = sine(0.5f, 220.0f, n);
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int seg = 0; seg < 3; ++seg)
    {
      if (seg == 1)
        rig.setTransposeEnabled(true); // engage mid-run
      if (seg == 2)
        rig.setTransposeEnabled(false); // disengage mid-run
      for (int off = seg * third; off < (seg + 1) * third; off += 256)
      {
        const float* bi[1] = {in.data() + off};
        float* bo[1] = {out.data() + off};
        rig.processBlock(bi, 1, bo, 1, 256);
      }
    }
    TDM_CHECK(tdm_test::allFinite(out.data(), n), "toggle run finite");
    const float inStep = maxStep(in, 0, n);
    TDM_CHECK(maxStep(out, 0, n) < 6.0f * inStep, "no clicks at toggles");
    bool wireTail = true; // settled disengaged tail is the exact wire again
    for (int i = 2 * third + 4096; i < n && wireTail; ++i)
      wireTail = out[static_cast<size_t>(i)] == in[static_cast<size_t>(i)];
    TDM_CHECK(wireTail, "disengage returns to exact wire");
    const double want = 220.0 * std::pow(2.0, -2.0 / 12.0);
    TDM_CHECK_CLOSE(peakNear(out, third + 8000, 4096, want), want, want * 0.02, "engaged middle is -2");
  }
  // Chain order: TightDrive receives transposed output (bit-exact manual chain).
  {
    const auto in = sine(0.4f, 220.0f, 32768);
    tdm::TechDeathRig rig;
    rig.setTransposeEnabled(true); // pre-reset: parked ramps, engine identical to manual
    rig.setTransposeSemitones(-12.0f);
    rig.setDriveEnabled(true);
    rig.setTight(0.5f);
    rig.setDrive(1.0f);
    rig.setBite(0.5f);
    rig.reset(kSr, 512);
    const auto rigOut = runRig(rig, in, 512);
    // Manual chain in documented order: transpose -> drive.
    tdm::GuitarTranspose eng;
    eng.setEnabled(true);
    eng.setShiftSt(-12.0f);
    eng.reset(kSr);
    tdm::TightDrive drv;
    drv.reset(kSr);
    drv.setEnabled(true);
    drv.setTight(0.5f);
    drv.setDrive(1.0f);
    drv.setBite(0.5f);
    std::vector<float> expect(in.size()), wet(in.size());
    for (size_t off = 0; off < in.size(); off += 512)
    {
      eng.processBlock(in.data() + off, wet.data() + off, 512);
      drv.processBlock(wet.data() + off, expect.data() + off, 512);
    }
    TDM_CHECK(rigOut == expect, "rig == manual transpose->drive exactly");
    // And NOT the reversed order.
    tdm::GuitarTranspose eng2;
    eng2.setEnabled(true);
    eng2.setShiftSt(-12.0f);
    eng2.reset(kSr);
    tdm::TightDrive drv2;
    drv2.reset(kSr);
    drv2.setEnabled(true);
    drv2.setTight(0.5f);
    drv2.setDrive(1.0f);
    drv2.setBite(0.5f);
    std::vector<float> reversed(in.size()), tmp(in.size());
    for (size_t off = 0; off < in.size(); off += 512)
    {
      drv2.processBlock(in.data() + off, tmp.data() + off, 512);
      eng2.processBlock(tmp.data() + off, reversed.data() + off, 512);
    }
    TDM_CHECK(rigOut != reversed, "order is not drive->transpose");
  }
  // DEV insert substitutes for production transpose at the same position.
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 256);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    ZeroInsert zero;
    rig.setTransposeInsert(&zero);
    const auto in = sine(0.5f, 220.0f, 4096);
    const auto out = runRig(rig, in, 256);
    bool silent = true;
    for (float v : out)
      silent = silent && v == 0.0f;
    TDM_CHECK(silent, "installed insert wins over production transpose");
    rig.setTransposeInsert(nullptr);
    const auto out2 = runRig(rig, in, 256);
    TDM_CHECK(tdm_test::peakAbs(out2.data(), 4096) > 0.1f, "production path resumes");
  }
  // Downstream IR: disengaged adds nothing; engaged feeds transposed audio.
  {
    std::vector<float> taps(2048, 0.0f);
    for (int k = 0; k < 2048; ++k)
      taps[static_cast<size_t>(k)] = 0.9f * std::exp(-3.0f * k / 2048.0f) * (k % 2 ? 1.0f : -1.0f);
    const float* ch[1] = {taps.data()};
    tdm::writeWavFloat32("/tmp/tdm_test_transpose_ir.wav", ch, 1, 2048, kSr);
    const auto in = noise(0.5f, 240000);
    auto runWith = [&](bool enabled, float st) {
      tdm::TechDeathRig rig;
      rig.reset(kSr, 512);
      rig.loadIr("/tmp/tdm_test_transpose_ir.wav");
      rig.setTransposeEnabled(enabled);
      rig.setTransposeSemitones(st);
      return runRig(rig, in, 512);
    };
    const auto dry = runWith(false, 0.0f);
    const auto wet = runWith(true, -2.0f);
    TDM_CHECK(tdm_test::allFinite(wet.data(), static_cast<int>(wet.size())), "IR downstream finite");
    TDM_CHECK(dry != wet, "transpose reaches downstream IR");
    // Disengage mid-run: after the FIR memory flushes, output rejoins the
    // never-engaged run exactly (downstream state fully restored).
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.loadIr("/tmp/tdm_test_transpose_ir.wav");
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    std::vector<float> out(in.size(), 0.0f);
    const size_t flipAt = 119808; // block-aligned (512 x 234)
    for (size_t off = 0; off < in.size(); off += 512)
    {
      if (off == flipAt)
        rig.setTransposeEnabled(false);
      const float* bi[1] = {in.data() + off};
      float* bo[1] = {out.data() + off};
      rig.processBlock(bi, 1, bo, 1, 512);
    }
    bool rejoined = true; // 8192 taps of wire input flush IR + ramps
    for (size_t i = flipAt + 16384; i < in.size() && rejoined; ++i)
      rejoined = out[i] == dry[i];
    TDM_CHECK(rejoined, "disengage restores downstream exactly");
    std::remove("/tmp/tdm_test_transpose_ir.wav");
  }
  // Extreme input stays finite through the engaged path.
  {
    tdm::TechDeathRig rig;
    rig.reset(kSr, 512);
    rig.setTransposeEnabled(true);
    rig.setTransposeSemitones(-2.0f);
    const auto in = sine(1.0e6f, 110.0f, 48000);
    const auto out = runRig(rig, in, 512);
    TDM_CHECK(tdm_test::allFinite(out.data(), 48000), "hot input finite");
    TDM_CHECK(tdm_test::peakAbs(out.data(), 48000) < 1.0e8f, "hot input bounded");
  }
}

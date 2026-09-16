// ToneShape v1 tests: per-control tonal behavior, bypass, reset, rates,
// stability, and rig integration. Response is measured the same honest way
// as the drive suite: quadrature fundamental correlation on settled sines
// with cycle-exact 0.1 s windows (all probe frequencies are multiples of
// 10 Hz, exact at 44.1/48/96 kHz).

#include "tests/Assert.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/ToneShape/ToneShape.h"

namespace
{
constexpr double kPi = 3.141592653589793;

std::vector<float> sine(float peak, float freqHz, double sr, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * std::sin(2.0f * (float)(kPi * freqHz * i / sr));
  return out;
}

void processAll(tdm::ToneShape& s, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  s.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}

// Fundamental amplitude via quadrature correlation (integer-cycle window).
float fundAmp(const float* x, int n, float freqHz, double sr)
{
  double s = 0.0, c = 0.0;
  for (int i = 0; i < n; ++i)
  {
    const double ph = 2.0 * kPi * freqHz * i / sr;
    s += x[i] * std::sin(ph);
    c += x[i] * std::cos(ph);
  }
  return static_cast<float>(2.0 * std::sqrt(s * s + c * c) / n);
}

// Fresh enabled unit (avoids state carryover).
tdm::ToneShape makeShape(double sr, float weight, float contour, float presence)
{
  tdm::ToneShape s;
  s.reset(sr);
  s.setWeight(weight);
  s.setContour(contour);
  s.setPresence(presence);
  s.setEnabled(true);
  return s;
}

// Run 0.5 s (settle filters + smoothing), return the last 0.1 s for measure.
std::vector<float> settled(tdm::ToneShape& s, float peak, float freqHz, double sr)
{
  const int n = static_cast<int>(0.5 * sr);
  const std::vector<float> in = sine(peak, freqHz, sr, n);
  std::vector<float> out;
  processAll(s, in, out);
  const int m = static_cast<int>(0.1 * sr);
  return std::vector<float>(out.end() - m, out.end());
}

float meanAbs(const float* x, int n)
{
  double acc = 0.0;
  for (int i = 0; i < n; ++i)
    acc += std::fabs(x[i]);
  return static_cast<float>(acc / n);
}

// Digital-exact one-pole section responses (closed forms evaluated on the
// unit circle — NOT analog approximations, which diverge near Nyquist).
// Returned COMPLEX: the parallel topology's gain is |1 + k*H|, and H carries
// real phase (e.g. the weight LP lags ~78 degrees at 1 kHz), so matching on
// magnitudes alone underpredicts by ~0.1 here.
std::complex<double> lpResp(float fcHz, float fHz, double sr)
{
  const double a = std::exp(-2.0 * kPi * fcHz / sr);
  const std::complex<double> z = std::exp(std::complex<double>(0.0, 2.0 * kPi * fHz / sr));
  return (1.0 - a) / (1.0 - a / z);
}

std::complex<double> hpResp(float fcHz, float fHz, double sr)
{
  const double a = std::exp(-2.0 * kPi * fcHz / sr);
  const std::complex<double> z = std::exp(std::complex<double>(0.0, 2.0 * kPi * fHz / sr));
  return a * (1.0 - 1.0 / z) / (1.0 - a / z);
}

// Parallel-shelf gain for mix k (nominal linear shelf gain 1+k at asymptote).
float parallelGain(const std::complex<double>& h, float k)
{
  return static_cast<float>(std::abs(1.0 + (double)k * h));
}
} // namespace

void runToneShapeTests()
{
  // Defaults are documented and stable; bypass is opt-in.
  {
    tdm::ToneShape s;
    TDM_CHECK_CLOSE(s.weight(), 0.5f, 1e-6f, "default weight");
    TDM_CHECK_CLOSE(s.contour(), 0.5f, 1e-6f, "default contour");
    TDM_CHECK_CLOSE(s.presence(), 0.5f, 1e-6f, "default presence");
    TDM_CHECK(!s.isEnabled(), "opt-in: starts disabled");
  }
  // Parameter extremes clamp; bad sample rate throws.
  {
    tdm::ToneShape s;
    s.reset(48000.0);
    s.setWeight(2.0f);
    s.setContour(-2.0f);
    s.setPresence(7.0f);
    TDM_CHECK_CLOSE(s.weight(), 1.0f, 1e-6f, "weight clamps high");
    TDM_CHECK_CLOSE(s.contour(), 0.0f, 1e-6f, "contour clamps low");
    TDM_CHECK_CLOSE(s.presence(), 1.0f, 1e-6f, "presence clamps high");
    bool threw = false;
    try
    {
      s.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad sample rate throws");
  }
  // Bypass (disabled, or enabled without reset) copies exactly, in place too.
  {
    tdm::ToneShape s;
    s.reset(48000.0);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 512);
    std::vector<float> out(512, 9.0f);
    s.processBlock(in.data(), out.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled bypass copies exactly");
    s.processBlock(out.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled in-place bypass copies exactly");
    tdm::ToneShape fresh; // enabled but never reset: safe copy, no coeffs
    fresh.setEnabled(true);
    fresh.processBlock(in.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "unreset shape copies exactly");
  }
  // Enabled at defaults from reset is bit-exact (all mixes snap to 0).
  {
    tdm::ToneShape s = makeShape(48000.0, 0.5f, 0.5f, 0.5f);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 2048);
    for (int i = 0; i < 2048; ++i)
      in[static_cast<size_t>(i)] += 0.2f * std::sin(2.0f * (float)(kPi * 3000.0 * i / 48000.0));
    std::vector<float> out;
    processAll(s, in, out);
    bool exact = out.size() == in.size();
    for (size_t i = 0; exact && i < in.size(); ++i)
      exact = out[i] == in[i];
    TDM_CHECK(exact, "enabled-at-defaults is bit-exact");
  }
  // Silence stays silence; reset clears charged state to exact silence.
  {
    tdm::ToneShape s = makeShape(48000.0, 1.0f, 0.0f, 1.0f);
    std::vector<float> hot = sine(0.8f, 60.0f, 48000.0, 4800); // charges LP states
    std::vector<float> tmp;
    processAll(s, hot, tmp);
    s.reset(48000.0);
    std::vector<float> z(512, 0.0f), out;
    processAll(s, z, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 512) == 0.0f, "post-reset silence exact");
    TDM_CHECK(tdm_test::allFinite(out.data(), 512), "silence stays finite");
  }
  // Weight: DC gain is exactly +/-6 dB at the extremes (shelf asymptote;
  // note +6 dB is 1.99526x, not 2x).
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::ToneShape up = makeShape(sr, 1.0f, 0.5f, 0.5f);
      tdm::ToneShape down = makeShape(sr, 0.0f, 0.5f, 0.5f);
      const int n = static_cast<int>(0.5 * sr);
      const std::vector<float> dc(static_cast<size_t>(n), 0.3f);
      std::vector<float> oUp, oDown;
      processAll(up, dc, oUp);
      processAll(down, dc, oDown);
      const int m = static_cast<int>(0.1 * sr);
      const float gUp = meanAbs(oUp.data() + (n - m), m) / 0.3f;
      const float gDown = meanAbs(oDown.data() + (n - m), m) / 0.3f;
      TDM_CHECK_CLOSE(gUp, 1.9952624f, 1e-3f, "weight +6 dB at full");
      TDM_CHECK_CLOSE(gDown, 0.5011872f, 1e-3f, "weight -6 dB at zero");
    }
  }
  // Weight: 1 kHz follows the design (|1 + k*H|, complex) within 1%.
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      const std::complex<double> h = lpResp(tdm::ToneShape::kWeightHz, 1000.0f, sr);
      for (const float w : {0.0f, 1.0f})
      {
        tdm::ToneShape s = makeShape(sr, w, 0.5f, 0.5f);
        const std::vector<float> seg = settled(s, 0.4f, 1000.0f, sr);
        const float got = fundAmp(seg.data(), static_cast<int>(seg.size()), 1000.0f, sr) / 0.4f;
        const float k = (w == 1.0f ? 1.9952624f : 0.5011872f) - 1.0f;
        TDM_CHECK_CLOSE(got, parallelGain(h, k), 0.01f, "weight 1 kHz matches design");
      }
    }
  }
  // Contour: center gain is exactly +/-6 dB at 600 Hz, all rates.
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      tdm::ToneShape up = makeShape(sr, 0.5f, 1.0f, 0.5f);
      tdm::ToneShape down = makeShape(sr, 0.5f, 0.0f, 0.5f);
      const std::vector<float> segUp = settled(up, 0.4f, 600.0f, sr);
      const std::vector<float> segDown = settled(down, 0.4f, 600.0f, sr);
      const float gUp = fundAmp(segUp.data(), static_cast<int>(segUp.size()), 600.0f, sr) / 0.4f;
      const float gDown = fundAmp(segDown.data(), static_cast<int>(segDown.size()), 600.0f, sr) / 0.4f;
      TDM_CHECK_CLOSE(gUp, 2.0f, 0.02f, "contour +6 dB at center");
      TDM_CHECK_CLOSE(gDown, 0.5f, 0.02f, "contour -6 dB at center");
    }
  }
  // Contour: stays local — 100 Hz and 3 kHz move less than +/-2 dB.
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      for (const float f : {100.0f, 3000.0f})
      {
        for (const float c : {0.0f, 1.0f})
        {
          tdm::ToneShape s = makeShape(sr, 0.5f, c, 0.5f);
          const std::vector<float> seg = settled(s, 0.4f, f, sr);
          const float g = fundAmp(seg.data(), static_cast<int>(seg.size()), f, sr) / 0.4f;
          TDM_CHECK(g > 0.79f && g < 1.26f, "contour skirts stay within 2 dB");
        }
      }
    }
  }
  // Presence: 15 kHz follows the design (|1 + k*H|, complex) within 1%.
  // (15 kHz stimulus in float32 carries ~1% phase-noise, hence 0.015.)
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      const std::complex<double> h = hpResp(tdm::ToneShape::kPresenceHz, 15000.0f, sr);
      for (const float p : {0.0f, 1.0f})
      {
        tdm::ToneShape s = makeShape(sr, 0.5f, 0.5f, p);
        const std::vector<float> seg = settled(s, 0.3f, 15000.0f, sr);
        const float got = fundAmp(seg.data(), static_cast<int>(seg.size()), 15000.0f, sr) / 0.3f;
        const float k = (p == 1.0f ? 1.7782794f : 0.5623413f) - 1.0f; // +/-5 dB
        TDM_CHECK_CLOSE(got, parallelGain(h, k), 0.015f, "presence 15 kHz matches design");
      }
    }
  }
  // Presence: 200 Hz barely moves at the extremes (shelf stays up high).
  {
    tdm::ToneShape up = makeShape(48000.0, 0.5f, 0.5f, 1.0f);
    tdm::ToneShape down = makeShape(48000.0, 0.5f, 0.5f, 0.0f);
    for (tdm::ToneShape* sp : {&up, &down})
    {
      const std::vector<float> seg = settled(*sp, 0.4f, 200.0f, 48000.0);
      const float g = fundAmp(seg.data(), static_cast<int>(seg.size()), 200.0f, 48000.0) / 0.4f;
      TDM_CHECK(std::fabs(g - 1.0f) < 0.06f, "presence leaves 200 Hz alone");
    }
  }
  // Stability: 2 s of hot stacked sines through both extreme corners stays
  // finite and bounded at every rate (biquad + shelves, no blowup).
  {
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
      for (const float v : {0.0f, 1.0f})
      {
        tdm::ToneShape s = makeShape(sr, v, v, v);
        const int n = static_cast<int>(2.0 * sr);
        std::vector<float> in(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
          in[static_cast<size_t>(i)] =
            0.5f * std::sin(2.0f * (float)(kPi * 60.0 * i / sr))
            + 0.3f * std::sin(2.0f * (float)(kPi * 600.0 * i / sr))
            + 0.2f * std::sin(2.0f * (float)(kPi * 3000.0 * i / sr));
        std::vector<float> out;
        processAll(s, in, out);
        TDM_CHECK(tdm_test::allFinite(out.data(), n), "hot corners stay finite");
        TDM_CHECK(tdm_test::peakAbs(out.data(), n) < 8.0f, "hot corners stay bounded");
      }
    }
  }
  // Parameter jumps mid-stream stay finite and converge (smoothing works).
  {
    tdm::ToneShape s = makeShape(48000.0, 0.0f, 0.0f, 0.0f);
    std::vector<float> in = sine(0.4f, 440.0f, 48000.0, 512);
    std::vector<float> out(512);
    bool finite = true;
    for (int b = 0; b < 40; ++b)
    {
      s.setWeight(b % 2 == 0 ? 1.0f : 0.0f);
      s.setContour(b % 2 == 0 ? 1.0f : 0.0f);
      s.setPresence(b % 2 == 0 ? 1.0f : 0.0f);
      s.processBlock(in.data(), out.data(), 512);
      finite = finite && tdm_test::allFinite(out.data(), 512);
    }
    TDM_CHECK(finite, "mid-stream jumps stay finite");
    s.setWeight(0.5f);
    s.setContour(0.5f);
    s.setPresence(0.5f);
    for (int b = 0; b < 60; ++b)
      s.processBlock(in.data(), out.data(), 512);
    float maxDiff = 0.0f;
    for (int i = 0; i < 512; ++i)
      maxDiff = std::max(maxDiff, std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]));
    TDM_CHECK(maxDiff < 1e-3f, "neutral reconverges after jumps");
  }
  // Rig integration: disabled by default (chain preserved); enabled at
  // neutral matches the bypassed rig bit-exactly; weight audibly moves it.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    TDM_CHECK(!rig.isShapeEnabled(), "rig shape off by default");
    std::vector<float> in = sine(0.4f, 220.0f, 48000.0, 2048);
    for (int i = 0; i < 2048; ++i)
      in[static_cast<size_t>(i)] += 0.15f * std::sin(2.0f * (float)(kPi * 90.0 * i / 48000.0));
    const float* bi[1] = {in.data()};
    std::vector<float> bypassed(2048), shaped(2048);
    float* bo[1] = {bypassed.data()};
    rig.processBlock(bi, 1, bo, 1, 2048);
    rig.setShapeEnabled(true); // neutral by default
    bo[0] = shaped.data();
    rig.processBlock(bi, 1, bo, 1, 2048);
    bool exact = true;
    for (int i = 0; i < 2048; ++i)
      exact = exact && shaped[static_cast<size_t>(i)] == bypassed[static_cast<size_t>(i)];
    TDM_CHECK(exact, "enabled-neutral shape matches bypassed rig");
    rig.setWeight(1.0f);
    rig.processBlock(bi, 1, bo, 1, 2048);
    float maxDiff = 0.0f;
    for (int i = 0; i < 2048; ++i)
      maxDiff = std::max(maxDiff, std::fabs(shaped[static_cast<size_t>(i)] - bypassed[static_cast<size_t>(i)]));
    TDM_CHECK(maxDiff > 0.01f, "weight audibly moves the rig");
    // Composition with OutputTrim (shape sits pre-trim): DC gains multiply.
    rig.setWeight(1.0f);
    rig.setContour(0.5f);
    rig.setPresence(0.5f);
    rig.setOutputTrimDb(6.0206f);
    std::vector<float> dc(512, 0.25f), o(512);
    const float* di[1] = {dc.data()};
    float* dob[1] = {o.data()};
    for (int b = 0; b < 200; ++b)
      rig.processBlock(di, 1, dob, 1, 512);
    TDM_CHECK_CLOSE(o[511], 1.0f, 1e-2f, "shape x trim gains compose");
  }
  // Params round-trip through the handoff, with clamping.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    tdm::RigParams p;
    p.shapeEnabled = true;
    p.weight = 0.7f;
    p.contour = 0.3f;
    p.presence = 0.8f;
    rig.setParams(p);
    const tdm::RigParams q = rig.params();
    TDM_CHECK(q.shapeEnabled && q.weight == 0.7f && q.contour == 0.3f && q.presence == 0.8f,
              "shape params round-trip");
    TDM_CHECK_CLOSE(rig.weight(), 0.7f, 1e-6f, "weight getter");
    rig.setWeight(5.0f);
    rig.setContour(-5.0f);
    rig.setPresence(5.0f);
    TDM_CHECK_CLOSE(rig.weight(), 1.0f, 1e-6f, "weight clamps high");
    TDM_CHECK_CLOSE(rig.contour(), 0.0f, 1e-6f, "contour clamps low");
    TDM_CHECK_CLOSE(rig.presence(), 1.0f, 1e-6f, "presence clamps high");
    const tdm::RigParams fresh;
    TDM_CHECK(!fresh.shapeEnabled && fresh.weight == tdm::ToneShape::kDefaultWeight
                  && fresh.contour == tdm::ToneShape::kDefaultContour
                  && fresh.presence == tdm::ToneShape::kDefaultPresence,
              "params mirror shape defaults");
  }
}

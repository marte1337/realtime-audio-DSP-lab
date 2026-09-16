#include "tests/Assert.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/TightDrive/TightDrive.h"

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

void processAll(tdm::TightDrive& d, const std::vector<float>& in, std::vector<float>& out)
{
  out.assign(in.size(), 0.0f);
  d.processBlock(in.data(), out.data(), static_cast<int>(in.size()));
}

// Peak amplitude of the fundamental via quadrature correlation. The window
// should hold an integer number of cycles (all probes below use 0.1 s
// windows with cycle-exact frequencies at 44.1/48/96 kHz).
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

float rms(const float* x, int n)
{
  double acc = 0.0;
  for (int i = 0; i < n; ++i)
    acc += (double)x[i] * x[i];
  return static_cast<float>(std::sqrt(acc / n));
}

// Residual (non-fundamental) RMS relative to fundamental RMS. Near 0 for
// linear processing, clearly positive for saturation, < 1 while the
// fundamental still dominates (overdrive, not fuzz).
float residualRatio(const float* x, int n, float freqHz, double sr)
{
  const float fundRms = fundAmp(x, n, freqHz, sr) / 1.41421356f;
  const float total = rms(x, n);
  if (fundRms <= 0.0f)
    return 0.0f;
  const float res = std::sqrt(total * total > fundRms * fundRms ? total * total - fundRms * fundRms : 0.0f);
  return res / fundRms;
}

// Fresh unit at the given settings and rate (avoids state carryover).
tdm::TightDrive makeDrive(double sr, float tight, float drive, float bite)
{
  tdm::TightDrive d;
  d.reset(sr);
  d.setTight(tight);
  d.setDrive(drive);
  d.setBite(bite);
  d.setEnabled(true);
  return d;
}

// Run 0.5 s (settle filters + smoothing), return the last 0.1 s for measure.
std::vector<float> settled(tdm::TightDrive& d, float peak, float freqHz, double sr)
{
  const int n = static_cast<int>(0.5 * sr);
  const std::vector<float> in = sine(peak, freqHz, sr, n);
  std::vector<float> out;
  processAll(d, in, out);
  const int m = static_cast<int>(0.1 * sr);
  return std::vector<float>(out.end() - m, out.end());
}
} // namespace

void runTightDriveTests()
{
  // Defaults are documented and stable; bypass is opt-in.
  {
    tdm::TightDrive d;
    TDM_CHECK_CLOSE(d.tight(), 0.5f, 1e-6f, "default tight");
    TDM_CHECK_CLOSE(d.drive(), 0.3f, 1e-6f, "default drive");
    TDM_CHECK_CLOSE(d.bite(), 0.5f, 1e-6f, "default bite");
    TDM_CHECK(!d.isEnabled(), "opt-in: starts disabled");
  }
  // Bypass (disabled, or enabled without reset) copies exactly, in place too.
  {
    tdm::TightDrive d;
    d.reset(48000.0);
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 512);
    std::vector<float> out(512, 9.0f);
    d.processBlock(in.data(), out.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled bypass copies exactly");
    d.processBlock(out.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "disabled in-place bypass copies exactly");
    tdm::TightDrive fresh;
    fresh.setEnabled(true);
    fresh.processBlock(in.data(), out.data(), 512);
    exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "unreset drive copies exactly");
    std::vector<float> z(512, 0.0f);
    d.processBlock(z.data(), out.data(), 512);
    TDM_CHECK(tdm_test::peakAbs(out.data(), 512) == 0.0f, "bypassed silence exact");
  }
  // Tight: LF attenuated far more than HF when Tight rises (Drive 0 so the
  // shaper stays ~linear and the measurement isolates the preconditioner).
  {
    const double sr = 48000.0;
    const int m = static_cast<int>(0.1 * sr);
    tdm::TightDrive lo = makeDrive(sr, 0.0f, 0.0f, 0.5f);
    tdm::TightDrive hi = makeDrive(sr, 1.0f, 0.0f, 0.5f);
    const std::vector<float> lfLo = settled(lo, 0.1f, 70.0f, sr);
    // Fresh units: filter states must not carry over between Tight settings.
    tdm::TightDrive lo2 = makeDrive(sr, 0.0f, 0.0f, 0.5f);
    tdm::TightDrive hi2 = makeDrive(sr, 1.0f, 0.0f, 0.5f);
    const std::vector<float> hfLo = settled(lo2, 0.1f, 880.0f, sr);
    const std::vector<float> lfHi = settled(hi, 0.1f, 70.0f, sr);
    const std::vector<float> hfHi = settled(hi2, 0.1f, 880.0f, sr);
    const float lfDrop = fundAmp(lfHi.data(), m, 70.0f, sr) / fundAmp(lfLo.data(), m, 70.0f, sr);
    const float hfDrop = fundAmp(hfHi.data(), m, 880.0f, sr) / fundAmp(hfLo.data(), m, 880.0f, sr);
    TDM_CHECK(lfDrop < 0.316f, "tight cuts lows 10+ dB");
    TDM_CHECK(hfDrop > 0.84f, "tight leaves highs within 1.5 dB");
    TDM_CHECK(lfDrop < 0.5f * hfDrop, "tight acts on lows, not overall level");
  }
  // Drive: -20 dB input stays ~linear at Drive 0, clearly saturates at
  // Drive 1 while the fundamental still dominates (overdrive, not fuzz).
  {
    const double sr = 48000.0;
    const int m = static_cast<int>(0.1 * sr);
    tdm::TightDrive clean = makeDrive(sr, 0.0f, 0.0f, 0.5f);
    tdm::TightDrive hot = makeDrive(sr, 0.0f, 1.0f, 0.5f);
    const std::vector<float> outClean = settled(clean, 0.1f, 220.0f, sr);
    const std::vector<float> outHot = settled(hot, 0.1f, 220.0f, sr);
    TDM_CHECK(residualRatio(outClean.data(), m, 220.0f, sr) < 0.005f, "low drive stays linear");
    const float hotRes = residualRatio(outHot.data(), m, 220.0f, sr);
    TDM_CHECK(hotRes > 0.02f && hotRes < 0.5f, "high drive saturates musically");
    TDM_CHECK(tdm_test::allFinite(outHot.data(), m), "driven output finite");
  }
  // Bite: presence extremes move 5 kHz by 6+ dB in opposite directions while
  // 220 Hz barely moves (spectral control, not global gain).
  {
    const double sr = 48000.0;
    const int m = static_cast<int>(0.1 * sr);
    tdm::TightDrive smooth = makeDrive(sr, 0.0f, 0.0f, 0.0f);
    tdm::TightDrive cut = makeDrive(sr, 0.0f, 0.0f, 1.0f);
    const std::vector<float> hiSmooth = settled(smooth, 0.1f, 5000.0f, sr);
    const std::vector<float> hiCut = settled(cut, 0.1f, 5000.0f, sr);
    // Measured end-to-end sweep at 5 kHz is 1.998x (~6 dB); bound at 1.7x
    // keeps margin while still catching a dead shelf (1.0x) or a sign flip.
    const float hiRatio =
      fundAmp(hiCut.data(), m, 5000.0f, sr) / fundAmp(hiSmooth.data(), m, 5000.0f, sr);
    TDM_CHECK(hiRatio > 1.7f, "bite moves presence ~6 dB");
    tdm::TightDrive smooth2 = makeDrive(sr, 0.0f, 0.0f, 0.0f);
    tdm::TightDrive cut2 = makeDrive(sr, 0.0f, 0.0f, 1.0f);
    const std::vector<float> loSmooth = settled(smooth2, 0.1f, 220.0f, sr);
    const std::vector<float> loCut = settled(cut2, 0.1f, 220.0f, sr);
    const float loRatio =
      fundAmp(loCut.data(), m, 220.0f, sr) / fundAmp(loSmooth.data(), m, 220.0f, sr);
    TDM_CHECK(loRatio > 0.84f && loRatio < 1.19f, "bite leaves lows within 1.5 dB");
    // Neutral bite (0.5 = exactly 0 dB shelf): the full neutral chain at
    // 880 Hz passes within ~1% (HPF@40/tanh/LPF@12k barely touch it).
    tdm::TightDrive neutral = makeDrive(sr, 0.0f, 0.0f, 0.5f);
    const std::vector<float> midNeutral = settled(neutral, 0.1f, 880.0f, sr);
    const std::vector<float> midRef = sine(0.1f, 880.0f, sr, m);
    const float neutralRatio =
      fundAmp(midNeutral.data(), m, 880.0f, sr) / fundAmp(midRef.data(), m, 880.0f, sr);
    TDM_CHECK(neutralRatio > 0.95f && neutralRatio < 1.0f, "neutral chain is transparent-ish");
  }
  // Harmonics: 440 Hz at Drive 0.6 gains audible saturation, stays finite.
  {
    const double sr = 48000.0;
    const int m = static_cast<int>(0.1 * sr);
    tdm::TightDrive d = makeDrive(sr, 0.5f, 0.6f, 0.5f);
    const std::vector<float> out = settled(d, 0.5f, 440.0f, sr);
    const float res = residualRatio(out.data(), m, 440.0f, sr);
    TDM_CHECK(res > 0.01f && res < 0.6f, "mid drive adds harmonics, fundamental dominates");
    TDM_CHECK(tdm_test::allFinite(out.data(), m), "saturated output finite");
  }
  // Silence in = silence out; DC blocked (HPF); no accumulation afterwards.
  {
    const double sr = 48000.0;
    tdm::TightDrive d = makeDrive(sr, 1.0f, 1.0f, 1.0f);
    std::vector<float> z(static_cast<size_t>(sr), 0.0f), out;
    processAll(d, z, out);
    TDM_CHECK(tdm_test::peakAbs(out.data(), (int)sr) == 0.0f, "silence stays exactly silent");
    std::vector<float> dc(static_cast<size_t>(sr), 0.1f);
    processAll(d, dc, out);
    TDM_CHECK(std::fabs(out.back()) < 1e-3f, "DC blocked after settle");
    // The falling 0.1 -> 0 edge legitimately rings the HPF for ~600 samples;
    // what matters is full settle afterwards (no accumulation/oscillation).
    processAll(d, z, out);
    const int tailN = static_cast<int>(sr / 2);
    bool tailZero = true;
    for (int i = (int)sr - tailN; i < (int)sr; ++i)
      tailZero = tailZero && out[static_cast<size_t>(i)] == 0.0f;
    TDM_CHECK(tailZero, "DC transient fully settles, no residue");
    TDM_CHECK(tdm_test::allFinite(out.data(), (int)sr), "DC path finite");
  }
  // Reset: bad rate throws; state clears; runs are deterministic.
  {
    tdm::TightDrive d;
    bool threw = false;
    try
    {
      d.reset(0.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "bad sample rate throws");
    auto runOnce = [] {
      tdm::TightDrive x = makeDrive(48000.0, 0.7f, 0.8f, 0.3f);
      std::vector<float> in = sine(0.5f, 110.0f, 48000.0, 4096);
      std::vector<float> out;
      processAll(x, in, out);
      return out;
    };
    const std::vector<float> a = runOnce(), b = runOnce();
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(exact, "runs are deterministic");
    // Reset after hot input returns to the documented zero state.
    tdm::TightDrive hot = makeDrive(48000.0, 1.0f, 1.0f, 1.0f);
    std::vector<float> loud = sine(0.9f, 110.0f, 48000.0, 4096), tmp;
    processAll(hot, loud, tmp);
    hot.reset(48000.0);
    std::vector<float> z(512, 0.0f);
    processAll(hot, z, tmp);
    TDM_CHECK(tdm_test::peakAbs(tmp.data(), 512) == 0.0f, "post-reset silence exact");
  }
  // Sample rates: Tight cutoff tracks frequency; Drive stays sane.
  for (const double sr : {44100.0, 48000.0, 96000.0})
  {
    const int m = static_cast<int>(0.1 * sr);
    tdm::TightDrive d = makeDrive(sr, 1.0f, 0.0f, 0.5f);
    const std::vector<float> out = settled(d, 0.1f, 320.0f, sr);
    const std::vector<float> ref = sine(0.1f, 320.0f, sr, m);
    const float ratio = fundAmp(out.data(), m, 320.0f, sr) / fundAmp(ref.data(), m, 320.0f, sr);
    TDM_CHECK(ratio > 0.647f && ratio < 0.767f, "cutoff frequency-correct across rates");
    tdm::TightDrive lin = makeDrive(sr, 0.0f, 0.0f, 0.5f);
    const std::vector<float> outLin = settled(lin, 0.1f, 220.0f, sr);
    TDM_CHECK(residualRatio(outLin.data(), m, 220.0f, sr) < 0.01f, "linear at all rates");
  }
  // Extremes: hot input at all-max stays finite/bounded; violent param
  // jumps stay finite (smoothing stability); clamping holds.
  {
    const double sr = 48000.0;
    tdm::TightDrive d = makeDrive(sr, 1.0f, 1.0f, 1.0f);
    std::vector<float> in = sine(0.9f, 110.0f, sr, 4800);
    for (int i = 0; i < 4800; ++i) // broadband-ish edge: add a high octave
      in[static_cast<size_t>(i)] += 0.3f * std::sin(2.0f * (float)(kPi * 3520.0 * i / sr));
    std::vector<float> out;
    processAll(d, in, out);
    TDM_CHECK(tdm_test::allFinite(out.data(), 4800), "all-max finite");
    TDM_CHECK(tdm_test::peakAbs(out.data(), 4800) < 2.0f, "all-max bounded");
    tdm::TightDrive j = makeDrive(sr, 0.0f, 0.0f, 0.0f);
    std::vector<float> blk = sine(0.5f, 1000.0f, sr, 256), ob;
    float worstStep = 0.0f;
    for (int b = 0; b < 30; ++b)
    {
      j.setTight(b % 2 ? 1.0f : 0.0f);
      j.setDrive(b % 2 ? 1.0f : 0.0f);
      j.setBite(b % 2 ? 1.0f : 0.0f);
      processAll(j, blk, ob);
      TDM_CHECK(tdm_test::allFinite(ob.data(), 256), "param jump finite");
      for (int i = 1; i < 256; ++i)
        worstStep = std::max(worstStep, std::fabs(ob[static_cast<size_t>(i)] - ob[static_cast<size_t>(i - 1)]));
    }
    // A 0.5-peak 1 kHz tone slews ~0.07/sample; an unsmoothed full-range
    // gain step would print multi-unit jumps. Smoothing keeps it musical.
    TDM_CHECK(worstStep < 0.5f, "param jumps stay click-free-ish");
    d.setTight(2.0f);
    TDM_CHECK_CLOSE(d.tight(), 1.0f, 1e-6f, "tight clamps high");
    d.setTight(-1.0f);
    TDM_CHECK_CLOSE(d.tight(), 0.0f, 1e-6f, "tight clamps low");
    d.setDrive(2.0f);
    TDM_CHECK_CLOSE(d.drive(), 1.0f, 1e-6f, "drive clamps high");
    d.setBite(-1.0f);
    TDM_CHECK_CLOSE(d.bite(), 0.0f, 1e-6f, "bite clamps low");
  }
  // Rig integration: disabled drive reproduces the previous rig exactly;
  // enabled drive audibly engages; full NAM path stays finite if present.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    TDM_CHECK(!rig.isDriveEnabled(), "rig drive off by default");
    std::vector<float> in = sine(0.5f, 220.0f, 48000.0, 1024);
    const float* bi[1] = {in.data()};
    std::vector<float> bypassed(1024), driven(1024);
    float* bo[1] = {bypassed.data()};
    rig.processBlock(bi, 1, bo, 1, 1024);
    bool exact = true;
    for (int i = 0; i < 1024; ++i)
      exact = exact && bypassed[static_cast<size_t>(i)] == in[static_cast<size_t>(i)];
    TDM_CHECK(exact, "disabled drive preserves rig path bit-exactly");
    rig.setTight(0.55f);
    rig.setDrive(0.35f);
    rig.setBite(0.6f);
    rig.setDriveEnabled(true);
    bo[0] = driven.data();
    rig.processBlock(bi, 1, bo, 1, 1024);
    bool changed = false;
    for (int i = 0; i < 1024; ++i)
      changed = changed || driven[static_cast<size_t>(i)] != bypassed[static_cast<size_t>(i)];
    TDM_CHECK(changed, "enabled drive engages in rig");
    TDM_CHECK(tdm_test::allFinite(driven.data(), 1024), "driven rig finite");

    std::string path = "../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore/example_models/wavenet_a2_max.nam";
    if (const char* env = std::getenv("TDM_TEST_NAM"))
      path = env;
    std::ifstream probe(path);
    TDM_CHECK(static_cast<bool>(probe), "A2 model present for drive integration");
    if (probe)
    {
      tdm::TechDeathRig full;
      full.reset(48000.0, 512);
      full.loadNam(path);
      full.setTight(0.55f);
      full.setDrive(0.35f);
      full.setBite(0.6f);
      full.setDriveEnabled(true);
      std::vector<float> din(2048), dout(2048);
      for (int i = 0; i < 2048; ++i)
        din[static_cast<size_t>(i)] =
          0.5f * std::sin(2.0f * (float)(kPi * 82.41 * i / 48000.0)) * std::exp(-3.0f * i / 2048.0f);
      const float* di[1] = {din.data()};
      float* doutPtr[1] = {dout.data()};
      full.processBlock(di, 1, doutPtr, 1, 2048);
      TDM_CHECK(tdm_test::allFinite(dout.data(), 2048), "drive+NAM finite");
    }
  }
}

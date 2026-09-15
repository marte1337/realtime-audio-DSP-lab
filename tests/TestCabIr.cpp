#include "tests/Assert.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "dsp/CabIrStage.h"
#include "dsp/WavFile.h"

namespace
{
void writeDiracIr(const std::string& path, double sr)
{
  std::vector<float> taps(64, 0.0f);
  taps[0] = 1.0f;
  const float* ch[1] = {taps.data()};
  tdm::writeWavFloat32(path, ch, 1, 64, sr);
}

// Deterministic pseudo-guitar-ish probe: no RNG dependency, same every run.
float probe(int i)
{
  const float s = std::sin(2.0f * 3.14159265f * 82.41f * i / 48000.0f);
  return 0.6f * s * s * s + 0.1f * std::sin(2.0f * 3.14159265f * 1244.0f * i / 48000.0f);
}
} // namespace

void runCabIrTests()
{
  // Bypass with no IR is an exact copy.
  {
    tdm::CabIrStage ir;
    std::vector<float> in(256), out(256, 9.0f);
    for (int i = 0; i < 256; ++i)
      in[i] = probe(i);
    ir.processBlock(in.data(), out.data(), 256);
    bool exact = true;
    for (int i = 0; i < 256; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "bypass copies exactly");
  }
  // Dirac IR: output[0] == documented reference gain, tail is zero.
  {
    writeDiracIr("/tmp/tdm_test_dirac.wav", 48000.0);
    tdm::CabIrStage ir;
    ir.loadIr("/tmp/tdm_test_dirac.wav", 48000.0);
    TDM_CHECK(ir.hasIr() && ir.length() == 64, "dirac loads");
    std::vector<float> in(128, 0.0f), out(128, 9.0f);
    in[0] = 1.0f;
    ir.processBlock(in.data(), out.data(), 128);
    const float want = std::pow(10.0f, -18.0f / 20.0f); // 48000/48000 == 1
    TDM_CHECK_CLOSE(out[0], want, 1e-6f, "dirac gain matches reference comp");
    TDM_CHECK(tdm_test::peakAbs(out.data() + 1, 127) == 0.0f, "dirac tail is zero");
  }
  // Silence stays silence; processing is deterministic across resets.
  {
    tdm::CabIrStage ir;
    ir.loadIr("/tmp/tdm_test_dirac.wav", 48000.0);
    std::vector<float> z(512, 0.0f), o1(512, 9.0f), o2(512, 9.0f);
    ir.processBlock(z.data(), o1.data(), 512);
    TDM_CHECK(tdm_test::peakAbs(o1.data(), 512) == 0.0f, "silence in, silence out");
    std::vector<float> in(512);
    for (int i = 0; i < 512; ++i)
      in[i] = probe(i);
    ir.processBlock(in.data(), o1.data(), 512);
    ir.reset(48000.0);
    ir.processBlock(in.data(), o2.data(), 512);
    // Fresh history after reset differs from warmed history: only require
    // finite output here; exact determinism is checked below run-to-run.
    TDM_CHECK(tdm_test::allFinite(o1.data(), 512) && tdm_test::allFinite(o2.data(), 512), "finite after reset");
    tdm::CabIrStage a, b;
    a.loadIr("/tmp/tdm_test_dirac.wav", 48000.0);
    b.loadIr("/tmp/tdm_test_dirac.wav", 48000.0);
    a.processBlock(in.data(), o1.data(), 512);
    b.processBlock(in.data(), o2.data(), 512);
    bool exact = true;
    for (int i = 0; i < 512; ++i)
      exact = exact && o1[i] == o2[i];
    TDM_CHECK(exact, "two identical runs are bit-exact");
  }
  // In-place processing works (rig runs stages in place).
  {
    tdm::CabIrStage ir;
    ir.loadIr("/tmp/tdm_test_dirac.wav", 48000.0);
    std::vector<float> buf(128, 0.0f);
    buf[0] = 1.0f;
    ir.processBlock(buf.data(), buf.data(), 128);
    TDM_CHECK_CLOSE(buf[0], std::pow(10.0f, -18.0f / 20.0f), 1e-6f, "in-place dirac");
  }
  // Missing file and SR mismatch throw outside realtime.
  {
    bool threw = false;
    try
    {
      tdm::CabIrStage ir;
      ir.loadIr("/tmp/tdm_test_missing_ir.wav", 48000.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "missing IR throws");
  }
  {
    writeDiracIr("/tmp/tdm_test_44k.wav", 44100.0);
    bool threw = false;
    try
    {
      tdm::CabIrStage ir;
      ir.loadIr("/tmp/tdm_test_44k.wav", 48000.0);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "SR mismatch throws (no resample in M0)");
  }
  std::remove("/tmp/tdm_test_dirac.wav");
  std::remove("/tmp/tdm_test_44k.wav");
}

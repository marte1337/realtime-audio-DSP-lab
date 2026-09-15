#include "tests/Assert.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/WavFile.h"

namespace
{
// Deterministic noise-ish probe (LCG + DC wander), same every run.
float noiseProbe(int i)
{
  uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u);
  x ^= x >> 15;
  const float u = static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
  return 0.7f * (2.0f * u - 1.0f);
}
} // namespace

void runRigTests()
{
  // Empty rig: stereo average broadcast exactly to every output.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 256);
    TDM_CHECK(!rig.hasNam() && !rig.hasIr(), "starts empty");
    std::vector<float> l(256, 0.25f), r(256, 0.75f), o1(256, 9.0f), o2(256, 9.0f);
    const float* in[2] = {l.data(), r.data()};
    float* out[2] = {o1.data(), o2.data()};
    rig.processBlock(in, 2, out, 2, 256);
    bool exact = true;
    for (int i = 0; i < 256; ++i)
      exact = exact && o1[i] == 0.5f && o2[i] == 0.5f;
    TDM_CHECK(exact, "average-to-mono broadcast is exact");
  }
  // Oversized blocks are chunked internally (maxBlock=64, 1000 frames).
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 64);
    std::vector<float> in(1000), o(1000, 9.0f);
    for (int i = 0; i < 1000; ++i)
      in[i] = noiseProbe(i);
    const float* bi[1] = {in.data()};
    float* bo[1] = {o.data()};
    rig.processBlock(bi, 1, bo, 1, 1000);
    bool exact = true;
    for (int i = 0; i < 1000; ++i)
      exact = exact && o[i] == in[i];
    TDM_CHECK(exact, "chunked bypass is exact");
  }
  // Loader contract errors surface outside realtime.
  {
    bool threw = false;
    try
    {
      tdm::TechDeathRig rig;
      rig.loadNam("/tmp/anything.nam");
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "load before reset throws");
  }
  {
    bool threw = false;
    try
    {
      tdm::TechDeathRig rig;
      rig.reset(48000.0, 256);
      rig.loadNam("/tmp/tdm_test_missing.nam");
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "missing NAM throws");
  }
  {
    bool threw = false;
    try
    {
      tdm::TechDeathRig rig;
      rig.reset(48000.0, 256);
      rig.loadIr("/tmp/tdm_test_missing.wav");
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "missing IR throws");
  }
  // IR-only rig: stable over ~5 s of hot input, deterministic run-to-run.
  {
    std::vector<float> taps(2048, 0.0f);
    for (int k = 0; k < 2048; ++k)
      taps[static_cast<size_t>(k)] = 0.9f * std::exp(-3.0f * k / 2048.0f) * (k % 2 ? 1.0f : -1.0f);
    const float* ch[1] = {taps.data()};
    tdm::writeWavFloat32("/tmp/tdm_test_rig_ir.wav", ch, 1, 2048, 48000.0);

    auto runOnce = [&] {
      tdm::TechDeathRig rig;
      rig.reset(48000.0, 512);
      rig.loadIr("/tmp/tdm_test_rig_ir.wav");
      std::vector<float> in(512), out(512);
      std::vector<float> acc;
      acc.reserve(240000);
      for (int b = 0; b < 469; ++b)
      {
        for (int i = 0; i < 512; ++i)
          in[static_cast<size_t>(i)] = noiseProbe(b * 512 + i);
        const float* bi[1] = {in.data()};
        float* bo[1] = {out.data()};
        rig.processBlock(bi, 1, bo, 1, 512);
        acc.insert(acc.end(), out.begin(), out.end());
      }
      return acc;
    };
    const std::vector<float> a = runOnce();
    const std::vector<float> b = runOnce();
    TDM_CHECK(a.size() == b.size() && tdm_test::allFinite(a.data(), static_cast<int>(a.size())),
              "5 s hot input stays finite");
    TDM_CHECK(tdm_test::peakAbs(a.data(), static_cast<int>(a.size())) < 5.0f, "5 s hot input stays bounded");
    bool exact = a.size() == b.size();
    for (size_t i = 0; exact && i < a.size(); ++i)
      exact = a[i] == b[i];
    TDM_CHECK(exact, "long runs are deterministic");
    std::remove("/tmp/tdm_test_rig_ir.wav");
  }
  // Full path with the real A2 model stays finite (integration through rig).
  {
    std::string path = "../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore/example_models/wavenet_a2_max.nam";
    if (const char* env = std::getenv("TDM_TEST_NAM"))
      path = env;
    std::ifstream probe(path);
    TDM_CHECK(static_cast<bool>(probe), "A2 model present for rig integration");
    if (probe)
    {
      tdm::TechDeathRig rig;
      rig.reset(48000.0, 512);
      rig.loadNam(path);
      std::vector<float> in(2048), out(2048);
      for (int i = 0; i < 2048; ++i)
        in[i] = noiseProbe(i);
      const float* bi[1] = {in.data()};
      float* bo[1] = {out.data()};
      rig.processBlock(bi, 1, bo, 1, 2048);
      TDM_CHECK(tdm_test::allFinite(out.data(), 2048), "NAM through rig is finite");
    }
  }
}

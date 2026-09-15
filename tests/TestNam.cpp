#include "tests/Assert.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/NamStage.h"

namespace
{
std::string modelPath()
{
  if (const char* env = std::getenv("TDM_TEST_NAM"))
    return env;
  return "../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore/example_models/wavenet_a2_max.nam";
}

std::vector<float> sineBlock(int start, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * (start + i) / 48000.0f);
  return out;
}
} // namespace

void runNamTests()
{
  // Bypass with no model is an exact copy.
  {
    tdm::NamStage st;
    TDM_CHECK(!st.hasModel(), "starts empty");
    std::vector<float> in = sineBlock(0, 256);
    std::vector<float> out(256, 9.0f);
    st.processBlock(in.data(), out.data(), 256);
    bool exact = true;
    for (int i = 0; i < 256; ++i)
      exact = exact && out[i] == in[i];
    TDM_CHECK(exact, "bypass copies exactly");
  }
  // Missing / corrupt models throw outside realtime, keeping old state.
  {
    bool threw = false;
    try
    {
      tdm::NamStage st;
      st.loadModel("/tmp/tdm_test_missing.nam", 48000.0, 512);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "missing model throws");
  }
  {
    std::ofstream f("/tmp/tdm_test_garbage.nam", std::ios::trunc);
    f << "{ this is not json";
    f.close();
    bool threw = false;
    try
    {
      tdm::NamStage st;
      st.loadModel("/tmp/tdm_test_garbage.nam", 48000.0, 512);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "garbage model throws");
    std::remove("/tmp/tdm_test_garbage.nam");
  }
  // Real A2 model: loads, runs finite/bounded, deterministic, resettable.
  const std::string path = modelPath();
  {
    std::ifstream probe(path);
    TDM_CHECK(static_cast<bool>(probe), std::string("A2 example model present: ") + path);
    if (!probe)
      return;
  }
  tdm::NamStage st;
  st.loadModel(path, 48000.0, 512);
  TDM_CHECK(st.hasModel(), "model loaded");
  TDM_CHECK_CLOSE(st.expectedSampleRate(), 48000.0, 1.0, "model expects 48k");
  {
    std::vector<float> z(512, 0.0f), o(512, 9.0f);
    st.processBlock(z.data(), o.data(), 512);
    TDM_CHECK(tdm_test::allFinite(o.data(), 512), "silence -> finite");
  }
  {
    std::vector<float> in = sineBlock(0, 4800);
    std::vector<float> o(4800);
    for (int off = 0; off < 4800; off += 512)
      st.processBlock(in.data() + off, o.data() + off, 512);
    TDM_CHECK(tdm_test::allFinite(o.data(), 4800), "sine -> finite");
    TDM_CHECK(tdm_test::peakAbs(o.data(), 4800) < 20.0f, "sine -> bounded");
    TDM_CHECK(tdm_test::peakAbs(o.data(), 4800) > 0.001f, "sine -> non-silent (amp does something)");
  }
  {
    // Two identically loaded stages fed identically must agree bit-exactly.
    tdm::NamStage other;
    other.loadModel(path, 48000.0, 512);
    std::vector<float> in = sineBlock(0, 1024);
    std::vector<float> o1(1024), o2(1024);
    st.reset(48000.0, 512);
    other.reset(48000.0, 512);
    st.processBlock(in.data(), o1.data(), 1024); // chunked internally (> maxBlock)
    other.processBlock(in.data(), o2.data(), 1024);
    bool exact = true;
    for (int i = 0; i < 1024; ++i)
      exact = exact && o1[i] == o2[i];
    TDM_CHECK(exact, "identical stages are bit-exact (incl. chunking)");
  }
  {
    // SR mismatch is rejected at load time (no resampler in M0).
    bool threw = false;
    try
    {
      tdm::NamStage other;
      other.loadModel(path, 44100.0, 512);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "44.1k host vs 48k model throws");
  }
}

#include "tests/Assert.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "dsp/WavFile.h"

namespace
{
// Hand-crafted PCM16 mono WAV: exercises the integer decode path that most
// real IRs use (our own writer only emits float32).
void writePcm16Mono(const std::string& path, const std::vector<int16_t>& samples, uint32_t sr)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
  f.write("RIFF", 4);
  uint32_t chunk = 36 + dataBytes;
  f.write(reinterpret_cast<const char*>(&chunk), 4); // LE on x86/ARM mac
  f.write("WAVEfmt ", 8);
  uint32_t v32 = 16;
  f.write(reinterpret_cast<const char*>(&v32), 4);
  uint16_t v16 = 1;
  f.write(reinterpret_cast<const char*>(&v16), 2); // PCM
  v16 = 1;
  f.write(reinterpret_cast<const char*>(&v16), 2); // mono
  f.write(reinterpret_cast<const char*>(&sr), 4);
  v32 = sr * 2;
  f.write(reinterpret_cast<const char*>(&v32), 4);
  v16 = 2;
  f.write(reinterpret_cast<const char*>(&v16), 2);
  v16 = 16;
  f.write(reinterpret_cast<const char*>(&v16), 2);
  f.write("data", 4);
  f.write(reinterpret_cast<const char*>(&dataBytes), 4);
  f.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
}
} // namespace

void runWavTests()
{
  // float32 roundtrip is bit-exact (writer and reader memcpy the same bytes).
  {
    std::vector<float> sine(512);
    for (int i = 0; i < 512; ++i)
      sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * i / 64.0f);
    const float* ch[1] = {sine.data()};
    tdm::writeWavFloat32("/tmp/tdm_test_rt.wav", ch, 1, 512, 48000.0);
    tdm::MonoWav back = tdm::loadWavMono("/tmp/tdm_test_rt.wav");
    TDM_CHECK(back.sampleRate == 48000.0, "roundtrip rate");
    TDM_CHECK(back.sourceChannels == 1, "roundtrip channels");
    TDM_CHECK(back.samples.size() == 512, "roundtrip length");
    bool exact = back.samples.size() == sine.size();
    for (size_t i = 0; exact && i < sine.size(); ++i)
      exact = back.samples[i] == sine[i];
    TDM_CHECK(exact, "roundtrip bit-exact");
  }
  // Stereo averages to mono: L=+0.5, R=-0.5 -> silence.
  {
    std::vector<float> l(64, 0.5f), r(64, -0.5f);
    const float* ch[2] = {l.data(), r.data()};
    tdm::writeWavFloat32("/tmp/tdm_test_st.wav", ch, 2, 64, 44100.0);
    tdm::MonoWav back = tdm::loadWavMono("/tmp/tdm_test_st.wav");
    TDM_CHECK(back.sourceChannels == 2, "stereo source noted");
    TDM_CHECK(back.sampleRate == 44100.0, "stereo rate kept");
    TDM_CHECK(tdm_test::peakAbs(back.samples.data(), 64) == 0.0f, "stereo averaged to silence");
  }
  // PCM16 extremes decode to +-1.0 scale.
  {
    writePcm16Mono("/tmp/tdm_test_16.wav", {0, 32767, -32768, 16384}, 48000);
    tdm::MonoWav back = tdm::loadWavMono("/tmp/tdm_test_16.wav");
    TDM_CHECK(back.samples.size() == 4, "pcm16 length");
    TDM_CHECK_CLOSE(back.samples[0], 0.0f, 1e-6f, "pcm16 zero");
    TDM_CHECK_CLOSE(back.samples[1], 32767 / 32768.0f, 1e-6f, "pcm16 max");
    TDM_CHECK_CLOSE(back.samples[2], -1.0f, 1e-6f, "pcm16 min");
    TDM_CHECK_CLOSE(back.samples[3], 0.5f, 1e-6f, "pcm16 half");
  }
  // Missing / corrupt files throw outside realtime (never crash).
  {
    bool threw = false;
    try
    {
      tdm::loadWavMono("/tmp/tdm_test_does_not_exist.wav");
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "missing file throws");
  }
  {
    std::ofstream f("/tmp/tdm_test_bad.wav", std::ios::binary | std::ios::trunc);
    f << "this is not a wav file at all, just text........";
    f.close();
    bool threw = false;
    try
    {
      tdm::loadWavMono("/tmp/tdm_test_bad.wav");
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    TDM_CHECK(threw, "garbage file throws");
  }
  std::remove("/tmp/tdm_test_rt.wav");
  std::remove("/tmp/tdm_test_st.wav");
  std::remove("/tmp/tdm_test_16.wav");
  std::remove("/tmp/tdm_test_bad.wav");
}

// tdm_render: offline file renderer for M0 (deterministic, no hardware).
//
// Usage:
//   tdm_render --in di.wav --out processed.wav [--nam amp.nam] [--ir cab.wav]
//
// Input is averaged to mono, run through the rig in 1024-frame blocks,
// written back as mono float32 WAV at the input rate.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/WavFile.h"

namespace
{
void usage()
{
  std::printf("usage: tdm_render --in di.wav --out processed.wav [--nam amp.nam] [--ir cab.wav]\n");
}
} // namespace

int main(int argc, char** argv)
{
  std::string inPath, outPath, namPath, irPath;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto need = [&](const char* flag, std::string& dst) {
      if (i + 1 >= argc)
      {
        std::printf("missing value for %s\n", flag);
        usage();
        std::exit(2);
      }
      dst = argv[++i];
    };
    if (a == "--in")
      need("--in", inPath);
    else if (a == "--out")
      need("--out", outPath);
    else if (a == "--nam")
      need("--nam", namPath);
    else if (a == "--ir")
      need("--ir", irPath);
    else
    {
      usage();
      return 2;
    }
  }
  if (inPath.empty() || outPath.empty())
  {
    usage();
    return 2;
  }

  try
  {
    tdm::MonoWav in = tdm::loadWavMono(inPath);
    constexpr int kBlock = 1024;
    tdm::TechDeathRig rig;
    rig.reset(in.sampleRate, kBlock);
    if (!namPath.empty())
      rig.loadNam(namPath);
    if (!irPath.empty())
      rig.loadIr(irPath);

    const int total = static_cast<int>(in.samples.size());
    std::vector<float> out(static_cast<size_t>(total), 0.0f);
    const float* inPtrs[1] = {in.samples.data()};
    float* outPtrs[1] = {out.data()};
    for (int off = 0; off < total; off += kBlock)
    {
      const int m = (total - off) < kBlock ? (total - off) : kBlock;
      const float* bi[1] = {inPtrs[0] + off};
      float* bo[1] = {outPtrs[0] + off};
      rig.processBlock(bi, 1, bo, 1, m);
    }

    double peak = 0.0;
    for (float v : out)
      if (std::fabs(v) > peak)
        peak = std::fabs(v);
    tdm::writeWavFloat32(outPath, const_cast<const float**>(outPtrs), 1, total, in.sampleRate);
    std::printf("rendered %d frames @ %.0f Hz (nam=%s ir=%s) peak=%.4f -> %s\n", total, in.sampleRate,
                namPath.empty() ? "-" : namPath.c_str(), irPath.empty() ? "-" : irPath.c_str(), peak,
                outPath.c_str());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_render: error: %s\n", e.what());
    return 1;
  }
}

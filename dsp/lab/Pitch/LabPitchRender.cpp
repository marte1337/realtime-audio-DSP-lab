// tdm_labpitch: offline audition renderer for the lab pitch prototypes.
//
// Usage:
//   tdm_labpitch --in di.wav --out shifted.wav --shift -7 [--fft 4096] [--hop 1024]
//   tdm_labpitch --in di.wav --out shifted.wav --shift -7 --multi
//   tdm_labpitch --in di.wav --out shifted.wav --shift -7 --wsola [--wms 20|30|40]
//
// Renders the input through LabPitchShift (or LabMultiPitch with --multi,
// LabWsolaShift with --wsola; no rig, no NAM, no IR in any case). --multi
// rejects --fft/--hop (band configs are study-pinned inside LabMultiPitch).
// --wsola rejects --fft/--hop/--multi; --wms selects the study-pinned
// WSOLA window (default 30 ms).
// --shift 0 is a true bit-exact bypass (latency 0, output == input).
// Output is latency-compensated: the tool feeds latency + tail extra
// zeros and drops the first latency outputs, so out[i] corresponds to
// in[i] shifted. Exit 0 on success, 1 on error, 2 on usage error.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/WavFile.h"
#include "dsp/lab/Pitch/LabMultiPitch.h"
#include "dsp/lab/Pitch/LabPitchShift.h"
#include "dsp/lab/Pitch/LabWsolaShift.h"

namespace
{
void usage()
{
  std::printf("usage: tdm_labpitch --in di.wav --out shifted.wav --shift -24..0 "
              "[--fft 256..16384] [--hop N/8..N/2] [--multi] [--wsola [--wms 20|30|40]]\n");
}

// Shared offline render: feed input + latency + tail zeros in blocks,
// drop the first latency outputs. P is LabPitchShift, LabMultiPitch or
// LabWsolaShift (same method shape, deliberately).
template <typename P>
std::vector<float> renderThrough(P& p, const std::vector<float>& in, int* latency)
{
  const int total = static_cast<int>(in.size());
  const int lat = p.latencySamples();
  const int paddedN = total + lat + p.tailSamples();
  std::vector<float> padded(static_cast<size_t>(paddedN), 0.0f);
  for (int i = 0; i < total; ++i)
    padded[static_cast<size_t>(i)] = in[i];
  std::vector<float> raw(static_cast<size_t>(paddedN), 0.0f);
  constexpr int kBlock = 1024;
  for (int off = 0; off < paddedN; off += kBlock)
  {
    const int m = (paddedN - off) < kBlock ? (paddedN - off) : kBlock;
    p.processBlock(padded.data() + off, raw.data() + off, m);
  }
  std::vector<float> out(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i)
    out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i + lat)];
  *latency = lat;
  return out;
}
} // namespace

int main(int argc, char** argv)
{
  std::string inPath, outPath, shift, fft, hop, wms;
  bool multi = false;
  bool wsola = false;
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
    else if (a == "--shift")
      need("--shift", shift);
    else if (a == "--fft")
      need("--fft", fft);
    else if (a == "--hop")
      need("--hop", hop);
    else if (a == "--multi")
      multi = true;
    else if (a == "--wsola")
      wsola = true;
    else if (a == "--wms")
      need("--wms", wms);
    else
    {
      usage();
      return 2;
    }
  }
  if (inPath.empty() || outPath.empty() || shift.empty())
  {
    usage();
    return 2;
  }
  if (multi && (!fft.empty() || !hop.empty()))
  {
    std::printf("tdm_labpitch: --multi rejects --fft/--hop (band configs are pinned)\n");
    return 2;
  }
  if (wsola && (multi || !fft.empty() || !hop.empty()))
  {
    std::printf("tdm_labpitch: --wsola rejects --fft/--hop/--multi\n");
    return 2;
  }
  if (!wms.empty() && !wsola)
  {
    std::printf("tdm_labpitch: --wms needs --wsola\n");
    return 2;
  }

  try
  {
    tdm::MonoWav in = tdm::loadWavMono(inPath);
    const int total = static_cast<int>(in.samples.size());
    const float shiftSt = std::stof(shift);
    std::vector<float> out;
    int lat = 0;
    std::string cfg;
    if (multi)
    {
      tdm::lab::LabMultiPitch p;
      p.setShiftSt(shiftSt);
      p.setEnabled(true);
      p.reset(in.sampleRate);
      out = renderThrough(p, in.samples, &lat);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "multi(4096/1024+2048/256,xover=%d,align=%d)", p.crossoverDelay(),
                    p.alignDelay());
      cfg = buf;
    }
    else if (wsola)
    {
      tdm::lab::LabWsolaShift p;
      if (!wms.empty())
        p.setConfig(std::stod(wms));
      p.setShiftSt(shiftSt);
      p.setEnabled(true);
      p.reset(in.sampleRate);
      out = renderThrough(p, in.samples, &lat);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "wsola(W=%d,Ha=%d,Hs=%d,D=%d)", p.frameLen(), p.analysisHop(),
                    p.synthHop(), p.tolerance());
      cfg = buf;
    }
    else
    {
      tdm::lab::LabPitchShift p;
      if (!fft.empty() || !hop.empty())
      {
        const int n = fft.empty() ? tdm::lab::LabPitchShift::kDefaultFftSize : std::stoi(fft);
        const int h = hop.empty() ? n / 4 : std::stoi(hop);
        p.setConfig(n, h);
      }
      p.setShiftSt(shiftSt);
      p.setEnabled(true);
      p.reset(in.sampleRate);
      out = renderThrough(p, in.samples, &lat);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "fft=%d hop=%d", p.fftSize(), p.hop());
      cfg = buf;
    }
    double peak = 0.0;
    for (int i = 0; i < total; ++i)
      if (std::fabs(out[static_cast<size_t>(i)]) > peak)
        peak = std::fabs(out[static_cast<size_t>(i)]);
    const float* ch[1] = {out.data()};
    tdm::writeWavFloat32(outPath, ch, 1, total, in.sampleRate);
    std::printf("labpitch: %d frames @ %.0f Hz shift=%.2f st %s latency=%d (%.1f ms) peak=%.4f -> %s\n",
                total, in.sampleRate, shiftSt, cfg.c_str(), lat, 1000.0 * lat / in.sampleRate, peak,
                outPath.c_str());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_labpitch: error: %s\n", e.what());
    return 1;
  }
}

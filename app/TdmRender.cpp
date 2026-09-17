// tdm_render: offline file renderer for M0 (deterministic, no hardware).
//
// Usage:
//   tdm_render --in di.wav --out processed.wav [--nam amp.nam] [--ir cab.wav]
//              [--gate-thresh db] [--gate-rel ms] [--input-trim db]
//              [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]
//              [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]
//              [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]
//              [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]
//              [--output-trim db]
//
// Passing either gate flag enables TechDeathGate (the other keeps its
// default); without gate flags the gate bypasses exactly (Milestone 0 path).
// Passing --tight-drive or any of --tight/--drive/--bite enables TightDrive
// (unspecified params keep their defaults); otherwise it bypasses exactly.
// Passing --tone-shape or any of --weight/--contour/--presence enables
// ToneShape (unspecified params keep their defaults); otherwise it bypasses
// exactly.
// Passing --delay or any of --delay-time/--delay-fb/--delay-mix enables
// Delay; passing --reverb or any of --reverb-decay/--reverb-mix enables
// Reverb (unspecified params keep their defaults); otherwise each unit
// bypasses exactly (dry is bit-exact).
//
// Input is averaged to mono, run through the rig in 1024-frame blocks,
// written back as mono float32 WAV at the input rate (exact L/R fold-down).

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
  std::printf("usage: tdm_render --in di.wav --out processed.wav [--nam amp.nam] [--ir cab.wav] "
              "[--gate-thresh db] [--gate-rel ms] [--input-trim db]\n"
              "       [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]\n"
              "       [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]\n"
              "       [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]\n"
              "       [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]\n"
              "       [--output-trim db]\n");
}
} // namespace

int main(int argc, char** argv)
{
  std::string inPath, outPath, namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite, weight, contour, presence, outputTrim;
  std::string delayTime, delayFb, delayMix, reverbDecay, reverbMix;
  bool driveEnable = false, shapeEnable = false, delayEnable = false, reverbEnable = false;
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
    else if (a == "--gate-thresh")
      need("--gate-thresh", gateThresh);
    else if (a == "--gate-rel")
      need("--gate-rel", gateRel);
    else if (a == "--input-trim")
      need("--input-trim", inputTrim);
    else if (a == "--tight-drive")
      driveEnable = true;
    else if (a == "--tight")
      need("--tight", tight);
    else if (a == "--drive")
      need("--drive", drive);
    else if (a == "--bite")
      need("--bite", bite);
    else if (a == "--tone-shape")
      shapeEnable = true;
    else if (a == "--weight")
      need("--weight", weight);
    else if (a == "--contour")
      need("--contour", contour);
    else if (a == "--presence")
      need("--presence", presence);
    else if (a == "--delay")
      delayEnable = true;
    else if (a == "--delay-time")
      need("--delay-time", delayTime);
    else if (a == "--delay-fb")
      need("--delay-fb", delayFb);
    else if (a == "--delay-mix")
      need("--delay-mix", delayMix);
    else if (a == "--reverb")
      reverbEnable = true;
    else if (a == "--reverb-decay")
      need("--reverb-decay", reverbDecay);
    else if (a == "--reverb-mix")
      need("--reverb-mix", reverbMix);
    else if (a == "--output-trim")
      need("--output-trim", outputTrim);
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
    if (!inputTrim.empty())
      rig.setInputTrimDb(std::stof(inputTrim));
    if (!gateThresh.empty() || !gateRel.empty())
    {
      if (!gateThresh.empty())
        rig.setGateThresholdDb(std::stof(gateThresh));
      if (!gateRel.empty())
        rig.setGateReleaseMs(std::stof(gateRel));
      rig.setGateEnabled(true);
    }
    if (driveEnable || !tight.empty() || !drive.empty() || !bite.empty())
    {
      if (!tight.empty())
        rig.setTight(std::stof(tight));
      if (!drive.empty())
        rig.setDrive(std::stof(drive));
      if (!bite.empty())
        rig.setBite(std::stof(bite));
      rig.setDriveEnabled(true);
    }
    if (shapeEnable || !weight.empty() || !contour.empty() || !presence.empty())
    {
      if (!weight.empty())
        rig.setWeight(std::stof(weight));
      if (!contour.empty())
        rig.setContour(std::stof(contour));
      if (!presence.empty())
        rig.setPresence(std::stof(presence));
      rig.setShapeEnabled(true);
    }
    if (delayEnable || !delayTime.empty() || !delayFb.empty() || !delayMix.empty())
    {
      if (!delayTime.empty())
        rig.setDelayTimeMs(std::stof(delayTime));
      if (!delayFb.empty())
        rig.setDelayFeedback(std::stof(delayFb));
      if (!delayMix.empty())
        rig.setDelayMix(std::stof(delayMix));
      rig.setDelayEnabled(true);
    }
    if (reverbEnable || !reverbDecay.empty() || !reverbMix.empty())
    {
      if (!reverbDecay.empty())
        rig.setReverbDecay(std::stof(reverbDecay));
      if (!reverbMix.empty())
        rig.setReverbMix(std::stof(reverbMix));
      rig.setReverbEnabled(true);
    }
    if (!namPath.empty())
      rig.loadNam(namPath);
    if (!irPath.empty())
      rig.loadIr(irPath);
    if (!outputTrim.empty())
      rig.setOutputTrimDb(std::stof(outputTrim));

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
    std::printf("rendered %d frames @ %.0f Hz (nam=%s ir=%s gate=%s trim=%.1f drive=%s shape=%s delay=%s reverb=%s out trim=%.1f) peak=%.4f -> %s\n",
                total, in.sampleRate, namPath.empty() ? "-" : namPath.c_str(),
                irPath.empty() ? "-" : irPath.c_str(), rig.isGateEnabled() ? "on" : "off", rig.inputTrimDb(),
                rig.isDriveEnabled() ? "on" : "off", rig.isShapeEnabled() ? "on" : "off",
                rig.isDelayEnabled() ? "on" : "off", rig.isReverbEnabled() ? "on" : "off",
                rig.outputTrimDb(), peak, outPath.c_str());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_render: error: %s\n", e.what());
    return 1;
  }
}

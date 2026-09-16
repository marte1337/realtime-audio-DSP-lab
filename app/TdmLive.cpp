// tdm_live: minimal CoreAudio duplex runner.
//
// Default input -> lock-free ring -> rig -> default output.
// Loads assets BEFORE audio starts; there is no live model swapping.
// Input/output devices must run at the same rate (set via Audio MIDI Setup),
// and that rate must match the NAM/IR assets (no resampling: mismatches fail
// loudly at load/start time by design).
//
// Usage:
//   tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms]
//              [--input-trim db]
//              [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]
//              [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]
//              [--output-trim db]
//   tdm_live --list            (show audio devices and exit)
//
// Passing either gate flag enables TechDeathGate (the other keeps its
// default); without gate flags the gate bypasses exactly (Milestone 0 path).
// Passing --tight-drive or any of --tight/--drive/--bite enables TightDrive
// (unspecified params keep their defaults); otherwise it bypasses exactly.
// Passing --tone-shape or any of --weight/--contour/--presence enables
// ToneShape (unspecified params keep their defaults); otherwise it bypasses
// exactly.
//
// Audio behavior lives in host/TdmEngine (shared with the developer app);
// this file is only flag parsing plus the run loop.

#include <cstdio>
#include <string>

#include "dsp/RigParams.h"
#include "host/TdmEngine.h"

int main(int argc, char** argv)
{
  std::string namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite, weight, contour, presence, outputTrim;
  bool driveEnable = false, shapeEnable = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--list")
    {
      std::string out, error;
      if (!TdmEngine::listDevices(out, error))
      {
        std::printf("tdm_live: error: %s\n", error.c_str());
        return 1;
      }
      std::printf("%s", out.c_str());
      return 0;
    }
    if (a == "--tight-drive")
    {
      driveEnable = true;
      continue;
    }
    if (a == "--tone-shape")
    {
      shapeEnable = true;
      continue;
    }
    if ((a == "--nam" || a == "--ir" || a == "--gate-thresh" || a == "--gate-rel" || a == "--input-trim"
         || a == "--tight" || a == "--drive" || a == "--bite" || a == "--weight" || a == "--contour"
         || a == "--presence" || a == "--output-trim")
        && i + 1 < argc)
    {
      if (a == "--nam")
        namPath = argv[++i];
      else if (a == "--ir")
        irPath = argv[++i];
      else if (a == "--gate-thresh")
        gateThresh = argv[++i];
      else if (a == "--gate-rel")
        gateRel = argv[++i];
      else if (a == "--input-trim")
        inputTrim = argv[++i];
      else if (a == "--tight")
        tight = argv[++i];
      else if (a == "--drive")
        drive = argv[++i];
      else if (a == "--bite")
        bite = argv[++i];
      else if (a == "--weight")
        weight = argv[++i];
      else if (a == "--contour")
        contour = argv[++i];
      else if (a == "--presence")
        presence = argv[++i];
      else
        outputTrim = argv[++i];
      continue;
    }
    std::printf("usage: tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms] "
                "[--input-trim db] [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]\n"
                "       [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]\n"
                "       [--output-trim db] | --list\n");
    return 2;
  }

  try
  {
    TdmEngine engine;
    tdm::RigParams params;
    if (!inputTrim.empty())
      params.inputTrimDb = std::stof(inputTrim);
    if (!gateThresh.empty() || !gateRel.empty())
    {
      if (!gateThresh.empty())
        params.gateThresholdDb = std::stof(gateThresh);
      if (!gateRel.empty())
        params.gateReleaseMs = std::stof(gateRel);
      params.gateEnabled = true;
    }
    if (driveEnable || !tight.empty() || !drive.empty() || !bite.empty())
    {
      if (!tight.empty())
        params.tight = std::stof(tight);
      if (!drive.empty())
        params.drive = std::stof(drive);
      if (!bite.empty())
        params.bite = std::stof(bite);
      params.driveEnabled = true;
    }
    if (shapeEnable || !weight.empty() || !contour.empty() || !presence.empty())
    {
      if (!weight.empty())
        params.weight = std::stof(weight);
      if (!contour.empty())
        params.contour = std::stof(contour);
      if (!presence.empty())
        params.presence = std::stof(presence);
      params.shapeEnabled = true;
    }
    if (!outputTrim.empty())
      params.outputTrimDb = std::stof(outputTrim);
    engine.rig().setParams(params);

    std::string error;
    if (!namPath.empty() && !engine.loadNam(namPath, error))
    {
      std::printf("tdm_live: error: %s\n", error.c_str());
      return 1;
    }
    if (!irPath.empty() && !engine.loadIr(irPath, error))
    {
      std::printf("tdm_live: error: %s\n", error.c_str());
      return 1;
    }
    if (!engine.start(error))
    {
      std::printf("tdm_live: error: %s\n", error.c_str());
      return 1;
    }

    const tdm::RigParams applied = engine.rig().params();
    std::printf("live: %.0fHz nam=%s ir=%s gate=%s trim=%.1f drive=%s shape=%s out trim=%.1f\n",
                engine.sampleRate(), engine.rig().hasNam() ? "yes" : "no",
                engine.rig().hasIr() ? "yes" : "no", applied.gateEnabled ? "on" : "off", applied.inputTrimDb,
                applied.driveEnabled ? "on" : "off", applied.shapeEnabled ? "on" : "off",
                applied.outputTrimDb);
    std::printf("press q + Enter to quit\n");
    char line[64] = {};
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
      if (line[0] == 'q' || line[0] == 'Q')
        break;

    engine.stop();
    std::printf("stopped: blocks=%llu underrunFrames=%llu overrunFrames=%llu\n",
                (unsigned long long)engine.blocks(), (unsigned long long)engine.underruns(),
                (unsigned long long)engine.overruns());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_live: error: %s\n", e.what());
    return 1;
  }
}

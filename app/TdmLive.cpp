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
//              [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]
//              [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]
//              [--output-trim db]
//              [--lab-wsola 0|-1|-2|-7]   (LAB AUDITION: W20 WSOLA pre-rig insert)
//              [--buffer N]              (request CoreAudio buffer frames)
//   tdm_live --list            (show audio devices and exit)
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
// bypasses exactly.
//
// Audio behavior lives in host/TdmEngine (shared with the developer app);
// this file is only flag parsing plus the run loop.
//
// LAB AUDITION (--lab-wsola): inserts the W20 WSOLA lab shifter before the
// rig for live guitar evaluation. Shift is fixed for the run (restart to
// change it); type e + Enter while running to toggle enable (click-free),
// q + Enter to quit. No latency compensation anywhere in the live path:
// output lags input by WSOLA latency + device buffering (all printed).

#include <cstdio>
#include <string>

#include "dsp/RigParams.h"
#include "host/BufferRequest.h"
#include "host/TdmEngine.h"

int main(int argc, char** argv)
{
  std::string namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite, weight, contour, presence, outputTrim;
  std::string delayTime, delayFb, delayMix, reverbDecay, reverbMix;
  std::string labWsola;
  std::string buffer;
  bool driveEnable = false, shapeEnable = false, delayEnable = false, reverbEnable = false;
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
    if (a == "--delay")
    {
      delayEnable = true;
      continue;
    }
    if (a == "--reverb")
    {
      reverbEnable = true;
      continue;
    }
    if ((a == "--nam" || a == "--ir" || a == "--gate-thresh" || a == "--gate-rel" || a == "--input-trim"
         || a == "--tight" || a == "--drive" || a == "--bite" || a == "--weight" || a == "--contour"
         || a == "--presence" || a == "--delay-time" || a == "--delay-fb" || a == "--delay-mix"
         || a == "--reverb-decay" || a == "--reverb-mix" || a == "--output-trim" || a == "--lab-wsola"
         || a == "--buffer")
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
      else if (a == "--delay-time")
        delayTime = argv[++i];
      else if (a == "--delay-fb")
        delayFb = argv[++i];
      else if (a == "--delay-mix")
        delayMix = argv[++i];
      else if (a == "--reverb-decay")
        reverbDecay = argv[++i];
      else if (a == "--reverb-mix")
        reverbMix = argv[++i];
      else if (a == "--lab-wsola")
        labWsola = argv[++i];
      else if (a == "--buffer")
        buffer = argv[++i];
      else
        outputTrim = argv[++i];
      continue;
    }
    std::printf("usage: tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms] "
                "[--input-trim db] [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]\n"
                "       [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]\n"
                "       [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]\n"
                "       [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]\n"
                "       [--output-trim db] [--lab-wsola 0|-1|-2|-7] [--buffer N] | --list\n");
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
    if (delayEnable || !delayTime.empty() || !delayFb.empty() || !delayMix.empty())
    {
      if (!delayTime.empty())
        params.delayTimeMs = std::stof(delayTime);
      if (!delayFb.empty())
        params.delayFeedback = std::stof(delayFb);
      if (!delayMix.empty())
        params.delayMix = std::stof(delayMix);
      params.delayEnabled = true;
    }
    if (reverbEnable || !reverbDecay.empty() || !reverbMix.empty())
    {
      if (!reverbDecay.empty())
        params.reverbDecay = std::stof(reverbDecay);
      if (!reverbMix.empty())
        params.reverbMix = std::stof(reverbMix);
      params.reverbEnabled = true;
    }
    if (!outputTrim.empty())
      params.outputTrimDb = std::stof(outputTrim);
    engine.rig().setParams(params);
    if (!labWsola.empty())
    {
      const float st = std::stof(labWsola);
      if (st != 0.0f && st != -1.0f && st != -2.0f && st != -7.0f)
      {
        std::printf("tdm_live: error: --lab-wsola must be one of 0|-1|-2|-7\n");
        return 2;
      }
      engine.configureLabWsola(st);
    }
    if (!buffer.empty())
    {
      int b = 0;
      try
      {
        b = std::stoi(buffer);
      }
      catch (...)
      {
        b = 0;
      }
      if (b <= 0)
      {
        std::printf("tdm_live: error: --buffer must be a positive frame count\n");
        return 2;
      }
      engine.setRequestedBufferFrames(b);
    }

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
    std::printf("live: %.0fHz nam=%s ir=%s gate=%s trim=%.1f drive=%s shape=%s delay=%s reverb=%s out trim=%.1f\n",
                engine.sampleRate(), engine.rig().hasNam() ? "yes" : "no",
                engine.rig().hasIr() ? "yes" : "no", applied.gateEnabled ? "on" : "off", applied.inputTrimDb,
                applied.driveEnabled ? "on" : "off", applied.shapeEnabled ? "on" : "off",
                applied.delayEnabled ? "on" : "off", applied.reverbEnabled ? "on" : "off",
                applied.outputTrimDb);
    {
      // Startup latency report: requested vs ACTUAL everywhere. Nothing is
      // compensated: output lags input by the printed total. Ring slack is
      // the input-thread/output-thread decoupling (~0 to 1 output block).
      const double sr = engine.sampleRate();
      const int inb = engine.inputBufferFrames(), outb = engine.outputBufferFrames();
      const int req = engine.requestedBufferFrames();
      const int wlat = engine.labWsolaLatency();
      const double devMs = 1000.0 * (inb + outb) / sr;
      const double wsolaMs = 1000.0 * wlat / sr;
      const double slackMs = 1000.0 * outb / sr;
      std::printf("audio: rate=%.0fHz requested buffer=%s\n", sr,
                  req > 0 ? std::to_string(req).c_str() : "default (flag omitted)");
      std::printf("audio: %s\n",
                  tdm_host::bufferReportLine("input", req > 0 ? static_cast<unsigned>(req) : 0,
                                             static_cast<unsigned>(inb), engine.inputBufferNote())
                      .c_str());
      std::printf("audio: in-device='%s'\n", engine.inputDeviceName().c_str());
      std::printf("audio: %s\n",
                  tdm_host::bufferReportLine("output", req > 0 ? static_cast<unsigned>(req) : 0,
                                             static_cast<unsigned>(outb), engine.outputBufferNote())
                      .c_str());
      std::printf("audio: out-device='%s'\n", engine.outputDeviceName().c_str());
      if (engine.labWsolaConfigured())
        std::printf("lab-wsola: W20 shift=%.0f st ENABLED (chain: input -> wsola -> rig -> output)\n",
                    std::stof(labWsola));
      std::printf("latency: device=%.1fms wsola=%.1fms ring-slack~0-%.1fms => total ~%.1f-%.1fms (uncompensated)\n",
                  devMs, wsolaMs, slackMs, devMs + wsolaMs, devMs + wsolaMs + slackMs);
    }
    std::printf("press q + Enter to quit%s\n",
                engine.labWsolaConfigured() ? ", e + Enter to toggle pitch" : "");
    bool wsOn = true;
    char line[64] = {};
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
      if (line[0] == 'q' || line[0] == 'Q')
        break;
      if ((line[0] == 'e' || line[0] == 'E') && engine.labWsolaConfigured())
      {
        wsOn = !wsOn;
        engine.setLabWsolaEnabled(wsOn);
        std::printf("lab-wsola: %s (latency-matched bypass, constant feel)\n", wsOn ? "ENABLED" : "bypassed");
      }
    }

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

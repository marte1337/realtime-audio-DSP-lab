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
//              [--transpose-shift ST]   (production GuitarTranspose, -12..+12)
//              [--tuner]                (chromatic tuner side-chain; u + Enter prints it)
//              [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]
//              [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]
//              [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]
//              [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]
//              [--output-trim db]
//              [--lab-wsola 0|-1|-2|-7]   (LAB AUDITION: WSOLA pre-rig insert)
//              [--lab-wsola-cfg WMS:TOLM:TOLP] (LAB AUDITION: shifter geometry;
//                                            default 20:0:0 = accepted baseline;
//                                            e.g. 20:640:320 latency-study geometry)
//              [--slam a|c|d]            (SLAM AUDITION: finalist insert; a = pre-rig
//                                        parallel mass, c = post-rig parallel mass,
//                                        d = split-tap transient burst)
//              [--slam-band Hz]          (SLAM AUDITION: branch corner, a/c only)
//              [--slam-amount 0..2]      (SLAM AUDITION: branch/burst blend)
//              [--slam-delay-ms ms]      (SLAM AUDITION: retard d-burst for rig
//                                        latency; 0 normally, 16 with transpose)
//              [--buffer N]              (request CoreAudio buffer frames)
//   tdm_live --list            (show audio devices and exit)
//
// Passing either gate flag enables TechDeathGate (the other keeps its
// default); without gate flags the gate bypasses exactly (Milestone 0 path).
// Passing --transpose-shift enables the production GuitarTranspose at that
// shift (Gate -> Transpose -> TightDrive position, baseline config). The
// validated primary use is fixed detune -1/-2; the flag accepts -12..+12
// (clamped). Type t + Enter while running to toggle transpose live
// (click-free); q + Enter quits.
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
#ifdef TDM_BENCH_LIVE
// BENCH AUDITION (--bench ID:SHIFT, this binary only): inserts a lab
// shifter before the rig (chain: input -> insert -> rig -> output).
// Accepted ids: "rb2" (Rubber Band R2 realtime), "t3k30" (TONE3000
// Transpose, 30 ms buffer, tonality off) and "gt2" (our own
// GuitarTranspose, default config); SHIFT is one of 0|-1|-2|-7 and
// fixed for the run. No e-toggle: the insert is always on. No latency
// compensation: output lags input by insert latency + device buffering
// (all printed).
#endif

#include <cstdio>
#include <string>

#include "dsp/RigParams.h"
#include "dsp/Tuner/Tuner.h"
#include "host/BufferRequest.h"
#include "host/TdmEngine.h"

int main(int argc, char** argv)
{
  std::string namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite, weight, contour, presence, outputTrim;
  std::string delayTime, delayFb, delayMix, reverbDecay, reverbMix;
  std::string transposeShift;
  bool tunerEnable = false;
  std::string labWsola;
  std::string labWsolaCfg;
  std::string labSlam, labSlamBand, labSlamAmount, labSlamDelay;
#ifdef TDM_BENCH_LIVE
  std::string labBench; // "ID:SHIFT", e.g. "rb2:-1"
#endif
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
    if (a == "--tuner")
    {
      tunerEnable = true;
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
         || a == "--transpose-shift" || a == "--tight" || a == "--drive" || a == "--bite"
         || a == "--weight" || a == "--contour"
         || a == "--presence" || a == "--delay-time" || a == "--delay-fb" || a == "--delay-mix"
         || a == "--reverb-decay" || a == "--reverb-mix" || a == "--output-trim" || a == "--lab-wsola"
         || a == "--lab-wsola-cfg" || a == "--buffer" || a == "--slam" || a == "--slam-band"
         || a == "--slam-amount" || a == "--slam-delay-ms"
#ifdef TDM_BENCH_LIVE
         || a == "--bench"
#endif
         )
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
      else if (a == "--transpose-shift")
        transposeShift = argv[++i];
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
      else if (a == "--lab-wsola-cfg")
        labWsolaCfg = argv[++i];
      else if (a == "--slam")
        labSlam = argv[++i];
      else if (a == "--slam-band")
        labSlamBand = argv[++i];
      else if (a == "--slam-amount")
        labSlamAmount = argv[++i];
      else if (a == "--slam-delay-ms")
        labSlamDelay = argv[++i];
      else if (a == "--buffer")
        buffer = argv[++i];
#ifdef TDM_BENCH_LIVE
      else if (a == "--bench")
        labBench = argv[++i];
#endif
      else
        outputTrim = argv[++i];
      continue;
    }
    std::printf("usage: tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms] "
                "[--input-trim db] [--transpose-shift ST] [--tuner]\n"
                "       [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]\n"
                "       [--tone-shape] [--weight 0..1] [--contour 0..1] [--presence 0..1]\n"
                "       [--delay] [--delay-time ms] [--delay-fb 0..0.85] [--delay-mix 0..1]\n"
                "       [--reverb] [--reverb-decay 0..1] [--reverb-mix 0..1]\n"
                "       [--output-trim db] [--lab-wsola 0|-1|-2|-7] [--lab-wsola-cfg WMS:TOLM:TOLP]\n"
                "       [--slam a|c|d] [--slam-band Hz] [--slam-amount 0..2] [--slam-delay-ms ms]\n"
                "       [--buffer N] | --list\n");
#ifdef TDM_BENCH_LIVE
    std::printf("note: this is tdm_bench_live; it also accepts [--bench rb2|t3k30|gt2:0|-1|-2|-7]\n");
#endif
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
    if (!transposeShift.empty())
    {
      params.transposeSemitones = std::stof(transposeShift);
      params.transposeEnabled = true;
    }
    if (tunerEnable)
      params.tunerEnabled = true;
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
    if (!labWsolaCfg.empty() && labWsola.empty())
    {
      std::printf("tdm_live: error: --lab-wsola-cfg needs --lab-wsola\n");
      return 2;
    }
    if (!labWsola.empty())
    {
      const float st = std::stof(labWsola);
      if (st != 0.0f && st != -1.0f && st != -2.0f && st != -7.0f)
      {
        std::printf("tdm_live: error: --lab-wsola must be one of 0|-1|-2|-7\n");
        return 2;
      }
      double wms = 20.0;
      int tolm = 0, tolp = 0;
      if (!labWsolaCfg.empty())
      {
        const size_t c1 = labWsolaCfg.find(':');
        const size_t c2 = labWsolaCfg.find(':', c1 == std::string::npos ? 0 : c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos)
        {
          std::printf("tdm_live: error: --lab-wsola-cfg must be WMS:TOLM:TOLP in samples\n");
          return 2;
        }
        try
        {
          wms = std::stod(labWsolaCfg.substr(0, c1));
          tolm = std::stoi(labWsolaCfg.substr(c1 + 1, c2 - c1 - 1));
          tolp = std::stoi(labWsolaCfg.substr(c2 + 1));
        }
        catch (...)
        {
          std::printf("tdm_live: error: --lab-wsola-cfg must be WMS:TOLM:TOLP in samples\n");
          return 2;
        }
      }
      engine.configureLabWsola(st, wms, tolm, tolp);
    }
    if (!labSlam.empty())
    {
      // SLAM LAB AUDITION: one finalist insert per run, never combined
      // with --lab-wsola (one audition at a time).
      if (!labWsola.empty())
      {
        std::printf("tdm_live: error: --slam cannot be combined with --lab-wsola\n");
        return 2;
      }
      if (labSlam.size() != 1 || (labSlam[0] != 'a' && labSlam[0] != 'c' && labSlam[0] != 'd'))
      {
        std::printf("tdm_live: error: --slam must be one of a|c|d\n");
        return 2;
      }
      float band = (labSlam[0] == 'c') ? 220.0f : 140.0f; // study voices
      float amount = 1.0f;
      double delayMs = 0.0;
      try
      {
        if (!labSlamBand.empty())
          band = std::stof(labSlamBand);
        if (!labSlamAmount.empty())
          amount = std::stof(labSlamAmount);
        if (!labSlamDelay.empty())
          delayMs = std::stod(labSlamDelay);
      }
      catch (...)
      {
        std::printf("tdm_live: error: --slam-band/--slam-amount/--slam-delay-ms must be numeric\n");
        return 2;
      }
      if (band < 40.0f || band > 600.0f || amount < 0.0f || amount > 2.0f || delayMs < 0.0
          || delayMs > 100.0)
      {
        std::printf("tdm_live: error: --slam-band 40..600, --slam-amount 0..2, --slam-delay-ms "
                    "0..100\n");
        return 2;
      }
      engine.configureLabSlam(labSlam[0], band, amount, delayMs);
      std::printf("lab slam: mode=%c band=%.0fHz amount=%.2f burst-delay=%.1fms\n", labSlam[0], band,
                  amount, delayMs);
    }
    else if (!labSlamBand.empty() || !labSlamAmount.empty() || !labSlamDelay.empty())
    {
      std::printf("tdm_live: error: --slam-band/--slam-amount/--slam-delay-ms need --slam\n");
      return 2;
    }
#ifdef TDM_BENCH_LIVE
    if (!labBench.empty())
    {
      // BENCH AUDITION: "ID:SHIFT", exactly one audition config per run,
      // never combined with --lab-wsola (one pre-rig insert at a time).
      if (!labWsola.empty())
      {
        std::printf("tdm_bench_live: error: --bench cannot be combined with --lab-wsola\n");
        return 2;
      }
      const size_t c = labBench.find(':');
      const std::string id = c == std::string::npos ? labBench : labBench.substr(0, c);
      float st = 0.0f;
      bool ok = (c != std::string::npos);
      if (ok)
      {
        try
        {
          // Strict: reject trailing junk ("-1x") that stof would ignore.
          size_t pos = 0;
          st = std::stof(labBench.substr(c + 1), &pos);
          ok = (pos == labBench.size() - c - 1);
        }
        catch (...)
        {
          ok = false;
        }
        ok = ok && (st == 0.0f || st == -1.0f || st == -2.0f || st == -7.0f);
      }
      if ((id != "rb2" && id != "t3k30" && id != "gt2") || !ok)
      {
        std::printf("tdm_bench_live: error: --bench must be rb2|t3k30|gt2:0|-1|-2|-7\n");
        return 2;
      }
      engine.configureLabBench(id, st);
    }
#endif
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
    char transposeDesc[32];
    if (applied.transposeEnabled)
      std::snprintf(transposeDesc, sizeof(transposeDesc), "%.1f st", (double)applied.transposeSemitones);
    else
      std::snprintf(transposeDesc, sizeof(transposeDesc), "off");
    std::printf("live: %.0fHz nam=%s ir=%s gate=%s trim=%.1f transpose=%s tuner=%s drive=%s shape=%s "
                "delay=%s reverb=%s out trim=%.1f\n",
                engine.sampleRate(), engine.rig().hasNam() ? "yes" : "no",
                engine.rig().hasIr() ? "yes" : "no", applied.gateEnabled ? "on" : "off", applied.inputTrimDb,
                transposeDesc, applied.tunerEnabled ? "on" : "off", applied.driveEnabled ? "on" : "off",
                applied.shapeEnabled ? "on" : "off", applied.delayEnabled ? "on" : "off",
                applied.reverbEnabled ? "on" : "off", applied.outputTrimDb);
    {
      // Startup latency report: requested vs ACTUAL everywhere. Nothing is
      // compensated: output lags input by the printed total. Ring slack is
      // the input-thread/output-thread decoupling (~0 to 1 output block).
      const double sr = engine.sampleRate();
      const int inb = engine.inputBufferFrames(), outb = engine.outputBufferFrames();
      const int req = engine.requestedBufferFrames();
      const int wlat = engine.labWsolaLatency();
      // Production transpose latency counts only while engaged: the
      // disengaged tap is an exact zero-latency wire (hot engine aside).
      const int tlat = applied.transposeEnabled ? engine.rig().transposeLatencySamples() : 0;
      const double devMs = 1000.0 * (inb + outb) / sr;
      const double wsolaMs = 1000.0 * wlat / sr;
      const double transposeMs = 1000.0 * tlat / sr;
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
        std::printf("lab-wsola: W%.0f Dm=%d Dp=%d shift=%.0f st ENABLED (chain: input -> wsola -> rig -> "
                    "output)\n",
                    engine.labWsolaWindowMs(), engine.labWsolaTolM(), engine.labWsolaTolP(),
                    std::stof(labWsola));
#ifdef TDM_BENCH_LIVE
      if (engine.labBenchConfigured())
      {
        // BENCH AUDITION: shift echo reuses the validated --bench value.
        const size_t bc = labBench.find(':');
        std::printf("lab-bench: id=%s shift=%.0f st ENABLED (chain: input -> bench -> rig -> output)\n",
                    engine.labBenchId().c_str(), std::stof(labBench.substr(bc + 1)));
      }
#endif
      std::printf("latency: device=%.1fms wsola=%.1fms transpose=%.1fms ring-slack~0-%.1fms => total "
                    "~%.1f-%.1fms (uncompensated)\n",
                  devMs, wsolaMs, transposeMs, slackMs, devMs + wsolaMs + transposeMs,
                  devMs + wsolaMs + transposeMs + slackMs);
#ifdef TDM_BENCH_LIVE
      if (engine.labBenchConfigured())
      {
        const double benchMs = 1000.0 * engine.labBenchLatency() / sr;
        std::printf("latency: device=%.1fms bench=%.1fms ring-slack~0-%.1fms => total ~%.1f-%.1fms "
                    "(uncompensated)\n",
                    devMs, benchMs, slackMs, devMs + benchMs, devMs + benchMs + slackMs);
      }
#endif
    }
    std::printf("press q + Enter to quit, t + Enter to toggle transpose, u + Enter for tuner%s\n",
                engine.labWsolaConfigured() ? ", e + Enter to toggle lab pitch" : "");
    bool wsOn = true;
    bool trOn = applied.transposeEnabled;
    char line[64] = {};
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
      if (line[0] == 'q' || line[0] == 'Q')
        break;
      if (line[0] == 't' || line[0] == 'T')
      {
        trOn = !trOn;
        engine.rig().setTransposeEnabled(trOn);
        std::printf("transpose: %s (%.1f st, %d-sample nominal latency while engaged)\n", trOn ? "ENABLED" : "off",
                    (double)engine.rig().transposeSemitones(), engine.rig().transposeLatencySamples());
      }
      if (line[0] == 'u' || line[0] == 'U')
      {
        tdm::TunerResult tr;
        engine.rig().tunerResult(tr);
        if (!engine.rig().isTunerEnabled())
          std::printf("tuner: off (pass --tuner to analyze)\n");
        else if (!tr.valid)
          std::printf("tuner: --- (no pitched signal)\n");
        else
        {
          const tdm::TunerNote nt = tdm::Tuner::noteFor(tr.frequencyHz);
          std::printf("tuner: %s%d %+05.1f c %.2f Hz conf=%.2f\n", nt.name, nt.octave, tr.cents,
                      tr.frequencyHz, tr.confidence);
        }
      }
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

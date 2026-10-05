// tdm_transpose_dev: DEV-only transpose A/B live host (GT2 vs TONE3000).
//
// Chain: input -> InputTrim -> Gate -> DEV transpose -> TightDrive -> NAM ->
// IR -> ToneShape -> Space -> OutputTrim -> output. The transpose stage sits
// INSIDE the rig (between Gate and TightDrive) via the TransposeInsert seam,
// so both engines share the exact same input, shift, and downstream chain.
//
// DEV/benchmark only: links the frozen TONE3000 reference (MIT engine +
// AGPLv3 JUCE) and our lab GT2. Never a product binary. Production targets
// (tdm_live, tdm_render, tdm_dev) are untouched by this file.
//
// Usage:
//   tdm_transpose_dev [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] ...
//              (same rig flags as tdm_live)
//              [--transpose ENGINE:SHIFT]  ENGINE=gt2|ours|t3k|ref, SHIFT=-12..+12
//              [--transpose-off]           start bypassed (latency-matched)
//              [--t3k-window 20|30|40|60]  (default 30 = auditioned reference)
//              [--t3k-tonality off|HZ]     (default off; HZ=1000..20000)
//              [--gt2-window MS] [--gt2-floor MS] [--gt2-corr MS]
//              [--gt2-fademin MS] [--gt2-fademax MS]
//              [--gt2-fadencc-hi X] [--gt2-fadencc-lo X]
//              [--gt2-onsetfade MS] [--gt2-onsetspan MS] [--gt2-lead MS]
//              [--gt2-refr MS] [--gt2-hpf HZ] [--gt2-smooth MS]
//              [--gt2-overmin DB] [--gt2-overmax DB]
//              [--gt2-cells N] [--gt2-skip N] [--gt2-resync on|off]
//              [--no-standby]              render only the active engine
//              [--buffer N] | --list
//
// GT2 advanced flags are start-time only (GT2 derives geometry at reset);
// engine/shift/enabled/T3K-window/tonality switch live via stdin:
//
//   ours | gt2        select our GT2 (settings preserved per engine)
//   ref | t3k         select the TONE3000 reference
//   shift X           shared shift in -12..+12 (both engines follow)
//   on | off         transpose enabled / latency-matched bypass
//   t3k-window MS    reference window 20|30|40|60 (live)
//   t3k-tonality off|HZ  reference tonality (live)
//   status            print engine/shift/enabled/T3K/latencies
//   q                quit
//
// Notes:
// - Downstream (TightDrive onward) never changes on an engine switch.
// - GT2 reset at exact 0.0 st is a zero-latency wire until restart: live
//   shifts away from a 0-start need a restart on the GT2 side (the host
//   warns). TONE3000 follows live shifts through 0 at full latency.
// - No latency compensation anywhere: output lags input by the printed total.

#include <cstdio>
#include <string>

#include "dsp/RigParams.h"
#include "dsp/lab/Pitch/DevTranspose.h"
#include "host/BufferRequest.h"
#include "host/TdmEngine.h"

namespace
{
void printStatus(tdm::lab::DevTranspose& stage)
{
  std::printf("transpose: engine=%s shift=%.2f st %s | t3k window=%dms tonality=%s | "
              "lat gt2=%d t3k=%d sr=%.0f\n",
              tdm::lab::devEngineName(stage.engine()), stage.shiftSt(),
              stage.isEnabled() ? "ENABLED" : "bypassed", stage.t3kWindowMs(),
              stage.t3kTonalityHz() <= 0.0f ? "off"
                                            : std::to_string(static_cast<int>(stage.t3kTonalityHz())).c_str(),
              stage.gt2LatencySamples(), stage.t3kLatencySamples(), stage.sampleRate());
}

// Strict float parse (rejects trailing junk that stof would ignore).
std::string ltrimmed(const std::string& s)
{
  const size_t b = s.find_first_not_of(" \t");
  return b == std::string::npos ? "" : s.substr(b);
}
bool parseFloat(const std::string& s, float& out)
{
  try
  {
    size_t pos = 0;
    out = std::stof(s, &pos);
    return pos == s.size();
  }
  catch (...)
  {
    return false;
  }
}
bool parseDouble(const std::string& s, double& out)
{
  try
  {
    size_t pos = 0;
    out = std::stod(s, &pos);
    return pos == s.size();
  }
  catch (...)
  {
    return false;
  }
}
bool parseInt(const std::string& s, int& out)
{
  try
  {
    size_t pos = 0;
    out = std::stoi(s, &pos);
    return pos == s.size();
  }
  catch (...)
  {
    return false;
  }
}
} // namespace

int main(int argc, char** argv)
{
  std::string namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite, weight, contour, presence, outputTrim;
  std::string delayTime, delayFb, delayMix, reverbDecay, reverbMix;
  std::string transpose, t3kWindow, t3kTonality, buffer;
  std::string gt2Window, gt2Floor, gt2Corr, gt2FadeMin, gt2FadeMax, gt2FadeHi, gt2FadeLo;
  std::string gt2OnsetFade, gt2OnsetSpan, gt2Lead, gt2Refr, gt2Hpf, gt2Smooth;
  std::string gt2OverMin, gt2OverMax, gt2Cells, gt2Skip, gt2Resync;
  bool driveEnable = false, shapeEnable = false, delayEnable = false, reverbEnable = false;
  bool transposeOff = false, noStandby = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--list")
    {
      std::string out, error;
      if (!TdmEngine::listDevices(out, error))
      {
        std::printf("tdm_transpose_dev: error: %s\n", error.c_str());
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
    if (a == "--transpose-off")
    {
      transposeOff = true;
      continue;
    }
    if (a == "--no-standby")
    {
      noStandby = true;
      continue;
    }
    auto take = [&](const std::string& flag, std::string& dst) {
      if (a == flag && i + 1 < argc)
      {
        dst = argv[++i];
        return true;
      }
      return false;
    };
    if (take("--nam", namPath) || take("--ir", irPath) || take("--gate-thresh", gateThresh)
        || take("--gate-rel", gateRel) || take("--input-trim", inputTrim) || take("--tight", tight)
        || take("--drive", drive) || take("--bite", bite) || take("--weight", weight)
        || take("--contour", contour) || take("--presence", presence) || take("--delay-time", delayTime)
        || take("--delay-fb", delayFb) || take("--delay-mix", delayMix)
        || take("--reverb-decay", reverbDecay) || take("--reverb-mix", reverbMix)
        || take("--output-trim", outputTrim) || take("--transpose", transpose)
        || take("--t3k-window", t3kWindow) || take("--t3k-tonality", t3kTonality)
        || take("--buffer", buffer) || take("--gt2-window", gt2Window) || take("--gt2-floor", gt2Floor)
        || take("--gt2-corr", gt2Corr) || take("--gt2-fademin", gt2FadeMin)
        || take("--gt2-fademax", gt2FadeMax) || take("--gt2-fadencc-hi", gt2FadeHi)
        || take("--gt2-fadencc-lo", gt2FadeLo) || take("--gt2-onsetfade", gt2OnsetFade)
        || take("--gt2-onsetspan", gt2OnsetSpan) || take("--gt2-lead", gt2Lead)
        || take("--gt2-refr", gt2Refr) || take("--gt2-hpf", gt2Hpf) || take("--gt2-smooth", gt2Smooth)
        || take("--gt2-overmin", gt2OverMin) || take("--gt2-overmax", gt2OverMax)
        || take("--gt2-cells", gt2Cells) || take("--gt2-skip", gt2Skip)
        || take("--gt2-resync", gt2Resync))
      continue;
    std::printf("usage: tdm_transpose_dev [tdm_live rig flags] [--transpose gt2|t3k:SHIFT] "
                "[--transpose-off]\n"
                "       [--t3k-window 20|30|40|60] [--t3k-tonality off|HZ] [--gt2-* ...] "
                "[--no-standby] [--buffer N] | --list\n");
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

    // DEV transpose stage (off-RT configuration, pre-start).
    tdm::lab::DevTranspose stage;
    stage.setWarmStandby(!noStandby);
    if (!transpose.empty())
    {
      const size_t c = transpose.find(':');
      tdm::lab::DevTranspose::Engine e = tdm::lab::DevTranspose::Engine::Gt2;
      float st = 0.0f;
      const bool ok = (c != std::string::npos) && tdm::lab::parseDevEngine(transpose.substr(0, c), e)
          && parseFloat(transpose.substr(c + 1), st) && st >= tdm::lab::DevTranspose::kMinShiftSt
          && st <= tdm::lab::DevTranspose::kMaxShiftSt;
      if (!ok)
      {
        std::printf("tdm_transpose_dev: error: --transpose must be gt2|ours|t3k|ref:-12..+12\n");
        return 2;
      }
      stage.setEngine(e); // throws only if T3K missing from the build (never here)
      stage.setShiftSt(st);
    }
    stage.setEnabled(!transposeOff);
    if (!t3kWindow.empty())
    {
      int ms = 0;
      if (!parseInt(t3kWindow, ms))
      {
        std::printf("tdm_transpose_dev: error: --t3k-window must be 20|30|40|60\n");
        return 2;
      }
      try
      {
        stage.setT3kWindowMs(ms);
      }
      catch (const std::exception& ex)
      {
        std::printf("tdm_transpose_dev: error: %s\n", ex.what());
        return 2;
      }
    }
    if (!t3kTonality.empty())
    {
      if (t3kTonality == "off")
      {
        stage.setT3kTonalityHz(0.0f);
      }
      else
      {
        float hz = 0.0f;
        if (!parseFloat(t3kTonality, hz))
        {
          std::printf("tdm_transpose_dev: error: --t3k-tonality must be off|HZ\n");
          return 2;
        }
        try
        {
          stage.setT3kTonalityHz(hz);
        }
        catch (const std::exception& ex)
        {
          std::printf("tdm_transpose_dev: error: %s\n", ex.what());
          return 2;
        }
      }
    }
    {
      // GT2 advanced DEV params: start-time only, validated as a whole.
      tdm::GuitarTranspose::Config cfg = tdm::lab::DevTranspose::knownGoodGt2();
      bool touched = false;
      double d = 0.0;
      int n = 0;
      auto needDouble = [&](const std::string& s, const char* flag, double& dst) {
        if (s.empty())
          return true;
        if (!parseDouble(s, d))
        {
          std::printf("tdm_transpose_dev: error: %s needs a number\n", flag);
          return false;
        }
        dst = d;
        touched = true;
        return true;
      };
      auto needInt = [&](const std::string& s, const char* flag, int& dst) {
        if (s.empty())
          return true;
        if (!parseInt(s, n))
        {
          std::printf("tdm_transpose_dev: error: %s needs an integer\n", flag);
          return false;
        }
        dst = n;
        touched = true;
        return true;
      };
      if (!needDouble(gt2Window, "--gt2-window", cfg.windowMs)
          || !needDouble(gt2Floor, "--gt2-floor", cfg.floorMs)
          || !needDouble(gt2Corr, "--gt2-corr", cfg.corrMs)
          || !needDouble(gt2FadeMin, "--gt2-fademin", cfg.fadeMinMs)
          || !needDouble(gt2FadeMax, "--gt2-fademax", cfg.fadeMaxMs)
          || !needDouble(gt2FadeHi, "--gt2-fadencc-hi", cfg.fadeNccHi)
          || !needDouble(gt2FadeLo, "--gt2-fadencc-lo", cfg.fadeNccLo)
          || !needDouble(gt2OnsetFade, "--gt2-onsetfade", cfg.onsetFadeMs)
          || !needDouble(gt2OnsetSpan, "--gt2-onsetspan", cfg.onsetSpanMs)
          || !needDouble(gt2Lead, "--gt2-lead", cfg.searchLeadMs)
          || !needDouble(gt2Refr, "--gt2-refr", cfg.refractoryMs)
          || !needDouble(gt2Hpf, "--gt2-hpf", cfg.detectorHpHz)
          || !needDouble(gt2Smooth, "--gt2-smooth", cfg.detectorSmoothMs)
          || !needDouble(gt2OverMin, "--gt2-overmin", cfg.onsetOverMinDb)
          || !needDouble(gt2OverMax, "--gt2-overmax", cfg.onsetOverMaxDb)
          || !needInt(gt2Cells, "--gt2-cells", cfg.historyCells)
          || !needInt(gt2Skip, "--gt2-skip", cfg.historySkip))
        return 2;
      if (!gt2Resync.empty())
      {
        if (gt2Resync != "on" && gt2Resync != "off")
        {
          std::printf("tdm_transpose_dev: error: --gt2-resync must be on|off\n");
          return 2;
        }
        cfg.enableResync = (gt2Resync == "on");
        touched = true;
      }
      if (touched)
      {
        try
        {
          stage.configureGt2(cfg);
        }
        catch (const std::exception& ex)
        {
          std::printf("tdm_transpose_dev: error: GT2 config rejected: %s\n", ex.what());
          return 2;
        }
        std::printf("transpose-dev: GT2 custom DEV config active (NOT the known-good baseline)\n");
      }
    }
    engine.rig().setTransposeInsert(&stage);

    if (!buffer.empty())
    {
      int b = 0;
      if (!parseInt(buffer, b) || b <= 0)
      {
        std::printf("tdm_transpose_dev: error: --buffer must be a positive frame count\n");
        return 2;
      }
      engine.setRequestedBufferFrames(b);
    }

    std::string error;
    if (!namPath.empty() && !engine.loadNam(namPath, error))
    {
      std::printf("tdm_transpose_dev: error: %s\n", error.c_str());
      return 1;
    }
    if (!irPath.empty() && !engine.loadIr(irPath, error))
    {
      std::printf("tdm_transpose_dev: error: %s\n", error.c_str());
      return 1;
    }
    if (!engine.start(error))
    {
      std::printf("tdm_transpose_dev: error: %s\n", error.c_str());
      return 1;
    }

    const tdm::RigParams applied = engine.rig().params();
    std::printf("live: %.0fHz nam=%s ir=%s gate=%s trim=%.1f drive=%s shape=%s delay=%s reverb=%s out trim=%.1f\n",
                engine.sampleRate(), engine.rig().hasNam() ? "yes" : "no",
                engine.rig().hasIr() ? "yes" : "no", applied.gateEnabled ? "on" : "off", applied.inputTrimDb,
                applied.driveEnabled ? "on" : "off", applied.shapeEnabled ? "on" : "off",
                applied.delayEnabled ? "on" : "off", applied.reverbEnabled ? "on" : "off",
                applied.outputTrimDb);
    printStatus(stage);
    {
      const double sr = engine.sampleRate();
      const int inb = engine.inputBufferFrames(), outb = engine.outputBufferFrames();
      const int req = engine.requestedBufferFrames();
      const double devMs = 1000.0 * (inb + outb) / sr;
      const double gt2Ms = 1000.0 * stage.gt2LatencySamples() / sr;
      const double t3kMs = 1000.0 * stage.t3kLatencySamples() / sr;
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
      std::printf("transpose: chain input -> trim -> gate -> TRANSPOSE -> drive -> nam -> ir -> shape -> "
                  "space -> out\n");
      std::printf("latency: device=%.1fms transpose(gt2=%.1fms t3k=%.1fms) ring-slack~0-%.1fms "
                  "(uncompensated)\n",
                  devMs, gt2Ms, t3kMs, slackMs);
    }
    if (stage.resetShiftSt() == 0.0f)
      std::printf("note: GT2 reset at 0 st (zero-latency wire); live shifts away from 0 need a restart "
                  "on the GT2 side\n");
    std::printf("commands: ours|ref | shift X | on|off | t3k-window MS | t3k-tonality off|HZ | status | "
                "q + Enter\n");

    char line[128] = {};
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
      std::string cmd(line);
      while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
        cmd.pop_back();
      if (cmd == "q" || cmd == "Q" || cmd == "quit" || cmd == "exit")
        break;
      tdm::lab::DevTranspose::Engine e = tdm::lab::DevTranspose::Engine::Gt2;
      if (tdm::lab::parseDevEngine(cmd, e))
      {
        stage.setEngine(e);
        std::printf("transpose: engine=%s (downstream unchanged)\n", tdm::lab::devEngineName(e));
      }
      else if (cmd == "on")
      {
        stage.setEnabled(true);
        std::printf("transpose: ENABLED\n");
      }
      else if (cmd == "off")
      {
        stage.setEnabled(false);
        std::printf("transpose: bypassed (latency-matched)\n");
      }
      else if (cmd.rfind("shift", 0) == 0 && (cmd.size() == 5 || cmd[5] == ' ' || cmd[5] == '-'
                                                                             || cmd[5] == '+' || cmd[5] == '.'
                                                                             || (cmd[5] >= '0' && cmd[5] <= '9')))
      {
        float st = 0.0f;
        if (!parseFloat(ltrimmed(cmd.substr(5)), st))
        {
          std::printf("transpose: usage: shift -12..+12\n");
          continue;
        }
        if (st < tdm::lab::DevTranspose::kMinShiftSt || st > tdm::lab::DevTranspose::kMaxShiftSt)
        {
          std::printf("transpose: shift must be in -12..+12\n");
          continue;
        }
        if (stage.resetShiftSt() == 0.0f && st != 0.0f)
          std::printf("transpose: note: GT2 was reset at 0 st and will ignore this until restart "
                      "(TONE3000 follows)\n");
        stage.setShiftSt(st);
        std::printf("transpose: shift=%.2f st (both engines)\n", st);
      }
      else if (cmd.rfind("t3k-window", 0) == 0)
      {
        int ms = 0;
        if (!parseInt(ltrimmed(cmd.substr(10)), ms))
        {
          std::printf("transpose: usage: t3k-window 20|30|40|60\n");
          continue;
        }
        try
        {
          stage.setT3kWindowMs(ms);
          std::printf("transpose: t3k window=%dms\n", ms);
        }
        catch (const std::exception& ex)
        {
          std::printf("transpose: error: %s\n", ex.what());
        }
      }
      else if (cmd.rfind("t3k-tonality", 0) == 0)
      {
        const std::string arg = ltrimmed(cmd.substr(12));
        if (arg == "off")
        {
          stage.setT3kTonalityHz(0.0f);
          std::printf("transpose: t3k tonality=off\n");
          continue;
        }
        float hz = 0.0f;
        if (!parseFloat(arg, hz))
        {
          std::printf("transpose: usage: t3k-tonality off|HZ\n");
          continue;
        }
        try
        {
          stage.setT3kTonalityHz(hz);
          std::printf("transpose: t3k tonality=%.0fHz\n", stage.t3kTonalityHz());
        }
        catch (const std::exception& ex)
        {
          std::printf("transpose: error: %s\n", ex.what());
        }
      }
      else if (cmd == "status")
      {
        printStatus(stage);
      }
      else if (!cmd.empty())
      {
        std::printf("transpose: unknown command '%s'\n", cmd.c_str());
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
    std::printf("tdm_transpose_dev: error: %s\n", e.what());
    return 1;
  }
}

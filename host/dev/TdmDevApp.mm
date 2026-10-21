// tdm_dev: Developer Control App v0 for TechDeathMachine.
//
// Small native-AppKit engineering UI around the shared TdmEngine (the same
// engine tdm_live uses). Deliberately unpolished: no presets, no meters,
// no realtime NAM/IR swapping.
//
// v0 loading policy (audition-friendly, not realtime-safe by design):
//   Stop audio -> choose NAM/IR -> Start audio.
// Parameter sliders/checkboxes apply LIVE while audio runs through the
// lock-free Rig handoff (applied at audio block boundaries).
//
// Audition starting point (UI initial state only, not a DSP assumption):
//   Input 0 dB, Gate ON -55 dB / 52 ms, Transpose ON GT2 -2.0 st (T3K ref at
//   30 ms / Tonality Off, GT2 at the known-good baseline), Drive ON
//   0.85 / 0.50 / 0.70, ToneShape ON neutral, Space OFF (dry brutal rhythm
//   first), Output 0 dB, reference NAM preloaded best-effort.
//
// Hidden flags (also used for headless verification):
//   --smoke-test   run offline rig/param checks, no GUI, no hardware
//   --smoke-ui     build the full UI and auto-quit (needs WindowServer)
//   --list-midi    print CoreMIDI sources (uid + name) and exit

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dsp/RigParams.h"
#include "dsp/Tuner/Tuner.h"
#include "dsp/lab/Pitch/DevTranspose.h"
#include "dsp/lab/Slam/DevSlam.h"
#include "dsp/lab/Alien/DevAlien.h"
#include "dsp/lab/SpecialFx/DevSpecFx.h"
#include "dsp/lab/Pitch/DevTransposeUi.h"
#include "host/TdmEngine.h"
#include "host/midi/MidiDispatch.h"
#include "host/midi/MidiInput.h"
#include "host/midi/MidiPersist.h"

// MIDI -> DSP sink (MIDI input thread): lock-free atomic setters ONLY,
// no UI, no allocation. UI reflection happens on the 10 Hz main tick.
struct TdmMidiSink : tdm::midi::ISink
{
  tdm::specfx::DevSpecFx* spec = nullptr;
  void setContinuous(tdm::midi::Target t, float v) override
  {
    if (t == tdm::midi::Target::SpecExpression)
      spec->setExpression(v);
    else if (t == tdm::midi::Target::SpecMix)
      spec->setMix(v);
  }
  void setBool(tdm::midi::Target t, bool v) override
  {
    if (t == tdm::midi::Target::SpecExprEnable)
      spec->setExpressionEnabled(v);
    else if (t == tdm::midi::Target::SpecGestEnable)
      spec->setGesturesEnabled(v);
  }
  void gesturePress(int s) override { spec->pressSlot(s); }
  void gestureRelease(int s) override { spec->releaseSlot(s); }
  void gestureOneShot(int s) override { spec->oneShotSlot(s); }
};

// Handler context (plain pointers: the controller owns everything and
// outlives the MIDI client, which is torn down in member order).
struct TdmMidiCtx
{
  tdm::midi::LearnManager* learn = nullptr;
  tdm::midi::Dispatcher* disp = nullptr;
};

static void tdmMidiHandler(const tdm::midi::Message* msgs, int n, void* ctx)
{
  TdmMidiCtx* c = static_cast<TdmMidiCtx*>(ctx);
  for (int i = 0; i < n; ++i)
  {
    if (c->learn->offer(msgs[i]))
      continue; // Learn consumes (no dispatch while capturing)
    c->disp->handleMessage(msgs[i]);
  }
}

// MIDI row helpers (main thread only).
static NSString* midiShortLabel(int t)
{
  switch (t)
  {
  case 0: return @"Expression";
  case 1: return @"Mix";
  case 2: return @"Expr Enable";
  case 3: return @"Gest Enable";
  default: return [NSString stringWithFormat:@"Slot %d", t - 3];
  }
}
static NSString* midiTriggerText(const tdm::midi::Trigger& tr)
{
  NSString* what = nil;
  switch (tr.type)
  {
  case tdm::midi::MsgType::Cc: what = [NSString stringWithFormat:@"CC%u", tr.number]; break;
  case tdm::midi::MsgType::NoteOn: what = [NSString stringWithFormat:@"N%u", tr.number]; break;
  case tdm::midi::MsgType::Program: what = [NSString stringWithFormat:@"PC%u", tr.number]; break;
  default: what = @"?";
  }
  if (tr.channel < 0)
    return [what stringByAppendingString:@" any"];
  return [NSString stringWithFormat:@"%@ ch%d", what, tr.channel + 1];
}
static NSString* midiLatestText(const tdm::midi::MidiInput::Latest& l)
{
  if (l.count == 0)
    return @"—";
  NSString* what = nil;
  switch (l.type)
  {
  case tdm::midi::MsgType::Cc:
    what = [NSString stringWithFormat:@"CC%u = %u", l.number, l.value];
    break;
  case tdm::midi::MsgType::NoteOn:
    what = [NSString stringWithFormat:@"Note %u on (%u)", l.number, l.value];
    break;
  case tdm::midi::MsgType::NoteOff:
    what = [NSString stringWithFormat:@"Note %u off", l.number];
    break;
  case tdm::midi::MsgType::Program: what = [NSString stringWithFormat:@"PC%u", l.number]; break;
  default: what = @"?";
  }
  return [NSString stringWithFormat:@"Ch %u | %@ · #%llu", l.channel + 1, what, l.count];
}
// Row mode segment <-> Action (continuous rows carry invert instead).
static tdm::midi::Action midiRowAction(int t, NSInteger seg)
{
  if (t <= 1)
    return tdm::midi::Action::Continuous;
  if (t <= 3)
    return (seg == 1) ? tdm::midi::Action::Momentary : tdm::midi::Action::Toggle;
  return (seg == 1) ? tdm::midi::Action::Momentary : tdm::midi::Action::OneShot;
}
static NSInteger midiRowSeg(int t, const tdm::midi::Mapping& m)
{
  if (t <= 1)
    return m.invert ? 1 : 0;
  if (t <= 3)
    return (m.action == tdm::midi::Action::Momentary) ? 1 : 0;
  return (m.action == tdm::midi::Action::Momentary) ? 1 : 0;
}

// Double-click probe, defined after the control classes below (ObjC needs
// global scope). External linkage so the in-namespace smoke-test can call it.
bool probeDoubleClickReset(std::string& detail);
// Full transpose-UI probe, defined after the controller (same pattern).
bool probeTransposeUiFull(std::string& detail);

namespace
{
// Audition starting point. Lives HERE (app layer), not in dsp/: these are
// context suggestions from listening work, not DSP defaults.
tdm::RigParams auditionDefaults()
{
  tdm::RigParams p;
  p.inputTrimDb = 0.0f;
  p.gateEnabled = true;
  p.gateThresholdDb = -55.0f;
  p.gateReleaseMs = 52.0f;
  p.tunerEnabled = true; // DEV: tuner readout live from the start (observes only)
  p.driveEnabled = true;
  p.tight = 0.85f;
  p.drive = 0.50f;
  p.bite = 0.70f;
  p.shapeEnabled = true;
  p.weight = 0.5f;
  p.contour = 0.5f;
  p.presence = 0.5f;
  p.delayEnabled = false; // space off: rhythm tone stays dry until auditioned
  p.delayTimeMs = tdm::Delay::kDefaultTimeMs;
  p.delayFeedback = tdm::Delay::kDefaultFeedback;
  p.delayMix = tdm::SpaceProcessor::kDefaultDelayMix;
  p.reverbEnabled = false;
  p.reverbDecay = tdm::Reverb::kDefaultDecay;
  p.reverbMix = tdm::SpaceProcessor::kDefaultReverbMix;
  p.outputTrimDb = 0.0f;
  return p;
}

const char* kReferenceNam = "assets/nam/6505_unboost.nam";

NSString* fmtDb(double v) { return [NSString stringWithFormat:@"%+.1f dB", v]; }
NSString* fmtMs(double v) { return [NSString stringWithFormat:@"%.0f ms", v]; }
NSString* fmt01(double v) { return [NSString stringWithFormat:@"%.2f", v]; }
NSString* fmtSt(double v) { return [NSString stringWithFormat:@"%+.1f st", v]; }
NSString* fmtTonality(double v)
{
  return v <= 0.0 ? @"Off" : [NSString stringWithFormat:@"%.0f Hz", v];
}
// GT2 advanced value label from the shared descriptor table (decimals+unit).
NSString* fmtGt2Field(int i, double v)
{
  const tdm::lab::Gt2UiField& f = tdm::lab::devGt2UiField(i);
  NSString* num = [NSString stringWithFormat:@"%.*f", f.decimals, v];
  if (f.unit[0] == '\0')
    return num;
  return [NSString stringWithFormat:@"%@ %s", num, f.unit];
}
NSString* baseName(const std::string& p)
{
  NSString* s = [NSString stringWithUTF8String:p.c_str()];
  return s.length > 0 ? [s lastPathComponent] : @"(none)";
}

// Field-wise GT2 Config equality (pending-vs-applied restart badge).
bool gt2ConfigsEqual(const tdm::GuitarTranspose::Config& a,
                     const tdm::GuitarTranspose::Config& b)
{
  return a.windowMs == b.windowMs && a.floorMs == b.floorMs && a.corrMs == b.corrMs
      && a.fadeMinMs == b.fadeMinMs && a.fadeMaxMs == b.fadeMaxMs && a.fadeNccHi == b.fadeNccHi
      && a.fadeNccLo == b.fadeNccLo && a.onsetFadeMs == b.onsetFadeMs && a.onsetSpanMs == b.onsetSpanMs
      && a.searchLeadMs == b.searchLeadMs && a.refractoryMs == b.refractoryMs
      && a.detectorHpHz == b.detectorHpHz && a.detectorSmoothMs == b.detectorSmoothMs
      && a.onsetOverMinDb == b.onsetOverMinDb && a.onsetOverMaxDb == b.onsetOverMaxDb
      && a.historyCells == b.historyCells && a.historySkip == b.historySkip
      && a.enableResync == b.enableResync;
}

enum SliderTag
{
  kTagInputTrim = 1,
  kTagGateThresh,
  kTagGateRel,
  kTagTight,
  kTagDrive,
  kTagBite,
  kTagWeight,
  kTagContour,
  kTagPresence,
  kTagDelayTime,
  kTagDelayFb,
  kTagDelayMix,
  kTagReverbDecay,
  kTagReverbMix,
  kTagOutTrim,
  kTagShift, // transpose section: shared shift + T3K tonality (live)
  kTagT3kTonality,
  kTagSlamAmount, // SLAM section: shared 0..100% macro (live)
  // ALIEN FX LAB section (post-IR tap; all live).
  kTagAlienMix,
  kTagAlienShiftHz,
  kTagAlienSweepMs,
  kTagAlienDepthMs,
  kTagAlienWarpTime,
  kTagAlienWarpHz,
  kTagAlienWarpSt,
  kTagAlienWarpFb,
  // CREATIVE PITCH section (laser engine; live).
  kTagSpecExpr,
  kTagSpecMix,
  kTagSpecDepth, // expression depth in st (Whammy/Dual/DualWham; Ring ignores)
  // CREATIVE PITCH — GESTURES section (staged per-slot edits; Save commits).
  kTagGestDepth,
  kTagGestTime,
  kTagGestRet
};

// GT2 advanced rows use their own tag band (field index + base) with a
// dedicated action; they never reach the rig paramChanged: switch.
constexpr int kGt2AdvTagBase = 100;

// Canonical double-click reset value per slider, sourced from the DSP
// stage defaults (NOT the audition starting points below). The slider
// subclass uses this; the smoke-test asserts it, so the mapping in the
// repo's test run.
double resetValueForTag(SliderTag tag)
{
  switch (tag)
  {
  case kTagInputTrim:
    return tdm::InputTrim::kDefaultTrimDb;
  case kTagGateThresh:
    return tdm::TechDeathGate::kDefaultThresholdDb;
  case kTagGateRel:
    return tdm::TechDeathGate::kDefaultReleaseMs;
  case kTagTight:
    return tdm::TightDrive::kDefaultTight;
  case kTagDrive:
    return tdm::TightDrive::kDefaultDrive;
  case kTagBite:
    return tdm::TightDrive::kDefaultBite;
  case kTagWeight:
    return tdm::ToneShape::kDefaultWeight;
  case kTagContour:
    return tdm::ToneShape::kDefaultContour;
  case kTagPresence:
    return tdm::ToneShape::kDefaultPresence;
  case kTagDelayTime:
    return tdm::Delay::kDefaultTimeMs;
  case kTagDelayFb:
    return tdm::Delay::kDefaultFeedback;
  case kTagDelayMix:
    return tdm::SpaceProcessor::kDefaultDelayMix;
  case kTagReverbDecay:
    return tdm::Reverb::kDefaultDecay;
  case kTagReverbMix:
    return tdm::SpaceProcessor::kDefaultReverbMix;
  case kTagOutTrim:
    return tdm::OutputTrim::kDefaultTrimDb;
  case kTagShift:
    return 0.0; // transpose shift default (double-click; live-safe, not bypass)
  case kTagT3kTonality:
    return 0.0; // tonality off
  case kTagSlamAmount:
    return 100.0; // SLAM macro default: study reference per flavour
  case kTagAlienMix:
    return 100.0; // alien lab defaults: full wet, lab starting points
  case kTagAlienShiftHz:
    return 150.0;
  case kTagAlienSweepMs:
    return 120.0;
  case kTagAlienDepthMs:
    return 12.0;
  case kTagAlienWarpTime:
    return 220.0;
  case kTagAlienWarpHz:
    return 100.0;
  case kTagAlienWarpSt:
    return 3.0;
  case kTagAlienWarpFb:
    return 0.5;
  case kTagSpecExpr:
    return 0.0; // expression heel (dry/unison start)
  case kTagSpecMix:
    return 100.0;
  case kTagSpecDepth:
    return 12.0; // full validated range
  case kTagGestDepth:
    return 12.0; // gesture modal defaults (per-slot factory on select)
  case kTagGestTime:
    return 100.0;
  case kTagGestRet:
    return 100.0;
  }
  return 0.0;
}

// Headless self-check: no GUI, no audio hardware. Returns exit code.
int smokeTest()
{
  int failures = 0;
  auto check = [&](bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
      ++failures;
  };
  // 1. Audition defaults round-trip through the handoff.
  {
    TdmEngine engine;
    check(!engine.isRunning(), "engine starts stopped");
    engine.rig().setParams(auditionDefaults());
    const tdm::RigParams q = engine.rig().params();
    check(q.gateEnabled && q.driveEnabled && q.gateThresholdDb == -55.0f && q.gateReleaseMs == 52.0f
              && q.tight == 0.85f && q.drive == 0.50f && q.bite == 0.70f,
          "audition defaults round-trip");
    // Double-click reset mapping: canonical DSP defaults, NOT audition values
    // (float compare: resetValueForTag widens the exact stage constants).
    check((float)resetValueForTag(kTagInputTrim) == 0.0f, "reset: input trim -> 0 dB");
    check((float)resetValueForTag(kTagGateThresh) == -55.0f, "reset: gate thresh -> -55 dB");
    check((float)resetValueForTag(kTagGateRel) == 50.0f, "reset: gate release -> 50 ms");
    check((float)resetValueForTag(kTagTight) == 0.5f, "reset: tight -> 0.50");
    check((float)resetValueForTag(kTagDrive) == 0.3f, "reset: drive -> 0.30");
    check((float)resetValueForTag(kTagBite) == 0.5f, "reset: bite -> 0.50");
    check((float)resetValueForTag(kTagWeight) == 0.5f, "reset: weight -> 0.50");
    check((float)resetValueForTag(kTagContour) == 0.5f, "reset: contour -> 0.50");
    check((float)resetValueForTag(kTagPresence) == 0.5f, "reset: presence -> 0.50");
    check((float)resetValueForTag(kTagDelayTime) == 220.0f, "reset: delay time -> 220 ms");
    check((float)resetValueForTag(kTagDelayFb) == 0.35f, "reset: delay fb -> 0.35");
    check((float)resetValueForTag(kTagDelayMix) == 0.25f, "reset: delay mix -> 0.25");
    check((float)resetValueForTag(kTagReverbDecay) == 0.4f, "reset: reverb decay -> 0.40");
    check((float)resetValueForTag(kTagReverbMix) == 0.20f, "reset: reverb mix -> 0.20");
    check((float)resetValueForTag(kTagOutTrim) == 0.0f, "reset: output trim -> 0 dB");
    check((float)resetValueForTag(kTagShift) == 0.0f, "reset: shift -> 0 st");
    check((float)resetValueForTag(kTagT3kTonality) == 0.0f, "reset: tonality -> Off");
    check((float)resetValueForTag(kTagSlamAmount) == 100.0f, "reset: slam amount -> 100%");
    check((float)resetValueForTag(kTagAlienMix) == 100.0f, "reset: alien mix -> 100%");
    check((float)resetValueForTag(kTagAlienShiftHz) == 150.0f, "reset: alien shift -> +150 Hz");
    check((float)resetValueForTag(kTagAlienSweepMs) == 120.0f, "reset: alien sweep -> 120 ms");
    check((float)resetValueForTag(kTagAlienDepthMs) == 12.0f, "reset: alien depth -> 12 ms");
    check((float)resetValueForTag(kTagAlienWarpTime) == 220.0f, "reset: alien warp time -> 220 ms");
    check((float)resetValueForTag(kTagAlienWarpHz) == 100.0f, "reset: alien warp hz -> +100 Hz");
    check((float)resetValueForTag(kTagAlienWarpSt) == 3.0f, "reset: alien warp st -> +3 st");
    check((float)resetValueForTag(kTagAlienWarpFb) == 0.5f, "reset: alien warp fb -> 0.50");
    check((float)resetValueForTag(kTagSpecExpr) == 0.0f, "reset: spec expr -> heel");
    check((float)resetValueForTag(kTagSpecMix) == 100.0f, "reset: spec mix -> 100%");
    check((float)resetValueForTag(kTagSpecDepth) == 12.0f, "reset: spec depth -> 12");
    check((float)resetValueForTag(kTagGestDepth) == 12.0f, "reset: gest depth -> 12");
    check((float)resetValueForTag(kTagGestTime) == 100.0f, "reset: gest time -> 100");
    check((float)resetValueForTag(kTagGestRet) == 100.0f, "reset: gest ret -> 100");
    // Double-click behavior on the real control (defined after the control
    // classes below): a synthesized double-click parks the reset value and
    // fires the normal action exactly once.
    {
      std::string detail;
      check(probeDoubleClickReset(detail),
            ("double-click resets slider and fires action" + detail).c_str());
    }
  }
  // 2. DEV transpose stage: adapter mapping, both engines render, switch
  // preserves configs, bypass preserves shift, restores land exactly.
  // (Runs WITH TONE3000: tdm_dev links the reference by design.)
  {
    using Stage = tdm::lab::DevTranspose;
    using Gt2 = tdm::GuitarTranspose;
    check(Stage::hasTone3000(), "dev app links the TONE3000 reference");
    check(tdm::lab::devGt2UiFieldCount() == 17, "adapter exposes 17 GT2 rows");
    Stage s;
    s.resetGt2ToBaseline();
    s.setEngine(Stage::Engine::Gt2);
    s.setShiftSt(-2.0f);
    s.setEnabled(true);
    s.reset(48000.0, 512);
    std::vector<float> in(4800), out(4800);
    for (int i = 0; i < 4800; ++i)
      in[size_t(i)] = 0.4f * std::sin(6.2831853f * i / 97.0f);
    auto runStage = [&](Stage& st) {
      for (int off = 0; off < 4800; off += 128)
        st.process(in.data() + off, out.data() + off, 128);
    };
    runStage(s);
    bool finite = true;
    for (float v : out)
      if (!std::isfinite(v))
        finite = false;
    check(finite, "GT2 stage render finite");
    // T3K reference renders finite at the same shared shift.
    s.setEngine(Stage::Engine::Tone3000);
    s.setT3kWindowMs(30);
    s.setT3kTonalityHz(0.0f);
    runStage(s);
    finite = true;
    for (float v : out)
      if (!std::isfinite(v))
        finite = false;
    check(finite, "T3K stage render finite");
    // Switch back and forth: configs preserved, shift shared.
    Gt2::Config custom = Stage::knownGoodGt2();
    custom.windowMs = 40.0;
    s.configureGt2(custom);
    s.setT3kWindowMs(60);
    s.setShiftSt(-7.0f);
    s.setEngine(Stage::Engine::Gt2);
    s.setEngine(Stage::Engine::Tone3000);
    s.setEngine(Stage::Engine::Gt2);
    check(s.gt2Config().windowMs == 40.0 && s.t3kWindowMs() == 60 && s.shiftSt() == -7.0f,
          "engine switching preserves configs + shared shift");
    // Bypass preserves shift; restores land exactly.
    s.setEnabled(false);
    s.setEnabled(true);
    check(s.shiftSt() == -7.0f, "bypass preserves shift");
    s.resetGt2ToBaseline();
    s.setT3kWindowMs(Stage::kDefaultT3kWindowMs);
    s.setT3kTonalityHz(Stage::kDefaultT3kTonalityHz);
    check(tdm::lab::devGt2UiIsBaseline(s.gt2Config()), "GT2 baseline restore exact");
    check(s.t3kWindowMs() == 30 && s.t3kTonalityHz() == 0.0f, "T3K reference restore exact");
    // GT2 -2 wrapper output still matches raw GT2 (short probe; the full
    // proof lives in tdm_tests).
    Gt2 raw;
    raw.setEnabled(true);
    raw.setShiftSt(-2.0f);
    raw.reset(48000.0);
    std::vector<float> rawOut(4800), devOut(4800);
    Stage d;
    d.resetGt2ToBaseline();
    d.setEngine(Stage::Engine::Gt2);
    d.setShiftSt(-2.0f);
    d.setEnabled(true);
    d.reset(48000.0, 512);
    for (int off = 0; off < 4800; off += 256)
    {
      raw.processBlock(in.data() + off, rawOut.data() + off, 256);
      d.process(in.data() + off, devOut.data() + off, 256);
    }
    float diff = 0.0f;
    for (int i = 0; i < 4800; ++i)
      diff = std::max(diff, std::fabs(rawOut[size_t(i)] - devOut[size_t(i)]));
    check(diff < 1e-6f, "GT2 -2 wrapper == raw baseline");
    // Real AppKit controls, driven programmatically (no audio hardware).
    {
      std::string detail;
      check(probeTransposeUiFull(detail), ("transpose UI wiring" + detail).c_str());
    }
  }
  // 3. Offline audio through the rig with audition params stays finite.
  {
    tdm::TechDeathRig rig;
    rig.reset(48000.0, 512);
    rig.setParams(auditionDefaults());
    std::vector<float> in(512), out(512);
    bool finite = true;
    for (int b = 0; b < 200 && finite; ++b)
    {
      for (int i = 0; i < 512; ++i)
        in[size_t(i)] = 0.4f * std::sin(6.2831853f * (b * 512 + i) / 97.0f);
      const float* bi[1] = {in.data()};
      float* bo[1] = {out.data()};
      rig.processBlock(bi, 1, bo, 1, 512);
      for (float v : out)
        if (!std::isfinite(v))
          finite = false;
    }
    check(finite, "offline audition chain is finite");
  }
  // 4. Reference NAM loads + renders finite (asset present, no hardware).
  {
    FILE* f = std::fopen(kReferenceNam, "rb");
    if (f != nullptr)
    {
      std::fclose(f);
      try
      {
        tdm::TechDeathRig rig;
        rig.reset(48000.0, 512);
        rig.loadNam(kReferenceNam);
        std::vector<float> in(512, 0.1f), out(512, 0.0f);
        const float* bi[1] = {in.data()};
        float* bo[1] = {out.data()};
        rig.processBlock(bi, 1, bo, 1, 512);
        bool finite = true;
        for (float v : out)
          if (!std::isfinite(v))
            finite = false;
        check(finite, "reference NAM renders finite");
      }
      catch (const std::exception& e)
      {
        std::printf("  load error: %s\n", e.what());
        check(false, "reference NAM renders finite");
      }
    }
    else
    {
      std::printf("[SKIP] reference NAM not found (%s)\n", kReferenceNam);
    }
  }
  // 5. Engine load calls never crash headless; outcome depends on hardware.
  {
    TdmEngine engine;
    std::string error;
    const bool ok = engine.loadNam(kReferenceNam, error);
    std::printf("[INFO] engine.loadNam headless -> %s (%s)\n", ok ? "ok" : "failed",
                ok ? engine.namPath().c_str() : error.c_str());
  }
  std::printf("smoke-test: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
} // namespace

// Slider with double-click reset to its canonical DSP default. The reset
// goes through the normal control action (paramChanged: -> rig setter ->
// label refresh), i.e. the same realtime-safe handoff as dragging. Note
// the first click of the double-click may briefly park the knob at the
// click point; the reset action immediately follows through the usual
// smoothed handoff, so the transient is inaudible.
@interface TdmResetSlider : NSSlider
@property(nonatomic) double resetValue;
@end

@implementation TdmResetSlider
- (void)mouseDown:(NSEvent*)event
{
  if (event.clickCount >= 2)
  {
    self.doubleValue = self.resetValue;
    [self sendAction:self.action to:self.target];
  }
  else
  {
    [super mouseDown:event];
  }
}
@end

// Momentary push button: fires downAction on press and upAction on
// release (super tracks the press, so control returns after mouseUp).
// Used for the performance-gesture TRIGGER (press/release slot command).
@interface TdmMomentaryButton : NSButton
@property(nonatomic) SEL downAction;
@property(nonatomic) SEL upAction;
@end

@implementation TdmMomentaryButton
- (void)mouseDown:(NSEvent*)event
{
  [self sendAction:self.downAction to:self.target];
  [super mouseDown:event];
  [self sendAction:self.upAction to:self.target];
}
@end

// Flipped document view for the scrollable control area: y=0 is visually at
// the top, so the form stays anchored below the title bar and disclosures
// only push lower content DOWN (never open blank space above).
@interface TdmFlippedView : NSView
@end

@implementation TdmFlippedView
- (BOOL)isFlipped
{
  return YES;
}
@end

// Compact cents bar for the tuner readout: center = in tune, needle at the
// smoothed deviation (±50 c full scale), green inside ±3 c, dim when invalid.
@interface TdmCentsBar : NSView
@property double cents;
@property BOOL valid;
@end

@implementation TdmCentsBar
- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  const CGFloat w = self.bounds.size.width;
  const CGFloat h = self.bounds.size.height;
  const CGFloat midX = w * 0.5;
  [[NSColor colorWithWhite:0.92 alpha:1.0] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, w, h) xRadius:3 yRadius:3] fill];
  // In-tune zone (|3 c|).
  [[NSColor colorWithCalibratedRed:0.75 green:0.90 blue:0.75 alpha:1.0] setFill];
  const CGFloat zone = (3.0 / 50.0) * (w * 0.5 - 4.0);
  NSRectFill(NSMakeRect(midX - zone, 2, zone * 2, h - 4));
  // End ticks + center line.
  [[NSColor colorWithWhite:0.55 alpha:1.0] setFill];
  NSRectFill(NSMakeRect(3, 2, 1, h - 4));
  NSRectFill(NSMakeRect(w - 4, 2, 1, h - 4));
  [[NSColor colorWithWhite:0.35 alpha:1.0] setFill];
  NSRectFill(NSMakeRect(midX - 0.5, 1, 1, h - 2));
  if (!self.valid)
    return;
  const double c = std::max(-50.0, std::min(50.0, self.cents));
  const CGFloat nx = midX + (c / 50.0) * (w * 0.5 - 6.0);
  if (std::fabs(c) <= 3.0)
    [[NSColor colorWithCalibratedRed:0.10 green:0.55 blue:0.20 alpha:1.0] setFill];
  else
    [[NSColor systemOrangeColor] setFill];
  NSRectFill(NSMakeRect(nx - 2, 1, 4, h - 2));
}
@end

// Minimal action target for the headless double-click check in smokeTest.
@interface TdmSmokeTarget : NSObject
@property(nonatomic) int actions;
@property(nonatomic) double lastValue;
- (void)sliderMoved:(NSSlider*)sender;
@end

@implementation TdmSmokeTarget
- (void)sliderMoved:(NSSlider*)sender
{
  self.actions++;
  self.lastValue = sender.doubleValue;
}
@end

// Headless probe of the real double-click override: a synthesized
// clickCount:2 event must park the reset value and fire the normal action
// exactly once (the same path dragging uses).
bool probeDoubleClickReset(std::string& detail)
{
  TdmSmokeTarget* target = [[TdmSmokeTarget alloc] init];
  TdmResetSlider* slider = [[TdmResetSlider alloc] initWithFrame:NSMakeRect(0, 0, 300, 22)];
  slider.minValue = (double)tdm::TechDeathGate::kMinThresholdDb;
  slider.maxValue = (double)tdm::TechDeathGate::kMaxThresholdDb;
  slider.doubleValue = -40.0;
  slider.resetValue = resetValueForTag(kTagGateThresh);
  slider.target = target;
  slider.action = @selector(sliderMoved:);
  NSEvent* dbl = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                    location:NSMakePoint(150, 11)
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                 eventNumber:0
                                  clickCount:2
                                    pressure:1.0];
  [slider mouseDown:dbl];
  if (target.actions == 1 && slider.doubleValue == -55.0 && target.lastValue == -55.0)
    return true;
  char buf[160];
  std::snprintf(buf, sizeof(buf), " [actions=%d value=%.3f last=%.3f]", target.actions, slider.doubleValue,
                target.lastValue);
  detail = buf;
  return false;
}

@interface DevController : NSObject <NSApplicationDelegate>
- (instancetype)initWithEngine:(TdmEngine*)engine;
- (void)buildUI;
- (void)autoQuitAfter:(NSTimeInterval)seconds;
- (void)refreshTuner; // 10 Hz readout snapshot (tick only)
// Headless UI-wiring probe (smoke-test only): drives the real transpose
// controls programmatically and verifies stage state. Needs no audio.
- (BOOL)runTransposeProbe:(std::string*)detail;
// Document-geometry check (probe only): every visible control inside the
// document with a valid frame, no overlaps, same for open panel children.
- (BOOL)checkDocGeometry:(std::string*)detail;
// Tuner wiring probe (smoke-test only): drives the tuner checkbox and the
// rig directly (no audio device) and verifies detection + transparency.
- (BOOL)runTunerProbe:(std::string*)detail;
// SLAM wiring probe (smoke-test only): drives the SLAM checkbox, flavour
// selector, and Amount slider programmatically and verifies router state,
// transparency, and rig-param isolation. Needs no audio.
- (BOOL)runSlamProbe:(std::string*)detail;
// Alien FX wiring probe (smoke-test only): same pattern for the Alien
// FX Lab section (effect selector, sub-selectors, sliders, trigger,
// transparency, rig-param isolation). Needs no audio.
- (BOOL)runAlienProbe:(std::string*)detail;
// Special FX wiring probe (smoke-test only): same pattern for the
// Special FX Lab section (voice/control/insert selectors, expression
// + envelope sliders, momentary Toe, transparency, rig isolation).
- (BOOL)runSpecProbe:(std::string*)detail;
// MIDI wiring probe (smoke-test only): section presence, synthetic
// dispatch through the real table+sink, Learn arm/commit, persistence
// round-trip to a temp path. Needs no MIDI server or hardware.
- (BOOL)runMidiProbe:(std::string*)detail;
@end

@implementation DevController
{
  TdmEngine* _engine; // owned (main-thread only except rig() param setters)
  // DEV transpose stage (C++ member: outlives the engine; installed into
  // the rig seam pre-start. UI drives it via atomic setters only, exactly
  // like the rig handoff; GT2 config edits go through configureGt2 on the
  // main thread and take effect on the next Start, like NAM/IR).
  tdm::lab::DevTranspose _stage;
  tdm::GuitarTranspose::Config _gt2Applied; // config used at last Start
  // DEV SLAM router (C++ member: outlives the engine; installed into the
  // three rig SLAM seams pre-start. UI drives it via atomic setters only,
  // exactly like the rig handoff; installed-but-off is transparent).
  tdm::lab::DevSlam _slam;
  NSButton* _slamCheck;
  NSSegmentedControl* _slamSeg;
  // DEV Alien FX router (C++ member: outlives the engine; installed into
  // the rig's post-IR alien seam pre-start. UI drives it via atomic
  // setters only, exactly like the rig handoff; installed-but-off is
  // transparent).
  tdm::alien::DevAlien _alien;
  NSButton* _alienCheck;
  NSSegmentedControl* _alienSeg;
  NSSegmentedControl* _alienDirSeg;
  NSSegmentedControl* _alienWarpSeg;
  // DEV Special FX router (C++ member: outlives the engine; installed
  // into the rig's pre-gain and post-IR spec seams pre-start. UI drives
  // it via atomic setters only; installed-but-off is transparent).
  tdm::specfx::DevSpecFx _spec;
  NSButton* _specCheck; // expression-section enable
  NSButton* _specGestCheck; // gesture-section enable (independent)
  NSSegmentedControl* _specVoiceSeg;
  NSSegmentedControl* _specDirSeg; // Whammy polarity (single source of truth)
  NSSegmentedControl* _specInsertSeg;
  // Generic MIDI control (host/midi): CoreMIDI transport + mapping
  // table + Learn + dispatcher -> TdmMidiSink (atomic DSP setters).
  // Main thread owns the table/UI; the MIDI thread only dispatches.
  // NOTE: _midi is declared LAST so it is destroyed FIRST (callbacks
  // stop before the table/learn/dispatcher it references go away).
  tdm::midi::MappingTable _midiTable;
  tdm::midi::LearnManager _midiLearn;
  TdmMidiSink _midiSink;
  std::unique_ptr<tdm::midi::Dispatcher> _midiDisp;
  TdmMidiCtx _midiCtx;
  tdm::midi::MidiInput _midi;
  NSPopUpButton* _midiSrcPopup;
  NSTextField* _midiStatus;
  NSTextField* _midiRecent;
  NSButton* _midiOmniCheck;
  NSTextField* _midiSaveLabel;
  NSTextField* _midiAssign[(int)tdm::midi::Target::Count];
  NSButton* _midiLearnBtn[(int)tdm::midi::Target::Count];
  NSSegmentedControl* _midiModeSeg[(int)tdm::midi::Target::Count];
  NSButton* _midiClearBtn[(int)tdm::midi::Target::Count];
  std::vector<uint32_t> _midiSrcUids; // popup index -> endpoint UID
  std::vector<std::string> _midiSrcNames;
  std::vector<char> _midiSrcConn; // popup index -> connected now
  std::string _midiPersistMsg;    // startup load note (shown in status)
  int _midiLearnTarget;           // armed target for Learn, or -1
  bool _midiOmniRestore;          // persisted OMNI flag (checkbox set at buildUI)
  bool _midiCalActive;            // continuous-Learn pedal calibration window
  int _midiCalTarget;
  tdm::midi::Trigger _midiCalTrigger;
  uint8_t _midiCalMin;
  uint8_t _midiCalMax;
  double _midiCalUntil; // CFAbsoluteTime deadline
  uint64_t _midiLastCount;
  uint64_t _midiCalLastCount;
  // Performance-gesture editor: per-slot STAGED configs + names (UI-side
  // only). Save commits the displayed slot; TRIGGER/1-Shot commit, then
  // fire by slot ID. External MIDI triggers bypass staging.
  NSPopUpButton* _specGestPopup;
  NSTextField* _specGestName;
  NSSegmentedControl* _specGestEffectSeg;
  NSSegmentedControl* _specGestDirSeg;
  NSSegmentedControl* _specGestMotionSeg;
  int _specGestSlot; // displayed slot (UI-side only; never addresses audio)
  tdm::specfx::GestureConfig _specGestStage[tdm::specfx::kNumGestureSlots];
  NSMutableArray<NSString*>* _specGestNames;
  NSWindow* _window;
  NSTextField* _statusLabel;
  NSTextField* _transposeStatus;
  NSTextField* _namLabel;
  NSTextField* _irLabel;
  NSButton* _transportButton;
  NSMutableDictionary<NSNumber*, NSSlider*>* _sliders;
  NSMutableDictionary<NSNumber*, NSTextField*>* _valueLabels;
  NSButton* _gateCheck;
  // Tuner readout (side-chain observer; display smoother is control-side).
  NSButton* _tunerCheck;
  NSTextField* _tunerNote;
  NSTextField* _tunerCents;
  NSTextField* _tunerHz;
  TdmCentsBar* _tunerBar;
  tdm::TunerDisplay _tunerDisplay;
  NSButton* _driveCheck;
  NSButton* _shapeCheck;
  NSButton* _delayCheck;
  NSButton* _reverbCheck;
  NSTimer* _tick;
  // Transpose main controls.
  NSButton* _transposeCheck;
  NSSegmentedControl* _engineSeg;
  NSStepper* _shiftStepper;
  double _lastStepper;
  // Scrollable control area: window -> _scrollView -> _docView (flipped, so
  // y=0 is visually at the top) -> all DEV controls. Disclosures grow the
  // DOCUMENT, never the window.
  NSScrollView* _scrollView;
  TdmFlippedView* _docView;
  CGFloat _collapsedDocHeight; // document height with both panels closed
  // Disclosure panels (in-place inside the document; lower views shift down).
  NSButton* _gt2Disc;
  NSButton* _t3kDisc;
  NSView* _gt2Box;
  NSView* _t3kBox;
  NSMutableArray<NSView*>* _lowerViews;
  NSMutableArray<NSNumber*>* _lowerBaseY; // collapsed origin.y per lower view
  CGFloat _discBottom; // flipped-y just below the disclosure row (panel insert point)
  CGFloat _advShown; // currently visible advanced height
  BOOL _gt2Open;
  BOOL _t3kOpen;
  // GT2 advanced rows (built from the shared adapter table).
  NSMutableDictionary<NSNumber*, NSSlider*>* _gt2Sliders;
  NSMutableDictionary<NSNumber*, NSTextField*>* _gt2Values;
  NSButton* _resyncCheck;
  NSTextField* _gt2StateLabel;
  NSTextField* _gt2HintLabel;
  // T3K reference rows.
  NSSegmentedControl* _t3kWindowSeg;
}

- (instancetype)initWithEngine:(TdmEngine*)engine
{
  if ((self = [super init]) != nil)
  {
    _engine = engine;
    _sliders = [NSMutableDictionary dictionary];
    _valueLabels = [NSMutableDictionary dictionary];
    _gt2Sliders = [NSMutableDictionary dictionary];
    _gt2Values = [NSMutableDictionary dictionary];
    _lowerViews = [NSMutableArray array];
    _lowerBaseY = [NSMutableArray array];
    // Transpose audition state (app layer, like auditionDefaults): enabled
    // GT2 at -2 st, reference at 30 ms / Tonality Off. Installed pre-start
    // (off-RT); Start/Stop never touches NAM/IR/drive state.
    _stage.setEnabled(true);
    _stage.setEngine(tdm::lab::DevTranspose::Engine::Gt2);
    _stage.setShiftSt(-2.0f);
    _stage.setT3kWindowMs(tdm::lab::DevTranspose::kDefaultT3kWindowMs);
    _stage.setT3kTonalityHz(tdm::lab::DevTranspose::kDefaultT3kTonalityHz);
    _stage.resetGt2ToBaseline();
    _gt2Applied = _stage.gt2Config();
    // SLAM audition state: off (dry start), Push, 100%. Installed into
    // all three rig seams pre-start (off-RT); Start/Stop never touches it.
    _slam.setEnabled(false);
    _slam.setFlavor(tdm::lab::DevSlam::Flavor::Push);
    _slam.setAmount01(1.0f);
    // Alien FX lab state: off (dry start), Frequency, lab starting
    // points. Installed into the post-IR seam pre-start (off-RT).
    _alien.setEnabled(false);
    _alien.setEffect(tdm::alien::DevAlien::Effect::Frequency);
    _alien.setMix(1.0f);
    _alien.setShiftHz(150.0f);
    _alien.setLaserDir(0);
    _alien.setSweepMs(120.0f);
    _alien.setDepthMs(12.0f);
    _alien.setWarpMode(0);
    _alien.setWarpTimeMs(220.0f);
    _alien.setWarpFb(0.5f);
    _alien.setWarpShiftHz(100.0f);
    _alien.setWarpSt(3.0f);
    // Creative Pitch lab state: both sections off (dry start), Whammy
    // Up, full depth, heel, pre-gain insert. Installed into both spec
    // seams pre-start (off-RT). Gesture slots init to factory in the
    // router; staging mirrors them.
    _spec.setExpressionEnabled(false);
    _spec.setGesturesEnabled(false);
    _spec.setVoice(tdm::specfx::DevSpecFx::Voice::Whammy);
    _spec.setExprDirection(tdm::specfx::GestureDir::Up);
    _spec.setExprDepth(tdm::specfx::kMaxGestureDepthSt);
    _spec.setExpression(0.0f);
    _spec.setMix(1.0f);
    _spec.setInsert(tdm::specfx::DevSpecFx::Insert::Pre);
    _specGestSlot = 0;
    _specGestNames = [NSMutableArray array];
    for (int i = 0; i < tdm::specfx::kNumGestureSlots; ++i)
    {
      _specGestStage[i] = tdm::specfx::defaultGestureConfig(i);
      [_specGestNames addObject:[NSString stringWithUTF8String:tdm::specfx::defaultGestureName(i)]];
    }
    // MIDI control wiring (main thread): sink -> _spec atomics,
    // dispatcher -> table+sink, transport -> static handler. Then
    // restore persisted mappings + gesture slots + source intents
    // (missing/corrupt file falls back to factory silently here; the
    // MIDI section status line reports it once built).
    _midiSink.spec = &_spec;
    _midiDisp.reset(new tdm::midi::Dispatcher(_midiTable, _midiSink));
    _midiCtx.learn = &_midiLearn;
    _midiCtx.disp = _midiDisp.get();
    _midi.setHandler(tdmMidiHandler, &_midiCtx);
    _midiLearnTarget = -1;
    _midiCalActive = false;
    _midiCalTarget = -1;
    _midiOmniRestore = false;
    _midiLastCount = 0;
    _midiCalLastCount = 0;
    [self midiApplyPersisted];
    _lastStepper = 0.0;
    _advShown = 0.0;
    _gt2Open = NO;
    _t3kOpen = NO;
    _engine->rig().setTransposeInsert(&_stage);
    _engine->rig().setSlamPreDrive(&_slam.preDrive());
    _engine->rig().setSlamPostNam(&_slam.postNam());
    _engine->rig().setSlamPostIr(&_slam.postIr());
    _engine->rig().setAlienPostIr(&_alien);
    _engine->rig().setSpecPre(&_spec.preTap());
    _engine->rig().setSpecPost(&_spec.postTap());
  }
  return self;
}

- (void)dealloc
{
  [_tick invalidate];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
  (void)sender;
  return YES;
}

- (void)autoQuitAfter:(NSTimeInterval)seconds
{
  [NSTimer scheduledTimerWithTimeInterval:seconds
                                   target:NSApp
                                 selector:@selector(terminate:)
                                 userInfo:nil
                                  repeats:NO];
}

- (NSTextField*)makeLabel:(NSString*)text frame:(NSRect)frame small:(BOOL)small
{
  NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
  label.stringValue = text;
  label.bezeled = NO;
  label.drawsBackground = NO;
  label.editable = NO;
  label.selectable = NO;
  if (small)
    label.font = [NSFont systemFontOfSize:11];
  return label;
}

- (void)addSliderRow:(NSString*)name
                 tag:(SliderTag)tag
                 min:(double)mn
                 max:(double)mx
                init:(double)init
               reset:(double)reset
                   y:(CGFloat)y
               width:(CGFloat)width
{
  NSTextField* nameLabel = [self makeLabel:name frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:nameLabel];
  TdmResetSlider* slider = [[TdmResetSlider alloc] initWithFrame:NSMakeRect(135, y, width - 135 - 90, 22)];
  slider.minValue = mn;
  slider.maxValue = mx;
  slider.doubleValue = init;
  slider.resetValue = reset;
  slider.continuous = YES;
  slider.target = self;
  slider.action = @selector(paramChanged:);
  slider.tag = tag;
  [_docView addSubview:slider];
  _sliders[@(tag)] = slider;
  NSTextField* value = [self makeLabel:@"" frame:NSMakeRect(width - 80, y, 70, 22) small:NO];
  [_docView addSubview:value];
  _valueLabels[@(tag)] = value;
}

// Shift row: same slider pattern plus an integer stepper (exact -12..+12
// in one click per semitone) and the shared value label.
- (void)addShiftRow:(CGFloat)y width:(CGFloat)width
{
  NSTextField* nameLabel = [self makeLabel:@"Shift" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:nameLabel];
  TdmResetSlider* slider =
      [[TdmResetSlider alloc] initWithFrame:NSMakeRect(135, y, width - 135 - 90 - 34, 22)];
  slider.minValue = tdm::lab::DevTranspose::kMinShiftSt;
  slider.maxValue = tdm::lab::DevTranspose::kMaxShiftSt;
  slider.doubleValue = _stage.shiftSt();
  slider.resetValue = resetValueForTag(kTagShift);
  slider.continuous = YES;
  slider.target = self;
  slider.action = @selector(paramChanged:);
  slider.tag = kTagShift;
  [_docView addSubview:slider];
  _sliders[@(kTagShift)] = slider;
  _shiftStepper = [[NSStepper alloc] initWithFrame:NSMakeRect(width - 114, y, 24, 22)];
  _shiftStepper.minValue = -1000.0;
  _shiftStepper.maxValue = 1000.0;
  _shiftStepper.increment = 1.0;
  _shiftStepper.valueWraps = NO;
  _shiftStepper.autorepeat = YES;
  _shiftStepper.doubleValue = _lastStepper;
  _shiftStepper.target = self;
  _shiftStepper.action = @selector(shiftStepped:);
  [_docView addSubview:_shiftStepper];
  NSTextField* value = [self makeLabel:@"" frame:NSMakeRect(width - 80, y, 70, 22) small:NO];
  [_docView addSubview:value];
  _valueLabels[@(kTagShift)] = value;
}

// One GT2 advanced row from the shared adapter table (slider + value).
// Sliders validate on release (continuous NO): each edit trial-configures
// the stage on the main thread, so invalid cross-field combos are rejected
// before they can reach stored state. Double-click resets the field to its
// baseline default, like every other slider in this app.
- (void)addGt2Row:(int)field y:(CGFloat)y width:(CGFloat)width inView:(NSView*)view
{
  const tdm::lab::Gt2UiField& f = tdm::lab::devGt2UiField(field);
  NSTextField* nameLabel =
      [self makeLabel:[NSString stringWithUTF8String:f.label] frame:NSMakeRect(12, y, 110, 22) small:YES];
  [view addSubview:nameLabel];
  TdmResetSlider* slider =
      [[TdmResetSlider alloc] initWithFrame:NSMakeRect(126, y, width - 126 - 84, 22)];
  slider.minValue = f.min;
  slider.maxValue = f.max;
  slider.doubleValue = tdm::lab::devGt2UiGet(_stage.gt2Config(), field);
  slider.resetValue = tdm::lab::devGt2UiGet(tdm::lab::DevTranspose::knownGoodGt2(), field);
  slider.continuous = NO;
  slider.target = self;
  slider.action = @selector(gt2AdvancedChanged:);
  slider.tag = kGt2AdvTagBase + field;
  [view addSubview:slider];
  _gt2Sliders[@(field)] = slider;
  NSTextField* value = [self makeLabel:@"" frame:NSMakeRect(width - 72, y, 64, 22) small:YES];
  [view addSubview:value];
  _gt2Values[@(field)] = value;
}

// GT2 advanced container, laid out bottom-up so the frame height is exact:
// hint(22) + restore(30) + resync(26) + 17 rows x 26 + title(24) = 544.
- (void)buildGt2Panel:(CGFloat)kWidth
{
  const CGFloat cw = kWidth - 40;
  const CGFloat rowH = 26;
  const CGFloat h = 22 + 30 + 26 + 17 * rowH + 24;
  _gt2Box = [[NSView alloc] initWithFrame:NSMakeRect(20, 0, cw, h)];
  _gt2Box.hidden = YES;
  [_docView addSubview:_gt2Box];
  CGFloat y = 0;
  _gt2HintLabel = [self makeLabel:@"Edits apply on next Start (Stop → Start)."
                            frame:NSMakeRect(12, y + 1, cw - 24, 22)
                            small:YES];
  [_gt2Box addSubview:_gt2HintLabel];
  y += 22;
  NSButton* restore = [NSButton buttonWithTitle:@"Restore GT2 Baseline"
                                         target:self
                                         action:@selector(gt2Restore:)];
  restore.frame = NSMakeRect(12, y + 3, 170, 24);
  restore.bezelStyle = NSBezelStyleRounded;
  [_gt2Box addSubview:restore];
  _gt2StateLabel = [self makeLabel:@"" frame:NSMakeRect(190, y + 4, cw - 200, 22) small:YES];
  [_gt2Box addSubview:_gt2StateLabel];
  y += 30;
  _resyncCheck = [NSButton checkboxWithTitle:@"Onset re-sync"
                                      target:self
                                      action:@selector(resyncToggled:)];
  _resyncCheck.frame = NSMakeRect(12, y + 2, 220, 22);
  _resyncCheck.state = _stage.gt2Config().enableResync ? NSControlStateValueOn : NSControlStateValueOff;
  [_gt2Box addSubview:_resyncCheck];
  y += 26;
  for (int i = 0; i < 17; ++i)
    [self addGt2Row:i y:y + (16 - i) * rowH width:cw inView:_gt2Box];
  y += 17 * rowH;
  NSTextField* title = [self makeLabel:@"GT2 Advanced — geometry applies on next Start"
                                 frame:NSMakeRect(12, y + 1, cw - 24, 22)
                                 small:YES];
  [_gt2Box addSubview:title];
  [self refreshGt2Rows];
}

// T3K reference container, bottom-up: restore(32) + tonality(30) + window(30)
// + title(24) = 116. All reference controls apply live.
- (void)buildT3kPanel:(CGFloat)kWidth
{
  const CGFloat cw = kWidth - 40;
  const CGFloat h = 32 + 30 + 30 + 24;
  _t3kBox = [[NSView alloc] initWithFrame:NSMakeRect(20, 0, cw, h)];
  _t3kBox.hidden = YES;
  [_docView addSubview:_t3kBox];
  CGFloat y = 0;
  NSButton* restore = [NSButton buttonWithTitle:@"Restore T3K Reference"
                                         target:self
                                         action:@selector(t3kRestore:)];
  restore.frame = NSMakeRect(12, y + 4, 170, 24);
  restore.bezelStyle = NSBezelStyleRounded;
  restore.toolTip = @"30 ms window, Tonality Off (the auditioned reference)";
  [_t3kBox addSubview:restore];
  NSTextField* live =
      [self makeLabel:@"Applies live." frame:NSMakeRect(190, y + 5, cw - 200, 22) small:YES];
  [_t3kBox addSubview:live];
  y += 32;
  NSTextField* tonLabel = [self makeLabel:@"Tonality" frame:NSMakeRect(12, y + 4, 110, 22) small:YES];
  [_t3kBox addSubview:tonLabel];
  TdmResetSlider* ton = [[TdmResetSlider alloc] initWithFrame:NSMakeRect(126, y + 4, cw - 126 - 84, 22)];
  ton.minValue = 0.0;
  ton.maxValue = 20000.0;
  ton.doubleValue = _stage.t3kTonalityHz();
  ton.resetValue = resetValueForTag(kTagT3kTonality);
  ton.continuous = YES;
  ton.target = self;
  ton.action = @selector(paramChanged:);
  ton.tag = kTagT3kTonality;
  [_t3kBox addSubview:ton];
  _sliders[@(kTagT3kTonality)] = ton;
  NSTextField* tonVal = [self makeLabel:@"" frame:NSMakeRect(cw - 72, y + 4, 64, 22) small:YES];
  [_t3kBox addSubview:tonVal];
  _valueLabels[@(kTagT3kTonality)] = tonVal;
  y += 30;
  NSTextField* winLabel = [self makeLabel:@"Window" frame:NSMakeRect(12, y + 4, 110, 22) small:YES];
  [_t3kBox addSubview:winLabel];
  _t3kWindowSeg =
      [NSSegmentedControl segmentedControlWithLabels:@[ @"20", @"30", @"40", @"60" ]
                                        trackingMode:NSSegmentSwitchTrackingSelectOne
                                              target:self
                                              action:@selector(t3kWindowSelected:)];
  _t3kWindowSeg.frame = NSMakeRect(126, y + 3, 240, 24);
  [self syncT3kWindowSeg];
  [_t3kBox addSubview:_t3kWindowSeg];
  NSTextField* winMs = [self makeLabel:@"ms" frame:NSMakeRect(372, y + 4, 40, 22) small:YES];
  [_t3kBox addSubview:winMs];
  y += 30;
  NSTextField* title = [self makeLabel:@"TONE3000 Reference — frozen engine, live controls"
                                 frame:NSMakeRect(12, y + 1, cw - 24, 22)
                                 small:YES];
  [_t3kBox addSubview:title];
}

// Disclosure layout (flipped doc coords: y=0 is visually at the top, so the
// upper form never moves): stack open panels below _discBottom, shift every
// lower view DOWN from its collapsed base by the inserted height, and grow
// the DOCUMENT. The window frame is never touched here.
- (void)layoutAdvanced
{
  const CGFloat gap = 8;
  CGFloat want = 0;
  if (_gt2Open)
    want += _gt2Box.frame.size.height + gap;
  if (_t3kOpen)
    want += _t3kBox.frame.size.height + gap;
  CGFloat y = _discBottom + gap;
  if (_gt2Open)
  {
    NSRect f = _gt2Box.frame;
    f.origin.y = y;
    _gt2Box.frame = f;
    _gt2Box.hidden = NO;
    y += f.size.height + gap;
  }
  else
  {
    _gt2Box.hidden = YES;
  }
  if (_t3kOpen)
  {
    NSRect f = _t3kBox.frame;
    f.origin.y = y;
    _t3kBox.frame = f;
    _t3kBox.hidden = NO;
    y += f.size.height + gap;
  }
  else
  {
    _t3kBox.hidden = YES;
  }
  _gt2Disc.title = _gt2Open ? @"GT2 Advanced \u25BE" : @"GT2 Advanced \u25B8";
  _t3kDisc.title = _t3kOpen ? @"TONE3000 Reference \u25BE" : @"TONE3000 Reference \u25B8";
  for (NSUInteger i = 0; i < _lowerViews.count; ++i)
  {
    NSRect f = _lowerViews[i].frame;
    f.origin.y = _lowerBaseY[i].doubleValue + want;
    _lowerViews[i].frame = f;
  }
  NSRect df = _docView.frame;
  df.size.height = _collapsedDocHeight + want;
  _docView.frame = df;
  _advShown = want;
  // Keep the disclosure row visible with minimal scrolling (a no-op when the
  // user toggled an already-visible button), then clamp the offset so a
  // collapse never leaves the clip view past the shrunken document.
  [_docView scrollRectToVisible:_gt2Disc.frame];
  NSClipView* clip = _scrollView.contentView;
  const NSPoint p = clip.bounds.origin;
  const CGFloat maxY = df.size.height - clip.bounds.size.height;
  if (p.y > maxY)
    [clip scrollToPoint:NSMakePoint(p.x, MAX(maxY, 0.0))];
}

- (void)buildUI
{
  const CGFloat kWidth = 620;
  // Window -> scroll view -> flipped document -> all DEV controls. The window
  // is created provisional and sized to the measured collapsed document (see
  // the tail of this method); disclosures grow the document, never the window.
  NSRect frame = NSMakeRect(0, 0, kWidth, 600);
  _window = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                    | NSWindowStyleMaskResizable)
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"TechDeathMachine — Dev Control v0";

  _scrollView = [[NSScrollView alloc] initWithFrame:_window.contentView.bounds];
  _scrollView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  _scrollView.hasVerticalScroller = YES;
  _scrollView.hasHorizontalScroller = NO;
  _scrollView.autohidesScrollers = NO; // persistent: expanded panels always read as scrollable
  [_window.contentView addSubview:_scrollView];

  _docView = [[TdmFlippedView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, 10000)];
  [_scrollView setDocumentView:_docView];

  // Top-down layout in flipped coords (y=0 is visually at the top). Pitches
  // match the previous bottom-up form exactly: same density, anchored below
  // the title bar.
  CGFloat y = 12;
  _statusLabel = [self makeLabel:@"Stopped" frame:NSMakeRect(20, y, kWidth - 40, 22) small:NO];
  [_docView addSubview:_statusLabel];
  y += 24;
  _transposeStatus = [self makeLabel:@"" frame:NSMakeRect(20, y, kWidth - 40, 22) small:YES];
  [_docView addSubview:_transposeStatus];
  y += 30;

  // NAM row.
  NSTextField* namTitle = [self makeLabel:@"NAM:" frame:NSMakeRect(20, y, 44, 22) small:NO];
  [_docView addSubview:namTitle];
  _namLabel = [self makeLabel:@"(none)" frame:NSMakeRect(66, y, kWidth - 66 - 130, 22) small:YES];
  _namLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _namLabel.selectable = YES;
  [_docView addSubview:_namLabel];
  NSButton* namButton = [NSButton buttonWithTitle:@"Choose NAM…"
                                           target:self
                                           action:@selector(chooseNam:)];
  namButton.frame = NSMakeRect(kWidth - 124, y - 2, 104, 26);
  namButton.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:namButton];
  y += 30;

  // IR row.
  NSTextField* irTitle = [self makeLabel:@"IR:" frame:NSMakeRect(20, y, 44, 22) small:NO];
  [_docView addSubview:irTitle];
  _irLabel = [self makeLabel:@"(none)" frame:NSMakeRect(66, y, kWidth - 66 - 130, 22) small:YES];
  _irLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _irLabel.selectable = YES;
  [_docView addSubview:_irLabel];
  NSButton* irButton = [NSButton buttonWithTitle:@"Choose IR…"
                                          target:self
                                          action:@selector(chooseIr:)];
  irButton.frame = NSMakeRect(kWidth - 124, y - 2, 104, 26);
  irButton.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:irButton];
  y += 34;

  // Transport row.
  _transportButton = [NSButton buttonWithTitle:@"Start audio"
                                        target:self
                                        action:@selector(toggleAudio:)];
  _transportButton.frame = NSMakeRect(20, y - 2, 120, 28);
  _transportButton.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:_transportButton];
  NSTextField* hint = [self makeLabel:@"Params apply live. NAM/IR need: Stop → load → Start."
                                frame:NSMakeRect(150, y, kWidth - 170, 22)
                                small:YES];
  [_docView addSubview:hint];
  y += 40;

  // Tuner row (side-chain readout of the raw input; observes only).
  _tunerCheck = [NSButton checkboxWithTitle:@"Tuner" target:self action:@selector(tunerToggled:)];
  _tunerCheck.frame = NSMakeRect(20, y, 80, 22);
  _tunerCheck.state = NSControlStateValueOn; // matches auditionDefaults
  [_docView addSubview:_tunerCheck];
  _tunerNote = [self makeLabel:@"—" frame:NSMakeRect(120, y - 3, 90, 28) small:NO];
  _tunerNote.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
  [_docView addSubview:_tunerNote];
  _tunerCents = [self makeLabel:@"" frame:NSMakeRect(220, y, 90, 22) small:NO];
  [_docView addSubview:_tunerCents];
  _tunerHz = [self makeLabel:@"" frame:NSMakeRect(320, y, 130, 22) small:YES];
  [_docView addSubview:_tunerHz];
  y += 30;
  _tunerBar = [[TdmCentsBar alloc] initWithFrame:NSMakeRect(20, y, kWidth - 40, 14)];
  _tunerBar.cents = 0.0;
  _tunerBar.valid = NO;
  [_docView addSubview:_tunerBar];
  y += 24;

  NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(20, y, kWidth - 40, 1)];
  sep.boxType = NSBoxSeparator;
  [_docView addSubview:sep];
  y += 30;

  // Trim + gate section. init: audition starting point (NOT the reset
  // default); min/max/reset come from the DSP stage constants.
  [self addSliderRow:@"Input Trim"
                 tag:kTagInputTrim
                 min:tdm::InputTrim::kMinTrimDb
                 max:tdm::InputTrim::kMaxTrimDb
                init:0
               reset:tdm::InputTrim::kDefaultTrimDb
                   y:y
               width:kWidth];
  y += 30;
  _gateCheck = [NSButton checkboxWithTitle:@"Gate" target:self action:@selector(gateToggled:)];
  _gateCheck.frame = NSMakeRect(20, y, 160, 22);
  _gateCheck.state = NSControlStateValueOn; // audition default
  [_docView addSubview:_gateCheck];
  y += 30;
  [self addSliderRow:@"Gate Thresh"
                 tag:kTagGateThresh
                 min:tdm::TechDeathGate::kMinThresholdDb
                 max:tdm::TechDeathGate::kMaxThresholdDb
                init:-55
               reset:tdm::TechDeathGate::kDefaultThresholdDb
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Gate Release"
                 tag:kTagGateRel
                 min:tdm::TechDeathGate::kMinReleaseMs
                 max:tdm::TechDeathGate::kMaxReleaseMs
                init:52
               reset:tdm::TechDeathGate::kDefaultReleaseMs
                   y:y
               width:kWidth];
  y += 40;

  // PITCH SHIFT group: TRANSPOSE (stable retuning, in signal-flow
  // position Gate -> Transpose -> Drive) + CREATIVE PITCH — EXPRESSION +
  // CREATIVE PITCH — GESTURES below the disclosure anchor. The three
  // sections enable independently; DSP routing is untouched by this
  // consolidation — only the DEV controls move together.
  NSTextField* pitchHead = [self makeLabel:@"PITCH SHIFT" frame:NSMakeRect(20, y, 180, 22) small:NO];
  [_docView addSubview:pitchHead];
  y += 26;
  // Transpose section. Engine/shift/enable apply live through the stage
  // atomics; GT2 advanced edits apply on the next Start (see below).
  _transposeCheck = [NSButton checkboxWithTitle:@"Transpose"
                                        target:self
                                        action:@selector(transposeToggled:)];
  _transposeCheck.frame = NSMakeRect(20, y, 180, 22);
  _transposeCheck.state = _stage.isEnabled() ? NSControlStateValueOn : NSControlStateValueOff;
  [_docView addSubview:_transposeCheck];
  y += 30;
  NSTextField* engineLabel = [self makeLabel:@"Engine" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:engineLabel];
  _engineSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Our GT2", @"T3K Ref" ]
                                                 trackingMode:NSSegmentSwitchTrackingSelectOne
                                                       target:self
                                                       action:@selector(engineSelected:)];
  _engineSeg.frame = NSMakeRect(135, y - 2, 220, 26);
  _engineSeg.selectedSegment =
      _stage.engine() == tdm::lab::DevTranspose::Engine::Tone3000 ? 1 : 0;
  [_docView addSubview:_engineSeg];
  NSButton* abButton = [NSButton buttonWithTitle:@"A/B" target:self action:@selector(abPressed:)];
  abButton.frame = NSMakeRect(365, y - 2, 64, 26);
  abButton.bezelStyle = NSBezelStyleRounded;
  abButton.toolTip = @"Flip between Our GT2 and the TONE3000 reference (one click)";
  [_docView addSubview:abButton];
  y += 30;
  [self addShiftRow:y width:kWidth];
  y += 30;
  _gt2Disc = [NSButton buttonWithTitle:@"GT2 Advanced \u25B8"
                               target:self
                               action:@selector(gt2Disclosure:)];
  _gt2Disc.frame = NSMakeRect(20, y - 2, 150, 26);
  _gt2Disc.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:_gt2Disc];
  _t3kDisc = [NSButton buttonWithTitle:@"TONE3000 Reference \u25B8"
                               target:self
                               action:@selector(t3kDisclosure:)];
  _t3kDisc.frame = NSMakeRect(180, y - 2, 200, 26);
  _t3kDisc.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:_t3kDisc];
  y += 30;
  // Advanced containers live here (hidden until disclosed). Everything built
  // after this point shifts down when a panel opens (see layoutAdvanced).
  _discBottom = y;
  [self buildGt2Panel:kWidth];
  [self buildT3kPanel:kWidth];
  const NSUInteger lowerStart = _docView.subviews.count;
  y += 10; // breathing room above Creative Pitch when panels are closed

  // CREATIVE PITCH — EXPRESSION subsection (DEV research:
  // expression-driven Laser at pre-gain and post-IR taps. One normalized
  // expression position drives per-voice heel/toe maps; the gestures
  // section below is independently enabled and automates moves).
  NSTextField* specHead = [self makeLabel:@"CREATIVE PITCH — EXPRESSION"
                                    frame:NSMakeRect(20, y, 280, 22)
                                    small:NO];
  [_docView addSubview:specHead];
  y += 30;
  _specCheck = [NSButton checkboxWithTitle:@"Expression"
                                    target:self
                                    action:@selector(specToggled:)];
  _specCheck.frame = NSMakeRect(20, y, 180, 22);
  _specCheck.state = NSControlStateValueOff; // dry start; enable to audition
  [_docView addSubview:_specCheck];
  y += 30;
  NSTextField* specVoiceLabel = [self makeLabel:@"Effect" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:specVoiceLabel];
  _specVoiceSeg = [NSSegmentedControl
      segmentedControlWithLabels:@[ @"Whammy", @"Dual", @"DualWham", @"Ring" ]
                    trackingMode:NSSegmentSwitchTrackingSelectOne
                          target:self
                          action:@selector(specVoiceSelected:)];
  _specVoiceSeg.frame = NSMakeRect(135, y - 2, 400, 26);
  _specVoiceSeg.selectedSegment = 0;
  [_docView addSubview:_specVoiceSeg];
  y += 30;
  NSTextField* specDirLabel = [self makeLabel:@"Direction" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:specDirLabel];
  _specDirSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Up", @"Down" ]
                                                  trackingMode:NSSegmentSwitchTrackingSelectOne
                                                        target:self
                                                        action:@selector(specDirSelected:)];
  _specDirSeg.frame = NSMakeRect(135, y - 2, 200, 26);
  _specDirSeg.selectedSegment = 0;
  [_docView addSubview:_specDirSeg];
  y += 30;
  [self addSliderRow:@"Depth"
                 tag:kTagSpecDepth
                 min:0
                 max:tdm::specfx::kMaxGestureDepthSt
                init:tdm::specfx::kMaxGestureDepthSt
               reset:tdm::specfx::kMaxGestureDepthSt
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Expression"
                 tag:kTagSpecExpr
                 min:0
                 max:100
                init:0
               reset:0
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Mix"
                 tag:kTagSpecMix
                 min:0
                 max:100
                init:100
               reset:100
                   y:y
               width:kWidth];
  y += 30;
  NSTextField* specInsertLabel = [self makeLabel:@"Insert" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:specInsertLabel];
  _specInsertSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Pre", @"Post" ]
                                                   trackingMode:NSSegmentSwitchTrackingSelectOne
                                                         target:self
                                                         action:@selector(specInsertSelected:)];
  _specInsertSeg.frame = NSMakeRect(135, y - 2, 200, 26);
  _specInsertSeg.selectedSegment = 0;
  [_docView addSubview:_specInsertSeg];
  y += 30;
  y += 8; // breathing room above the gestures caption
  NSTextField* gestHead = [self makeLabel:@"CREATIVE PITCH — GESTURES"
                                    frame:NSMakeRect(20, y, 280, 22)
                                    small:NO];
  [_docView addSubview:gestHead];
  y += 30;
  _specGestCheck = [NSButton checkboxWithTitle:@"Gestures"
                                        target:self
                                        action:@selector(specGestToggled:)];
  _specGestCheck.frame = NSMakeRect(20, y, 180, 22);
  _specGestCheck.state = NSControlStateValueOff; // dry start; works with Expression off
  [_docView addSubview:_specGestCheck];
  y += 30;
  NSTextField* gestSlotLabel = [self makeLabel:@"Gesture" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestSlotLabel];
  _specGestPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(135, y - 2, 200, 26) pullsDown:NO];
  for (int i = 0; i < tdm::specfx::kNumGestureSlots; ++i)
    [_specGestPopup addItemWithTitle:[NSString stringWithFormat:@"%d: %@", i + 1, _specGestNames[i]]];
  [_specGestPopup selectItemAtIndex:0];
  _specGestPopup.target = self;
  _specGestPopup.action = @selector(specGestPopupSelected:);
  [_docView addSubview:_specGestPopup];
  TdmMomentaryButton* gestTrig =
      [[TdmMomentaryButton alloc] initWithFrame:NSMakeRect(345, y - 2, 90, 26)];
  gestTrig.title = @"Trigger";
  gestTrig.bezelStyle = NSBezelStyleRounded;
  gestTrig.target = self;
  gestTrig.downAction = @selector(specGestDown:);
  gestTrig.upAction = @selector(specGestUp:);
  gestTrig.toolTip = @"Commits staged edits, then momentary-triggers the displayed slot";
  [_docView addSubview:gestTrig];
  NSButton* gestOne =
      [NSButton buttonWithTitle:@"1-Shot" target:self action:@selector(specGestOneShot:)];
  gestOne.frame = NSMakeRect(445, y - 2, 80, 26);
  gestOne.bezelStyle = NSBezelStyleRounded;
  gestOne.toolTip = @"Commits staged edits, then one-shot-triggers the displayed slot";
  [_docView addSubview:gestOne];
  y += 30;
  NSTextField* gestNameLabel = [self makeLabel:@"Name" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestNameLabel];
  _specGestName = [[NSTextField alloc] initWithFrame:NSMakeRect(135, y, 200, 22)];
  _specGestName.stringValue = _specGestNames[0];
  [_docView addSubview:_specGestName];
  y += 30;
  NSTextField* gestEffectLabel =
      [self makeLabel:@"Effect" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestEffectLabel];
  _specGestEffectSeg = [NSSegmentedControl
      segmentedControlWithLabels:@[ @"Whammy", @"Dual", @"DualWham", @"Ring" ]
                    trackingMode:NSSegmentSwitchTrackingSelectOne
                          target:self
                          action:@selector(specGestEffectSelected:)];
  _specGestEffectSeg.frame = NSMakeRect(135, y - 2, 400, 26);
  _specGestEffectSeg.selectedSegment = 0;
  [_docView addSubview:_specGestEffectSeg];
  y += 30;
  NSTextField* gestDirLabel = [self makeLabel:@"Direction" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestDirLabel];
  _specGestDirSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Up", @"Down" ]
                                                    trackingMode:NSSegmentSwitchTrackingSelectOne
                                                          target:self
                                                          action:@selector(specGestDirSelected:)];
  _specGestDirSeg.frame = NSMakeRect(135, y - 2, 200, 26);
  _specGestDirSeg.selectedSegment = 0;
  [_docView addSubview:_specGestDirSeg];
  y += 30;
  [self addSliderRow:@"Depth"
                 tag:kTagGestDepth
                 min:0
                 max:tdm::specfx::kMaxGestureDepthSt
                init:12
               reset:12
                   y:y
               width:kWidth];
  y += 30;
  NSTextField* gestMotionLabel =
      [self makeLabel:@"Motion" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestMotionLabel];
  _specGestMotionSeg = [NSSegmentedControl
      segmentedControlWithLabels:@[ @"To Depth", @"From Depth" ]
                    trackingMode:NSSegmentSwitchTrackingSelectOne
                          target:self
                          action:@selector(specGestMotionSelected:)];
  _specGestMotionSeg.frame = NSMakeRect(135, y - 2, 260, 26);
  _specGestMotionSeg.selectedSegment = 0;
  [_docView addSubview:_specGestMotionSeg];
  y += 30;
  [self addSliderRow:@"Time"
                 tag:kTagGestTime
                 min:0
                 max:1000
                init:50
               reset:50
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Return"
                 tag:kTagGestRet
                 min:0
                 max:1000
                init:100
               reset:100
                   y:y
               width:kWidth];
  y += 30;
  NSTextField* gestSaveLabel = [self makeLabel:@"Slot" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:gestSaveLabel];
  NSButton* gestSave = [NSButton buttonWithTitle:@"Save Slot" target:self action:@selector(specGestSave:)];
  gestSave.frame = NSMakeRect(135, y - 2, 110, 26);
  gestSave.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:gestSave];
  NSButton* gestReset =
      [NSButton buttonWithTitle:@"Reset Slot" target:self action:@selector(specGestReset:)];
  gestReset.frame = NSMakeRect(255, y - 2, 110, 26);
  gestReset.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:gestReset];
  y += 30;
  [self refreshGestureUI]; // sync fields to staging (slot 0 factory)
  [self refreshSpecGating]; // per-voice Direction/Depth enablement
  y += 14; // gap above the SLAM section

  // SLAM section (DEV audition: Push/Crush/Mass across three taps; Impact
  // stays parked in research — DSP preserved, hidden from the selector).
  _slamCheck = [NSButton checkboxWithTitle:@"SLAM" target:self action:@selector(slamToggled:)];
  _slamCheck.frame = NSMakeRect(20, y, 180, 22);
  _slamCheck.state = NSControlStateValueOff; // dry start; enable to audition
  [_docView addSubview:_slamCheck];
  y += 30;
  NSTextField* slamFlavorLabel = [self makeLabel:@"Flavor" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:slamFlavorLabel];
  _slamSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Push", @"Crush", @"Mass" ]
                                              trackingMode:NSSegmentSwitchTrackingSelectOne
                                                    target:self
                                                    action:@selector(slamFlavorSelected:)];
  _slamSeg.frame = NSMakeRect(135, y - 2, 320, 26);
  _slamSeg.selectedSegment = 0;
  [_docView addSubview:_slamSeg];
  y += 30;
  [self addSliderRow:@"Amount"
                 tag:kTagSlamAmount
                 min:0
                 max:100
                init:100
               reset:100
                   y:y
               width:kWidth];
  y += 30;
  y += 14; // gap above the Drive section

  // Drive section.
  _driveCheck = [NSButton checkboxWithTitle:@"TightDrive" target:self action:@selector(driveToggled:)];
  _driveCheck.frame = NSMakeRect(20, y, 180, 22);
  _driveCheck.state = NSControlStateValueOn; // audition default
  [_docView addSubview:_driveCheck];
  y += 30;
  [self addSliderRow:@"Tight"
                 tag:kTagTight
                 min:tdm::TightDrive::kMinTight
                 max:tdm::TightDrive::kMaxTight
                init:0.85
               reset:tdm::TightDrive::kDefaultTight
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Drive"
                 tag:kTagDrive
                 min:tdm::TightDrive::kMinDrive
                 max:tdm::TightDrive::kMaxDrive
                init:0.50
               reset:tdm::TightDrive::kDefaultDrive
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Bite"
                 tag:kTagBite
                 min:tdm::TightDrive::kMinBite
                 max:tdm::TightDrive::kMaxBite
                init:0.70
               reset:tdm::TightDrive::kDefaultBite
                   y:y
               width:kWidth];
  y += 40;

  // ALIEN FX LAB section (DEV research: Frequency | Laser | Warp at the
  // post-IR tap, pre-ToneShape. All rows stay visible — the router dips
  // through dry on effect switches, so every control is live anytime).
  _alienCheck = [NSButton checkboxWithTitle:@"Alien FX" target:self action:@selector(alienToggled:)];
  _alienCheck.frame = NSMakeRect(20, y, 180, 22);
  _alienCheck.state = NSControlStateValueOff; // dry start; enable to audition
  [_docView addSubview:_alienCheck];
  y += 30;
  NSTextField* alienEffectLabel = [self makeLabel:@"Effect" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:alienEffectLabel];
  _alienSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Frequency", @"Laser", @"Warp" ]
                                               trackingMode:NSSegmentSwitchTrackingSelectOne
                                                     target:self
                                                     action:@selector(alienEffectSelected:)];
  _alienSeg.frame = NSMakeRect(135, y - 2, 340, 26);
  _alienSeg.selectedSegment = 0;
  [_docView addSubview:_alienSeg];
  y += 30;
  [self addSliderRow:@"Mix"
                 tag:kTagAlienMix
                 min:0
                 max:100
                init:100
               reset:100
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Shift Hz" // UI range: study sweep; core allows +-2000
                 tag:kTagAlienShiftHz
                 min:-600
                 max:600
                init:150
               reset:150
                   y:y
               width:kWidth];
  y += 30;
  NSTextField* alienDirLabel = [self makeLabel:@"Laser" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:alienDirLabel];
  _alienDirSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Down", @"Up", @"Cycle" ]
                                                  trackingMode:NSSegmentSwitchTrackingSelectOne
                                                        target:self
                                                        action:@selector(alienDirSelected:)];
  _alienDirSeg.frame = NSMakeRect(135, y - 2, 300, 26);
  _alienDirSeg.selectedSegment = 0;
  [_docView addSubview:_alienDirSeg];
  y += 30;
  [self addSliderRow:@"Sweep ms"
                 tag:kTagAlienSweepMs
                 min:tdm::alien::AlienLaser::kMinSweepMs
                 max:tdm::alien::AlienLaser::kMaxSweepMs
                init:120
               reset:120
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Depth ms"
                 tag:kTagAlienDepthMs
                 min:tdm::alien::AlienLaser::kMinDepthMs
                 max:tdm::alien::AlienLaser::kMaxDepthMs
                init:12
               reset:12
                   y:y
               width:kWidth];
  y += 30;
  NSButton* alienTrigger = [NSButton buttonWithTitle:@"Trigger Laser"
                                              target:self
                                              action:@selector(alienTrigger:)];
  alienTrigger.frame = NSMakeRect(135, y - 2, 160, 26);
  [_docView addSubview:alienTrigger];
  y += 30;
  NSTextField* alienWarpLabel = [self makeLabel:@"Warp" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:alienWarpLabel];
  _alienWarpSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Freq", @"Pitch" ]
                                                   trackingMode:NSSegmentSwitchTrackingSelectOne
                                                         target:self
                                                         action:@selector(alienWarpModeSelected:)];
  _alienWarpSeg.frame = NSMakeRect(135, y - 2, 220, 26);
  _alienWarpSeg.selectedSegment = 0;
  [_docView addSubview:_alienWarpSeg];
  y += 30;
  [self addSliderRow:@"Warp Time" // UI max 600 ms: router pre-roll budget
                 tag:kTagAlienWarpTime
                 min:tdm::alien::AlienWarp::kMinTimeMs
                 max:600
                init:220
               reset:220
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Warp Hz"
                 tag:kTagAlienWarpHz
                 min:tdm::alien::AlienWarp::kMinShiftHz
                 max:tdm::alien::AlienWarp::kMaxShiftHz
                init:100
               reset:100
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Warp st"
                 tag:kTagAlienWarpSt
                 min:tdm::alien::AlienDoppler::kMinSt
                 max:tdm::alien::AlienDoppler::kMaxSt
                init:3
               reset:3
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Warp Fdbk"
                 tag:kTagAlienWarpFb
                 min:0
                 max:tdm::alien::AlienWarp::kMaxFeedback
                init:0.5
               reset:0.5
                   y:y
               width:kWidth];
  y += 30;
  y += 14; // gap above the ToneShape section

  // ToneShape section (post-cab; neutral is transparent).
  _shapeCheck = [NSButton checkboxWithTitle:@"ToneShape" target:self action:@selector(shapeToggled:)];
  _shapeCheck.frame = NSMakeRect(20, y, 180, 22);
  _shapeCheck.state = NSControlStateValueOn; // audition default (neutral)
  [_docView addSubview:_shapeCheck];
  y += 30;
  [self addSliderRow:@"Weight"
                 tag:kTagWeight
                 min:tdm::ToneShape::kMinWeight
                 max:tdm::ToneShape::kMaxWeight
                init:0.50
               reset:tdm::ToneShape::kDefaultWeight
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Contour"
                 tag:kTagContour
                 min:tdm::ToneShape::kMinContour
                 max:tdm::ToneShape::kMaxContour
                init:0.50
               reset:tdm::ToneShape::kDefaultContour
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Presence"
                 tag:kTagPresence
                 min:tdm::ToneShape::kMinPresence
                 max:tdm::ToneShape::kMaxPresence
                init:0.50
               reset:tdm::ToneShape::kDefaultPresence
                   y:y
               width:kWidth];
  y += 40;

  // Space section (first stereo stage; both units OFF at audition default
  // so the rhythm tone stays dry until leads/ambience are auditioned).
  _delayCheck = [NSButton checkboxWithTitle:@"Delay" target:self action:@selector(delayToggled:)];
  _delayCheck.frame = NSMakeRect(20, y, 180, 22);
  _delayCheck.state = NSControlStateValueOff; // audition default
  [_docView addSubview:_delayCheck];
  y += 30;
  [self addSliderRow:@"Delay Time"
                 tag:kTagDelayTime
                 min:tdm::Delay::kMinTimeMs
                 max:tdm::Delay::kMaxTimeMs
                init:tdm::Delay::kDefaultTimeMs
               reset:tdm::Delay::kDefaultTimeMs
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Delay Fdbk"
                 tag:kTagDelayFb
                 min:tdm::Delay::kMinFeedback
                 max:tdm::Delay::kMaxFeedback
                init:0.35
               reset:tdm::Delay::kDefaultFeedback
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Delay Mix"
                 tag:kTagDelayMix
                 min:tdm::SpaceProcessor::kMinMix
                 max:tdm::SpaceProcessor::kMaxMix
                init:0.25
               reset:tdm::SpaceProcessor::kDefaultDelayMix
                   y:y
               width:kWidth];
  y += 40;
  _reverbCheck = [NSButton checkboxWithTitle:@"Reverb" target:self action:@selector(reverbToggled:)];
  _reverbCheck.frame = NSMakeRect(20, y, 180, 22);
  _reverbCheck.state = NSControlStateValueOff; // audition default
  [_docView addSubview:_reverbCheck];
  y += 30;
  [self addSliderRow:@"Reverb Decay"
                 tag:kTagReverbDecay
                 min:tdm::Reverb::kMinDecay
                 max:tdm::Reverb::kMaxDecay
                init:0.40
               reset:tdm::Reverb::kDefaultDecay
                   y:y
               width:kWidth];
  y += 30;
  [self addSliderRow:@"Reverb Mix"
                 tag:kTagReverbMix
                 min:tdm::SpaceProcessor::kMinMix
                 max:tdm::SpaceProcessor::kMaxMix
                init:0.20
               reset:tdm::SpaceProcessor::kDefaultReverbMix
                   y:y
               width:kWidth];
  y += 40;

  // Output section.
  [self addSliderRow:@"Output Trim"
                 tag:kTagOutTrim
                 min:tdm::OutputTrim::kMinTrimDb
                 max:tdm::OutputTrim::kMaxTrimDb
                init:0
               reset:tdm::OutputTrim::kDefaultTrimDb
                   y:y
               width:kWidth];
  y += 40;

  // MIDI CONTROL section (generic CoreMIDI -> mapping table -> DSP
  // atomics; FCB1010 is the first test device, nothing is
  // controller-specific). All mapping edits are mouse-driven and
  // work with no hardware connected.
  NSTextField* midiHead = [self makeLabel:@"MIDI CONTROL" frame:NSMakeRect(20, y, 200, 22) small:NO];
  [_docView addSubview:midiHead];
  y += 30;
  NSTextField* midiSrcLabel = [self makeLabel:@"Input Source" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:midiSrcLabel];
  _midiSrcPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(135, y - 2, 250, 26) pullsDown:NO];
  _midiSrcPopup.target = self;
  _midiSrcPopup.action = @selector(midiSrcSelected:);
  _midiSrcPopup.toolTip = @"Toggle connection per source (multiple sources stay connected)";
  [_docView addSubview:_midiSrcPopup];
  NSButton* midiRefresh = [NSButton buttonWithTitle:@"Refresh" target:self action:@selector(midiRefresh:)];
  midiRefresh.frame = NSMakeRect(395, y - 2, 90, 26);
  midiRefresh.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:midiRefresh];
  y += 30;
  NSTextField* midiRecentLabel =
      [self makeLabel:@"Recent Input" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:midiRecentLabel];
  _midiRecent = [self makeLabel:@"—" frame:NSMakeRect(135, y, 440, 22) small:NO];
  [_docView addSubview:_midiRecent];
  y += 30;
  _midiOmniCheck = [NSButton checkboxWithTitle:@"Learn OMNI"
                                        target:self
                                        action:@selector(midiOmniToggled:)];
  _midiOmniCheck.frame = NSMakeRect(20, y, 130, 22);
  _midiOmniCheck.state = _midiOmniRestore ? NSControlStateValueOn : NSControlStateValueOff;
  _midiOmniCheck.toolTip = @"Learn ignores source + channel (any controller drives the target)";
  [_docView addSubview:_midiOmniCheck];
  _midiStatus = [self makeLabel:@"" frame:NSMakeRect(160, y, 415, 22) small:NO];
  [_docView addSubview:_midiStatus];
  y += 30;
  NSTextField* midiMapHead = [self makeLabel:@"MAPPINGS" frame:NSMakeRect(20, y, 200, 22) small:NO];
  [_docView addSubview:midiMapHead];
  y += 30;
  for (int t = 0; t < (int)tdm::midi::Target::Count; ++t)
  {
    NSTextField* lab = [self makeLabel:midiShortLabel(t) frame:NSMakeRect(20, y, 110, 22) small:NO];
    lab.toolTip = [NSString stringWithUTF8String:tdm::midi::targetName((tdm::midi::Target)t)];
    [_docView addSubview:lab];
    _midiAssign[t] = [self makeLabel:@"—" frame:NSMakeRect(135, y, 130, 22) small:NO];
    [_docView addSubview:_midiAssign[t]];
    NSButton* learn = [NSButton buttonWithTitle:@"Learn" target:self action:@selector(midiLearnBtn:)];
    learn.frame = NSMakeRect(275, y - 2, 62, 26);
    learn.bezelStyle = NSBezelStyleRounded;
    learn.tag = t;
    [_docView addSubview:learn];
    _midiLearnBtn[t] = learn;
    NSArray<NSString*>* modes = (t <= 1) ? @[ @"Norm", @"Inv" ]
        : (t <= 3)                       ? @[ @"Tgl", @"Mom" ]
                                         : @[ @"1-Shot", @"Mom" ];
    NSSegmentedControl* seg = [NSSegmentedControl segmentedControlWithLabels:modes
                                                               trackingMode:NSSegmentSwitchTrackingSelectOne
                                                                     target:self
                                                                     action:@selector(midiModeSeg:)];
    seg.frame = NSMakeRect(347, y - 2, 140, 26);
    seg.selectedSegment = 0;
    seg.tag = 100 + t;
    [_docView addSubview:seg];
    _midiModeSeg[t] = seg;
    NSButton* clear = [NSButton buttonWithTitle:@"×" target:self action:@selector(midiClearRow:)];
    clear.frame = NSMakeRect(497, y - 2, 28, 26);
    clear.bezelStyle = NSBezelStyleRounded;
    clear.tag = 200 + t;
    clear.toolTip = @"Clear this mapping";
    [_docView addSubview:clear];
    _midiClearBtn[t] = clear;
    y += 30;
  }
  NSTextField* midiSaveHead = [self makeLabel:@"Mappings" frame:NSMakeRect(20, y, 110, 22) small:NO];
  [_docView addSubview:midiSaveHead];
  NSButton* midiSave = [NSButton buttonWithTitle:@"Save Now" target:self action:@selector(midiSave:)];
  midiSave.frame = NSMakeRect(135, y - 2, 100, 26);
  midiSave.bezelStyle = NSBezelStyleRounded;
  midiSave.toolTip = @"Persist mappings + all gesture slots + source intents";
  [_docView addSubview:midiSave];
  NSButton* midiClearAll =
      [NSButton buttonWithTitle:@"Clear Maps" target:self action:@selector(midiClearAll:)];
  midiClearAll.frame = NSMakeRect(245, y - 2, 100, 26);
  midiClearAll.bezelStyle = NSBezelStyleRounded;
  [_docView addSubview:midiClearAll];
  _midiSaveLabel = [self makeLabel:@"" frame:NSMakeRect(355, y, 220, 22) small:YES];
  [_docView addSubview:_midiSaveLabel];
  y += 30;
  [self refreshMidiSources];
  [self refreshMidiRows];
  y += 10; // gap above the footer
  NSTextField* foot = [self makeLabel:@"Dev build: MIDI maps persist; no rig presets, no meters."
                                frame:NSMakeRect(20, y, kWidth - 40, 22)
                                small:YES];
  [_docView addSubview:foot];

  // Disclosure bookkeeping: every view below the transpose section shifts
  // down from its collapsed base when an advanced panel opens (see
  // layoutAdvanced).
  _lowerViews = [[_docView.subviews subarrayWithRange:NSMakeRange(
                       lowerStart, _docView.subviews.count - lowerStart)] mutableCopy];
  [_lowerBaseY removeAllObjects];
  for (NSView* v in _lowerViews)
    [_lowerBaseY addObject:@(v.frame.origin.y)];

  // The document height is measured from the finished top-down layout (footer
  // row + bottom pad, mirroring the 12 pt top pad).
  _collapsedDocHeight = y + 22 + 12;
  NSRect docFrame = _docView.frame;
  docFrame.size.height = _collapsedDocHeight;
  _docView.frame = docFrame;

  // The window shows a practical slice of the document: the full collapsed
  // height when it fits, otherwise clamped against the visible screen so the
  // title bar and bottom stay reachable. Disclosures never resize the window.
  CGFloat maxContentH = _collapsedDocHeight;
  NSScreen* screen = [NSScreen mainScreen];
  if (screen != nil)
    maxContentH = MIN(maxContentH, screen.visibleFrame.size.height - 100.0);
  if (maxContentH < 320.0)
    maxContentH = MIN(_collapsedDocHeight, 320.0);
  [_window setContentSize:NSMakeSize(kWidth, maxContentH)];
  _window.contentMinSize = NSMakeSize(kWidth, 320.0);
  _window.contentMaxSize = NSMakeSize(kWidth + 400.0, maxContentH);
  [_window center];
  [_scrollView.contentView scrollToPoint:NSMakePoint(0.0, 0.0)]; // start at the top

  [self refreshFileLabels];
  [self refreshTransposeStatus];
  [self tick:nil];
  _tick = [NSTimer scheduledTimerWithTimeInterval:0.1
                                           target:self
                                         selector:@selector(tick:)
                                         userInfo:nil
                                          repeats:YES];
  [_window makeKeyAndOrderFront:nil];
}

- (void)alert:(NSString*)message info:(NSString*)info
{
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = message;
  alert.informativeText = info;
  [alert addButtonWithTitle:@"OK"];
  [alert beginSheetModalForWindow:_window completionHandler:nil];
}

- (void)refreshFileLabels
{
  _namLabel.stringValue = baseName(_engine->namPath());
  _irLabel.stringValue = baseName(_engine->irPath());
}

- (void)refreshValueLabels:(tdm::RigParams)p
{
  _valueLabels[@(kTagInputTrim)].stringValue = fmtDb(p.inputTrimDb);
  _valueLabels[@(kTagGateThresh)].stringValue = fmtDb(p.gateThresholdDb);
  _valueLabels[@(kTagGateRel)].stringValue = fmtMs(p.gateReleaseMs);
  _valueLabels[@(kTagTight)].stringValue = fmt01(p.tight);
  _valueLabels[@(kTagDrive)].stringValue = fmt01(p.drive);
  _valueLabels[@(kTagBite)].stringValue = fmt01(p.bite);
  _valueLabels[@(kTagWeight)].stringValue = fmt01(p.weight);
  _valueLabels[@(kTagContour)].stringValue = fmt01(p.contour);
  _valueLabels[@(kTagPresence)].stringValue = fmt01(p.presence);
  _valueLabels[@(kTagDelayTime)].stringValue = fmtMs(p.delayTimeMs);
  _valueLabels[@(kTagDelayFb)].stringValue = fmt01(p.delayFeedback);
  _valueLabels[@(kTagDelayMix)].stringValue = fmt01(p.delayMix);
  _valueLabels[@(kTagReverbDecay)].stringValue = fmt01(p.reverbDecay);
  _valueLabels[@(kTagReverbMix)].stringValue = fmt01(p.reverbMix);
  _valueLabels[@(kTagOutTrim)].stringValue = fmtDb(p.outputTrimDb);
  // Transpose value labels show APPLIED (clamped) stage state, like the rig.
  _valueLabels[@(kTagShift)].stringValue = fmtSt(_stage.shiftSt());
  _valueLabels[@(kTagT3kTonality)].stringValue = fmtTonality(_stage.t3kTonalityHz());
  _valueLabels[@(kTagSlamAmount)].stringValue =
      [NSString stringWithFormat:@"%.0f%%", _slam.amount01() * 100.0];
  _valueLabels[@(kTagAlienMix)].stringValue =
      [NSString stringWithFormat:@"%.0f%%", _alien.mix01() * 100.0];
  _valueLabels[@(kTagAlienShiftHz)].stringValue =
      [NSString stringWithFormat:@"%+.0f Hz", _alien.shiftHz()];
  _valueLabels[@(kTagAlienSweepMs)].stringValue =
      [NSString stringWithFormat:@"%.0f ms", _alien.sweepMs()];
  _valueLabels[@(kTagAlienDepthMs)].stringValue =
      [NSString stringWithFormat:@"%.1f ms", _alien.depthMs()];
  _valueLabels[@(kTagAlienWarpTime)].stringValue =
      [NSString stringWithFormat:@"%.0f ms", _alien.warpTimeMs()];
  _valueLabels[@(kTagAlienWarpHz)].stringValue =
      [NSString stringWithFormat:@"%+.0f Hz", _alien.warpShiftHz()];
  _valueLabels[@(kTagAlienWarpSt)].stringValue = fmtSt(_alien.warpSt());
  _valueLabels[@(kTagAlienWarpFb)].stringValue = fmt01(_alien.warpFb());
  _valueLabels[@(kTagSpecExpr)].stringValue =
      [NSString stringWithFormat:@"%.0f%%", _spec.expression() * 100.0];
  _valueLabels[@(kTagSpecMix)].stringValue =
      [NSString stringWithFormat:@"%.0f%%", _spec.mix01() * 100.0];
  _valueLabels[@(kTagSpecDepth)].stringValue =
      [NSString stringWithFormat:@"%.1f st", _spec.exprDepth()];
  // Gesture labels show STAGED (possibly uncommitted) edits, like the fields.
  const tdm::specfx::GestureConfig& gst = _specGestStage[_specGestSlot];
  _valueLabels[@(kTagGestDepth)].stringValue = [NSString stringWithFormat:@"%.1f st", gst.depthSt];
  _valueLabels[@(kTagGestTime)].stringValue = [NSString stringWithFormat:@"%.0f ms", gst.timeMs];
  _valueLabels[@(kTagGestRet)].stringValue = [NSString stringWithFormat:@"%.0f ms", gst.retMs];
}

// GT2 advanced value labels from stored (pending) config.
- (void)refreshGt2Rows
{
  const tdm::GuitarTranspose::Config& cfg = _stage.gt2Config();
  for (int i = 0; i < tdm::lab::devGt2UiFieldCount(); ++i)
    _gt2Values[@(i)].stringValue = fmtGt2Field(i, tdm::lab::devGt2UiGet(cfg, i));
  _resyncCheck.state = cfg.enableResync ? NSControlStateValueOn : NSControlStateValueOff;
  _gt2StateLabel.stringValue =
      tdm::lab::devGt2UiIsBaseline(cfg) ? @"baseline ✓ (auditioned)" : @"CUSTOM (not the baseline)";
}

// T3K window segmented follows stored window.
- (void)syncT3kWindowSeg
{
  const int ms = _stage.t3kWindowMs();
  _t3kWindowSeg.selectedSegment = (ms == 20) ? 0 : (ms == 40) ? 2 : (ms == 60) ? 3 : 1;
}

// Compact transpose status (10 Hz, lock-free reads only).
- (void)refreshTransposeStatus
{
  const BOOL on = _stage.isEnabled();
  const BOOL isT3k = _stage.engine() == tdm::lab::DevTranspose::Engine::Tone3000;
  NSMutableString* s = [NSMutableString
      stringWithFormat:@"Transpose %@ · %@ · %+.1f st · lat %d", on ? @"ON" : @"bypassed",
                       isT3k ? @"T3K Ref" : @"Our GT2", _stage.shiftSt(), _stage.latencySamples()];
  if (_engine->isRunning())
    [s appendFormat:@" · %.0f Hz / %d frames", _engine->sampleRate(), _engine->outputBufferFrames()];
  if (_stage.resetShiftSt() == 0.0f && _stage.shiftSt() != 0.0f)
    [s appendString:@" · GT2 @0st needs Stop→Start"];
  _transposeStatus.stringValue = s;
  [self refreshGt2Rows];
  // Restart badge: stored config differs from the running engine's.
  if (_engine->isRunning() && !gt2ConfigsEqual(_stage.gt2Config(), _gt2Applied))
    _gt2HintLabel.stringValue = @"● differs from the running engine — Stop → Start to apply.";
  else if (![_gt2HintLabel.stringValue hasPrefix:@"Invalid"])
    _gt2HintLabel.stringValue = @"Edits apply on next Start (Stop → Start).";
}

- (void)tick:(NSTimer*)timer
{
  (void)timer;
  const tdm::RigParams p = _engine->rig().params(); // lock-free snapshot
  [self refreshValueLabels:p];
  [self refreshTransposeStatus];
  [self refreshTuner];
  [self midiTick];
  if (_engine->isRunning())
  {
    _statusLabel.stringValue =
        [NSString stringWithFormat:@"Running @ %.0f Hz · blocks %llu · xruns %llu/%llu", _engine->sampleRate(),
                                   _engine->blocks(), _engine->underruns(), _engine->overruns()];
    _transportButton.title = @"Stop audio";
  }
  else
  {
    _statusLabel.stringValue = @"Stopped — choose NAM/IR, then Start";
    _transportButton.title = @"Start audio";
  }
}

- (void)paramChanged:(NSSlider*)sender
{
  const double v = sender.doubleValue;
  switch (sender.tag)
  {
  case kTagInputTrim:
    _engine->rig().setInputTrimDb((float)v);
    break;
  case kTagGateThresh:
    _engine->rig().setGateThresholdDb((float)v);
    break;
  case kTagGateRel:
    _engine->rig().setGateReleaseMs((float)v);
    break;
  case kTagTight:
    _engine->rig().setTight((float)v);
    break;
  case kTagDrive:
    _engine->rig().setDrive((float)v);
    break;
  case kTagBite:
    _engine->rig().setBite((float)v);
    break;
  case kTagWeight:
    _engine->rig().setWeight((float)v);
    break;
  case kTagContour:
    _engine->rig().setContour((float)v);
    break;
  case kTagPresence:
    _engine->rig().setPresence((float)v);
    break;
  case kTagDelayTime:
    _engine->rig().setDelayTimeMs((float)v);
    break;
  case kTagDelayFb:
    _engine->rig().setDelayFeedback((float)v);
    break;
  case kTagDelayMix:
    _engine->rig().setDelayMix((float)v);
    break;
  case kTagReverbDecay:
    _engine->rig().setReverbDecay((float)v);
    break;
  case kTagReverbMix:
    _engine->rig().setReverbMix((float)v);
    break;
  case kTagOutTrim:
    _engine->rig().setOutputTrimDb((float)v);
    break;
  case kTagShift:
    _stage.setShiftSt((float)v); // shared live shift, both engines follow
    break;
  case kTagT3kTonality:
    _stage.setT3kTonalityHz((float)v); // live reference control
    break;
  case kTagSlamAmount:
    _slam.setAmount01((float)(v / 100.0)); // live macro 0..100%
    break;
  case kTagAlienMix:
    _alien.setMix((float)(v / 100.0)); // live lab controls
    break;
  case kTagAlienShiftHz:
    _alien.setShiftHz((float)v);
    break;
  case kTagAlienSweepMs:
    _alien.setSweepMs((float)v);
    break;
  case kTagAlienDepthMs:
    _alien.setDepthMs((float)v);
    break;
  case kTagAlienWarpTime:
    _alien.setWarpTimeMs((float)v);
    break;
  case kTagAlienWarpHz:
    _alien.setWarpShiftHz((float)v);
    break;
  case kTagAlienWarpSt:
    _alien.setWarpSt((float)v);
    break;
  case kTagAlienWarpFb:
    _alien.setWarpFb((float)v);
    break;
  case kTagSpecExpr:
    _spec.setExpression((float)(v / 100.0)); // live lab controls
    break;
  case kTagSpecMix:
    _spec.setMix((float)(v / 100.0));
    break;
  case kTagSpecDepth:
    _spec.setExprDepth((float)v); // live (engine clamps to 0..12)
    break;
  case kTagGestDepth: // staged gesture edits (Save commits the slot)
    _specGestStage[_specGestSlot].depthSt = (float)v;
    break;
  case kTagGestTime:
    _specGestStage[_specGestSlot].timeMs = (float)v;
    break;
  case kTagGestRet:
    _specGestStage[_specGestSlot].retMs = (float)v;
    break;
  default:
    break;
  }
  [self refreshValueLabels:_engine->rig().params()];
}

// Transpose actions: every setter is a lock-free atomic store applied at the
// next audio block boundary — the same handoff the rig sliders use. Engine
// switches keep the stage crossfade + warm standby, so A/B is one click.
- (void)transposeToggled:(NSButton*)sender
{
  _stage.setEnabled(sender.state == NSControlStateValueOn);
  [self refreshTransposeStatus];
}

- (void)selectEngine:(tdm::lab::DevTranspose::Engine)engine
{
  _stage.setEngine(engine);
  _engineSeg.selectedSegment = (engine == tdm::lab::DevTranspose::Engine::Tone3000) ? 1 : 0;
  [self refreshTransposeStatus];
}

- (void)engineSelected:(NSSegmentedControl*)sender
{
  [self selectEngine:(sender.selectedSegment == 1) ? tdm::lab::DevTranspose::Engine::Tone3000
                                                  : tdm::lab::DevTranspose::Engine::Gt2];
}

- (void)abPressed:(id)sender
{
  (void)sender;
  [self selectEngine:(_stage.engine() == tdm::lab::DevTranspose::Engine::Tone3000)
                  ? tdm::lab::DevTranspose::Engine::Gt2
                  : tdm::lab::DevTranspose::Engine::Tone3000];
}

// Integer stepper: rounds the current shift, then steps exactly ±1 st.
- (void)shiftStepped:(NSStepper*)sender
{
  const double cur = sender.doubleValue;
  const double dir = (cur > _lastStepper) ? 1.0 : (cur < _lastStepper) ? -1.0 : 0.0;
  _lastStepper = cur;
  if (std::fabs(cur) > 500.0) // re-park far from the ±1000 rails (invisible)
  {
    sender.doubleValue = 0.0;
    _lastStepper = 0.0;
  }
  if (dir == 0.0)
    return;
  const double target =
      std::clamp(std::round((double)_stage.shiftSt()) + dir,
                 (double)tdm::lab::DevTranspose::kMinShiftSt, (double)tdm::lab::DevTranspose::kMaxShiftSt);
  _stage.setShiftSt((float)target);
  _sliders[@(kTagShift)].doubleValue = target;
  [self refreshValueLabels:_engine->rig().params()];
}

- (void)gt2Disclosure:(id)sender
{
  (void)sender;
  _gt2Open = !_gt2Open;
  if (_gt2Open)
    _t3kOpen = NO; // accordion: only one large panel at a time
  [self layoutAdvanced];
}

- (void)t3kDisclosure:(id)sender
{
  (void)sender;
  _t3kOpen = !_t3kOpen;
  if (_t3kOpen)
    _gt2Open = NO; // accordion: only one large panel at a time
  [self layoutAdvanced];
}

// GT2 advanced edit (fires on release): per-field range check via the shared
// adapter, then an eager whole-config trial on the main thread. Invalid
// cross-field combos are rejected before they reach stored state; accepted
// edits apply on the next Start (geometry derives at reset, like NAM/IR).
- (void)gt2AdvancedChanged:(NSSlider*)sender
{
  const int field = (int)sender.tag - kGt2AdvTagBase;
  tdm::GuitarTranspose::Config candidate = _stage.gt2Config();
  if (!tdm::lab::devGt2UiSet(candidate, field, sender.doubleValue))
  {
    sender.doubleValue = tdm::lab::devGt2UiGet(_stage.gt2Config(), field);
    return; // unreachable: slider range == descriptor range, but stay safe
  }
  try
  {
    _stage.configureGt2(candidate);
  }
  catch (const std::exception&)
  {
    sender.doubleValue = tdm::lab::devGt2UiGet(_stage.gt2Config(), field);
    _gt2HintLabel.stringValue = @"Invalid combination (e.g. fade max < fade min) — reverted.";
    [self refreshGt2Rows];
    return;
  }
  [self refreshGt2Rows];
  [self refreshTransposeStatus];
}

- (void)resyncToggled:(NSButton*)sender
{
  tdm::GuitarTranspose::Config candidate = _stage.gt2Config();
  candidate.enableResync = (sender.state == NSControlStateValueOn);
  try
  {
    _stage.configureGt2(candidate);
  }
  catch (const std::exception&)
  {
    sender.state = _stage.gt2Config().enableResync ? NSControlStateValueOn : NSControlStateValueOff;
    return;
  }
  [self refreshGt2Rows];
  [self refreshTransposeStatus];
}

- (void)gt2Restore:(id)sender
{
  (void)sender;
  _stage.resetGt2ToBaseline();
  const tdm::GuitarTranspose::Config& cfg = _stage.gt2Config();
  for (int i = 0; i < tdm::lab::devGt2UiFieldCount(); ++i)
    _gt2Sliders[@(i)].doubleValue = tdm::lab::devGt2UiGet(cfg, i);
  [self refreshGt2Rows];
  [self refreshTransposeStatus];
}

- (void)t3kWindowSelected:(NSSegmentedControl*)sender
{
  const int ms = (sender.selectedSegment == 0) ? 20
      : (sender.selectedSegment == 2)          ? 40
      : (sender.selectedSegment == 3)          ? 60
                                               : 30;
  _stage.setT3kWindowMs(ms); // live: engine-documented window switch
  [self syncT3kWindowSeg];
}

- (void)t3kRestore:(id)sender
{
  (void)sender;
  _stage.setT3kWindowMs(tdm::lab::DevTranspose::kDefaultT3kWindowMs);
  _stage.setT3kTonalityHz(tdm::lab::DevTranspose::kDefaultT3kTonalityHz);
  [self syncT3kWindowSeg];
  _sliders[@(kTagT3kTonality)].doubleValue = _stage.t3kTonalityHz();
  [self refreshValueLabels:_engine->rig().params()];
}

- (void)gateToggled:(NSButton*)sender
{
  _engine->rig().setGateEnabled(sender.state == NSControlStateValueOn);
}

- (void)tunerToggled:(NSButton*)sender
{
  _engine->rig().setTunerEnabled(sender.state == NSControlStateValueOn);
}

// SLAM actions: lock-free atomic stores adopted at the next audio block,
// exactly like the rig handoff. Flavour/enable changes dip through dry.
- (void)slamToggled:(NSButton*)sender
{
  _slam.setEnabled(sender.state == NSControlStateValueOn);
}

- (void)slamFlavorSelected:(NSSegmentedControl*)sender
{
  using Flavor = tdm::lab::DevSlam::Flavor;
  const NSInteger seg = sender.selectedSegment;
  _slam.setFlavor(seg == 1 ? Flavor::Crush : seg == 2 ? Flavor::Mass : Flavor::Push);
}

// Alien FX actions: lock-free atomic stores adopted at the next audio
// block, exactly like the rig handoff. Effect/enable changes dip through
// dry; the trigger fires one laser sweep (Down/Up) on demand.
- (void)alienToggled:(NSButton*)sender
{
  _alien.setEnabled(sender.state == NSControlStateValueOn);
}

- (void)alienEffectSelected:(NSSegmentedControl*)sender
{
  using Effect = tdm::alien::DevAlien::Effect;
  const NSInteger seg = sender.selectedSegment;
  _alien.setEffect(seg == 1 ? Effect::Laser : seg == 2 ? Effect::Warp : Effect::Frequency);
}

- (void)alienDirSelected:(NSSegmentedControl*)sender
{
  _alien.setLaserDir((int)sender.selectedSegment); // 0 Down, 1 Up, 2 Cycle
}

- (void)alienWarpModeSelected:(NSSegmentedControl*)sender
{
  _alien.setWarpMode((int)sender.selectedSegment); // 0 FreqWarp, 1 PitchWarp
}

- (void)alienTrigger:(NSButton*)sender
{
  (void)sender;
  _alien.triggerLaser();
}

// Creative Pitch + gesture actions: lock-free atomic stores adopted at
// the next audio block, exactly like the rig handoff. TRIGGER is
// momentary (down/up pair) and commits staged edits before firing.
- (void)specToggled:(NSButton*)sender
{
  _spec.setExpressionEnabled(sender.state == NSControlStateValueOn);
}

- (void)specGestToggled:(NSButton*)sender
{
  _spec.setGesturesEnabled(sender.state == NSControlStateValueOn);
}

- (void)specVoiceSelected:(NSSegmentedControl*)sender
{
  using Voice = tdm::specfx::DevSpecFx::Voice;
  const NSInteger seg = sender.selectedSegment;
  _spec.setVoice(seg == 1   ? Voice::Dual
                 : seg == 2 ? Voice::DualWham
                 : seg == 3 ? Voice::Ring
                            : Voice::Whammy);
  [self refreshSpecGating];
}

- (void)specDirSelected:(NSSegmentedControl*)sender
{
  _spec.setExprDirection((sender.selectedSegment == 1) ? tdm::specfx::GestureDir::Down
                                                      : tdm::specfx::GestureDir::Up);
}

// Per-voice control gating (single source of truth in SpecGesture.h):
// Direction applies to Whammy expression only; Depth to every pitched
// voice (Ring's map is fixed). Disabled — never a silent no-op dial.
- (void)refreshSpecGating
{
  const int v = static_cast<int>(_spec.voice());
  _specDirSeg.enabled = tdm::specfx::exprVoiceUsesDir(v) ? YES : NO;
  _sliders[@(kTagSpecDepth)].enabled = tdm::specfx::exprVoiceUsesDepth(v) ? YES : NO;
}

- (void)specInsertSelected:(NSSegmentedControl*)sender
{
  using Insert = tdm::specfx::DevSpecFx::Insert;
  _spec.setInsert(sender.selectedSegment == 1 ? Insert::Post : Insert::Pre);
}

// Sync every gesture field to the displayed slot's staging (popup, name,
// segments, sliders + per-slot double-click resets, labels).
- (void)refreshGestureUI
{
  const int s = _specGestSlot;
  const tdm::specfx::GestureConfig& c = _specGestStage[s];
  [_specGestPopup selectItemAtIndex:s];
  _specGestName.stringValue = _specGestNames[s];
  _specGestEffectSeg.selectedSegment =
      (c.voice == 1) ? 1 : (c.voice == 3) ? 2 : (c.voice == 2) ? 3 : 0;
  _specGestDirSeg.selectedSegment = (c.dir == tdm::specfx::GestureDir::Down) ? 1 : 0;
  // Direction only steers Whammy (polarity) and Ring (sweep direction);
  // Dual/DualWham have fixed spreads, so the dial parks disabled.
  _specGestDirSeg.enabled = tdm::specfx::gestureVoiceUsesDir(c.voice) ? YES : NO;
  _specGestMotionSeg.selectedSegment =
      (c.motion == tdm::specfx::GestureMotion::FromDepth) ? 1 : 0;
  _sliders[@(kTagGestDepth)].doubleValue = c.depthSt;
  _sliders[@(kTagGestTime)].doubleValue = c.timeMs;
  _sliders[@(kTagGestRet)].doubleValue = c.retMs;
  const tdm::specfx::GestureConfig fc = tdm::specfx::defaultGestureConfig(s);
  ((TdmResetSlider*)_sliders[@(kTagGestDepth)]).resetValue = fc.depthSt;
  ((TdmResetSlider*)_sliders[@(kTagGestTime)]).resetValue = fc.timeMs;
  ((TdmResetSlider*)_sliders[@(kTagGestRet)]).resetValue = fc.retMs;
  [self refreshValueLabels:_engine->rig().params()];
}

- (void)specGestPopupSelected:(NSPopUpButton*)sender
{
  _specGestSlot = (int)sender.indexOfSelectedItem;
  [self refreshGestureUI];
}

- (void)specGestEffectSelected:(NSSegmentedControl*)sender
{
  const NSInteger seg = sender.selectedSegment;
  _specGestStage[_specGestSlot].voice =
      (seg == 1) ? 1 : (seg == 2) ? 3 : (seg == 3) ? 2 : 0; // Dual/DualWham/Ring/Whammy
  _specGestDirSeg.enabled =
      tdm::specfx::gestureVoiceUsesDir(_specGestStage[_specGestSlot].voice) ? YES : NO;
}

- (void)specGestDirSelected:(NSSegmentedControl*)sender
{
  _specGestStage[_specGestSlot].dir =
      (sender.selectedSegment == 1) ? tdm::specfx::GestureDir::Down : tdm::specfx::GestureDir::Up;
}

- (void)specGestMotionSelected:(NSSegmentedControl*)sender
{
  _specGestStage[_specGestSlot].motion = (sender.selectedSegment == 1)
      ? tdm::specfx::GestureMotion::FromDepth
      : tdm::specfx::GestureMotion::ToDepth;
}

// Commit the displayed slot's staging (fields + name) to the router.
- (void)specGestSave:(id)sender
{
  (void)sender;
  const int s = _specGestSlot;
  const tdm::specfx::GestureConfig& c = _specGestStage[s];
  _specGestNames[s] = _specGestName.stringValue;
  [_specGestPopup itemAtIndex:s]
      .title = [NSString stringWithFormat:@"%d: %@", s + 1, _specGestNames[s]];
  _spec.setSlotVoice(s, static_cast<tdm::specfx::DevSpecFx::Voice>(c.voice));
  _spec.setSlotDir(s, c.dir);
  _spec.setSlotMotion(s, c.motion);
  _spec.setSlotDepthSt(s, c.depthSt);
  _spec.setSlotTimeMs(s, c.timeMs);
  _spec.setSlotRetMs(s, c.retMs);
}

// Restore the displayed slot (router + staging + fields) to factory.
- (void)specGestReset:(id)sender
{
  (void)sender;
  const int s = _specGestSlot;
  _spec.resetSlot(s);
  _specGestStage[s] = tdm::specfx::defaultGestureConfig(s);
  _specGestNames[s] = [NSString stringWithUTF8String:tdm::specfx::defaultGestureName(s)];
  [_specGestPopup itemAtIndex:s]
      .title = [NSString stringWithFormat:@"%d: %@", s + 1, _specGestNames[s]];
  [self refreshGestureUI];
}

- (void)specGestDown:(id)sender
{
  (void)sender;
  [self specGestSave:nil]; // trigger what the fields show
  _spec.pressSlot(_specGestSlot);
}

- (void)specGestUp:(id)sender
{
  (void)sender;
  _spec.releaseSlot(_specGestSlot);
}

- (void)specGestOneShot:(id)sender
{
  (void)sender;
  [self specGestSave:nil];
  _spec.oneShotSlot(_specGestSlot);
}

// Restore persisted MIDI state at startup (main thread, pre-UI):
// mappings -> table, slots -> router + staging + names, source UIDs
// -> connection intents (connect if present now, else on hot-plug).
- (void)midiApplyPersisted
{
  tdm::midi::PersistedState st;
  std::string err;
  if (!tdm::midi::loadFile(tdm::midi::defaultPath(), st, err))
    _midiPersistMsg = err + " (factory)";
  for (const tdm::midi::Mapping& m : st.mappings)
    _midiTable.addOrReplace(m);
  for (int i = 0; i < tdm::specfx::kNumGestureSlots; ++i)
  {
    _specGestStage[i] = st.slots[i].cfg;
    _specGestNames[i] = [NSString stringWithUTF8String:st.slots[i].name.c_str()];
    const tdm::specfx::GestureConfig& c = st.slots[i].cfg;
    _spec.setSlotVoice(i, static_cast<tdm::specfx::DevSpecFx::Voice>(c.voice));
    _spec.setSlotDir(i, c.dir);
    _spec.setSlotMotion(i, c.motion);
    _spec.setSlotDepthSt(i, c.depthSt);
    _spec.setSlotTimeMs(i, c.timeMs);
    _spec.setSlotRetMs(i, c.retMs);
  }
  for (uint32_t uid : st.midiSources)
    _midi.setConnected(uid, true);
  _midiOmniRestore = st.omniLearn;
}

// Rebuild the source popup + status line (main thread only).
- (void)refreshMidiSources
{
  [_midiSrcPopup removeAllItems];
  _midiSrcUids.clear();
  _midiSrcNames.clear();
  _midiSrcConn.clear();
  if (!_midi.available())
  {
    [_midiSrcPopup addItemWithTitle:@"MIDI unavailable"];
    [[_midiSrcPopup itemAtIndex:0] setEnabled:NO];
    _midiStatus.stringValue = @"No MIDI server (sandboxed shell?)";
    return;
  }
  const std::vector<tdm::midi::MidiInput::Source> srcs = _midi.refreshSources();
  if (srcs.empty())
  {
    [_midiSrcPopup addItemWithTitle:@"No MIDI sources"];
    [[_midiSrcPopup itemAtIndex:0] setEnabled:NO];
  }
  int nConn = 0;
  std::string firstConn;
  for (const auto& s : srcs)
  {
    _midiSrcUids.push_back(s.uid);
    _midiSrcNames.push_back(s.name);
    _midiSrcConn.push_back(s.connected ? 1 : 0);
    NSString* title = [NSString stringWithFormat:@"%s%s", s.connected ? "✓ " : "",
                                                 s.name.c_str()];
    [_midiSrcPopup addItemWithTitle:title];
    if (s.connected)
    {
      ++nConn;
      if (firstConn.empty())
        firstConn = s.name;
    }
  }
  if (!srcs.empty())
    [_midiSrcPopup selectItemAtIndex:0];
  NSMutableString* st = [NSMutableString string];
  if (nConn == 0)
    [st appendString:@"Not connected"];
  else if (nConn == 1)
    [st appendFormat:@"Connected: %s", firstConn.c_str()];
  else
    [st appendFormat:@"%d connected", nConn];
  if (!_midiPersistMsg.empty())
    [st appendFormat:@" · MIDI file: %s", _midiPersistMsg.c_str()];
  _midiStatus.stringValue = st;
}

// Sync every mapping row to the table + Learn/cal state (main thread).
- (void)refreshMidiRows
{
  for (int t = 0; t < (int)tdm::midi::Target::Count; ++t)
  {
    const tdm::midi::Target tgt = (tdm::midi::Target)t;
    tdm::midi::Mapping m;
    if (_midiTable.findTarget(tgt, m))
    {
      _midiAssign[t].stringValue = midiTriggerText(m.trigger);
      _midiModeSeg[t].selectedSegment = midiRowSeg(t, m);
    }
    else
      _midiAssign[t].stringValue = @"—"; // mode seg keeps pending default
    if (_midiCalActive && _midiCalTarget == t)
      _midiLearnBtn[t].title = @"Move pedal…";
    else if (_midiLearnTarget == t)
      _midiLearnBtn[t].title = @"Learning…";
    else
      _midiLearnBtn[t].title = @"Learn";
  }
}

- (void)midiSrcSelected:(NSPopUpButton*)sender
{
  const NSInteger idx = sender.indexOfSelectedItem;
  if (idx < 0 || idx >= (NSInteger)_midiSrcUids.size())
    return;
  const uint32_t uid = _midiSrcUids[(size_t)idx];
  _midi.setConnected(uid, _midiSrcConn[(size_t)idx] == 0); // toggle
  [self refreshMidiSources];
}

- (void)midiRefresh:(id)sender
{
  (void)sender;
  [self refreshMidiSources];
}

- (void)midiOmniToggled:(NSButton*)sender
{
  (void)sender; // read at arm time; persisted on Save
}

- (void)midiLearnBtn:(NSButton*)sender
{
  const int t = (int)sender.tag;
  if (_midiCalActive) // second press finishes pedal calibration early
  {
    [self midiFinishCal];
    return;
  }
  if (_midiLearnTarget == t) // toggle off
  {
    _midiLearn.cancel();
    _midiLearnTarget = -1;
    [self refreshMidiRows];
    return;
  }
  _midiLearn.arm((tdm::midi::Target)t, _midiOmniCheck.state == NSControlStateValueOn);
  _midiLearnTarget = t;
  [self refreshMidiRows];
}

- (void)midiModeSeg:(NSSegmentedControl*)sender
{
  const int t = (int)sender.tag - 100;
  const tdm::midi::Target tgt = (tdm::midi::Target)t;
  tdm::midi::Mapping m;
  if (!_midiTable.findTarget(tgt, m))
    return; // no mapping: seg is the pending default for next Learn
  if (t <= 1)
    m.invert = (sender.selectedSegment == 1);
  else
  {
    const tdm::midi::Action a = midiRowAction(t, sender.selectedSegment);
    if (a == tdm::midi::Action::Momentary && !tdm::midi::MappingTable::triggerHasRelease(m.trigger))
    {
      _midiSaveLabel.stringValue = @"PC has no release — use 1-Shot/Toggle";
      [self refreshMidiRows]; // revert the seg to the stored action
      return;
    }
    m.action = a;
  }
  _midiTable.addOrReplace(m); // also clears edge/toggle memory
  [self refreshMidiRows];
}

- (void)midiClearRow:(NSButton*)sender
{
  const int t = (int)sender.tag - 200;
  if (_midiLearnTarget == t)
  {
    _midiLearn.cancel();
    _midiLearnTarget = -1;
  }
  if (_midiCalActive && _midiCalTarget == t)
    _midiCalActive = false;
  _midiTable.removeTarget((tdm::midi::Target)t);
  [self refreshMidiRows];
}

- (void)midiClearAll:(id)sender
{
  (void)sender;
  _midiLearn.cancel();
  _midiLearnTarget = -1;
  _midiCalActive = false;
  _midiTable.clear();
  [self refreshMidiRows];
}

// Commit every staged gesture slot (displayed first, incl. its name
// field), then persist mappings + slots + sources + OMNI (main thread;
// file IO never touches audio/MIDI threads).
- (void)midiSave:(id)sender
{
  (void)sender;
  [self specGestSave:nil];
  for (int i = 0; i < tdm::specfx::kNumGestureSlots; ++i)
  {
    if (i == _specGestSlot)
      continue;
    const tdm::specfx::GestureConfig& c = _specGestStage[i];
    _spec.setSlotVoice(i, static_cast<tdm::specfx::DevSpecFx::Voice>(c.voice));
    _spec.setSlotDir(i, c.dir);
    _spec.setSlotMotion(i, c.motion);
    _spec.setSlotDepthSt(i, c.depthSt);
    _spec.setSlotTimeMs(i, c.timeMs);
    _spec.setSlotRetMs(i, c.retMs);
  }
  tdm::midi::PersistedState st;
  st.omniLearn = _midiOmniCheck.state == NSControlStateValueOn;
  st.midiSources = _midi.wantedSources();
  st.mappings = _midiTable.snapshot();
  for (int i = 0; i < tdm::specfx::kNumGestureSlots; ++i)
  {
    st.slots[i].name = _specGestNames[i].UTF8String;
    tdm::specfx::GestureConfig c;
    c.voice = static_cast<int>(_spec.slotVoice(i));
    c.dir = _spec.slotDir(i);
    c.motion = _spec.slotMotion(i);
    c.depthSt = _spec.slotDepthSt(i);
    c.timeMs = _spec.slotTimeMs(i);
    c.retMs = _spec.slotRetMs(i);
    st.slots[i].cfg = c;
  }
  std::string err;
  if (tdm::midi::saveFile(tdm::midi::defaultPath(), st, err))
  {
    NSDateFormatter* fmt = [[NSDateFormatter alloc] init];
    fmt.dateFormat = @"HH:mm:ss";
    _midiSaveLabel.stringValue = [NSString stringWithFormat:@"Saved %@", [fmt stringFromDate:[NSDate date]]];
  }
  else
    _midiSaveLabel.stringValue = [NSString stringWithFormat:@"Save failed: %s", err.c_str()];
}

// Commit a Learned trigger (main thread, from midiTick). Continuous
// targets need a CC pedal: anything else re-arms with a hint, and a
// CC opens the 2.5 s pedal-calibration window instead of committing.
- (void)midiCommitLearned:(const tdm::midi::Trigger&)cap
{
  const int t = _midiLearnTarget;
  _midiLearnTarget = -1;
  if (t <= 1)
  {
    if (cap.type != tdm::midi::MsgType::Cc)
    {
      _midiLearn.arm((tdm::midi::Target)t, _midiOmniCheck.state == NSControlStateValueOn);
      _midiLearnTarget = t; // stay armed: Expression/Mix need a CC pedal
      _midiLearnBtn[t].title = @"Need CC…";
      return;
    }
    _midiCalActive = true;
    _midiCalTarget = t;
    _midiCalTrigger = cap;
    _midiCalMin = 127;
    _midiCalMax = 0;
    _midiCalUntil = CFAbsoluteTimeGetCurrent() + 2.5;
    _midiCalLastCount = _midi.latest().count;
    [self refreshMidiRows];
    return;
  }
  tdm::midi::Action a = midiRowAction(t, _midiModeSeg[t].selectedSegment);
  if (a == tdm::midi::Action::Momentary && !tdm::midi::MappingTable::triggerHasRelease(cap))
    a = (t <= 3) ? tdm::midi::Action::Toggle : tdm::midi::Action::OneShot; // PC: no release
  tdm::midi::Mapping m;
  m.trigger = cap;
  m.target = (tdm::midi::Target)t;
  m.action = a;
  _midiTable.addOrReplace(m);
  [self refreshMidiRows];
}

// Finish pedal calibration: observed CC window becomes the mapping's
// input range (narrow/empty sweeps fall back to full 0..127).
- (void)midiFinishCal
{
  const int t = _midiCalTarget;
  _midiCalActive = false;
  _midiCalTarget = -1;
  tdm::midi::Mapping m;
  m.trigger = _midiCalTrigger;
  m.target = (tdm::midi::Target)t;
  m.action = tdm::midi::Action::Continuous;
  m.invert = (_midiModeSeg[t].selectedSegment == 1);
  if (_midiCalMax > _midiCalMin && _midiCalMax - _midiCalMin >= 16)
  {
    m.calMin = _midiCalMin;
    m.calMax = _midiCalMax;
  }
  _midiTable.addOrReplace(m);
  [self refreshMidiRows];
}

// MIDI housekeeping on the 10 Hz tick (main thread): hot-plug drain,
// monitor snapshot, Learn commit, calibration tracking, and MIDI->UI
// reflection (sliders/checkboxes follow the same DSP atomics the MIDI
// sink drives, so pedal moves visibly move the UI with no fighting).
- (void)midiTick
{
  if (_midi.needsRefresh())
    [self refreshMidiSources];
  const tdm::midi::MidiInput::Latest lat = _midi.latest();
  if (lat.count != _midiLastCount)
  {
    _midiLastCount = lat.count;
    _midiRecent.stringValue = midiLatestText(lat);
  }
  tdm::midi::Trigger cap;
  if (_midiLearnTarget >= 0 && _midiLearn.takeCapture(cap))
    [self midiCommitLearned:cap];
  else if (_midiCalActive)
  {
    if (lat.count != _midiCalLastCount && lat.type == tdm::midi::MsgType::Cc
        && lat.number == _midiCalTrigger.number
        && (_midiCalTrigger.sourceUid == 0 || lat.uid == _midiCalTrigger.sourceUid)
        && (_midiCalTrigger.channel < 0 || lat.channel == (uint8_t)_midiCalTrigger.channel))
    {
      _midiCalLastCount = lat.count;
      if (lat.value < _midiCalMin)
        _midiCalMin = lat.value;
      if (lat.value > _midiCalMax)
        _midiCalMax = lat.value;
    }
    if (CFAbsoluteTimeGetCurrent() >= _midiCalUntil)
      [self midiFinishCal];
  }
  _sliders[@(kTagSpecExpr)].doubleValue = _spec.expression() * 100.0;
  _sliders[@(kTagSpecMix)].doubleValue = _spec.mix01() * 100.0;
  _specCheck.state = _spec.isExpressionEnabled() ? NSControlStateValueOn : NSControlStateValueOff;
  _specGestCheck.state = _spec.isGesturesEnabled() ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)refreshTuner
{
  tdm::TunerResult raw;
  _engine->rig().tunerResult(raw); // lock-free snapshot
  _tunerDisplay.update(raw);
  const tdm::TunerDisplay::State& st = _tunerDisplay.state();
  if (!st.valid)
  {
    _tunerNote.stringValue = @"—";
    _tunerCents.stringValue = @"";
    _tunerHz.stringValue = @"";
    _tunerBar.valid = NO;
    [_tunerBar setNeedsDisplay:YES];
    return;
  }
  _tunerNote.stringValue = [NSString stringWithFormat:@"%s%d", st.name, st.octave];
  const double c = st.cents;
  _tunerCents.stringValue = [NSString stringWithFormat:@"%@%02.0f ¢", c < 0 ? @"−" : @"+", std::fabs(c)];
  _tunerHz.stringValue = [NSString stringWithFormat:@"%.2f Hz", st.frequencyHz];
  _tunerBar.cents = c;
  _tunerBar.valid = YES;
  [_tunerBar setNeedsDisplay:YES];
}

- (void)driveToggled:(NSButton*)sender
{
  _engine->rig().setDriveEnabled(sender.state == NSControlStateValueOn);
}

- (void)shapeToggled:(NSButton*)sender
{
  _engine->rig().setShapeEnabled(sender.state == NSControlStateValueOn);
}

- (void)delayToggled:(NSButton*)sender
{
  _engine->rig().setDelayEnabled(sender.state == NSControlStateValueOn);
}

- (void)reverbToggled:(NSButton*)sender
{
  _engine->rig().setReverbEnabled(sender.state == NSControlStateValueOn);
}

- (void)toggleAudio:(id)sender
{
  (void)sender;
  if (_engine->isRunning())
  {
    _engine->stop();
  }
  else
  {
    std::string error;
    if (!_engine->start(error))
    {
      [self alert:@"Could not start audio" info:[NSString stringWithUTF8String:error.c_str()]];
    }
    else
    {
      // Start adopts the stored GT2 config into the running engine (reset
      // path); snapshot it so the restart badge can compare.
      _gt2Applied = _stage.gt2Config();
    }
  }
  [self tick:nil];
}

- (BOOL)checkDocGeometry:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [") + msg + "]";
    return NO;
  };
  const CGFloat docW = _docView.frame.size.width;
  const CGFloat docH = _docView.frame.size.height;
  if (!(docW > 0.0 && docH > 0.0 && std::isfinite(docW) && std::isfinite(docH)))
    return fail("document has invalid size");
  // Visible doc-level controls: valid frames, inside the document, no overlaps.
  NSMutableArray<NSView*>* vis = [NSMutableArray array];
  for (NSView* v in _docView.subviews)
  {
    if (v.hidden)
      continue;
    const NSRect f = v.frame;
    if (!(f.size.width > 0.0 && f.size.height > 0.0 && std::isfinite(f.origin.x)
          && std::isfinite(f.origin.y)))
      return fail("control has invalid frame");
    if (f.origin.x < -0.5 || f.origin.y < -0.5 || NSMaxX(f) > docW + 0.5 || NSMaxY(f) > docH + 0.5)
      return fail("control outside document bounds");
    [vis addObject:v];
  }
  for (NSUInteger i = 0; i < vis.count; ++i)
    for (NSUInteger j = i + 1; j < vis.count; ++j)
      if (NSIntersectsRect(vis[i].frame, vis[j].frame))
        return fail("controls overlap");
  // Open panels: same checks in panel-local coords.
  for (NSView* panel in @[ _gt2Box, _t3kBox ])
  {
    if (panel.hidden)
      continue;
    NSMutableArray<NSView*>* pvis = [NSMutableArray array];
    for (NSView* v in panel.subviews)
    {
      if (v.hidden)
        continue;
      const NSRect f = v.frame;
      if (!(f.size.width > 0.0 && f.size.height > 0.0))
        return fail("panel control has invalid frame");
      if (f.origin.x < -0.5 || f.origin.y < -0.5 || NSMaxX(f) > panel.frame.size.width + 0.5
          || NSMaxY(f) > panel.frame.size.height + 0.5)
        return fail("panel control outside panel bounds");
      [pvis addObject:v];
    }
    for (NSUInteger i = 0; i < pvis.count; ++i)
      for (NSUInteger j = i + 1; j < pvis.count; ++j)
        if (NSIntersectsRect(pvis[i].frame, pvis[j].frame))
          return fail("panel controls overlap");
  }
  return YES;
}

- (BOOL)runTransposeProbe:(std::string*)detail
{
  using Stage = tdm::lab::DevTranspose;
  auto fail = [&](const char* msg) {
    *detail = std::string(" [") + msg + "]";
    return NO;
  };
  // Audition state + seam install.
  if (_engine->rig().transposeInsert() != &_stage)
    return fail("stage not installed in rig seam");
  if (!_stage.isEnabled() || _stage.engine() != Stage::Engine::Gt2 || _stage.shiftSt() != -2.0f)
    return fail("audition state != ON/GT2/-2");
  if (_transposeCheck.state != NSControlStateValueOn || _engineSeg.selectedSegment != 0)
    return fail("controls != audition state");
  // Segmented select + A/B flip preserve the crossfade path (one click each).
  _engineSeg.selectedSegment = 1;
  [self engineSelected:_engineSeg];
  if (_stage.engine() != Stage::Engine::Tone3000)
    return fail("segmented did not select T3K");
  [self abPressed:nil];
  if (_stage.engine() != Stage::Engine::Gt2 || _engineSeg.selectedSegment != 0)
    return fail("A/B did not flip back to GT2");
  // Shift slider drives shared shift; stepper lands exact integers.
  _sliders[@(kTagShift)].doubleValue = -7.0;
  [self paramChanged:_sliders[@(kTagShift)]];
  if (_stage.shiftSt() != -7.0f)
    return fail("shift slider did not reach stage");
  _shiftStepper.doubleValue = _lastStepper + 1.0;
  [self shiftStepped:_shiftStepper];
  if (_stage.shiftSt() != -6.0f)
    return fail("stepper did not land -6 exact");
  // Bypass preserves shift.
  _transposeCheck.state = NSControlStateValueOff;
  [self transposeToggled:_transposeCheck];
  if (_stage.isEnabled() || _stage.shiftSt() != -6.0f)
    return fail("bypass lost shift");
  _transposeCheck.state = NSControlStateValueOn;
  [self transposeToggled:_transposeCheck];
  // Scrollable disclosure geometry: the DOCUMENT grows, the window never does.
  const CGFloat driveY = _driveCheck.frame.origin.y;
  const CGFloat winH0 = _window.frame.size.height;
  const CGFloat docH0 = _docView.frame.size.height;
  if (_statusLabel.frame.origin.y > 24.0)
    return fail("content not top-aligned at launch");
  if (docH0 != _collapsedDocHeight || _advShown != 0.0)
    return fail("initial document geometry wrong");
  if (![self checkDocGeometry:detail])
    return NO;
  // GT2 open: doc grows by panel+gap, lowers shift DOWN (flipped), window fixed.
  [self gt2Disclosure:nil];
  if (!_gt2Open || _gt2Box.hidden)
    return fail("GT2 disclosure did not open");
  if (_advShown != 544.0 + 8.0)
    return fail("GT2 open height wrong");
  if (_docView.frame.size.height != docH0 + 544.0 + 8.0)
    return fail("document did not grow for GT2");
  if (_driveCheck.frame.origin.y != driveY + _advShown)
    return fail("lower views did not shift for GT2");
  if (_statusLabel.frame.origin.y > 24.0)
    return fail("top moved after GT2 open");
  if (_window.frame.size.height != winH0)
    return fail("window grew with GT2 disclosure");
  if (![self checkDocGeometry:detail])
    return NO;
  // Accordion: opening T3K closes GT2.
  [self t3kDisclosure:nil];
  if (!_t3kOpen || _t3kBox.hidden || _gt2Open || !_gt2Box.hidden)
    return fail("accordion did not swap to T3K");
  if (_advShown != 116.0 + 8.0 || _docView.frame.size.height != docH0 + 116.0 + 8.0)
    return fail("T3K open height wrong");
  if (_window.frame.size.height != winH0)
    return fail("window grew with T3K disclosure");
  if (![self checkDocGeometry:detail])
    return NO;
  // Accordion the other way: GT2 reopens, T3K closes.
  [self gt2Disclosure:nil];
  if (!_gt2Open || _t3kOpen || _advShown != 544.0 + 8.0)
    return fail("accordion did not swap back to GT2");
  if (_window.frame.size.height != winH0)
    return fail("window grew swapping panels");
  // Scroll reachability: with GT2 open the document exceeds the viewport;
  // Output Trim scrolls into view, and the MIDI section (now last) is
  // reachable at the bottom.
  {
    NSClipView* clip = _scrollView.contentView;
    const CGFloat clipH = clip.bounds.size.height;
    const CGFloat docH = _docView.frame.size.height;
    if (!(docH > clipH))
      return fail("no scroll range when expanded");
    const NSRect outF = _sliders[@(kTagOutTrim)].frame;
    if (NSMaxY(outF) > docH + 0.5)
      return fail("output trim outside document");
    const CGFloat wantY = NSMinY(outF) - 40.0; // bring Output Trim on screen
    [clip scrollToPoint:NSMakePoint(0.0, MAX(0.0, MIN(wantY, docH - clipH)))];
    if (!NSIntersectsRect(clip.bounds, outF))
      return fail("output trim not reachable by scroll");
    [clip scrollToPoint:NSMakePoint(0.0, docH - clipH)];
    if (!NSIntersectsRect(clip.bounds, _midiSaveLabel.frame))
      return fail("MIDI section not reachable by scroll");
    [clip scrollToPoint:NSMakePoint(0.0, 0.0)]; // park back at the top
  }
  if (![self checkDocGeometry:detail])
    return NO;
  // Collapse: geometry restores exactly, window untouched.
  [self gt2Disclosure:nil];
  if (_gt2Open || _t3kOpen || _advShown != 0.0)
    return fail("disclosures did not close");
  if (_driveCheck.frame.origin.y != driveY || _docView.frame.size.height != docH0)
    return fail("layout did not restore after close");
  if (_window.frame.size.height != winH0)
    return fail("window changed across disclosures");
  // Window stays within the visible screen (when a screen is present).
  NSScreen* scr = [NSScreen mainScreen];
  if (scr != nil && _window.frame.size.height > scr.visibleFrame.size.height + 1.0)
    return fail("window exceeds visible screen");
  if (![self checkDocGeometry:detail])
    return NO;
  // GT2 advanced edit reaches stored config; baseline badge flips.
  NSSlider* winSlider = _gt2Sliders[@(0)];
  winSlider.doubleValue = 40.0;
  [self gt2AdvancedChanged:winSlider];
  if (_stage.gt2Config().windowMs != 40.0)
    return fail("GT2 edit did not reach stored config");
  if (tdm::lab::devGt2UiIsBaseline(_stage.gt2Config()))
    return fail("CUSTOM state not detected");
  // Invalid cross-field combo reverts (fadeMin 40, then fadeMax 30).
  _gt2Sliders[@(3)].doubleValue = 40.0;
  [self gt2AdvancedChanged:_gt2Sliders[@(3)]];
  _gt2Sliders[@(4)].doubleValue = 30.0;
  [self gt2AdvancedChanged:_gt2Sliders[@(4)]];
  if (_stage.gt2Config().fadeMaxMs != 120.0)
    return fail("invalid combo was not rejected");
  // Restore returns every row + the badge to baseline.
  [self gt2Restore:nil];
  if (!tdm::lab::devGt2UiIsBaseline(_stage.gt2Config()))
    return fail("GT2 restore missed baseline");
  if (_gt2Sliders[@(0)].doubleValue != 30.0)
    return fail("GT2 restore did not sync rows");
  // T3K window select + restore.
  _t3kWindowSeg.selectedSegment = 3;
  [self t3kWindowSelected:_t3kWindowSeg];
  if (_stage.t3kWindowMs() != 60)
    return fail("T3K window did not reach stage");
  [self t3kRestore:nil];
  if (_stage.t3kWindowMs() != 30 || _stage.t3kTonalityHz() != 0.0f)
    return fail("T3K restore missed reference");
  return YES;
}

- (BOOL)runTunerProbe:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [tuner: ") + msg + "]";
    return NO;
  };
  // Audition default: checkbox on, rig param on.
  if (_tunerCheck.state != NSControlStateValueOn)
    return fail("checkbox not on");
  if (!_engine->rig().isTunerEnabled())
    return fail("rig param not on");
  // Drive the rig directly (no audio device): E2 sine must read E2.
  _engine->rig().reset(48000.0, 512);
  const int n = 512 * 188; // block-aligned: no ragged tail reads
  std::vector<float> in(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 82.41 * i / 48000.0);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (int off = 0; off < n; off += 512)
  {
    const float* bi[1] = {in.data() + off};
    float* bo[1] = {out.data() + off};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  tdm::TunerResult r;
  _engine->rig().tunerResult(r);
  if (!r.valid || r.midiNote != 40 || std::fabs(r.cents) > 2.0f)
    return fail("E2 not detected through rig");
  // Display path: two ticks confirm from blank, labels show E2.
  [self refreshTuner];
  [self refreshTuner];
  if (![_tunerNote.stringValue isEqualToString:@"E2"])
    return fail("note label did not show E2");
  if (!_tunerBar.valid)
    return fail("cents bar not valid");
  // Disable via the checkbox action: readout blanks promptly.
  _tunerCheck.state = NSControlStateValueOff;
  [self tunerToggled:_tunerCheck];
  if (_engine->rig().isTunerEnabled())
    return fail("disable did not reach rig");
  {
    // Adoption happens at the next block boundary (house param model).
    const float* bi[1] = {in.data()};
    float* bo[1] = {out.data()};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().tunerResult(r);
  if (r.valid)
    return fail("disable did not blank result");
  // Re-enable and confirm detection resumes.
  _tunerCheck.state = NSControlStateValueOn;
  [self tunerToggled:_tunerCheck];
  for (int off = 0; off < n; off += 512)
  {
    const float* bi[1] = {in.data() + off};
    float* bo[1] = {out.data() + off};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().tunerResult(r);
  if (!r.valid || r.midiNote != 40)
    return fail("re-enable did not resume");
  // Transparency through the real UI path: identical output on/off.
  std::vector<float> off(512 * 8, 0.0f), on(512 * 8, 0.0f);
  _tunerCheck.state = NSControlStateValueOff;
  [self tunerToggled:_tunerCheck];
  _engine->rig().reset(48000.0, 512); // identical start state, tuner off
  for (int b = 0; b < 8; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {off.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().reset(48000.0, 512);
  _tunerCheck.state = NSControlStateValueOn;
  [self tunerToggled:_tunerCheck];
  for (int b = 0; b < 8; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {on.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  if (off != on)
    return fail("tuner changed rig output");
  if (![self checkDocGeometry:detail])
    return NO;
  return YES;
}

- (BOOL)runSlamProbe:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [slam: ") + msg + "]";
    return NO;
  };
  using Flavor = tdm::lab::DevSlam::Flavor;
  // Defaults: checkbox off, Push, 100%, LED dark, seams installed.
  if (_slamCheck.state != NSControlStateValueOff)
    return fail("checkbox not off");
  if (_slam.isEnabled() || _slam.flavor() != Flavor::Push || _slam.amount01() != 1.0f)
    return fail("router defaults wrong");
  if (_slamSeg.selectedSegment != 0)
    return fail("segment not on Push");
  if (_engine->rig().slamPreDrive() == nullptr || _engine->rig().slamPostNam() == nullptr
      || _engine->rig().slamPostIr() == nullptr)
    return fail("seams not installed");
  // Drive the Amount slider through the real action.
  _sliders[@(kTagSlamAmount)].doubleValue = 65.0;
  [self paramChanged:_sliders[@(kTagSlamAmount)]];
  if (std::fabs(_slam.amount01() - 0.65f) > 1e-6f)
    return fail("amount did not reach router");
  [self refreshValueLabels:_engine->rig().params()];
  if (![_valueLabels[@(kTagSlamAmount)].stringValue isEqualToString:@"65%"])
    return fail("amount label wrong");
  _sliders[@(kTagSlamAmount)].doubleValue = 100.0;
  [self paramChanged:_sliders[@(kTagSlamAmount)]];
  // Flavour selector reaches the router for all three flavours.
  if ([_slamSeg segmentCount] != 3)
    return fail("selector is not Push/Crush/Mass");
  for (NSInteger seg = 0; seg < 3; ++seg)
  {
    _slamSeg.selectedSegment = seg;
    [self slamFlavorSelected:_slamSeg];
    const Flavor want = seg == 1 ? Flavor::Crush : seg == 2 ? Flavor::Mass : Flavor::Push;
    if (_slam.flavor() != want)
      return fail("flavour did not reach router");
  }
  // Transparency: installed-but-off renders bit-identical to no seams.
  _engine->rig().reset(48000.0, 512);
  const int n = 512 * 16; // block-aligned: no ragged tail reads
  std::vector<float> in(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 110.0 * i / 48000.0);
  std::vector<float> off(static_cast<size_t>(n), 0.0f), bare(static_cast<size_t>(n), 0.0f);
  _slamCheck.state = NSControlStateValueOff;
  [self slamToggled:_slamCheck];
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {off.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setSlamPreDrive(nullptr);
  _engine->rig().setSlamPostNam(nullptr);
  _engine->rig().setSlamPostIr(nullptr);
  _engine->rig().reset(48000.0, 512);
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {bare.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setSlamPreDrive(&_slam.preDrive());
  _engine->rig().setSlamPostNam(&_slam.postNam());
  _engine->rig().setSlamPostIr(&_slam.postIr());
  if (off != bare)
    return fail("installed-but-off changed rig output");
  // Attack signal for the flavour loop + telemetry (two decaying pips).
  const int na = 512 * 96;
  std::vector<float> atk(static_cast<size_t>(na), 0.0f);
  for (int p = 0; p < 2; ++p)
  {
    const int at = 24000 + p * 24000;
    for (int i = 0; i < 4800 && at + i < na; ++i)
      atk[static_cast<size_t>(at + i)] +=
          0.8f * std::sin(2.0 * 3.14159265358979 * 100.0 * i / 48000.0) * std::exp(-i / 960.0f);
  }
  // Enabled flavours actually render (each tap live) and rig params stay.
  const tdm::RigParams before = _engine->rig().params();
  _engine->rig().reset(48000.0, 512);
  _slamCheck.state = NSControlStateValueOff;
  [self slamToggled:_slamCheck];
  std::vector<float> atkDry(static_cast<size_t>(na), 0.0f);
  for (int b = 0; b < 96; ++b)
  {
    const float* bi[1] = {atk.data() + b * 512};
    float* bo[1] = {atkDry.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  for (NSInteger seg = 0; seg < 3; ++seg)
  {
    _slamSeg.selectedSegment = seg;
    [self slamFlavorSelected:_slamSeg];
    _slamCheck.state = NSControlStateValueOn;
    [self slamToggled:_slamCheck];
    std::vector<float> wet(static_cast<size_t>(na), 0.0f);
    for (int b = 0; b < 96; ++b)
    {
      const float* bi[1] = {atk.data() + b * 512};
      float* bo[1] = {wet.data() + b * 512};
      _engine->rig().processBlock(bi, 1, bo, 1, 512);
    }
    if (wet == atkDry)
      return fail("flavour rendered dry");
    bool finite = true;
    for (float v : wet)
      finite = finite && std::isfinite(v);
    if (!finite)
      return fail("flavour non-finite");
  }
  const tdm::RigParams after = _engine->rig().params();
  if (before.gateEnabled != after.gateEnabled || before.tight != after.tight
      || before.driveEnabled != after.driveEnabled || before.inputTrimDb != after.inputTrimDb)
    return fail("flavours touched rig params");
  // Back to defaults for a clean handoff.
  _slamCheck.state = NSControlStateValueOff;
  [self slamToggled:_slamCheck];
  _slamSeg.selectedSegment = 0;
  [self slamFlavorSelected:_slamSeg];
  if (![self checkDocGeometry:detail])
    return NO;
  return YES;
}

- (BOOL)runAlienProbe:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [alien: ") + msg + "]";
    return NO;
  };
  using Effect = tdm::alien::DevAlien::Effect;
  // Defaults: checkbox off, Frequency, lab starting points, seam installed.
  if (_alienCheck.state != NSControlStateValueOff)
    return fail("checkbox not off");
  if (_alien.isEnabled() || _alien.effect() != Effect::Frequency || _alien.mix01() != 1.0f)
    return fail("router defaults wrong");
  if (_alien.shiftHz() != 150.0f || _alien.laserDir() != 0 || _alien.sweepMs() != 120.0f
      || _alien.depthMs() != 12.0f)
    return fail("laser/shift defaults wrong");
  if (_alien.warpMode() != 0 || _alien.warpTimeMs() != 220.0f || _alien.warpFb() != 0.5f
      || _alien.warpShiftHz() != 100.0f || _alien.warpSt() != 3.0f)
    return fail("warp defaults wrong");
  if (_alienSeg.selectedSegment != 0 || _alienDirSeg.selectedSegment != 0
      || _alienWarpSeg.selectedSegment != 0)
    return fail("segments not on Frequency/Down/Freq");
  if (_engine->rig().alienPostIr() == nullptr)
    return fail("seam not installed");
  // Drive every slider through the real action; adoption + label.
  _sliders[@(kTagAlienMix)].doubleValue = 65.0;
  [self paramChanged:_sliders[@(kTagAlienMix)]];
  if (std::fabs(_alien.mix01() - 0.65f) > 1e-6f)
    return fail("mix did not reach router");
  [self refreshValueLabels:_engine->rig().params()];
  if (![_valueLabels[@(kTagAlienMix)].stringValue isEqualToString:@"65%"])
    return fail("mix label wrong");
  _sliders[@(kTagAlienMix)].doubleValue = 100.0;
  [self paramChanged:_sliders[@(kTagAlienMix)]];
  _sliders[@(kTagAlienShiftHz)].doubleValue = -75.0;
  [self paramChanged:_sliders[@(kTagAlienShiftHz)]];
  if (_alien.shiftHz() != -75.0f)
    return fail("shift did not reach router");
  _sliders[@(kTagAlienSweepMs)].doubleValue = 250.0;
  [self paramChanged:_sliders[@(kTagAlienSweepMs)]];
  if (_alien.sweepMs() != 250.0f)
    return fail("sweep did not reach router");
  _sliders[@(kTagAlienDepthMs)].doubleValue = 20.0;
  [self paramChanged:_sliders[@(kTagAlienDepthMs)]];
  if (_alien.depthMs() != 20.0f)
    return fail("depth did not reach router");
  _sliders[@(kTagAlienWarpTime)].doubleValue = 300.0;
  [self paramChanged:_sliders[@(kTagAlienWarpTime)]];
  if (_alien.warpTimeMs() != 300.0f)
    return fail("warp time did not reach router");
  _sliders[@(kTagAlienWarpHz)].doubleValue = -50.0;
  [self paramChanged:_sliders[@(kTagAlienWarpHz)]];
  if (_alien.warpShiftHz() != -50.0f)
    return fail("warp hz did not reach router");
  _sliders[@(kTagAlienWarpSt)].doubleValue = -4.0;
  [self paramChanged:_sliders[@(kTagAlienWarpSt)]];
  if (_alien.warpSt() != -4.0f)
    return fail("warp st did not reach router");
  _sliders[@(kTagAlienWarpFb)].doubleValue = 0.7;
  [self paramChanged:_sliders[@(kTagAlienWarpFb)]];
  if (std::fabs(_alien.warpFb() - 0.7f) > 1e-6f)
    return fail("warp fb did not reach router");
  // Restore lab starting points before rendering.
  _sliders[@(kTagAlienShiftHz)].doubleValue = 150.0;
  [self paramChanged:_sliders[@(kTagAlienShiftHz)]];
  _sliders[@(kTagAlienSweepMs)].doubleValue = 120.0;
  [self paramChanged:_sliders[@(kTagAlienSweepMs)]];
  _sliders[@(kTagAlienDepthMs)].doubleValue = 12.0;
  [self paramChanged:_sliders[@(kTagAlienDepthMs)]];
  _sliders[@(kTagAlienWarpTime)].doubleValue = 220.0;
  [self paramChanged:_sliders[@(kTagAlienWarpTime)]];
  _sliders[@(kTagAlienWarpHz)].doubleValue = 100.0;
  [self paramChanged:_sliders[@(kTagAlienWarpHz)]];
  _sliders[@(kTagAlienWarpSt)].doubleValue = 3.0;
  [self paramChanged:_sliders[@(kTagAlienWarpSt)]];
  _sliders[@(kTagAlienWarpFb)].doubleValue = 0.5;
  [self paramChanged:_sliders[@(kTagAlienWarpFb)]];
  // Selectors reach the router (effect + both sub-selectors).
  if ([_alienSeg segmentCount] != 3 || [_alienDirSeg segmentCount] != 3
      || [_alienWarpSeg segmentCount] != 2)
    return fail("selector segment counts wrong");
  for (NSInteger seg = 0; seg < 3; ++seg)
  {
    _alienSeg.selectedSegment = seg;
    [self alienEffectSelected:_alienSeg];
    const Effect want = seg == 1 ? Effect::Laser : seg == 2 ? Effect::Warp : Effect::Frequency;
    if (_alien.effect() != want)
      return fail("effect did not reach router");
  }
  for (NSInteger seg = 0; seg < 3; ++seg)
  {
    _alienDirSeg.selectedSegment = seg;
    [self alienDirSelected:_alienDirSeg];
    if (_alien.laserDir() != (int)seg)
      return fail("laser dir did not reach router");
  }
  for (NSInteger seg = 0; seg < 2; ++seg)
  {
    _alienWarpSeg.selectedSegment = seg;
    [self alienWarpModeSelected:_alienWarpSeg];
    if (_alien.warpMode() != (int)seg)
      return fail("warp mode did not reach router");
  }
  _alienSeg.selectedSegment = 0;
  [self alienEffectSelected:_alienSeg];
  _alienDirSeg.selectedSegment = 0;
  [self alienDirSelected:_alienDirSeg];
  _alienWarpSeg.selectedSegment = 0;
  [self alienWarpModeSelected:_alienWarpSeg];
  [self alienTrigger:nil]; // exercise the trigger path (Down chirp on enable)
  // Transparency: installed-but-off renders bit-identical to no seam.
  _engine->rig().reset(48000.0, 512);
  const int n = 512 * 16; // block-aligned: no ragged tail reads
  std::vector<float> in(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 110.0 * i / 48000.0);
  std::vector<float> off(static_cast<size_t>(n), 0.0f), bare(static_cast<size_t>(n), 0.0f);
  _alienCheck.state = NSControlStateValueOff;
  [self alienToggled:_alienCheck];
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {off.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setAlienPostIr(nullptr);
  _engine->rig().reset(48000.0, 512);
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {bare.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setAlienPostIr(&_alien);
  if (off != bare)
    return fail("installed-but-off changed rig output");
  // Enabled effects actually render and rig params stay.
  const int na = 512 * 96;
  std::vector<float> atk(static_cast<size_t>(na), 0.0f);
  for (int p = 0; p < 2; ++p)
  {
    const int at = 24000 + p * 24000;
    for (int i = 0; i < 4800 && at + i < na; ++i)
      atk[static_cast<size_t>(at + i)] +=
          0.8f * std::sin(2.0 * 3.14159265358979 * 100.0 * i / 48000.0) * std::exp(-i / 960.0f);
  }
  const tdm::RigParams before = _engine->rig().params();
  _engine->rig().reset(48000.0, 512);
  _alienCheck.state = NSControlStateValueOff;
  [self alienToggled:_alienCheck];
  std::vector<float> atkDry(static_cast<size_t>(na), 0.0f);
  for (int b = 0; b < 96; ++b)
  {
    const float* bi[1] = {atk.data() + b * 512};
    float* bo[1] = {atkDry.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  for (NSInteger seg = 0; seg < 3; ++seg)
  {
    _alienSeg.selectedSegment = seg;
    [self alienEffectSelected:_alienSeg];
    _alienCheck.state = NSControlStateValueOn;
    [self alienToggled:_alienCheck];
    std::vector<float> wet(static_cast<size_t>(na), 0.0f);
    for (int b = 0; b < 96; ++b)
    {
      const float* bi[1] = {atk.data() + b * 512};
      float* bo[1] = {wet.data() + b * 512};
      _engine->rig().processBlock(bi, 1, bo, 1, 512);
      if (seg == 1 && b == 0)
        [self alienTrigger:nil]; // Down chirp after enable settles, so it sounds
    }
    if (wet == atkDry)
      return fail("effect rendered dry");
    bool finite = true;
    for (float v : wet)
      finite = finite && std::isfinite(v);
    if (!finite)
      return fail("effect non-finite");
  }
  const tdm::RigParams after = _engine->rig().params();
  if (before.gateEnabled != after.gateEnabled || before.tight != after.tight
      || before.driveEnabled != after.driveEnabled || before.inputTrimDb != after.inputTrimDb)
    return fail("effects touched rig params");
  // Back to defaults for a clean handoff.
  _alienCheck.state = NSControlStateValueOff;
  [self alienToggled:_alienCheck];
  _alienSeg.selectedSegment = 0;
  [self alienEffectSelected:_alienSeg];
  if (![self checkDocGeometry:detail])
    return NO;
  return YES;
}

- (BOOL)runSpecProbe:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [spec: ") + msg + "]";
    return NO;
  };
  using Voice = tdm::specfx::DevSpecFx::Voice;
  using Insert = tdm::specfx::DevSpecFx::Insert;
  // Defaults: both sections off, Whammy/Up, full depth, heel, 100%, Pre,
  // slot 0 staged+committed.
  if (_specCheck.state != NSControlStateValueOff || _specGestCheck.state != NSControlStateValueOff)
    return fail("checkbox not off");
  if (_spec.isExpressionEnabled() || _spec.isGesturesEnabled() || _spec.voice() != Voice::Whammy
      || _spec.exprDirection() != tdm::specfx::GestureDir::Up || _spec.exprDepth() != 12.0f)
    return fail("router defaults wrong");
  if (_spec.expression() != 0.0f || _spec.mix01() != 1.0f || _spec.insert() != Insert::Pre)
    return fail("router param defaults wrong");
  if (_spec.slotVoice(0) != Voice::Whammy || _spec.slotTimeMs(0) != 50.0f
      || _spec.slotRetMs(0) != 100.0f || _spec.slotDepthSt(0) != 12.0f)
    return fail("router slot 0 not factory");
  if (_specVoiceSeg.selectedSegment != 0 || _specInsertSeg.selectedSegment != 0
      || _specDirSeg.selectedSegment != 0)
    return fail("segments not on Whammy/Up/Pre");
  if ([_specGestPopup numberOfItems] != tdm::specfx::kNumGestureSlots
      || [_specGestPopup indexOfSelectedItem] != 0)
    return fail("gesture popup wrong");
  if (_engine->rig().specPre() == nullptr || _engine->rig().specPost() == nullptr)
    return fail("seams not installed");
  // Drive every creative slider through the real action; adoption + label.
  _sliders[@(kTagSpecExpr)].doubleValue = 75.0;
  [self paramChanged:_sliders[@(kTagSpecExpr)]];
  if (std::fabs(_spec.expression() - 0.75f) > 1e-6f)
    return fail("expression did not reach router");
  [self refreshValueLabels:_engine->rig().params()];
  if (![_valueLabels[@(kTagSpecExpr)].stringValue isEqualToString:@"75%"])
    return fail("expression label wrong");
  _sliders[@(kTagSpecExpr)].doubleValue = 0.0;
  [self paramChanged:_sliders[@(kTagSpecExpr)]];
  _sliders[@(kTagSpecMix)].doubleValue = 50.0;
  [self paramChanged:_sliders[@(kTagSpecMix)]];
  if (std::fabs(_spec.mix01() - 0.5f) > 1e-6f)
    return fail("mix did not reach router");
  _sliders[@(kTagSpecMix)].doubleValue = 100.0;
  [self paramChanged:_sliders[@(kTagSpecMix)]];
  _sliders[@(kTagSpecDepth)].doubleValue = 6.0;
  [self paramChanged:_sliders[@(kTagSpecDepth)]];
  if (std::fabs(_spec.exprDepth() - 6.0f) > 1e-6f)
    return fail("depth did not reach router");
  [self refreshValueLabels:_engine->rig().params()];
  if (![_valueLabels[@(kTagSpecDepth)].stringValue isEqualToString:@"6.0 st"])
    return fail("depth label wrong");
  _sliders[@(kTagSpecDepth)].doubleValue = 12.0;
  [self paramChanged:_sliders[@(kTagSpecDepth)]];
  // Selectors reach the router (4 voices + direction + insert).
  if ([_specVoiceSeg segmentCount] != 4 || [_specInsertSeg segmentCount] != 2
      || [_specDirSeg segmentCount] != 2)
    return fail("selector segment counts wrong");
  for (NSInteger seg = 0; seg < 4; ++seg)
  {
    _specVoiceSeg.selectedSegment = seg;
    [self specVoiceSelected:_specVoiceSeg];
    const Voice want = seg == 1   ? Voice::Dual
        : seg == 2                ? Voice::DualWham
        : seg == 3                ? Voice::Ring
                                  : Voice::Whammy;
    if (_spec.voice() != want)
      return fail("voice did not reach router");
  }
  for (NSInteger seg = 0; seg < 2; ++seg)
  {
    _specDirSeg.selectedSegment = seg;
    [self specDirSelected:_specDirSeg];
    if (_spec.exprDirection()
        != (seg == 1 ? tdm::specfx::GestureDir::Down : tdm::specfx::GestureDir::Up))
      return fail("direction did not reach router");
  }
  _specDirSeg.selectedSegment = 0;
  [self specDirSelected:_specDirSeg];
  // Per-voice gating: Ring parks Direction + Depth disabled (fixed map);
  // Whammy re-arms both. The dials keep their values for the return trip.
  _specVoiceSeg.selectedSegment = 3;
  [self specVoiceSelected:_specVoiceSeg];
  if (_specDirSeg.enabled || _sliders[@(kTagSpecDepth)].enabled)
    return fail("ring gating wrong");
  _specVoiceSeg.selectedSegment = 0;
  [self specVoiceSelected:_specVoiceSeg];
  if (!_specDirSeg.enabled || !_sliders[@(kTagSpecDepth)].enabled)
    return fail("whammy gating wrong");
  for (NSInteger seg = 0; seg < 2; ++seg)
  {
    _specInsertSeg.selectedSegment = seg;
    [self specInsertSelected:_specInsertSeg];
    if (_spec.insert() != (seg == 1 ? Insert::Post : Insert::Pre))
      return fail("insert did not reach router");
  }
  _specVoiceSeg.selectedSegment = 0;
  [self specVoiceSelected:_specVoiceSeg];
  _specInsertSeg.selectedSegment = 0;
  [self specInsertSelected:_specInsertSeg];
  // Popup selects slots; fields refresh per slot (Slow Dive factory).
  [_specGestPopup selectItemAtIndex:1];
  [self specGestPopupSelected:_specGestPopup];
  if (_sliders[@(kTagGestTime)].doubleValue != 600.0
      || _sliders[@(kTagGestDepth)].doubleValue != 12.0
      || _specGestEffectSeg.selectedSegment != 0 || _specGestDirSeg.selectedSegment != 1
      || _specGestMotionSeg.selectedSegment != 0
      || ![_specGestName.stringValue isEqualToString:@"Slow Dive"])
    return fail("slot 1 fields wrong");
  // Gesture direction gating: Whammy steers it, Dual parks it disabled.
  if (!_specGestDirSeg.enabled)
    return fail("gesture dir gating wrong");
  _specGestEffectSeg.selectedSegment = 1;
  [self specGestEffectSelected:_specGestEffectSeg];
  if (_specGestDirSeg.enabled)
    return fail("dual gesture dir should park");
  _specGestEffectSeg.selectedSegment = 0;
  [self specGestEffectSelected:_specGestEffectSeg];
  if (((TdmResetSlider*)_sliders[@(kTagGestTime)]).resetValue != 600.0)
    return fail("per-slot double-click reset wrong");
  [_specGestPopup selectItemAtIndex:0];
  [self specGestPopupSelected:_specGestPopup];
  // Staging: field edits do NOT touch router atomics until Save.
  [_specGestPopup selectItemAtIndex:2];
  [self specGestPopupSelected:_specGestPopup];
  _sliders[@(kTagGestDepth)].doubleValue = 6.0;
  [self paramChanged:_sliders[@(kTagGestDepth)]];
  _sliders[@(kTagGestTime)].doubleValue = 111.0;
  [self paramChanged:_sliders[@(kTagGestTime)]];
  [self refreshValueLabels:_engine->rig().params()];
  if (![_valueLabels[@(kTagGestDepth)].stringValue isEqualToString:@"6.0 st"])
    return fail("gesture depth label wrong");
  if (_spec.slotDepthSt(2) != 12.0f || _spec.slotTimeMs(2) != 400.0f)
    return fail("staging leaked to router");
  _specGestName.stringValue = @"Probe Dive";
  [self specGestSave:nil];
  if (_spec.slotDepthSt(2) != 6.0f || _spec.slotTimeMs(2) != 111.0f)
    return fail("save did not commit slot");
  if (![[_specGestPopup itemAtIndex:2].title isEqualToString:@"3: Probe Dive"])
    return fail("save did not rename popup");
  if (_spec.slotTimeMs(1) != 600.0f)
    return fail("save touched another slot");
  [self specGestReset:nil];
  if (_spec.slotDepthSt(2) != 12.0f || _spec.slotTimeMs(2) != 400.0f
      || _sliders[@(kTagGestTime)].doubleValue != 400.0
      || ![_specGestName.stringValue isEqualToString:@"Reverse Dive"])
    return fail("reset did not restore factory");
  [_specGestPopup selectItemAtIndex:0];
  [self specGestPopupSelected:_specGestPopup];
  // Momentary TRIGGER: down activates the displayed slot, up releases.
  // (Enabled, like hardware: an Off tap never runs its engine, so ramps
  // only advance while live — the legacy live_ behavior.)
  _engine->rig().reset(48000.0, 512);
  _specCheck.state = NSControlStateValueOn;
  [self specToggled:_specCheck];
  _specGestCheck.state = NSControlStateValueOn;
  [self specGestToggled:_specGestCheck];
  if (!_spec.isExpressionEnabled() || !_spec.isGesturesEnabled())
    return fail("section enables lost");
  std::vector<float> tick(512, 0.1f);
  [self specGestDown:nil];
  _spec.preTap().process(tick.data(), tick.data(), 512);
  if (!_spec.preGestureActive())
    return fail("trigger down lost");
  [self specGestUp:nil];
  for (int k = 0; k < 12; ++k) // slot 0 return 100 ms needs 4800+ samples
    _spec.preTap().process(tick.data(), tick.data(), 512);
  if (_spec.preGestureActive())
    return fail("trigger up lost");
  // External trigger bypasses the displayed slot: popup shows 0, but a
  // direct slot-5 command adopts slot 5's (slow) rate, provable from the
  // displacement after one block (slot 0's 50 ms would read ~0.21).
  _spec.setSlotTimeMs(5, 800.0f);
  _spec.pressSlot(5);
  _spec.preTap().process(tick.data(), tick.data(), 512);
  if (!_spec.preGestureActive() || _spec.preGestureDisp() > 0.05f)
    return fail("external trigger used wrong slot");
  _spec.releaseSlot(5);
  for (int k = 0; k < 20; ++k)
    _spec.preTap().process(tick.data(), tick.data(), 512);
  if (_spec.preGestureActive())
    return fail("external release stuck");
  _spec.resetSlot(5);
  // One-shot: activates, then auto-releases with no up command.
  [self specGestOneShot:nil];
  _spec.preTap().process(tick.data(), tick.data(), 512);
  if (!_spec.preGestureActive())
    return fail("one-shot did not fire");
  for (int k = 0; k < 40; ++k)
    _spec.preTap().process(tick.data(), tick.data(), 512);
  if (_spec.preGestureActive())
    return fail("one-shot stuck");
  // Gestures fire with the Expression section OFF (independent enable):
  // a held Fast Toe bends the tone, release returns to bit-dry.
  _specCheck.state = NSControlStateValueOff;
  [self specToggled:_specCheck];
  std::vector<float> tone(512, 0.0f);
  for (int i = 0; i < 512; ++i)
    tone[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 110.0 * i / 48000.0);
  [self specGestDown:nil];
  std::vector<float> held(512, 0.0f);
  for (int k = 0; k < 9; ++k) // slot 0 Time 50 ms: hold well past the top
    _spec.preTap().process(tone.data(), held.data(), 512);
  if (held == tone)
    return fail("gesture rendered dry with expression off");
  [self specGestUp:nil];
  for (int k = 0; k < 24; ++k) // slot 0 Return 100 ms + margin
    _spec.preTap().process(tone.data(), held.data(), 512);
  // Near-dry, not bit-dry: the legacy gain glide stalls ~7e-6 short of
  // its snap window (kept for golden-exactness; -109 dB, re-snapped by
  // the next dip-bottom clear). The engine tests pin the same bound.
  float relDiff = 0.0f;
  for (int i = 0; i < 512; ++i)
    relDiff = std::max(relDiff, std::fabs(held[static_cast<size_t>(i)] - tone[static_cast<size_t>(i)]));
  if (_spec.preGestureActive() || relDiff > 1e-5f)
    return fail("gesture release not dry with expression off");
  _specGestCheck.state = NSControlStateValueOff;
  [self specGestToggled:_specGestCheck];
  // Transparency: installed-but-off renders bit-identical to no seams.
  _engine->rig().reset(48000.0, 512);
  const int n = 512 * 16; // block-aligned: no ragged tail reads
  std::vector<float> in(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 110.0 * i / 48000.0);
  std::vector<float> off(static_cast<size_t>(n), 0.0f), bare(static_cast<size_t>(n), 0.0f);
  _specCheck.state = NSControlStateValueOff;
  [self specToggled:_specCheck];
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {off.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setSpecPre(nullptr);
  _engine->rig().setSpecPost(nullptr);
  _engine->rig().reset(48000.0, 512);
  for (int b = 0; b < 16; ++b)
  {
    const float* bi[1] = {in.data() + b * 512};
    float* bo[1] = {bare.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  _engine->rig().setSpecPre(&_spec.preTap());
  _engine->rig().setSpecPost(&_spec.postTap());
  if (off != bare)
    return fail("installed-but-off changed rig output");
  // Attack pips for the voice loop + envelope response.
  const int na = 512 * 96;
  std::vector<float> atk(static_cast<size_t>(na), 0.0f);
  for (int p = 0; p < 2; ++p)
  {
    const int at = 24000 + p * 24000;
    for (int i = 0; i < 4800 && at + i < na; ++i)
      atk[static_cast<size_t>(at + i)] +=
          0.8f * std::sin(2.0 * 3.14159265358979 * 100.0 * i / 48000.0) * std::exp(-i / 960.0f);
  }
  const tdm::RigParams before = _engine->rig().params();
  _engine->rig().reset(48000.0, 512);
  _specCheck.state = NSControlStateValueOff;
  [self specToggled:_specCheck];
  std::vector<float> atkDry(static_cast<size_t>(na), 0.0f);
  for (int b = 0; b < 96; ++b)
  {
    const float* bi[1] = {atk.data() + b * 512};
    float* bo[1] = {atkDry.data() + b * 512};
    _engine->rig().processBlock(bi, 1, bo, 1, 512);
  }
  // Enabled voices render at toe for both inserts (all four, plus a
  // Whammy-Down pass — polarity must bend the tone the other way).
  _sliders[@(kTagSpecExpr)].doubleValue = 100.0;
  [self paramChanged:_sliders[@(kTagSpecExpr)]];
  for (NSInteger ins = 0; ins < 2; ++ins)
  {
    _specInsertSeg.selectedSegment = ins;
    [self specInsertSelected:_specInsertSeg];
    for (NSInteger seg = 0; seg < 5; ++seg)
    {
      // Pass 4 re-renders Whammy with Direction Down (polarity must bend
      // the tone rather than cancel); passes 0..3 run Up.
      _specVoiceSeg.selectedSegment = (seg == 4) ? 0 : seg;
      [self specVoiceSelected:_specVoiceSeg];
      _specDirSeg.selectedSegment = (seg == 4) ? 1 : 0;
      [self specDirSelected:_specDirSeg];
      _specCheck.state = NSControlStateValueOn;
      [self specToggled:_specCheck];
      std::vector<float> wet(static_cast<size_t>(na), 0.0f);
      for (int b = 0; b < 96; ++b)
      {
        const float* bi[1] = {atk.data() + b * 512};
        float* bo[1] = {wet.data() + b * 512};
        _engine->rig().processBlock(bi, 1, bo, 1, 512);
      }
      if (wet == atkDry)
        return fail("voice rendered dry");
      bool finite = true;
      for (float v : wet)
        finite = finite && std::isfinite(v);
      if (!finite)
        return fail("voice non-finite");
    }
  }
  // Slow Dive (slot 1) through the real trigger: held dive renders
  // (whammy, heel) and releases; telemetry tracks the displacement.
  _specInsertSeg.selectedSegment = 0;
  [self specInsertSelected:_specInsertSeg];
  _specVoiceSeg.selectedSegment = 0;
  [self specVoiceSelected:_specVoiceSeg];
  _specDirSeg.selectedSegment = 0;
  [self specDirSelected:_specDirSeg];
  _sliders[@(kTagSpecExpr)].doubleValue = 0.0;
  [self paramChanged:_sliders[@(kTagSpecExpr)]];
  [_specGestPopup selectItemAtIndex:1];
  [self specGestPopupSelected:_specGestPopup];
  _specCheck.state = NSControlStateValueOn;
  [self specToggled:_specCheck];
  _specGestCheck.state = NSControlStateValueOn;
  [self specGestToggled:_specGestCheck];
  [self specGestDown:nil];
  {
    std::vector<float> wet(static_cast<size_t>(na), 0.0f);
    for (int b = 0; b < 96; ++b)
    {
      const float* bi[1] = {atk.data() + b * 512};
      float* bo[1] = {wet.data() + b * 512};
      _engine->rig().processBlock(bi, 1, bo, 1, 512);
    }
    if (wet == atkDry)
      return fail("dive rendered dry");
    bool finite = true;
    for (float v : wet)
      finite = finite && std::isfinite(v);
    if (!finite)
      return fail("dive non-finite");
    if (!_spec.preGestureActive() || _spec.preGestureDisp() < 0.9f)
      return fail("dive telemetry wrong");
  }
  [self specGestUp:nil];
  for (int b = 0; b < 40; ++b) // slot 1 return 300 ms needs 14400+ samples
  {
    const float* bi[1] = {atk.data() + (b % 96) * 512};
    float bo[512];
    float* boPtr[1] = {bo};
    _engine->rig().processBlock(bi, 1, boPtr, 1, 512);
  }
  if (_spec.preGestureActive())
    return fail("dive release stuck");
  [_specGestPopup selectItemAtIndex:0];
  [self specGestPopupSelected:_specGestPopup];
  const tdm::RigParams after = _engine->rig().params();
  if (before.gateEnabled != after.gateEnabled || before.tight != after.tight
      || before.driveEnabled != after.driveEnabled || before.inputTrimDb != after.inputTrimDb)
    return fail("voices touched rig params");
  // Back to defaults for a clean handoff (slots were saved+reset in
  // place above, so router atomics are factory; restore the fields too).
  _specCheck.state = NSControlStateValueOff;
  [self specToggled:_specCheck];
  _specGestCheck.state = NSControlStateValueOff;
  [self specGestToggled:_specGestCheck];
  _specVoiceSeg.selectedSegment = 0;
  [self specVoiceSelected:_specVoiceSeg];
  _specDirSeg.selectedSegment = 0;
  [self specDirSelected:_specDirSeg];
  _specInsertSeg.selectedSegment = 0;
  [self specInsertSelected:_specInsertSeg];
  _sliders[@(kTagSpecExpr)].doubleValue = 0.0;
  [self paramChanged:_sliders[@(kTagSpecExpr)]];
  _sliders[@(kTagSpecDepth)].doubleValue = 12.0;
  [self paramChanged:_sliders[@(kTagSpecDepth)]];
  [_specGestPopup selectItemAtIndex:0];
  [self specGestPopupSelected:_specGestPopup];
  if (![self checkDocGeometry:detail])
    return NO;
  return YES;
}

- (BOOL)runMidiProbe:(std::string*)detail
{
  auto fail = [&](const char* msg) {
    *detail = std::string(" [midi: ") + msg + "]";
    return NO;
  };
  using tdm::midi::Action;
  using tdm::midi::Mapping;
  using tdm::midi::Message;
  using tdm::midi::MsgType;
  using tdm::midi::Target;
  using tdm::midi::Trigger;
  // Section presence (12 rows + transport controls).
  if (_midiSrcPopup == nil || _midiStatus == nil || _midiRecent == nil || _midiOmniCheck == nil
      || _midiSaveLabel == nil)
    return fail("section missing");
  for (int t = 0; t < (int)Target::Count; ++t)
    if (_midiAssign[t] == nil || _midiLearnBtn[t] == nil || _midiModeSeg[t] == nil
        || _midiClearBtn[t] == nil)
      return fail("mapping row missing");
  // Synthetic dispatch through the REAL table+sink reaches _spec.
  _midiTable.clear();
  Mapping ce;
  ce.trigger.sourceUid = 0;
  ce.trigger.channel = -1;
  ce.trigger.type = MsgType::Cc;
  ce.trigger.number = 7;
  ce.target = Target::SpecExpression;
  ce.action = Action::Continuous;
  _midiTable.addOrReplace(ce);
  Message cc;
  cc.channel = 0;
  cc.type = MsgType::Cc;
  cc.number = 7;
  cc.value = 64;
  _midiDisp->handleMessage(cc);
  if (std::fabs(_spec.expression() - 64.0f / 127.0f) > 1e-6f)
    return fail("expression dispatch lost");
  [self midiTick]; // UI reflection follows the DSP atomic
  if (std::fabs(_sliders[@(kTagSpecExpr)].doubleValue - 6400.0 / 127.0) > 1e-6)
    return fail("expression reflection lost");
  Mapping tg;
  tg.trigger.type = MsgType::Program;
  tg.trigger.number = 6;
  tg.target = Target::SpecGestEnable;
  tg.action = Action::Toggle;
  _midiTable.addOrReplace(tg);
  Message pc;
  pc.type = MsgType::Program;
  pc.number = 6;
  _midiDisp->handleMessage(pc);
  if (!_spec.isGesturesEnabled())
    return fail("toggle on lost");
  [self midiTick];
  if (_specGestCheck.state != NSControlStateValueOn)
    return fail("checkbox reflection lost");
  _midiDisp->handleMessage(pc);
  if (_spec.isGesturesEnabled())
    return fail("toggle off lost");
  // Gesture slot by ID through the dispatcher + router (slot 0 one-shot
  // fires and completes with Expression OFF, like hardware).
  Mapping os;
  os.trigger.type = MsgType::Program;
  os.trigger.number = 10;
  os.target = Target::GestSlot0;
  os.action = Action::OneShot;
  _midiTable.addOrReplace(os);
  _spec.setExpressionEnabled(false);
  _spec.setGesturesEnabled(true);
  _engine->rig().reset(48000.0, 512);
  Message fire;
  fire.type = MsgType::Program;
  fire.number = 10;
  _midiDisp->handleMessage(fire);
  std::vector<float> tick(512, 0.1f);
  _spec.preTap().process(tick.data(), tick.data(), 512);
  if (!_spec.preGestureActive())
    return fail("MIDI one-shot lost");
  for (int k = 0; k < 40; ++k) // slot 0: 50 ms + 100 ms + margin
    _spec.preTap().process(tick.data(), tick.data(), 512);
  if (_spec.preGestureActive())
    return fail("MIDI one-shot stuck");
  // Learn arm -> offer -> tick-commit, all headless (slot row 6 = Slot 2).
  [self midiLearnBtn:_midiLearnBtn[5]];
  if (_midiLearn.armed() != Target::GestSlot1)
    return fail("learn did not arm");
  Message key;
  key.channel = 0;
  key.type = MsgType::NoteOn;
  key.number = 60;
  key.value = 100;
  if (!_midiLearn.offer(key))
    return fail("learn did not consume");
  [self midiTick];
  Mapping got;
  if (!_midiTable.findTarget(Target::GestSlot1, got) || got.trigger.number != 60
      || got.trigger.type != MsgType::NoteOn || got.action != Action::OneShot)
    return fail("learn did not commit");
  if (![_midiAssign[5].stringValue isEqualToString:@"N60 ch1"])
    return fail("learn row text wrong");
  [self midiClearRow:_midiClearBtn[5]]; // real action (tag math included)
  if (_midiTable.findTarget(Target::GestSlot1, got))
    return fail("clear row wrong");
  if (![_midiAssign[5].stringValue isEqualToString:@"—"])
    return fail("clear row wrong");
  // Persistence round-trip to a temp path (never the real default).
  tdm::midi::PersistedState st = tdm::midi::defaultState();
  st.mappings = _midiTable.snapshot();
  st.slots[1].name = "Probe Slot";
  std::string err;
  const std::string tmp = "/tmp/tdm-midi-probe.json";
  if (!tdm::midi::saveFile(tmp, st, err))
    return fail("probe save failed");
  tdm::midi::PersistedState back;
  if (!tdm::midi::loadFile(tmp, back, err) || back.slots[1].name != "Probe Slot"
      || back.mappings.size() != st.mappings.size())
    return fail("probe load mismatch");
  std::remove(tmp.c_str());
  if (!tdm::midi::loadJson("{bad", back, err))
  {
    // expected failure; err carries the reason
  }
  else
    return fail("corrupt JSON accepted");
  _midiTable.clear();
  [self refreshMidiRows];
  if (![self checkDocGeometry:detail])
    return NO;
  return YES;
}

- (void)chooseNam:(id)sender
{
  (void)sender;
  if (_engine->isRunning())
  {
    [self alert:@"Stop audio first" info:@"v0 needs Stop → load NAM → Start (no realtime model swapping yet)."];
    return;
  }
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  UTType* namType = [UTType typeWithFilenameExtension:@"nam"];
  if (namType != nil)
    panel.allowedContentTypes = @[ namType ];
  if ([panel runModal] != NSModalResponseOK)
    return;
  const std::string path = panel.URL.path.UTF8String;
  std::string error;
  if (!_engine->loadNam(path, error))
    [self alert:@"Could not load NAM" info:[NSString stringWithUTF8String:error.c_str()]];
  [self refreshFileLabels];
}

- (void)chooseIr:(id)sender
{
  (void)sender;
  if (_engine->isRunning())
  {
    [self alert:@"Stop audio first" info:@"v0 needs Stop → load IR → Start (no realtime swapping yet)."];
    return;
  }
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (NSString* ext in @[ @"wav", @"aif", @"aiff" ])
  {
    UTType* t = [UTType typeWithFilenameExtension:ext];
    if (t != nil)
      [types addObject:t];
  }
  if (types.count > 0)
    panel.allowedContentTypes = types;
  if ([panel runModal] != NSModalResponseOK)
    return;
  const std::string path = panel.URL.path.UTF8String;
  std::string error;
  if (!_engine->loadIr(path, error))
    [self alert:@"Could not load IR" info:[NSString stringWithUTF8String:error.c_str()]];
  [self refreshFileLabels];
}

@end

// Headless transpose-UI probe: builds the real window + controller and
// drives every transpose control programmatically (see runTransposeProbe).
// Engine is deleted before the pool drains so the stage outlives it.
bool probeTransposeUiFull(std::string& detail)
{
  [NSApplication sharedApplication]; // construction only; never runs
  TdmEngine* engine = new TdmEngine;
  engine->rig().setParams(auditionDefaults());
  BOOL ok = NO;
  @autoreleasepool
  {
    DevController* controller = [[DevController alloc] initWithEngine:engine];
    [controller buildUI];
    ok = [controller runTransposeProbe:&detail];
    if (ok == YES)
      ok = [controller runTunerProbe:&detail];
    if (ok == YES)
      ok = [controller runSlamProbe:&detail];
    if (ok == YES)
      ok = [controller runAlienProbe:&detail];
    if (ok == YES)
      ok = [controller runSpecProbe:&detail];
    if (ok == YES)
      ok = [controller runMidiProbe:&detail];
    delete engine; // stage (controller ivar) still alive: correct order
  }
  return ok == YES;
}

int main(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i)
  {
    if (std::string(argv[i]) == "--smoke-test")
      return smokeTest();
    if (std::string(argv[i]) == "--list-midi")
    {
      tdm::midi::MidiInput in;
      if (!in.available())
      {
        std::printf("MIDI unavailable (no server)\n");
        return 1;
      }
      const std::vector<tdm::midi::MidiInput::Source> srcs = in.refreshSources();
      if (srcs.empty())
        std::printf("No MIDI sources\n");
      for (const auto& s : srcs)
        std::printf("%u\t%s\n", s.uid, s.name.c_str());
      return 0;
    }
  }
  bool smokeUi = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--smoke-ui")
      smokeUi = true;

  @autoreleasepool
  {
    TdmEngine* engine = new TdmEngine;
    engine->rig().setParams(auditionDefaults());
    // Best-effort reference NAM preload (label shows the outcome, no modal).
    if (FILE* f = std::fopen(kReferenceNam, "rb"))
    {
      std::fclose(f);
      std::string error;
      engine->loadNam(kReferenceNam, error); // headless/no-device -> error kept, app still opens
    }

    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    DevController* controller = [[DevController alloc] initWithEngine:engine];
    app.delegate = controller;
    [controller buildUI];

    // Minimal menu so Cmd-Q works.
    NSMenu* menu = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menu addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit tdm_dev" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    app.mainMenu = menu;

    [app activateIgnoringOtherApps:YES];
    if (smokeUi)
      [controller autoQuitAfter:1.0];
    [app run];
    delete engine;
  }
  return 0;
}

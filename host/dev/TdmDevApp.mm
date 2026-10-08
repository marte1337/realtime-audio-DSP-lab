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

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/RigParams.h"
#include "dsp/Tuner/Tuner.h"
#include "dsp/lab/Pitch/DevTranspose.h"
#include "dsp/lab/Slam/DevSlam.h"
#include "dsp/lab/Pitch/DevTransposeUi.h"
#include "host/TdmEngine.h"

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
  kTagSlamAmount // SLAM section: shared 0..100% macro (live)
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
    _lastStepper = 0.0;
    _advShown = 0.0;
    _gt2Open = NO;
    _t3kOpen = NO;
    _engine->rig().setTransposeInsert(&_stage);
    _engine->rig().setSlamPreDrive(&_slam.preDrive());
    _engine->rig().setSlamPostNam(&_slam.postNam());
    _engine->rig().setSlamPostIr(&_slam.postIr());
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

  // Transpose section (signal-flow position: Gate -> Transpose -> Drive).
  // Engine/shift/enable apply live through the stage atomics; GT2 advanced
  // edits apply on the next Start (see the disclosure panels below).
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
  y += 10; // breathing room above the SLAM section when panels are closed

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
  y += 34;
  NSTextField* foot = [self makeLabel:@"Dev build: no presets, no meters, no experimental FX."
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
  // Scroll reachability: with GT2 open the document exceeds the viewport and
  // Output Trim is reachable by scrolling to the bottom.
  {
    NSClipView* clip = _scrollView.contentView;
    const CGFloat clipH = clip.bounds.size.height;
    const CGFloat docH = _docView.frame.size.height;
    if (!(docH > clipH))
      return fail("no scroll range when expanded");
    const NSRect outF = _sliders[@(kTagOutTrim)].frame;
    if (NSMaxY(outF) > docH + 0.5)
      return fail("output trim outside document");
    [clip scrollToPoint:NSMakePoint(0.0, docH - clipH)];
    const NSRect vis = clip.bounds; // clip bounds are in document coords
    if (!NSIntersectsRect(vis, outF))
      return fail("output trim not reachable by scroll");
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

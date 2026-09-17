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
//   Input 0 dB, Gate ON -55 dB / 52 ms, Drive ON 0.85 / 0.50 / 0.70,
//   ToneShape ON neutral, Space OFF (dry brutal rhythm first), Output 0 dB,
//   reference NAM preloaded best-effort.
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
#include "host/TdmEngine.h"

// Double-click probe, defined after the control classes below (ObjC needs
// global scope). External linkage so the in-namespace smoke-test can call it.
bool probeDoubleClickReset(std::string& detail);

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
NSString* baseName(const std::string& p)
{
  NSString* s = [NSString stringWithUTF8String:p.c_str()];
  return s.length > 0 ? [s lastPathComponent] : @"(none)";
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
  kTagOutTrim
};

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
    // Double-click behavior on the real control (defined after the control
    // classes below): a synthesized double-click parks the reset value and
    // fires the normal action exactly once.
    {
      std::string detail;
      check(probeDoubleClickReset(detail),
            ("double-click resets slider and fires action" + detail).c_str());
    }
  }
  // 2. Offline audio through the rig with audition params stays finite.
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
  // 3. Reference NAM loads + renders finite (asset present, no hardware).
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
  // 4. Engine load calls never crash headless; outcome depends on hardware.
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
@end

@implementation DevController
{
  TdmEngine* _engine; // owned (main-thread only except rig() param setters)
  NSWindow* _window;
  NSTextField* _statusLabel;
  NSTextField* _namLabel;
  NSTextField* _irLabel;
  NSButton* _transportButton;
  NSMutableDictionary<NSNumber*, NSSlider*>* _sliders;
  NSMutableDictionary<NSNumber*, NSTextField*>* _valueLabels;
  NSButton* _gateCheck;
  NSButton* _driveCheck;
  NSButton* _shapeCheck;
  NSButton* _delayCheck;
  NSButton* _reverbCheck;
  NSTimer* _tick;
}

- (instancetype)initWithEngine:(TdmEngine*)engine
{
  if ((self = [super init]) != nil)
  {
    _engine = engine;
    _sliders = [NSMutableDictionary dictionary];
    _valueLabels = [NSMutableDictionary dictionary];
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
  [_window.contentView addSubview:nameLabel];
  TdmResetSlider* slider = [[TdmResetSlider alloc] initWithFrame:NSMakeRect(135, y, width - 135 - 90, 22)];
  slider.minValue = mn;
  slider.maxValue = mx;
  slider.doubleValue = init;
  slider.resetValue = reset;
  slider.continuous = YES;
  slider.target = self;
  slider.action = @selector(paramChanged:);
  slider.tag = tag;
  [_window.contentView addSubview:slider];
  _sliders[@(tag)] = slider;
  NSTextField* value = [self makeLabel:@"" frame:NSMakeRect(width - 80, y, 70, 22) small:NO];
  [_window.contentView addSubview:value];
  _valueLabels[@(tag)] = value;
}

- (void)buildUI
{
  const CGFloat kWidth = 620;
  const CGFloat kHeight = 1030; // +260 for the Space section (2 checks + 5 rows)
  NSRect frame = NSMakeRect(0, 0, kWidth, kHeight);
  _window = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"TechDeathMachine — Dev Control v0";
  [_window center];

  CGFloat y = kHeight - 34;
  _statusLabel = [self makeLabel:@"Stopped" frame:NSMakeRect(20, y, kWidth - 40, 22) small:NO];
  [_window.contentView addSubview:_statusLabel];
  y -= 30;

  // NAM row.
  NSTextField* namTitle = [self makeLabel:@"NAM:" frame:NSMakeRect(20, y, 44, 22) small:NO];
  [_window.contentView addSubview:namTitle];
  _namLabel = [self makeLabel:@"(none)" frame:NSMakeRect(66, y, kWidth - 66 - 130, 22) small:YES];
  _namLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _namLabel.selectable = YES;
  [_window.contentView addSubview:_namLabel];
  NSButton* namButton = [NSButton buttonWithTitle:@"Choose NAM…"
                                           target:self
                                           action:@selector(chooseNam:)];
  namButton.frame = NSMakeRect(kWidth - 124, y - 2, 104, 26);
  namButton.bezelStyle = NSBezelStyleRounded;
  [_window.contentView addSubview:namButton];
  y -= 30;

  // IR row.
  NSTextField* irTitle = [self makeLabel:@"IR:" frame:NSMakeRect(20, y, 44, 22) small:NO];
  [_window.contentView addSubview:irTitle];
  _irLabel = [self makeLabel:@"(none)" frame:NSMakeRect(66, y, kWidth - 66 - 130, 22) small:YES];
  _irLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _irLabel.selectable = YES;
  [_window.contentView addSubview:_irLabel];
  NSButton* irButton = [NSButton buttonWithTitle:@"Choose IR…"
                                          target:self
                                          action:@selector(chooseIr:)];
  irButton.frame = NSMakeRect(kWidth - 124, y - 2, 104, 26);
  irButton.bezelStyle = NSBezelStyleRounded;
  [_window.contentView addSubview:irButton];
  y -= 34;

  // Transport row.
  _transportButton = [NSButton buttonWithTitle:@"Start audio"
                                        target:self
                                        action:@selector(toggleAudio:)];
  _transportButton.frame = NSMakeRect(20, y - 2, 120, 28);
  _transportButton.bezelStyle = NSBezelStyleRounded;
  [_window.contentView addSubview:_transportButton];
  NSTextField* hint = [self makeLabel:@"Params apply live. NAM/IR need: Stop → load → Start."
                                frame:NSMakeRect(150, y, kWidth - 170, 22)
                                small:YES];
  [_window.contentView addSubview:hint];
  y -= 40;

  NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(20, y, kWidth - 40, 1)];
  sep.boxType = NSBoxSeparator;
  [_window.contentView addSubview:sep];
  y -= 30;

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
  y -= 30;
  _gateCheck = [NSButton checkboxWithTitle:@"Gate enable" target:self action:@selector(gateToggled:)];
  _gateCheck.frame = NSMakeRect(20, y, 160, 22);
  _gateCheck.state = NSControlStateValueOn; // audition default
  [_window.contentView addSubview:_gateCheck];
  y -= 30;
  [self addSliderRow:@"Gate Thresh"
                 tag:kTagGateThresh
                 min:tdm::TechDeathGate::kMinThresholdDb
                 max:tdm::TechDeathGate::kMaxThresholdDb
                init:-55
               reset:tdm::TechDeathGate::kDefaultThresholdDb
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Gate Release"
                 tag:kTagGateRel
                 min:tdm::TechDeathGate::kMinReleaseMs
                 max:tdm::TechDeathGate::kMaxReleaseMs
                init:52
               reset:tdm::TechDeathGate::kDefaultReleaseMs
                   y:y
               width:kWidth];
  y -= 40;

  // Drive section.
  _driveCheck = [NSButton checkboxWithTitle:@"TightDrive enable" target:self action:@selector(driveToggled:)];
  _driveCheck.frame = NSMakeRect(20, y, 180, 22);
  _driveCheck.state = NSControlStateValueOn; // audition default
  [_window.contentView addSubview:_driveCheck];
  y -= 30;
  [self addSliderRow:@"Tight"
                 tag:kTagTight
                 min:tdm::TightDrive::kMinTight
                 max:tdm::TightDrive::kMaxTight
                init:0.85
               reset:tdm::TightDrive::kDefaultTight
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Drive"
                 tag:kTagDrive
                 min:tdm::TightDrive::kMinDrive
                 max:tdm::TightDrive::kMaxDrive
                init:0.50
               reset:tdm::TightDrive::kDefaultDrive
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Bite"
                 tag:kTagBite
                 min:tdm::TightDrive::kMinBite
                 max:tdm::TightDrive::kMaxBite
                init:0.70
               reset:tdm::TightDrive::kDefaultBite
                   y:y
               width:kWidth];
  y -= 40;

  // ToneShape section (post-cab; neutral is transparent).
  _shapeCheck = [NSButton checkboxWithTitle:@"ToneShape enable" target:self action:@selector(shapeToggled:)];
  _shapeCheck.frame = NSMakeRect(20, y, 180, 22);
  _shapeCheck.state = NSControlStateValueOn; // audition default (neutral)
  [_window.contentView addSubview:_shapeCheck];
  y -= 30;
  [self addSliderRow:@"Weight"
                 tag:kTagWeight
                 min:tdm::ToneShape::kMinWeight
                 max:tdm::ToneShape::kMaxWeight
                init:0.50
               reset:tdm::ToneShape::kDefaultWeight
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Contour"
                 tag:kTagContour
                 min:tdm::ToneShape::kMinContour
                 max:tdm::ToneShape::kMaxContour
                init:0.50
               reset:tdm::ToneShape::kDefaultContour
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Presence"
                 tag:kTagPresence
                 min:tdm::ToneShape::kMinPresence
                 max:tdm::ToneShape::kMaxPresence
                init:0.50
               reset:tdm::ToneShape::kDefaultPresence
                   y:y
               width:kWidth];
  y -= 40;

  // Space section (first stereo stage; both units OFF at audition default
  // so the rhythm tone stays dry until leads/ambience are auditioned).
  _delayCheck = [NSButton checkboxWithTitle:@"Delay enable" target:self action:@selector(delayToggled:)];
  _delayCheck.frame = NSMakeRect(20, y, 180, 22);
  _delayCheck.state = NSControlStateValueOff; // audition default
  [_window.contentView addSubview:_delayCheck];
  y -= 30;
  [self addSliderRow:@"Delay Time"
                 tag:kTagDelayTime
                 min:tdm::Delay::kMinTimeMs
                 max:tdm::Delay::kMaxTimeMs
                init:tdm::Delay::kDefaultTimeMs
               reset:tdm::Delay::kDefaultTimeMs
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Delay Fdbk"
                 tag:kTagDelayFb
                 min:tdm::Delay::kMinFeedback
                 max:tdm::Delay::kMaxFeedback
                init:0.35
               reset:tdm::Delay::kDefaultFeedback
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Delay Mix"
                 tag:kTagDelayMix
                 min:tdm::SpaceProcessor::kMinMix
                 max:tdm::SpaceProcessor::kMaxMix
                init:0.25
               reset:tdm::SpaceProcessor::kDefaultDelayMix
                   y:y
               width:kWidth];
  y -= 40;
  _reverbCheck = [NSButton checkboxWithTitle:@"Reverb enable" target:self action:@selector(reverbToggled:)];
  _reverbCheck.frame = NSMakeRect(20, y, 180, 22);
  _reverbCheck.state = NSControlStateValueOff; // audition default
  [_window.contentView addSubview:_reverbCheck];
  y -= 30;
  [self addSliderRow:@"Reverb Decay"
                 tag:kTagReverbDecay
                 min:tdm::Reverb::kMinDecay
                 max:tdm::Reverb::kMaxDecay
                init:0.40
               reset:tdm::Reverb::kDefaultDecay
                   y:y
               width:kWidth];
  y -= 30;
  [self addSliderRow:@"Reverb Mix"
                 tag:kTagReverbMix
                 min:tdm::SpaceProcessor::kMinMix
                 max:tdm::SpaceProcessor::kMaxMix
                init:0.20
               reset:tdm::SpaceProcessor::kDefaultReverbMix
                   y:y
               width:kWidth];
  y -= 40;

  // Output section.
  [self addSliderRow:@"Output Trim"
                 tag:kTagOutTrim
                 min:tdm::OutputTrim::kMinTrimDb
                 max:tdm::OutputTrim::kMaxTrimDb
                init:0
               reset:tdm::OutputTrim::kDefaultTrimDb
                   y:y
               width:kWidth];
  y -= 34;
  NSTextField* foot = [self makeLabel:@"Dev build: no presets, no meters, no experimental FX."
                                frame:NSMakeRect(20, y, kWidth - 40, 22)
                                small:YES];
  [_window.contentView addSubview:foot];

  [self refreshFileLabels];
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
}

- (void)tick:(NSTimer*)timer
{
  (void)timer;
  const tdm::RigParams p = _engine->rig().params(); // lock-free snapshot
  [self refreshValueLabels:p];
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
  default:
    break;
  }
  [self refreshValueLabels:_engine->rig().params()];
}

- (void)gateToggled:(NSButton*)sender
{
  _engine->rig().setGateEnabled(sender.state == NSControlStateValueOn);
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
      [self alert:@"Could not start audio" info:[NSString stringWithUTF8String:error.c_str()]];
  }
  [self tick:nil];
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

#pragma once

// RigParams: plain control-side snapshot of every realtime-tweakable rig
// parameter. GUI/host code owns this struct; TechDeathRig mirrors it behind
// lock-free atomics (see TechDeathRig.h). No GUI types here on purpose: the
// struct must stay includable from DSP, host, tests, and any future
// Standalone/VST3/AU shell without dragging in a toolkit.
//
// Defaults mirror the DSP stage defaults exactly (single-source references
// live next to each field). The developer app's *audition* starting point
// (e.g. hotter gate/drive settings) is UI initial state, not a DSP
// assumption, so it lives in the app layer, not here.

#include "dsp/Gate/TechDeathGate.h"
#include "dsp/InputTrim.h"
#include "dsp/OutputTrim.h"
#include "dsp/TightDrive/TightDrive.h"
#include "dsp/ToneShape/ToneShape.h"

namespace tdm
{
struct RigParams
{
  float inputTrimDb = InputTrim::kDefaultTrimDb;
  bool gateEnabled = false;
  float gateThresholdDb = TechDeathGate::kDefaultThresholdDb;
  float gateReleaseMs = TechDeathGate::kDefaultReleaseMs;
  bool driveEnabled = false;
  float tight = TightDrive::kDefaultTight;
  float drive = TightDrive::kDefaultDrive;
  float bite = TightDrive::kDefaultBite;
  bool shapeEnabled = false;
  float weight = ToneShape::kDefaultWeight;
  float contour = ToneShape::kDefaultContour;
  float presence = ToneShape::kDefaultPresence;
  float outputTrimDb = OutputTrim::kDefaultTrimDb;
};
} // namespace tdm

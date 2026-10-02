#pragma once

// DevTransposeUi: GUI-independent control adapter for the DEV transpose
// stage (LAB/DEV, never production).
//
// The AppKit developer app and the headless unit tests share this table so
// control values map to DevTranspose/GuitarTransposeV2 identically in both.
// It intentionally contains no UI toolkit types: plain structs + inline
// range-checked accessors over GuitarTransposeV2::Config.
//
// Contents:
// - the 17 GT2 Config fields as an ordered descriptor table (key, label,
//   unit, per-field min/max/step/decimals, bool flag). Ranges mirror the
//   validation in GuitarTransposeV2::reset(); cross-field relations
//   (fadeMax >= fadeMin, floor < window, nccHi > nccLo, skip < cells) are
//   enforced by DevTranspose::configureGt2, not here.
// - baseline comparison against DevTranspose::knownGoodGt2().
//
// Realtime note: everything here runs on control threads (UI, tests).
// Nothing here is called from the audio callback.

#include "dsp/lab/Pitch/DevTranspose.h"
#include "dsp/lab/Pitch/GuitarTransposeV2.h"

namespace tdm
{
namespace lab
{
struct Gt2UiField
{
  const char* key; // stable id (matches the --gt2-* CLI flag suffix)
  const char* label; // short UI row label
  const char* unit; // display unit ("", "ms", "Hz", "dB")
  double min = 0.0; // per-field valid range (from GT2 reset validation)
  double max = 1.0;
  double step = 0.1; // sensible UI increment
  int decimals = 1; // value-label precision
};

inline int devGt2UiFieldCount()
{
  return 17;
}

// i in [0, 17): fixed order, stable across builds.
inline const Gt2UiField& devGt2UiField(int i)
{
  static const Gt2UiField kFields[17] = {
      {"window", "Window", "ms", 10.0, 120.0, 1.0, 1},
      {"floor", "Floor", "ms", 0.5, 10.0, 0.1, 1},
      {"corr", "Corr window", "ms", 5.0, 60.0, 1.0, 1},
      {"fademin", "Fade min", "ms", 4.0, 120.0, 1.0, 1},
      {"fademax", "Fade max", "ms", 4.0, 250.0, 1.0, 1},
      {"fadencc-hi", "Fade NCC hi", "", 0.0, 1.0, 0.01, 2},
      {"fadencc-lo", "Fade NCC lo", "", 0.0, 1.0, 0.01, 2},
      {"onsetfade", "Resync fade", "ms", 0.5, 10.0, 0.1, 1},
      {"onsetspan", "Resync span", "ms", 1.0, 12.0, 0.1, 1},
      {"lead", "Search lead", "ms", 1.0, 16.0, 0.1, 1},
      {"refr", "Refractory", "ms", 5.0, 200.0, 1.0, 0},
      {"hpf", "Detector HPF", "Hz", 100.0, 4000.0, 10.0, 0},
      {"smooth", "Det smooth", "ms", 0.5, 10.0, 0.1, 1},
      {"overmin", "Over min", "dB", 3.0, 24.0, 0.5, 1},
      {"overmax", "Over max", "dB", 1.0, 18.0, 0.5, 1},
      {"cells", "Hist cells", "", 10.0, 200.0, 1.0, 0},
      {"skip", "Hist skip", "", 1.0, 20.0, 1.0, 0},
      // NOTE: the 18th Config field (enableResync) is intentionally NOT a
      // slider row; the UI owns it as a checkbox. This table holds the 17
      // numeric fields only.
  };
  return kFields[i];
}

// Read numeric field i from a config.
inline double devGt2UiGet(const GuitarTransposeV2::Config& cfg, int i)
{
  switch (i)
  {
  case 0:
    return cfg.windowMs;
  case 1:
    return cfg.floorMs;
  case 2:
    return cfg.corrMs;
  case 3:
    return cfg.fadeMinMs;
  case 4:
    return cfg.fadeMaxMs;
  case 5:
    return cfg.fadeNccHi;
  case 6:
    return cfg.fadeNccLo;
  case 7:
    return cfg.onsetFadeMs;
  case 8:
    return cfg.onsetSpanMs;
  case 9:
    return cfg.searchLeadMs;
  case 10:
    return cfg.refractoryMs;
  case 11:
    return cfg.detectorHpHz;
  case 12:
    return cfg.detectorSmoothMs;
  case 13:
    return cfg.onsetOverMinDb;
  case 14:
    return cfg.onsetOverMaxDb;
  case 15:
    return static_cast<double>(cfg.historyCells);
  case 16:
    return static_cast<double>(cfg.historySkip);
  default:
    return 0.0;
  }
}

// Write field i into a config. Range-checked against the descriptor;
// out-of-range values are rejected (false, config untouched). Integer
// fields (cells/skip) round to nearest. Cross-field relations are NOT
// checked here: callers validate the whole config via
// DevTranspose::configureGt2 before storing it.
inline bool devGt2UiSet(GuitarTransposeV2::Config& cfg, int i, double v)
{
  if (i < 0 || i >= devGt2UiFieldCount())
    return false;
  const Gt2UiField& f = devGt2UiField(i);
  if (!(v >= f.min && v <= f.max))
    return false;
  switch (i)
  {
  case 0:
    cfg.windowMs = v;
    break;
  case 1:
    cfg.floorMs = v;
    break;
  case 2:
    cfg.corrMs = v;
    break;
  case 3:
    cfg.fadeMinMs = v;
    break;
  case 4:
    cfg.fadeMaxMs = v;
    break;
  case 5:
    cfg.fadeNccHi = v;
    break;
  case 6:
    cfg.fadeNccLo = v;
    break;
  case 7:
    cfg.onsetFadeMs = v;
    break;
  case 8:
    cfg.onsetSpanMs = v;
    break;
  case 9:
    cfg.searchLeadMs = v;
    break;
  case 10:
    cfg.refractoryMs = v;
    break;
  case 11:
    cfg.detectorHpHz = v;
    break;
  case 12:
    cfg.detectorSmoothMs = v;
    break;
  case 13:
    cfg.onsetOverMinDb = v;
    break;
  case 14:
    cfg.onsetOverMaxDb = v;
    break;
  case 15:
    cfg.historyCells = static_cast<int>(v + 0.5);
    break;
  case 16:
    cfg.historySkip = static_cast<int>(v + 0.5);
    break;
  default:
    return false;
  }
  return true;
}

// Exact baseline comparison (all 17 Config fields incl. enableResync).
inline bool devGt2UiIsBaseline(const GuitarTransposeV2::Config& cfg)
{
  const GuitarTransposeV2::Config base = DevTranspose::knownGoodGt2();
  return cfg.windowMs == base.windowMs && cfg.floorMs == base.floorMs && cfg.corrMs == base.corrMs
      && cfg.fadeMinMs == base.fadeMinMs && cfg.fadeMaxMs == base.fadeMaxMs
      && cfg.fadeNccHi == base.fadeNccHi && cfg.fadeNccLo == base.fadeNccLo
      && cfg.onsetFadeMs == base.onsetFadeMs && cfg.onsetSpanMs == base.onsetSpanMs
      && cfg.searchLeadMs == base.searchLeadMs && cfg.refractoryMs == base.refractoryMs
      && cfg.detectorHpHz == base.detectorHpHz && cfg.detectorSmoothMs == base.detectorSmoothMs
      && cfg.onsetOverMinDb == base.onsetOverMinDb && cfg.onsetOverMaxDb == base.onsetOverMaxDb
      && cfg.historyCells == base.historyCells && cfg.historySkip == base.historySkip
      && cfg.enableResync == base.enableResync;
}
} // namespace lab
} // namespace tdm

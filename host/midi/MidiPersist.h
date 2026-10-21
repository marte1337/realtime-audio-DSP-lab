#pragma once

// MidiPersist: versioned JSON save/load for MIDI mappings, connected
// source UIDs, and all eight gesture slot configs (host/midi). Pure
// C++, main thread only (file IO never touches audio/MIDI threads).
//
// Failure policy: missing file -> defaults (not an error); corrupt
// file or wrong version -> defaults + error string; unknown enum
// strings -> that ENTRY is skipped (rest of file still loads); every
// number is range-checked and clamped. Nothing throws.

#include <cstdint>
#include <string>
#include <vector>

#include "dsp/lab/SpecialFx/SpecGesture.h"
#include "host/midi/MidiMapping.h"

namespace tdm
{
namespace midi
{

struct PersistedSlot
{
  std::string name;
  specfx::GestureConfig cfg;
};

struct PersistedState
{
  int version = 1;
  bool omniLearn = false;
  std::vector<uint32_t> midiSources; // CoreMIDI UIDs (never indexes)
  std::vector<Mapping> mappings;     // runtime state zeroed
  PersistedSlot slots[specfx::kNumGestureSlots];
};

PersistedState defaultState(); // factory gesture slots, empty mappings
std::string saveJson(const PersistedState& s);
bool loadJson(const std::string& text, PersistedState& out, std::string& error);
bool saveFile(const std::string& path, const PersistedState& s, std::string& error);
bool loadFile(const std::string& path, PersistedState& out,
              std::string& error); // missing -> defaults, returns true
std::string defaultPath(); // ~/Library/Application Support/TechDeathMachine/midi.json

} // namespace midi
} // namespace tdm

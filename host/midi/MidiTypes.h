#pragma once

// MidiTypes: generic MIDI control vocabulary for TDM (host/midi).
// Pure C++11, no CoreMIDI dependency, no DSP dependency — the parser
// takes raw bytes so tests inject synthetic traffic without hardware.
//
// Control flow (see MidiDispatch / MidiInput):
//   CoreMIDI bytes -> StreamParser -> Message -> Dispatcher -> ISink
//   (DSP atomic setters). The DSP never knows the source, which keeps
//   this path reusable for future VST3/AU automation.

#include <cstdint>

namespace tdm
{
namespace midi
{

// Channel-voice messages we act on. Everything else decodes as Other
// (timing, tune request, sysex, malformed) and is safely ignored.
enum class MsgType : uint8_t
{
  Cc = 0,
  NoteOn = 1,
  NoteOff = 2, // incl. Note On with velocity 0
  Program = 3,
  Other = 255
};

// One decoded message. UIDs come from CoreMIDI endpoint properties;
// synthetic/test traffic uses 0 (unknown) or an explicit fake UID.
struct Message
{
  uint32_t sourceUid = 0;
  uint8_t channel = 0; // 0..15
  MsgType type = MsgType::Other;
  uint8_t number = 0; // CC# / note# / program#
  uint8_t value = 0;  // CC value / velocity (0 for Program)
};

// Control targets: stable IDs (persisted, never reordered). Continuous
// targets take 0..1, bool targets take on/off, slot targets take
// press/release/one-shot by stable gesture ID. VST3 parameters later
// bind to these same IDs.
enum class Target : int8_t
{
  None = -1,
  SpecExpression = 0,
  SpecMix = 1,
  SpecExprEnable = 2,
  SpecGestEnable = 3,
  GestSlot0 = 4,
  GestSlot1 = 5,
  GestSlot2 = 6,
  GestSlot3 = 7,
  GestSlot4 = 8,
  GestSlot5 = 9,
  GestSlot6 = 10,
  GestSlot7 = 11,
  Count = 12
};

inline bool targetIsContinuous(Target t)
{
  return t == Target::SpecExpression || t == Target::SpecMix;
}
inline bool targetIsBool(Target t)
{
  return t == Target::SpecExprEnable || t == Target::SpecGestEnable;
}
inline int targetSlot(Target t) // 0..7, or -1
{
  const int v = static_cast<int>(t) - static_cast<int>(Target::GestSlot0);
  return (v >= 0 && v < 8) ? v : -1;
}
inline const char* targetName(Target t)
{
  switch (t)
  {
  case Target::SpecExpression: return "Creative Expression";
  case Target::SpecMix: return "Creative Mix";
  case Target::SpecExprEnable: return "Expression Enable";
  case Target::SpecGestEnable: return "Gestures Enable";
  case Target::GestSlot0: return "Gesture Slot 1";
  case Target::GestSlot1: return "Gesture Slot 2";
  case Target::GestSlot2: return "Gesture Slot 3";
  case Target::GestSlot3: return "Gesture Slot 4";
  case Target::GestSlot4: return "Gesture Slot 5";
  case Target::GestSlot5: return "Gesture Slot 6";
  case Target::GestSlot6: return "Gesture Slot 7";
  case Target::GestSlot7: return "Gesture Slot 8";
  default: return "?";
  }
}

// How a matched message drives its target. Not every combination is
// offered: Continuous needs CC values (notes/PC carry none),
// Momentary needs a release signal (PC has none — use OneShot or
// Toggle for PC triggers instead; releases are never fabricated).
enum class Action : uint8_t
{
  Continuous = 0, // CC value -> 0..1 (invert + calibration window)
  Momentary = 1,  // press/release pair (note, or CC >=64 / <64)
  OneShot = 2,    // single trigger edge -> oneShotSlot (slots only)
  Toggle = 3      // trigger edge flips a bool (bool targets only)
};

inline const char* actionName(Action a)
{
  switch (a)
  {
  case Action::Continuous: return "Continuous";
  case Action::Momentary: return "Momentary";
  case Action::OneShot: return "One-shot";
  case Action::Toggle: return "Toggle";
  default: return "?";
  }
}

// Streaming byte parser: feed raw MIDI (CoreMIDI packets, potentially
// split mid-message with running status spanning feeds). Decoded
// messages append to out[0..cap); overflow drops the oldest excess
// (bounded by construction). Malformed bytes are skipped, never fatal.
class StreamParser
{
public:
  void reset()
  {
    running_ = 0;
    need_ = 0;
    have_ = 0;
    sys_[0] = sys_[1] = 0;
    inSysex_ = false;
  }

  // Returns the number of messages written to out.
  int feed(const uint8_t* data, int len, uint32_t sourceUid, Message* out, int cap)
  {
    int n = 0;
    for (int i = 0; i < len; ++i)
    {
      const uint8_t b = data[i];
      if (b >= 0xF8) // realtime: single byte, ignored, keeps running status
        continue;
      if (b == 0xF0)
      {
        inSysex_ = true;
        running_ = 0;
        need_ = 0;
        continue;
      }
      if (inSysex_)
      {
        if (b < 0x80)
          continue; // sysex payload (realtime already passed through)
        inSysex_ = false; // F7 ends it, any status aborts it
        if (b == 0xF7)
          continue;
      }
      if (b >= 0x80) // status byte
      {
        running_ = 0;
        need_ = 0;
        if (b < 0xF0) // channel voice: arm running status
        {
          running_ = b;
          need_ = (voiceLen(b) == 2) ? 2 : 1;
          have_ = 0;
        }
        else if (b == 0xF1 || b == 0xF3) // 1 data byte, ignored
        {
          running_ = 0x00; // special: swallow one byte, no running status
          need_ = 1;
          have_ = 0;
        }
        else if (b == 0xF2) // 2 data bytes, ignored
        {
          running_ = 0x00;
          need_ = 2;
          have_ = 0;
        }
        // 0xF4..0xF7 (except F0/F7 handled): no data, ignored
        continue;
      }
      // Data byte.
      if (need_ == 0)
      {
        if (running_ >= 0x80) // running status re-arms the voice length
        {
          need_ = (voiceLen(running_) == 2) ? 2 : 1;
          have_ = 0;
        }
        else
          continue; // stray data byte: skip
      }
      sys_[have_++] = b & 0x7F;
      if (have_ < need_)
        continue;
      have_ = 0;
      if (running_ == 0x00) // swallowed system-common payload
      {
        need_ = 0;
        continue;
      }
      if (n < cap)
        emit(sourceUid, running_, sys_[0], (need_ == 2) ? sys_[1] : 0, out[n++]);
      need_ = 0; // running status (incl. 1-byte voices) re-arms on next data
    }
    return n;
  }

private:
  static int voiceLen(uint8_t status) // data bytes for 0x80..0xEF
  {
    const uint8_t hi = status & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
  }
  static void emit(uint32_t uid, uint8_t status, uint8_t d0, uint8_t d1, Message& m)
  {
    m.sourceUid = uid;
    m.channel = status & 0x0F;
    const uint8_t hi = status & 0xF0;
    if (hi == 0x90 && d1 != 0)
    {
      m.type = MsgType::NoteOn;
      m.number = d0;
      m.value = d1;
    }
    else if (hi == 0x80 || hi == 0x90) // 0x90 with vel 0 falls here
    {
      m.type = MsgType::NoteOff;
      m.number = d0;
      m.value = d1;
    }
    else if (hi == 0xB0)
    {
      m.type = MsgType::Cc;
      m.number = d0;
      m.value = d1;
    }
    else if (hi == 0xC0)
    {
      m.type = MsgType::Program;
      m.number = d0;
      m.value = 0;
    }
    else // Aftertouch / PitchBend: decoded as Other (ignored downstream)
    {
      m.type = MsgType::Other;
      m.number = d0;
      m.value = d1;
    }
  }

  uint8_t running_ = 0; // 0 = none, 0x00-sentinel overloaded below
  int need_ = 0;
  int have_ = 0;
  uint8_t sys_[2] = {0, 0};
  bool inSysex_ = false;
};

} // namespace midi
} // namespace tdm

#pragma once

// MidiDispatch: MIDI message -> generic TDM control commands
// (host/midi). Pure C++, no CoreMIDI dependency.
//
// The sink is the ONLY dispatch dependency: the DEV app implements it
// with lock-free DSP setters today; a future VST3/AU wrapper binds
// the same sink interface to host automation. Dispatch runs on the
// MIDI input thread; every sink call must be realtime-safe (atomics
// only — the existing DevSpecFx setters already qualify).

#include "host/midi/MidiMapping.h"

namespace tdm
{
namespace midi
{

struct ISink
{
  virtual ~ISink() = default;
  virtual void setContinuous(Target t, float v01) = 0; // continuous targets
  virtual void setBool(Target t, bool v) = 0;          // bool targets
  virtual void gesturePress(int slot) = 0;             // 0..7
  virtual void gestureRelease(int slot) = 0;
  virtual void gestureOneShot(int slot) = 0;
};

class Dispatcher
{
public:
  Dispatcher(MappingTable& table, ISink& sink) : table_(table), sink_(sink) {}

  // Handle one decoded message (MIDI thread). Bounded: at most
  // kMaxMappings matches, each a few atomics. Unknown/Other messages
  // match nothing and cost one table scan.
  void handleMessage(const Message& m)
  {
    if (m.type == MsgType::Other)
      return;
    table_.forEachMatch(m, [&](Mapping& mp) { fire(mp, m); });
  }

private:
  void fire(Mapping& mp, const Message& m)
  {
    switch (mp.action)
    {
    case Action::Continuous:
      if (m.type == MsgType::Cc && targetIsContinuous(mp.target))
        sink_.setContinuous(mp.target, ccTo01(m.value, mp));
      break;
    case Action::Momentary:
      momentary(mp, m);
      break;
    case Action::OneShot:
      if (targetSlot(mp.target) < 0)
        break; // defensive: table compatibility normally forbids this
      if (m.type == MsgType::NoteOn || m.type == MsgType::Program
          || (m.type == MsgType::Cc && risingEdge(mp, m.value)))
        sink_.gestureOneShot(targetSlot(mp.target));
      else if (m.type == MsgType::Cc)
        fallingEdge(mp, m.value); // track the arm state, no fire
      break;
    case Action::Toggle:
      if (!targetIsBool(mp.target))
        break; // defensive
      if (m.type == MsgType::NoteOn || m.type == MsgType::Program
          || (m.type == MsgType::Cc && risingEdge(mp, m.value)))
      {
        mp.toggleState = !mp.toggleState;
        sink_.setBool(mp.target, mp.toggleState);
      }
      else if (m.type == MsgType::Cc)
        fallingEdge(mp, m.value);
      break;
    }
  }

  void momentary(Mapping& mp, const Message& m)
  {
    const int slot = targetSlot(mp.target);
    const bool isBool = targetIsBool(mp.target);
    if (slot < 0 && !isBool)
      return; // defensive
    bool press = false, release = false;
    if (m.type == MsgType::NoteOn)
      press = true;
    else if (m.type == MsgType::NoteOff)
      release = true;
    else if (m.type == MsgType::Cc)
    {
      if (m.value >= 64)
        press = true;
      else
        release = true;
    }
    else
      return; // Program has no release: never momentary (see compatible())
    if (press)
    {
      if (slot >= 0)
        sink_.gesturePress(slot);
      else
        sink_.setBool(mp.target, true);
    }
    else if (release)
    {
      if (slot >= 0)
        sink_.gestureRelease(slot);
      else
        sink_.setBool(mp.target, false);
    }
  }

  static float ccTo01(uint8_t v, const Mapping& mp)
  {
    const float lo = (float)mp.calMin, hi = (float)mp.calMax;
    float f = (hi > lo) ? ((float)v - lo) / (hi - lo) : (float)v / 127.0f;
    if (f < 0.0f)
      f = 0.0f;
    else if (f > 1.0f)
      f = 1.0f;
    return mp.invert ? (1.0f - f) : f;
  }
  // CC trigger edges (>= 64): rising fires, falling re-arms. A held
  // pedal repeating values fires exactly once — no retrigger spam,
  // and no fabricated releases.
  static bool risingEdge(Mapping& mp, uint8_t v)
  {
    const bool above = v >= 64;
    const bool fire = above && !mp.lastAbove;
    mp.lastAbove = above;
    return fire;
  }
  static void fallingEdge(Mapping& mp, uint8_t v)
  {
    mp.lastAbove = v >= 64;
  }

  MappingTable& table_;
  ISink& sink_;
};

} // namespace midi
} // namespace tdm

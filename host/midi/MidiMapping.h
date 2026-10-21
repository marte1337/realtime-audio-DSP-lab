#pragma once

// MidiMapping: trigger -> target assignment table + MIDI Learn capture
// (host/midi). Pure C++, no CoreMIDI dependency.
//
// Threading: the table is read on the MIDI input thread (dispatch) and
// written on the main thread (learn commit / clear / load). A mutex
// guards the table AND per-mapping runtime state; it is held only for
// microseconds and NEVER on the audio thread (the audio thread only
// sees lock-free DSP atomics). Mapping count is capped so dispatch is
// bounded.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "host/midi/MidiTypes.h"

namespace tdm
{
namespace midi
{

// What fires a mapping. sourceUid 0 and channel -1 are wildcards
// ("any source/channel"); Learn captures exact values unless OMNI is
// armed. Trigger identity never depends on display labels.
struct Trigger
{
  uint32_t sourceUid = 0;
  int8_t channel = -1;
  MsgType type = MsgType::Cc;
  uint8_t number = 0;

  bool matches(const Message& m) const
  {
    if (m.type == MsgType::Other)
      return false;
    // Key aliasing: a NoteOff reaches the mapping learned from its key
    // (triggers are always stored as NoteOn), so Momentary releases
    // arrive without a second mapping. OneShot/Toggle ignore NoteOff
    // at dispatch, so the alias only ever releases.
    if (m.type == MsgType::NoteOff)
    {
      if (type != MsgType::NoteOn)
        return false;
    }
    else if (m.type != type)
      return false;
    if (m.number != number)
      return false;
    if (channel >= 0 && m.channel != (uint8_t)channel)
      return false;
    if (sourceUid != 0 && m.sourceUid != sourceUid)
      return false;
    return true;
  }
  bool operator==(const Trigger& o) const
  {
    return sourceUid == o.sourceUid && channel == o.channel && type == o.type
        && number == o.number;
  }
};

struct Mapping
{
  Trigger trigger;
  Target target = Target::None;
  Action action = Action::Continuous;
  bool invert = false;     // continuous direction (false = normal)
  uint8_t calMin = 0;      // continuous input window (pedal calibration)
  uint8_t calMax = 127;    // (calMin < calMax enforced on commit)
  // Runtime state below is touched ONLY while the table mutex is held
  // (dispatch on the MIDI thread).
  bool lastAbove = false;  // CC edge memory (>=64) for OneShot/Toggle
  bool toggleState = false;// Toggle memory for bool targets
};

class MappingTable
{
public:
  static constexpr int kMaxMappings = 32;

  // Main thread: install a mapping. The same trigger can only drive
  // ONE mapping, so an existing owner is replaced (no duplicates, no
  // silent double-fires). Returns false when target/action are
  // incompatible or the table is full (all 32 slots owned by OTHER
  // triggers).
  bool addOrReplace(const Mapping& m)
  {
    if (!compatible(m.target, m.action))
      return false;
    Mapping c = m;
    if (c.calMax <= c.calMin)
    {
      c.calMin = 0;
      c.calMax = 127;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = maps_.begin(); it != maps_.end();)
      it = (it->trigger == c.trigger) ? maps_.erase(it) : (it + 1);
    if ((int)maps_.size() >= kMaxMappings)
      return false;
    c.lastAbove = false;
    c.toggleState = false;
    maps_.push_back(c);
    return true;
  }

  // Main thread.
  bool removeTrigger(const Trigger& t)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = maps_.begin(); it != maps_.end(); ++it)
      if (it->trigger == t)
      {
        maps_.erase(it);
        return true;
      }
    return false;
  }
  bool removeTarget(Target t) // clears the (first) mapping driving t
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = maps_.begin(); it != maps_.end(); ++it)
      if (it->target == t)
      {
        maps_.erase(it);
        return true;
      }
    return false;
  }
  void clear()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    maps_.clear();
  }
  // Main thread: snapshot for UI + persistence (copy out, never alias).
  std::vector<Mapping> snapshot() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return maps_;
  }
  bool findTarget(Target t, Mapping& out) const // first mapping driving t
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& m : maps_)
      if (m.target == t)
      {
        out = m;
        return true;
      }
    return false;
  }

  // MIDI thread: run fn(m) for every mapping matching msg (mutex held
  // throughout; fn must not block — it only touches atomics).
  template <typename Fn> void forEachMatch(const Message& msg, Fn&& fn)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& m : maps_)
      if (m.trigger.matches(msg))
        fn(m);
  }

  // Target/action compatibility: Continuous needs a CC value source;
  // Momentary needs a release signal (notes/CC, never PC); OneShot is
  // slots-only; Toggle is bools-only.
  static bool compatible(Target t, Action a)
  {
    if (t == Target::None || t == Target::Count)
      return false;
    switch (a)
    {
    case Action::Continuous: return targetIsContinuous(t);
    case Action::Momentary: return targetIsBool(t) || targetSlot(t) >= 0;
    case Action::OneShot: return targetSlot(t) >= 0;
    case Action::Toggle: return targetIsBool(t);
    default: return false;
    }
  }
  // Momentary offers only make sense for triggers with a release.
  static bool triggerHasRelease(const Trigger& t)
  {
    return t.type == MsgType::NoteOn || t.type == MsgType::NoteOff || t.type == MsgType::Cc;
  }

private:
  mutable std::mutex mutex_;
  std::vector<Mapping> maps_;
};

// MIDI Learn capture state. arm()/cancel()/takeCapture() run on the
// main thread; offer() runs on the MIDI thread. Handoff is atomic —
// no locks, no allocation, no UI calls from the MIDI side.
class LearnManager
{
public:
  void arm(Target t, bool omni)
  {
    omni_.store(omni ? 1 : 0, std::memory_order_relaxed);
    hasCapture_.store(0, std::memory_order_relaxed);
    armed_.store(static_cast<int>(t), std::memory_order_release);
  }
  void cancel()
  {
    armed_.store(static_cast<int>(Target::None), std::memory_order_release);
    hasCapture_.store(0, std::memory_order_relaxed);
  }
  Target armed() const { return static_cast<Target>(armed_.load(std::memory_order_acquire)); }

  // MIDI thread: capture the first actionable message while armed.
  // Returns true when the message was consumed for Learn (the caller
  // must NOT also dispatch it). Notes/CC/PC capture; Other never.
  bool offer(const Message& m)
  {
    if (armed_.load(std::memory_order_acquire) == static_cast<int>(Target::None))
      return false;
    if (m.type == MsgType::Other)
      return false;
    if (hasCapture_.load(std::memory_order_relaxed) != 0)
      return true; // armed + captured: still consume until commit/cancel
    capUid_.store(m.sourceUid, std::memory_order_relaxed);
    capCh_.store(m.channel, std::memory_order_relaxed);
    capType_.store(static_cast<uint8_t>(m.type), std::memory_order_relaxed);
    capNum_.store(m.number, std::memory_order_relaxed);
    hasCapture_.store(1, std::memory_order_release);
    return true;
  }

  // Main thread: take a pending capture (false = none yet). OMNI
  // captures wildcard source/channel.
  bool takeCapture(Trigger& out)
  {
    if (hasCapture_.load(std::memory_order_acquire) == 0)
      return false;
    const bool omni = omni_.load(std::memory_order_relaxed) != 0;
    out.sourceUid = omni ? 0 : capUid_.load(std::memory_order_relaxed);
    out.channel = omni ? (int8_t)-1 : (int8_t)capCh_.load(std::memory_order_relaxed);
    out.type = static_cast<MsgType>(capType_.load(std::memory_order_relaxed));
    // NoteOff capture normalizes to NoteOn: either edge addresses the
    // same key, and Momentary needs the On identity.
    if (out.type == MsgType::NoteOff)
      out.type = MsgType::NoteOn;
    out.number = capNum_.load(std::memory_order_relaxed);
    hasCapture_.store(0, std::memory_order_relaxed);
    armed_.store(static_cast<int>(Target::None), std::memory_order_release);
    return true;
  }

private:
  std::atomic<int> armed_{static_cast<int>(Target::None)};
  std::atomic<int> hasCapture_{0};
  std::atomic<int> omni_{0};
  std::atomic<uint32_t> capUid_{0};
  std::atomic<uint8_t> capCh_{0};
  std::atomic<uint8_t> capType_{0};
  std::atomic<uint8_t> capNum_{0};
};

} // namespace midi
} // namespace tdm

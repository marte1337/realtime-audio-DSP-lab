#pragma once

// MidiInput: CoreMIDI source discovery + connection + packet intake
// (host/midi). Transport only — parsing feeds a handler owned by the
// app (Learn consume -> Dispatcher -> sink). No DSP here.
//
// Threading: ALL methods are main-thread-only EXCEPT the handler,
// which runs on a CoreMIDI-owned input thread (never the audio
// thread). The handler must be fast and realtime-safe (atomics only).
// Hot-plug notifications set a flag the main-thread tick drains.

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "host/midi/MidiTypes.h"

namespace tdm
{
namespace midi
{

class MidiInput
{
public:
  using Handler = void (*)(const Message* msgs, int n, void* ctx);

  struct Source
  {
    uint32_t uid = 0;
    std::string name;
    bool connected = false;
  };

  MidiInput();
  ~MidiInput();
  MidiInput(const MidiInput&) = delete;
  MidiInput& operator=(const MidiInput&) = delete;

  bool available() const { return client_ != 0 && port_ != 0; }
  void setHandler(Handler h, void* ctx)
  {
    handler_ = h;
    handlerCtx_ = ctx;
  }

  // Re-enumerate sources; reconnect wanted UIDs that are present
  // (hot-plug recovery). Returns the current source list.
  std::vector<Source> refreshSources();
  // Connect/disconnect one endpoint by UID (UIDs persist across
  // launches; enumeration indexes do not). Unknown UID -> false.
  bool setConnected(uint32_t uid, bool on);
  // Wanted UIDs (persisted intent, incl. currently absent devices).
  std::vector<uint32_t> wantedSources() const;
  // True when CoreMIDI reported a setup change since the last refresh.
  bool needsRefresh() const;

  // Monitor snapshot: latest message + lifetime count (main thread).
  // Fields are individually atomic; a torn multi-field read under a
  // pedal flood is harmless (advisory display only).
  struct Latest
  {
    uint32_t uid = 0;
    uint8_t channel = 0;
    MsgType type = MsgType::Other;
    uint8_t number = 0;
    uint8_t value = 0;
    uint64_t count = 0;
  };
  Latest latest() const;

  // CoreMIDI callback entry points (called by the framework on its own
  // threads; not for general use). ingest takes a MIDIPacketList*.
  void ingest(const void* pktlist, uint32_t srcUid);
  void flagRefresh();

private:
  uint32_t client_ = 0; // MIDIClientRef / MIDIPortRef are 32-bit IDs;
  uint32_t port_ = 0;   // CoreMIDI includes stay in the .cpp.
  Handler handler_ = nullptr;
  void* handlerCtx_ = nullptr;
  StreamParser parser_;
  std::set<uint32_t> wanted_; // main thread only (persisted intent)
  std::set<uint32_t> live_;   // main thread only (actual connections)
  std::atomic<int> needsRefresh_{0};
  // Latest-message snapshot (MIDI thread writes, main thread reads):
  std::atomic<uint32_t> latUid_{0};
  std::atomic<uint8_t> latCh_{0};
  std::atomic<uint8_t> latType_{static_cast<uint8_t>(MsgType::Other)};
  std::atomic<uint8_t> latNum_{0};
  std::atomic<uint8_t> latVal_{0};
  std::atomic<uint64_t> latCount_{0};
};

} // namespace midi
} // namespace tdm

// MidiInput implementation: CoreMIDI client/port/source management.

#include "host/midi/MidiInput.h"

#include <CoreMIDI/CoreMIDI.h>

namespace tdm
{
namespace midi
{
namespace
{

std::string cfToString(CFStringRef s)
{
  if (s == nullptr)
    return "";
  char buf[256];
  if (!CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
    return "";
  return buf;
}

std::string endpointName(MIDIEndpointRef ep)
{
  CFStringRef s = nullptr;
  if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &s) == noErr && s != nullptr)
  {
    const std::string n = cfToString(s);
    CFRelease(s);
    if (!n.empty())
      return n;
  }
  if (MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &s) == noErr && s != nullptr)
  {
    const std::string n = cfToString(s);
    CFRelease(s);
    if (!n.empty())
      return n;
  }
  return "(unnamed MIDI source)";
}

int32_t endpointUid(MIDIEndpointRef ep)
{
  int32_t uid = 0;
  if (MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &uid) != noErr)
    return 0;
  return uid;
}

} // namespace

static void readProc(const MIDIPacketList* pkts, void* readRefCon, void* srcRefCon)
{
  MidiInput* self = static_cast<MidiInput*>(readRefCon);
  const auto uid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(srcRefCon));
  self->ingest(pkts, uid);
}

static void notifyProc(const MIDINotification* msg, void* refCon)
{
  MidiInput* self = static_cast<MidiInput*>(refCon);
  if (msg->messageID == kMIDIMsgSetupChanged)
    self->flagRefresh();
}

MidiInput::MidiInput()
{
  MIDIClientRef client = 0;
  if (MIDIClientCreate(CFSTR("TechDeathMachine"), notifyProc, this, &client) != noErr)
    return;
  client_ = client;
  MIDIPortRef port = 0;
  if (MIDIInputPortCreate(client, CFSTR("TDM Control In"), readProc, this, &port) != noErr)
  {
    MIDIClientDispose(client);
    client_ = 0;
    return;
  }
  port_ = port;
}

MidiInput::~MidiInput()
{
  if (port_ != 0)
    MIDIPortDispose(port_);
  if (client_ != 0)
    MIDIClientDispose(client_);
}

void MidiInput::ingest(const void* pktlist, uint32_t srcUid)
{
  const MIDIPacketList* pkts = static_cast<const MIDIPacketList*>(pktlist);
  const MIDIPacket* pkt = &pkts->packet[0];
  for (UInt32 i = 0; i < pkts->numPackets; ++i)
  {
    Message msgs[64];
    const int n = parser_.feed(pkt->data, (int)pkt->length, srcUid, msgs, 64);
    if (n > 0 && handler_ != nullptr)
      handler_(msgs, n, handlerCtx_);
    if (n > 0)
    {
      const Message& last = msgs[n - 1];
      latUid_.store(last.sourceUid, std::memory_order_relaxed);
      latCh_.store(last.channel, std::memory_order_relaxed);
      latType_.store(static_cast<uint8_t>(last.type), std::memory_order_relaxed);
      latNum_.store(last.number, std::memory_order_relaxed);
      latVal_.store(last.value, std::memory_order_relaxed);
      latCount_.fetch_add((uint64_t)n, std::memory_order_relaxed);
    }
    pkt = MIDIPacketNext(pkt);
  }
}

void MidiInput::flagRefresh()
{
  needsRefresh_.store(1, std::memory_order_release);
}

std::vector<uint32_t> MidiInput::wantedSources() const
{
  return std::vector<uint32_t>(wanted_.begin(), wanted_.end());
}

bool MidiInput::needsRefresh() const
{
  return needsRefresh_.load(std::memory_order_acquire) != 0;
}

MidiInput::Latest MidiInput::latest() const
{
  Latest l;
  l.uid = latUid_.load(std::memory_order_relaxed);
  l.channel = latCh_.load(std::memory_order_relaxed);
  l.type = static_cast<MsgType>(latType_.load(std::memory_order_relaxed));
  l.number = latNum_.load(std::memory_order_relaxed);
  l.value = latVal_.load(std::memory_order_relaxed);
  l.count = latCount_.load(std::memory_order_relaxed);
  return l;
}

std::vector<MidiInput::Source> MidiInput::refreshSources()
{
  needsRefresh_.store(0, std::memory_order_release);
  std::vector<Source> out;
  if (!available())
    return out;
  // Hot-plug recovery without flapping stable connections: drop live
  // UIDs whose endpoint vanished, connect wanted UIDs that appeared.
  // Runs on setup changes and explicit refreshes only.
  for (auto it = live_.begin(); it != live_.end();)
  {
    MIDIObjectRef obj = 0;
    MIDIObjectType type = kMIDIObjectType_Other;
    if (MIDIObjectFindByUniqueID((MIDIUniqueID)*it, &obj, &type) != noErr
        || type != kMIDIObjectType_Source)
      it = live_.erase(it); // endpoint gone; CoreMIDI dropped the link
    else
      ++it;
  }
  const ItemCount n = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; ++i)
  {
    const MIDIEndpointRef ep = MIDIGetSource(i);
    const uint32_t uid = (uint32_t)endpointUid(ep);
    Source s;
    s.uid = uid;
    s.name = endpointName(ep);
    if (uid != 0 && wanted_.count(uid) != 0 && live_.count(uid) == 0)
    {
      void* refCon = reinterpret_cast<void*>((uintptr_t)uid);
      if (MIDIPortConnectSource(port_, ep, refCon) == noErr)
        live_.insert(uid);
    }
    s.connected = (uid != 0 && live_.count(uid) != 0);
    out.push_back(s);
  }
  return out;
}

bool MidiInput::setConnected(uint32_t uid, bool on)
{
  if (uid == 0)
    return false;
  if (!on)
  {
    wanted_.erase(uid); // intent cleared even with no server (stays cleared)
    live_.erase(uid);
    if (available())
    {
      MIDIObjectRef obj = 0;
      MIDIObjectType type = kMIDIObjectType_Other;
      if (MIDIObjectFindByUniqueID((MIDIUniqueID)uid, &obj, &type) == noErr
          && type == kMIDIObjectType_Source)
        MIDIPortDisconnectSource(port_, (MIDIEndpointRef)obj);
    }
    return true;
  }
  wanted_.insert(uid);
  if (!available())
    return false; // intent kept; a later refresh connects it
  if (live_.count(uid) != 0)
    return true;
  MIDIObjectRef obj = 0;
  MIDIObjectType type = kMIDIObjectType_Other;
  if (MIDIObjectFindByUniqueID((MIDIUniqueID)uid, &obj, &type) != noErr
      || type != kMIDIObjectType_Source)
    return false; // not present now; wanted_ keeps the intent for later
  void* refCon = reinterpret_cast<void*>((uintptr_t)uid);
  if (MIDIPortConnectSource(port_, (MIDIEndpointRef)obj, refCon) != noErr)
    return false;
  live_.insert(uid);
  return true;
}

} // namespace midi
} // namespace tdm

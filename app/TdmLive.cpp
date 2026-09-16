// tdm_live: minimal CoreAudio duplex runner for M0.
//
// Default input -> lock-free ring -> rig -> default output.
// Loads assets BEFORE audio starts; there is no live model swapping in M0.
// Input/output devices must run at the same rate (set via Audio MIDI Setup),
// and that rate must match the NAM/IR assets (no resampling in M0).
//
// Usage:
//   tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms]
//              [--input-trim db]
//              [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1]
//   tdm_live --list            (show audio devices and exit)
//
// Passing either gate flag enables TechDeathGate (the other keeps its
// default); without gate flags the gate bypasses exactly (Milestone 0 path).
// Passing --tight-drive or any of --tight/--drive/--bite enables TightDrive
// (unspecified params keep their defaults); otherwise it bypasses exactly.

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"

namespace
{
// Power-of-two single-producer/single-consumer float ring. No allocation
// after construction; safe between the two HAL IO threads.
class MonoRing
{
public:
  explicit MonoRing(size_t capacity)
  : cap_(capacity)
  , mask_(capacity - 1)
  , buf_(capacity, 0.0f)
  {
  }

  size_t push(const float* x, size_t n, uint64_t& dropped)
  {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t free = cap_ - (w - r);
    if (n > free)
    {
      dropped += n - free;
      read_.store(w + n - cap_, std::memory_order_release);
    }
    const size_t pos = w & mask_;
    const size_t first = pos + n <= cap_ ? n : cap_ - pos;
    std::memcpy(buf_.data() + pos, x, first * sizeof(float));
    if (first < n)
      std::memcpy(buf_.data(), x + first, (n - first) * sizeof(float));
    write_.store(w + n, std::memory_order_release);
    return n;
  }

  size_t pop(float* out, size_t n)
  {
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t r = read_.load(std::memory_order_relaxed);
    size_t avail = w - r;
    if (avail > n)
      avail = n;
    const size_t pos = r & mask_;
    const size_t first = pos + avail <= cap_ ? avail : cap_ - pos;
    std::memcpy(out, buf_.data() + pos, first * sizeof(float));
    if (first < avail)
      std::memcpy(out + first, buf_.data(), (avail - first) * sizeof(float));
    read_.store(r + avail, std::memory_order_release);
    return avail;
  }

private:
  const size_t cap_, mask_;
  std::vector<float> buf_;
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
};

struct LiveCtx
{
  tdm::TechDeathRig* rig = nullptr;
  MonoRing* ring = nullptr;
  int inChannels = 0;
  bool inInterleaved = false;
  int outChannels = 0;
  bool outInterleaved = false;
  int maxBlock = 0;
  std::vector<float> inScratch;  // input thread only
  std::vector<float> outScratch; // output thread only
  std::atomic<uint64_t> underruns{0};
  std::atomic<uint64_t> overruns{0};
  std::atomic<uint64_t> blocks{0};
};

OSStatus inputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* inData, const AudioTimeStamp*,
                   AudioBufferList*, const AudioTimeStamp*, void* ctx)
{
  auto* c = static_cast<LiveCtx*>(ctx);
  // Average all input channels to mono via the preallocated scratch,
  // chunked so an unexpectedly large device block cannot overrun it.
  const UInt32 cap = static_cast<UInt32>(c->inScratch.size());
  UInt32 total = 0;
  if (c->inInterleaved)
  {
    total = inData->mBuffers[0].mDataByteSize / (sizeof(float) * static_cast<UInt32>(c->inChannels));
    const float* p = static_cast<const float*>(inData->mBuffers[0].mData);
    for (UInt32 base = 0; base < total; base += cap)
    {
      const UInt32 m = base + cap <= total ? cap : total - base;
      for (UInt32 i = 0; i < m; ++i)
      {
        double acc = 0.0;
        for (int ch = 0; ch < c->inChannels; ++ch)
          acc += p[(base + i) * c->inChannels + ch];
        c->inScratch[i] = static_cast<float>(acc / c->inChannels);
      }
      uint64_t dropped = 0;
      c->ring->push(c->inScratch.data(), m, dropped);
      if (dropped > 0)
        c->overruns.fetch_add(dropped, std::memory_order_relaxed);
    }
  }
  else
  {
    total = inData->mNumberBuffers > 0 ? inData->mBuffers[0].mDataByteSize / sizeof(float) : 0;
    for (UInt32 base = 0; base < total; base += cap)
    {
      const UInt32 m = base + cap <= total ? cap : total - base;
      for (UInt32 i = 0; i < m; ++i)
      {
        double acc = 0.0;
        for (int ch = 0; ch < c->inChannels && static_cast<UInt32>(ch) < inData->mNumberBuffers; ++ch)
          acc += static_cast<const float*>(inData->mBuffers[ch].mData)[base + i];
        c->inScratch[i] = static_cast<float>(acc / c->inChannels);
      }
      uint64_t dropped = 0;
      c->ring->push(c->inScratch.data(), m, dropped);
      if (dropped > 0)
        c->overruns.fetch_add(dropped, std::memory_order_relaxed);
    }
  }
  return noErr;
}

OSStatus outputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                    AudioBufferList* outData, const AudioTimeStamp*, void* ctx)
{
  auto* c = static_cast<LiveCtx*>(ctx);
  UInt32 frames = 0;
  if (c->outInterleaved)
    frames = outData->mBuffers[0].mDataByteSize / (sizeof(float) * static_cast<UInt32>(c->outChannels));
  else
    frames = outData->mNumberBuffers > 0 ? outData->mBuffers[0].mDataByteSize / sizeof(float) : 0;

  UInt32 done = 0;
  while (done < frames)
  {
    UInt32 m = frames - done;
    if (m > static_cast<UInt32>(c->maxBlock))
      m = static_cast<UInt32>(c->maxBlock);
    const size_t got = c->ring->pop(c->outScratch.data(), m);
    if (got < m)
    {
      c->underruns.fetch_add(m - got, std::memory_order_relaxed);
      std::memset(c->outScratch.data() + got, 0, (m - got) * sizeof(float));
    }
    const float* bi[1] = {c->outScratch.data()};
    float* bo[1] = {c->outScratch.data()};
    c->rig->processBlock(bi, 1, bo, 1, static_cast<int>(m));
    for (UInt32 i = 0; i < m; ++i)
    {
      float v = c->outScratch[i];
      v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); // interface clamp, as in the reference standalone
      if (c->outInterleaved)
      {
        float* p = static_cast<float*>(outData->mBuffers[0].mData);
        for (int ch = 0; ch < c->outChannels; ++ch)
          p[(done + i) * c->outChannels + ch] = v;
      }
      else
      {
        for (int ch = 0; ch < c->outChannels && static_cast<UInt32>(ch) < outData->mNumberBuffers; ++ch)
          static_cast<float*>(outData->mBuffers[ch].mData)[done + i] = v;
      }
    }
    done += m;
  }
  c->blocks.fetch_add(1, std::memory_order_relaxed);
  return noErr;
}

void halCheck(OSStatus st, const char* what)
{
  if (st != noErr)
    throw std::runtime_error(std::string(what) + " (OSStatus " + std::to_string(st) + ")");
}

AudioDeviceID defaultDevice(bool input)
{
  AudioObjectPropertyAddress addr = {input ? kAudioHardwarePropertyDefaultInputDevice
                                           : kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  AudioDeviceID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  halCheck(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &dev), "get default device");
  if (dev == kAudioObjectUnknown)
    throw std::runtime_error("no default audio device");
  return dev;
}

double deviceSampleRate(AudioDeviceID dev)
{
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  Float64 sr = 0;
  UInt32 size = sizeof(sr);
  halCheck(AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &sr), "get device sample rate");
  return sr;
}

AudioStreamBasicDescription deviceFormat(AudioDeviceID dev, bool input, int& channels, bool& interleaved)
{
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamFormat,
                                     input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
  AudioStreamBasicDescription asbd = {};
  UInt32 size = sizeof(asbd);
  halCheck(AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &asbd), "get device stream format");
  if (asbd.mFormatID != kAudioFormatLinearPCM || !(asbd.mFormatFlags & kAudioFormatFlagIsFloat)
      || asbd.mBitsPerChannel != 32)
    throw std::runtime_error("device stream must be 32-bit float PCM");
  channels = static_cast<int>(asbd.mChannelsPerFrame);
  interleaved = !(asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
  if (channels < 1)
    throw std::runtime_error("device reports no channels");
  return asbd;
}

UInt32 deviceFrameSize(AudioDeviceID dev)
{
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 n = 0;
  UInt32 size = sizeof(n);
  halCheck(AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &n), "get buffer frame size");
  return n;
}

std::string cfToString(CFStringRef s)
{
  char buf[256] = {};
  if (s != nullptr)
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
  return buf;
}

int listDevices()
{
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  halCheck(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size), "list devices");
  std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
  halCheck(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devs.data()),
           "list devices");
  for (AudioDeviceID d : devs)
  {
    CFStringRef name = nullptr;
    UInt32 n = sizeof(name);
    AudioObjectPropertyAddress na = {kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
    std::string label = "?";
    if (AudioObjectGetPropertyData(d, &na, 0, nullptr, &n, &name) == noErr)
    {
      label = cfToString(name);
      if (name != nullptr)
        CFRelease(name);
    }
    std::printf("id=%u name=%s in=%.0fHz out=%.0fHz\n", d, label.c_str(), deviceSampleRate(d),
                deviceSampleRate(d));
  }
  return 0;
}
} // namespace

int main(int argc, char** argv)
{
  std::string namPath, irPath, gateThresh, gateRel, inputTrim;
  std::string tight, drive, bite;
  bool driveEnable = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--list")
      return listDevices();
    if (a == "--tight-drive")
    {
      driveEnable = true;
      continue;
    }
    if ((a == "--nam" || a == "--ir" || a == "--gate-thresh" || a == "--gate-rel" || a == "--input-trim"
         || a == "--tight" || a == "--drive" || a == "--bite")
        && i + 1 < argc)
    {
      if (a == "--nam")
        namPath = argv[++i];
      else if (a == "--ir")
        irPath = argv[++i];
      else if (a == "--gate-thresh")
        gateThresh = argv[++i];
      else if (a == "--gate-rel")
        gateRel = argv[++i];
      else if (a == "--input-trim")
        inputTrim = argv[++i];
      else if (a == "--tight")
        tight = argv[++i];
      else if (a == "--drive")
        drive = argv[++i];
      else
        bite = argv[++i];
      continue;
    }
    std::printf("usage: tdm_live [--nam amp.nam] [--ir cab.wav] [--gate-thresh db] [--gate-rel ms] "
                "[--input-trim db] [--tight-drive] [--tight 0..1] [--drive 0..1] [--bite 0..1] | --list\n");
    return 2;
  }

  try
  {
    const AudioDeviceID inDev = defaultDevice(true);
    const AudioDeviceID outDev = defaultDevice(false);
    const double inSr = deviceSampleRate(inDev);
    const double outSr = deviceSampleRate(outDev);
    if (std::fabs(inSr - outSr) > 1.0)
      throw std::runtime_error("input (" + std::to_string(inSr) + " Hz) and output (" + std::to_string(outSr)
                               + " Hz) rates differ; align them in Audio MIDI Setup");

    LiveCtx ctx;
    deviceFormat(inDev, true, ctx.inChannels, ctx.inInterleaved);
    deviceFormat(outDev, false, ctx.outChannels, ctx.outInterleaved);
    const UInt32 frameSize = deviceFrameSize(outDev);
    ctx.maxBlock = frameSize > 2048 ? static_cast<int>(frameSize) : 2048;
    ctx.inScratch.assign(1u << 14, 0.0f); // chunked in inputProc; independent of maxBlock
    ctx.outScratch.assign(static_cast<size_t>(ctx.maxBlock), 0.0f);

    tdm::TechDeathRig rig;
    rig.reset(outSr, ctx.maxBlock);
    if (!inputTrim.empty())
      rig.setInputTrimDb(std::stof(inputTrim));
    if (!gateThresh.empty() || !gateRel.empty())
    {
      if (!gateThresh.empty())
        rig.setGateThresholdDb(std::stof(gateThresh));
      if (!gateRel.empty())
        rig.setGateReleaseMs(std::stof(gateRel));
      rig.setGateEnabled(true);
    }
    if (driveEnable || !tight.empty() || !drive.empty() || !bite.empty())
    {
      if (!tight.empty())
        rig.setTight(std::stof(tight));
      if (!drive.empty())
        rig.setDrive(std::stof(drive));
      if (!bite.empty())
        rig.setBite(std::stof(bite));
      rig.setDriveEnabled(true);
    }
    if (!namPath.empty())
      rig.loadNam(namPath);
    if (!irPath.empty())
      rig.loadIr(irPath);
    MonoRing ring(1u << 15);
    ctx.rig = &rig;
    ctx.ring = &ring;

    AudioDeviceIOProcID inProc = nullptr, outProc = nullptr;
    halCheck(AudioDeviceCreateIOProcID(inDev, inputProc, &ctx, &inProc), "create input proc");
    halCheck(AudioDeviceCreateIOProcID(outDev, outputProc, &ctx, &outProc), "create output proc");
    halCheck(AudioDeviceStart(inDev, inProc), "start input");
    halCheck(AudioDeviceStart(outDev, outProc), "start output");

    std::printf("live: in=%dch out=%dch %.0fHz block<=%d nam=%s ir=%s gate=%s trim=%.1f drive=%s\n",
                ctx.inChannels, ctx.outChannels, outSr, ctx.maxBlock, rig.hasNam() ? "yes" : "no",
                rig.hasIr() ? "yes" : "no", rig.isGateEnabled() ? "on" : "off", rig.inputTrimDb(),
                rig.isDriveEnabled() ? "on" : "off");
    std::printf("press q + Enter to quit\n");
    char line[64] = {};
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
      if (line[0] == 'q' || line[0] == 'Q')
        break;

    AudioDeviceStop(outDev, outProc);
    AudioDeviceStop(inDev, inProc);
    AudioDeviceDestroyIOProcID(outDev, outProc);
    AudioDeviceDestroyIOProcID(inDev, inProc);
    std::printf("stopped: blocks=%llu underrunFrames=%llu overrunFrames=%llu\n",
                (unsigned long long)ctx.blocks.load(), (unsigned long long)ctx.underruns.load(),
                (unsigned long long)ctx.overruns.load());
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("tdm_live: error: %s\n", e.what());
    return 1;
  }
}

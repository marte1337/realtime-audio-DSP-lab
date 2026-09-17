// TdmEngine: CoreAudio duplex owner around TechDeathRig.
//
// Audio behavior is intentionally identical to the original tdm_live runner
// this was extracted from: default input averaged to mono through a
// lock-free ring, rig processing on the output thread in maxBlock-sized
// chunks, [-1, 1] interface clamp on the way out. Only the ownership and
// lifecycle changed (reusable class + stop-to-load policy).

#include "host/TdmEngine.h"

#include <CoreAudio/CoreAudio.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Private HAL access for the IO procs (lets the public header stay free of
// CoreAudio types).
struct TdmEngineAudio
{
  static OSStatus inputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* inData,
                            const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* ctx);
  static OSStatus outputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList*,
                             const AudioTimeStamp*, AudioBufferList* outData, const AudioTimeStamp*, void* ctx);
};

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
} // namespace

// Live HAL state. Owned by TdmEngine, alive only between start() and stop().
// The IO threads touch ring/scratches/rig_/counters; the control thread owns
// the rest. Touched only on the control thread outside the procs.
struct TdmEngine::Hal
{
  std::unique_ptr<MonoRing> ring;
  std::vector<float> inScratch;   // input thread only
  std::vector<float> outScratch;  // output thread only (rig channel 0 / mono)
  std::vector<float> outScratchR; // rig channel 1 when stereo
  int inChannels = 0;
  bool inInterleaved = false;
  int outChannels = 0;
  bool outInterleaved = false;
  int maxBlock = 0;
  AudioDeviceID inDev = kAudioObjectUnknown;
  AudioDeviceID outDev = kAudioObjectUnknown;
  AudioDeviceIOProcID inProc = nullptr;
  AudioDeviceIOProcID outProc = nullptr;
  bool inStarted = false;
  bool outStarted = false;
};

OSStatus TdmEngineAudio::inputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* inData,
                                   const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* ctx)
{
  auto* self = static_cast<TdmEngine*>(ctx);
  TdmEngine::Hal* h = self->hal_.get();
  // Average all input channels to mono via the preallocated scratch,
  // chunked so an unexpectedly large device block cannot overrun it.
  const UInt32 cap = static_cast<UInt32>(h->inScratch.size());
  UInt32 total = 0;
  if (h->inInterleaved)
  {
    total = inData->mBuffers[0].mDataByteSize / (sizeof(float) * static_cast<UInt32>(h->inChannels));
    const float* p = static_cast<const float*>(inData->mBuffers[0].mData);
    for (UInt32 base = 0; base < total; base += cap)
    {
      const UInt32 m = base + cap <= total ? cap : total - base;
      for (UInt32 i = 0; i < m; ++i)
      {
        double acc = 0.0;
        for (int ch = 0; ch < h->inChannels; ++ch)
          acc += p[(base + i) * h->inChannels + ch];
        h->inScratch[i] = static_cast<float>(acc / h->inChannels);
      }
      uint64_t dropped = 0;
      h->ring->push(h->inScratch.data(), m, dropped);
      if (dropped > 0)
        self->overruns_.fetch_add(dropped, std::memory_order_relaxed);
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
        for (int ch = 0; ch < h->inChannels && static_cast<UInt32>(ch) < inData->mNumberBuffers; ++ch)
          acc += static_cast<const float*>(inData->mBuffers[ch].mData)[base + i];
        h->inScratch[i] = static_cast<float>(acc / h->inChannels);
      }
      uint64_t dropped = 0;
      h->ring->push(h->inScratch.data(), m, dropped);
      if (dropped > 0)
        self->overruns_.fetch_add(dropped, std::memory_order_relaxed);
    }
  }
  return noErr;
}

OSStatus TdmEngineAudio::outputProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList*,
                                    const AudioTimeStamp*, AudioBufferList* outData, const AudioTimeStamp*, void* ctx)
{
  auto* self = static_cast<TdmEngine*>(ctx);
  TdmEngine::Hal* h = self->hal_.get();
  UInt32 frames = 0;
  if (h->outInterleaved)
    frames = outData->mBuffers[0].mDataByteSize / (sizeof(float) * static_cast<UInt32>(h->outChannels));
  else
    frames = outData->mNumberBuffers > 0 ? outData->mBuffers[0].mDataByteSize / sizeof(float) : 0;

  UInt32 done = 0;
  while (done < frames)
  {
    UInt32 m = frames - done;
    if (m > static_cast<UInt32>(h->maxBlock))
      m = static_cast<UInt32>(h->maxBlock);
    const size_t got = h->ring->pop(h->outScratch.data(), m);
    if (got < m)
    {
      self->underruns_.fetch_add(m - got, std::memory_order_relaxed);
      std::memset(h->outScratch.data() + got, 0, (m - got) * sizeof(float));
    }
    // Space made the rig stereo-capable: one device channel renders the
    // mono fold-down, two or more get the L/R pair (extra channels cycle).
    const int wantStereo = (h->outChannels >= 2) ? 2 : 1;
    const float* bi[1] = {h->outScratch.data()};
    float* bo[2] = {h->outScratch.data(), h->outScratchR.data()};
    self->rig_.processBlock(bi, 1, bo, wantStereo, static_cast<int>(m));
    for (UInt32 i = 0; i < m; ++i)
    {
      const float l = h->outScratch[i];
      const float r = wantStereo == 2 ? h->outScratchR[i] : l;
      if (h->outInterleaved)
      {
        float* p = static_cast<float*>(outData->mBuffers[0].mData);
        for (int ch = 0; ch < h->outChannels; ++ch)
        {
          float v = (ch % 2 == 0) ? l : r;
          v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); // interface clamp, as in the reference standalone
          p[(done + i) * h->outChannels + ch] = v;
        }
      }
      else
      {
        for (int ch = 0; ch < h->outChannels && static_cast<UInt32>(ch) < outData->mNumberBuffers; ++ch)
        {
          float v = (ch % 2 == 0) ? l : r;
          v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); // interface clamp, as in the reference standalone
          static_cast<float*>(outData->mBuffers[ch].mData)[done + i] = v;
        }
      }
    }
    done += m;
  }
  self->blocks_.fetch_add(1, std::memory_order_relaxed);
  return noErr;
}

TdmEngine::TdmEngine() = default;

TdmEngine::~TdmEngine()
{
  stop();
}

void TdmEngine::stop()
{
  if (hal_)
  {
    if (hal_->outStarted)
      AudioDeviceStop(hal_->outDev, hal_->outProc);
    if (hal_->inStarted)
      AudioDeviceStop(hal_->inDev, hal_->inProc);
    if (hal_->outProc != nullptr)
      AudioDeviceDestroyIOProcID(hal_->outDev, hal_->outProc);
    if (hal_->inProc != nullptr)
      AudioDeviceDestroyIOProcID(hal_->inDev, hal_->inProc);
    hal_.reset();
  }
  running_.store(false, std::memory_order_release);
}

bool TdmEngine::start(std::string& error)
{
  if (isRunning())
  {
    error = "audio is already running";
    return false;
  }
  std::unique_ptr<Hal> h(new Hal);
  try
  {
    const AudioDeviceID inDev = defaultDevice(true);
    const AudioDeviceID outDev = defaultDevice(false);
    const double inSr = deviceSampleRate(inDev);
    const double outSr = deviceSampleRate(outDev);
    if (std::fabs(inSr - outSr) > 1.0)
      throw std::runtime_error("input (" + std::to_string(inSr) + " Hz) and output (" + std::to_string(outSr)
                               + " Hz) rates differ; align them in Audio MIDI Setup");

    deviceFormat(inDev, true, h->inChannels, h->inInterleaved);
    deviceFormat(outDev, false, h->outChannels, h->outInterleaved);
    const UInt32 frameSize = deviceFrameSize(outDev);
    h->maxBlock = frameSize > 2048 ? static_cast<int>(frameSize) : 2048;
    h->inScratch.assign(1u << 14, 0.0f); // chunked in inputProc; independent of maxBlock
    h->outScratch.assign(static_cast<size_t>(h->maxBlock), 0.0f);
    h->outScratchR.assign(static_cast<size_t>(h->maxBlock), 0.0f); // rig right channel when stereo

    rig_.reset(outSr, h->maxBlock);
    // A rate change in Audio MIDI Setup between load and start must fail
    // loudly: there is no resampler, and a stale asset would play wrong.
    if (rig_.hasNam())
    {
      const double expected = rig_.namExpectedSampleRate();
      if (expected > 0.0 && std::fabs(expected - outSr) > 1.0)
        throw std::runtime_error("loaded NAM expects " + std::to_string(expected) + " Hz but the device runs at "
                                 + std::to_string(outSr) + " Hz; reload a matching model");
    }
    if (rig_.hasIr())
    {
      const double loaded = rig_.irSampleRate();
      if (loaded > 0.0 && std::fabs(loaded - outSr) > 1.0)
        throw std::runtime_error("loaded IR is " + std::to_string(loaded) + " Hz but the device runs at "
                                 + std::to_string(outSr) + " Hz; reload a matching IR");
    }
    h->ring.reset(new MonoRing(1u << 15));

    h->inDev = inDev;
    h->outDev = outDev;
    halCheck(AudioDeviceCreateIOProcID(inDev, TdmEngineAudio::inputProc, this, &h->inProc), "create input proc");
    halCheck(AudioDeviceCreateIOProcID(outDev, TdmEngineAudio::outputProc, this, &h->outProc), "create output proc");
    halCheck(AudioDeviceStart(inDev, h->inProc), "start input");
    h->inStarted = true;
    halCheck(AudioDeviceStart(outDev, h->outProc), "start output");
    h->outStarted = true;

    hal_ = std::move(h);
    sampleRate_ = outSr;
    blocks_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    return true;
  }
  catch (const std::exception& e)
  {
    if (h->outStarted)
      AudioDeviceStop(h->outDev, h->outProc);
    if (h->inStarted)
      AudioDeviceStop(h->inDev, h->inProc);
    if (h->outProc != nullptr)
      AudioDeviceDestroyIOProcID(h->outDev, h->outProc);
    if (h->inProc != nullptr)
      AudioDeviceDestroyIOProcID(h->inDev, h->inProc);
    error = e.what();
    return false;
  }
}

void TdmEngine::ensureResetForLoad()
{
  // Validates loaders against the device rate even before the first start.
  const double sr = deviceSampleRate(defaultDevice(false)); // throws when headless
  if (rig_.sampleRate() <= 0.0 || std::fabs(rig_.sampleRate() - sr) > 1.0)
    rig_.reset(sr, rig_.maxBlockSize() > 0 ? rig_.maxBlockSize() : 2048);
}

bool TdmEngine::loadNam(const std::string& path, std::string& error)
{
  if (isRunning())
  {
    error = "stop audio before loading a NAM model (v0 requires Stop -> load -> Start)";
    return false;
  }
  try
  {
    ensureResetForLoad();
    rig_.loadNam(path);
    namPath_ = path;
    return true;
  }
  catch (const std::exception& e)
  {
    error = e.what();
    return false;
  }
}

bool TdmEngine::loadIr(const std::string& path, std::string& error)
{
  if (isRunning())
  {
    error = "stop audio before loading an IR (v0 requires Stop -> load -> Start)";
    return false;
  }
  try
  {
    ensureResetForLoad();
    rig_.loadIr(path);
    irPath_ = path;
    return true;
  }
  catch (const std::exception& e)
  {
    error = e.what();
    return false;
  }
}

bool TdmEngine::listDevices(std::string& out, std::string& error)
{
  try
  {
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    halCheck(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size), "list devices");
    std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
    halCheck(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devs.data()),
             "list devices");
    out.clear();
    char line[512];
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
      std::snprintf(line, sizeof(line), "id=%u name=%s in=%.0fHz out=%.0fHz\n", d, label.c_str(),
                    deviceSampleRate(d), deviceSampleRate(d));
      out += line;
    }
    return true;
  }
  catch (const std::exception& e)
  {
    error = e.what();
    return false;
  }
}

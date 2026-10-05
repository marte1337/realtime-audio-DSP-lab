// DevTranspose: see header. GT2 path is JUCE-free; the TONE3000 path
// compiles only with TDM_HAVE_TONE3000 (the DEV binary's build flag).

#include "dsp/lab/Pitch/DevTranspose.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#ifdef TDM_HAVE_TONE3000
#include <juce_audio_basics/juce_audio_basics.h>

#include "Transpose.h"
#endif

namespace tdm
{
namespace lab
{
namespace
{
// Exact replica of Transpose::latencySamples (integer truncation included)
// so the GT2-only build reports T3K latency identically without JUCE.
int t3kLatencySamples(int windowMs, double sampleRate)
{
  const int dMin = static_cast<int>(sampleRate * 2.0 * 0.001);
  const int dMax = static_cast<int>(sampleRate * windowMs * 0.001);
  return (dMin + dMax) / 2;
}

bool validT3kWindow(int ms)
{
  return ms == 20 || ms == 30 || ms == 40 || ms == 60;
}
} // namespace

#ifdef TDM_HAVE_TONE3000
struct DevTranspose::T3kState
{
  Transpose engine;
  juce::AudioBuffer<float> buf;
  int maxBlock = 0;
};
#endif

DevTranspose::DevTranspose()
{
#ifdef TDM_HAVE_TONE3000
  t3k_ = std::unique_ptr<T3kState>(new T3kState());
#endif
  static_assert(std::atomic<bool>::is_always_lock_free, "DevTranspose needs lock-free bool atomics");
  static_assert(std::atomic<int>::is_always_lock_free, "DevTranspose needs lock-free int atomics");
  static_assert(std::atomic<float>::is_always_lock_free, "DevTranspose needs lock-free float atomics");
}

DevTranspose::~DevTranspose() = default;

void DevTranspose::configureGt2(const GuitarTranspose::Config& cfg)
{
  // Validate now (off-RT): trial reset on a scratch engine at the live
  // rate when known, else 48 kHz. Throws std::invalid_argument like GT2.
  GuitarTranspose probe;
  probe.setConfig(cfg);
  probe.setEnabled(true);
  probe.setShiftSt(shiftReq_.load(std::memory_order_relaxed));
  probe.reset(sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
  gt2Cfg_ = cfg; // stored only on success; takes effect on next reset()
}

void DevTranspose::setEngine(Engine engine)
{
  if (engine == Engine::Tone3000 && !hasTone3000())
    throw std::invalid_argument("DevTranspose: TONE3000 engine not in this build");
  engineReq_.store(static_cast<int>(engine), std::memory_order_release);
}

void DevTranspose::setShiftSt(float semitones)
{
  if (!std::isfinite(semitones))
    throw std::invalid_argument("DevTranspose: shift must be finite");
  shiftReq_.store(std::clamp(semitones, kMinShiftSt, kMaxShiftSt), std::memory_order_release);
}

void DevTranspose::setT3kWindowMs(int ms)
{
  if (!validT3kWindow(ms))
    throw std::invalid_argument("DevTranspose: T3K window must be 20/30/40/60 ms");
  t3kWindowReq_.store(ms, std::memory_order_release);
}

void DevTranspose::setT3kTonalityHz(float hz)
{
  if (!std::isfinite(hz) || hz < 0.0f)
    throw std::invalid_argument("DevTranspose: T3K tonality must be >= 0 (0 = off)");
  if (hz > 0.0f && hz < 1000.0f)
    hz = 1000.0f; // deck span floor (Transpose::kTonalityMinHz)
  if (hz > 20000.0f)
    hz = 20000.0f; // deck span ceiling (Transpose::kTonalityOffHz)
  t3kTonalityReq_.store(hz, std::memory_order_release);
}

void DevTranspose::reset(double sampleRate, int maxBlockSize)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("DevTranspose: sample rate out of range");
  if (maxBlockSize <= 0)
    throw std::invalid_argument("DevTranspose: max block size must be positive");
  sampleRate_ = sampleRate;
  maxBlock_ = maxBlockSize;

  // Adopt control state at reset so the first block is correct.
  const float shift = shiftReq_.load(std::memory_order_relaxed);
  activeEngine_ = static_cast<Engine>(engineReq_.load(std::memory_order_relaxed));
  if (activeEngine_ == Engine::Tone3000 && !hasTone3000())
    activeEngine_ = Engine::Gt2;
  switchFrom_ = activeEngine_;
  switchRamp_ = 1.0f;
  ramp_ = enabledReq_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
  appliedShift_ = shift;
  resetShift_ = shift;
  appliedT3kWindow_ = t3kWindowReq_.load(std::memory_order_relaxed);
  appliedT3kTonality_ = t3kTonalityReq_.load(std::memory_order_relaxed);

  // GT2 is permanently enabled internally: bypass lives in this wrapper
  // (the LabWsolaLive pattern). Reset at the requested shift.
  gt2_.setConfig(gt2Cfg_); // validated at configure time; throws here if hand-built
  gt2_.setEnabled(true);
  gt2_.setShiftSt(shift);
  gt2_.reset(sampleRate); // throws on bad config/rate
  gt2Latency_ = gt2_.latencySamples();
  t3kMaxLatency_ = ::tdm::lab::t3kLatencySamples(60, sampleRate);

#ifdef TDM_HAVE_TONE3000
  {
    T3kState& s = *t3k_;
    s.maxBlock = maxBlockSize;
    s.buf.setSize(1, maxBlockSize, false, false, false);
    s.engine.prepare(sampleRate, maxBlockSize);
    Transpose::Params p;
    p.semitones = shift;
    p.tonalityHz = appliedT3kTonality_;
    p.window = (appliedT3kWindow_ == 20)
        ? Transpose::Window::ms20
        : (appliedT3kWindow_ == 40) ? Transpose::Window::ms40
                                    : (appliedT3kWindow_ == 60) ? Transpose::Window::ms60
                                                                : Transpose::Window::ms30;
    s.engine.setParams(p);
    s.engine.setEnabled(true); // power blend runs from here (live: inaudible)
  }
#endif

  // Dry ring covers the deepest possible bypass delay plus one block.
  const int maxLat = std::max(gt2Latency_, t3kMaxLatency_);
  dryDelay_.assign(static_cast<size_t>(maxLat + maxBlockSize), 0.0f);
  delayWrite_ = maxLat;
  scratchA_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
  scratchB_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
}

int DevTranspose::activeLatency() const
{
  if (activeEngine_ == Engine::Tone3000)
    return ::tdm::lab::t3kLatencySamples(appliedT3kWindow_, sampleRate_);
  return gt2Latency_;
}

int DevTranspose::bypassLatency() const
{
  return activeLatency();
}

int DevTranspose::latencySamples() const
{
  if (sampleRate_ <= 0.0)
    return 0;
  const Engine e = static_cast<Engine>(engineReq_.load(std::memory_order_acquire));
  if (e == Engine::Tone3000)
    return t3kLatencySamples();
  return gt2Latency_;
}

int DevTranspose::t3kLatencySamples() const
{
  if (sampleRate_ <= 0.0)
    return 0;
  return ::tdm::lab::t3kLatencySamples(t3kWindowReq_.load(std::memory_order_acquire), sampleRate_);
}

void DevTranspose::process(const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (sampleRate_ <= 0.0 || static_cast<int>(dryDelay_.size()) == 0)
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f; // reset-never: silence, like the engines
    return;
  }

  // Block-boundary adoption (audio thread only from here on).
  const float target = enabledReq_.load(std::memory_order_acquire) ? 1.0f : 0.0f;
  const Engine wantEngine = static_cast<Engine>(engineReq_.load(std::memory_order_acquire));
  if (wantEngine != activeEngine_ && (wantEngine == Engine::Gt2 || hasTone3000()))
  {
    switchFrom_ = activeEngine_;
    activeEngine_ = wantEngine;
    switchRamp_ = 0.0f;
  }
  const float wantShift = shiftReq_.load(std::memory_order_acquire);
  const bool shiftMoved = (wantShift != appliedShift_);
  if (shiftMoved)
  {
    appliedShift_ = wantShift;
    gt2_.setShiftSt(wantShift); // per-sample adoption, no alloc (GT2 RT-safe)
  }
#ifdef TDM_HAVE_TONE3000
  const int wantWindow = t3kWindowReq_.load(std::memory_order_acquire);
  const float wantTonality = t3kTonalityReq_.load(std::memory_order_acquire);
  if (shiftMoved || wantWindow != appliedT3kWindow_ || wantTonality != appliedT3kTonality_)
  {
    appliedT3kWindow_ = wantWindow;
    appliedT3kTonality_ = wantTonality;
    T3kState& s = *t3k_;
    Transpose::Params p;
    p.semitones = wantShift;
    p.tonalityHz = wantTonality;
    p.window = (wantWindow == 20)
        ? Transpose::Window::ms20
        : (wantWindow == 40) ? Transpose::Window::ms40
                             : (wantWindow == 60) ? Transpose::Window::ms60 : Transpose::Window::ms30;
    s.engine.setParams(p); // documented audio-thread safe, no alloc
  }
#endif

  const int ring = static_cast<int>(dryDelay_.size());
  // Pass 1: tap dry BEFORE any engine runs (in-place safe: input may alias
  // output, and neither engine may clobber the tapped dry).
  for (int i = 0; i < numFrames; ++i)
    dryDelay_[static_cast<size_t>((delayWrite_ + i) % ring)] = input[i];

  // Pass 2: render engines into scratch. Standby renders whenever warming
  // or mid-switch-crossfade; otherwise only the active engine runs.
  const bool switching = (switchRamp_ < 1.0f);
  const bool runGt2 = (activeEngine_ == Engine::Gt2) || (switching && switchFrom_ == Engine::Gt2)
      || (warmStandby_ && hasTone3000());
  const bool runT3k = hasTone3000()
      && ((activeEngine_ == Engine::Tone3000) || (switching && switchFrom_ == Engine::Tone3000)
          || warmStandby_);
  if (runGt2)
    gt2_.processBlock(input, scratchA_.data(), numFrames);
#ifdef TDM_HAVE_TONE3000
  if (runT3k)
  {
    T3kState& s = *t3k_;
    s.buf.setSize(1, numFrames, false, false, true); // no realloc (<= maxBlock)
    float* ch = s.buf.getWritePointer(0);
    for (int i = 0; i < numFrames; ++i)
      ch[i] = input[i];
    s.engine.process(s.buf);
    const float* out = s.buf.getReadPointer(0);
    for (int i = 0; i < numFrames; ++i)
      scratchB_[static_cast<size_t>(i)] = out[i];
  }
#else
  (void)runT3k;
#endif

  // Pass 3: engine select (+ switch crossfade), then bypass ramp.
  const int lat = bypassLatency();
  const float* wetA = (activeEngine_ == Engine::Gt2) ? scratchA_.data() : scratchB_.data();
  const float* wetFrom = (switchFrom_ == Engine::Gt2) ? scratchA_.data() : scratchB_.data();
  for (int i = 0; i < numFrames; ++i)
  {
    float wet = wetA[static_cast<size_t>(i)];
    if (switching)
    {
      const float t = switchRamp_;
      wet = wetFrom[static_cast<size_t>(i)] + t * (wet - wetFrom[static_cast<size_t>(i)]);
      switchRamp_ = std::min(1.0f, t + 1.0f / static_cast<float>(kRampSamples));
      if (switchRamp_ >= 1.0f)
      {
        switchFrom_ = activeEngine_; // park: steady-state fast paths below
        wet = wetA[static_cast<size_t>(i)];
      }
    }
    const float dryLate = dryDelay_[static_cast<size_t>((delayWrite_ + i - lat) % ring)];
    if (ramp_ == 1.0f && target == 1.0f)
    {
      output[i] = wet;
    }
    else if (ramp_ == 0.0f && target == 0.0f)
    {
      output[i] = dryLate;
    }
    else
    {
      if (ramp_ < target)
        ramp_ = std::min(target, ramp_ + 1.0f / static_cast<float>(kRampSamples));
      else if (ramp_ > target)
        ramp_ = std::max(target, ramp_ - 1.0f / static_cast<float>(kRampSamples));
      output[i] = (ramp_ == 1.0f) ? wet : (ramp_ == 0.0f) ? dryLate : dryLate + ramp_ * (wet - dryLate);
    }
  }
  delayWrite_ += numFrames;
}

const char* devEngineName(DevTranspose::Engine engine)
{
  return engine == DevTranspose::Engine::Tone3000 ? "t3k-ref" : "ours-gt2";
}

bool parseDevEngine(const std::string& s, DevTranspose::Engine& engine)
{
  if (s == "gt2" || s == "ours" || s == "ours-gt2")
  {
    engine = DevTranspose::Engine::Gt2;
    return true;
  }
  if (s == "t3k" || s == "ref" || s == "t3k-ref" || s == "tone3000")
  {
    engine = DevTranspose::Engine::Tone3000;
    return true;
  }
  return false;
}
} // namespace lab
} // namespace tdm

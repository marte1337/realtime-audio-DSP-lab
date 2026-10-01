// BenchTone3000: see header. Research/benchmark use only (MIT engine +
// AGPLv3 JUCE dependency; local benchmarking, no distribution).

#include "dsp/lab/bench/BenchTone3000.h"

#include <string>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Transpose.h"

namespace tdm
{
namespace bench
{
struct BenchTone3000::Impl
{
  BenchTone3000::Window window = BenchTone3000::Window::Ms30;
  float tonalityHz = 0.0f;
  double sampleRate = 0.0;
  int maxBlock = 0;
  double shiftSt = 0.0;

  std::unique_ptr<Transpose> t;
  juce::AudioBuffer<float> buf;

  std::string idStr;
  std::string configStr;
  Transpose::Params paramsForEngine() const
  {
    Transpose::Params p;
    p.semitones = static_cast<float>(shiftSt);
    p.tonalityHz = tonalityHz;
    switch (window)
    {
    case BenchTone3000::Window::Ms20:
      p.window = Transpose::Window::ms20;
      break;
    case BenchTone3000::Window::Ms40:
      p.window = Transpose::Window::ms40;
      break;
    case BenchTone3000::Window::Ms60:
      p.window = Transpose::Window::ms60;
      break;
    case BenchTone3000::Window::Ms30:
    default:
      p.window = Transpose::Window::ms30;
      break;
    }
    return p;
  }
};

BenchTone3000::BenchTone3000(Window window, float tonalityHz) : impl_(new Impl())
{
  Impl& d = *impl_;
  d.window = window;
  d.tonalityHz = tonalityHz > 0.0f ? tonalityHz : 0.0f;
  const int ms = (window == Window::Ms20) ? 20 : (window == Window::Ms40) ? 40 : (window == Window::Ms60) ? 60 : 30;
  d.idStr = "t3k" + std::to_string(ms);
  d.configStr = "TONE3000 b8461cc Transpose, " + std::to_string(ms) + " ms buffer, tonality ";
  if (d.tonalityHz > 0.0f)
  {
    d.idStr += "t";
    d.configStr += std::to_string(static_cast<int>(d.tonalityHz)) + " Hz";
  }
  else
  {
    d.configStr += "off";
  }
}

BenchTone3000::~BenchTone3000() = default;

const char* BenchTone3000::id() const
{
  return impl_->idStr.c_str();
}

const char* BenchTone3000::config() const
{
  return impl_->configStr.c_str();
}

void BenchTone3000::setShiftSemitones(double semitones)
{
  impl_->shiftSt = semitones > 0.0 ? 0.0 : semitones;
}

void BenchTone3000::prepare(double sampleRate, int maxBlock)
{
  Impl& d = *impl_;
  d.sampleRate = sampleRate;
  d.maxBlock = maxBlock > 0 ? maxBlock : 1;
  d.buf.setSize(1, d.maxBlock, false, false, false);
  reset();
}

void BenchTone3000::reset()
{
  Impl& d = *impl_;
  // Fresh engine instance (off-RT): identical deterministic state every
  // render. Power on + fixed params for the run.
  d.t = std::make_unique<Transpose>();
  d.t->prepare(d.sampleRate, d.maxBlock);
  d.t->setParams(d.paramsForEngine());
  d.t->setEnabled(true);
}

void BenchTone3000::processBlock(const float* input, float* output, int n)
{
  Impl& d = *impl_;
  if (n <= 0 || !d.t)
    return;
  // Fit the preallocated buffer to this block without reallocating.
  d.buf.setSize(1, n, false, false, true);
  float* ch = d.buf.getWritePointer(0);
  for (int i = 0; i < n; ++i)
    ch[i] = input[i];
  d.t->process(d.buf);
  const float* out = d.buf.getReadPointer(0);
  for (int i = 0; i < n; ++i)
    output[i] = out[i];
}

int BenchTone3000::latencySamples() const
{
  const Impl& d = *impl_;
  return Transpose::latencySamples(d.paramsForEngine().window, d.sampleRate);
}

int BenchTone3000::tailSamples() const
{
  return 0; // fully causal: no lookahead, no flush requirement
}
} // namespace bench
} // namespace tdm

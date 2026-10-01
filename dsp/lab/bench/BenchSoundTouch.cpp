// BenchSoundTouch: see header. Research/benchmark use only (LGPL).

#include "dsp/lab/bench/BenchSoundTouch.h"

#include <string>
#include <vector>

#include "SoundTouch.h"

using namespace soundtouch;

namespace tdm
{
namespace bench
{
struct BenchSoundTouch::Impl
{
  double sampleRate = 0.0;
  int maxBlock = 0;
  double shiftSt = 0.0;

  std::unique_ptr<SoundTouch> st;

  std::vector<float> fifo;
  int fifoRead = 0;
  int fifoCount = 0;

  int latency = 0;
  int tail = 0;
  std::string configStr = "SoundTouch 2.4.1 (TDStretch+RateTransposer, default auto tables)";
};

BenchSoundTouch::BenchSoundTouch() : impl_(new Impl()) {}
BenchSoundTouch::~BenchSoundTouch() = default;

const char* BenchSoundTouch::id() const
{
  return "st";
}

const char* BenchSoundTouch::config() const
{
  return impl_->configStr.c_str();
}

void BenchSoundTouch::setShiftSemitones(double semitones)
{
  impl_->shiftSt = semitones > 0.0 ? 0.0 : semitones;
}

void BenchSoundTouch::prepare(double sampleRate, int maxBlock)
{
  Impl& d = *impl_;
  d.sampleRate = sampleRate;
  d.maxBlock = maxBlock;
  reset();
}

void BenchSoundTouch::reset()
{
  Impl& d = *impl_;
  d.st.reset(new SoundTouch());
  d.st->setSampleRate(static_cast<uint>(d.sampleRate));
  d.st->setChannels(1);
  d.st->setPitchSemiTones(d.shiftSt);
  // Prime the pipeline with silence and read the steady-state depth as
  // the reported latency figure (SoundTouch has no start-delay API).
  d.fifoRead = 0;
  d.fifoCount = 0;
  const int prime = static_cast<int>(d.sampleRate * 1.5);
  std::vector<float> zeros(static_cast<size_t>(prime), 0.0f);
  d.st->putSamples(zeros.data(), static_cast<uint>(prime));
  d.latency = static_cast<int>(d.st->numUnprocessedSamples());
  d.st->clear();
  d.fifo.assign(static_cast<size_t>(d.latency + 2 * d.maxBlock + 1024), 0.0f);
  d.tail = d.latency + d.maxBlock;
}

void BenchSoundTouch::processBlock(const float* input, float* output, int n)
{
  Impl& d = *impl_;
  if (n <= 0)
    return;
  const int cap = static_cast<int>(d.fifo.size());
  d.st->putSamples(input, static_cast<uint>(n));
  float tmp[8192];
  for (;;)
  {
    const uint got = d.st->receiveSamples(tmp, sizeof(tmp) / sizeof(tmp[0]));
    if (got == 0)
      break;
    for (uint i = 0; i < got && d.fifoCount < cap; ++i)
    {
      d.fifo[static_cast<size_t>((d.fifoRead + d.fifoCount) % cap)] = tmp[i];
      ++d.fifoCount;
    }
  }
  for (int i = 0; i < n; ++i)
  {
    float v = 0.0f;
    if (d.fifoCount > 0)
    {
      v = d.fifo[static_cast<size_t>(d.fifoRead)];
      d.fifoRead = (d.fifoRead + 1) % cap;
      --d.fifoCount;
    }
    output[i] = v;
  }
}

int BenchSoundTouch::latencySamples() const
{
  return impl_->latency;
}

int BenchSoundTouch::tailSamples() const
{
  return impl_->tail;
}
} // namespace bench
} // namespace tdm

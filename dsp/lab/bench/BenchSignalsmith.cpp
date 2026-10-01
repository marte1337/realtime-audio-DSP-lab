// BenchSignalsmith: see header. MIT dependency, research use.

#include "dsp/lab/bench/BenchSignalsmith.h"

#include <string>
#include <vector>

#include "signalsmith-stretch.h"

namespace tdm
{
namespace bench
{
namespace
{
constexpr long kSeed = 0x51ab3; // deterministic (only used past 2x)
} // namespace

struct BenchSignalsmith::Impl
{
  double sampleRate = 0.0;
  int maxBlock = 0;
  double shiftSt = 0.0;

  std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> st;

  // process() scratch (channel-pointer arrays + warmup buffers).
  std::vector<float> warm;
  const float* inCh[1] = {nullptr};
  float* outCh[1] = {nullptr};

  int latency = 0;
  int tail = 0;
  std::string configStr = "Signalsmith Stretch 1.4.0 presetDefault + transpose";
};

BenchSignalsmith::BenchSignalsmith() : impl_(new Impl()) {}
BenchSignalsmith::~BenchSignalsmith() = default;

const char* BenchSignalsmith::id() const
{
  return "ss";
}

const char* BenchSignalsmith::config() const
{
  return impl_->configStr.c_str();
}

void BenchSignalsmith::setShiftSemitones(double semitones)
{
  impl_->shiftSt = semitones > 0.0 ? 0.0 : semitones;
}

void BenchSignalsmith::prepare(double sampleRate, int maxBlock)
{
  Impl& d = *impl_;
  d.sampleRate = sampleRate;
  d.maxBlock = maxBlock;
  d.warm.assign(static_cast<size_t>(maxBlock), 0.0f);
  reset();
}

void BenchSignalsmith::reset()
{
  Impl& d = *impl_;
  d.st.reset(new signalsmith::stretch::SignalsmithStretch<float>(kSeed));
  d.st->presetDefault(1, static_cast<float>(d.sampleRate));
  d.st->setTransposeSemitones(static_cast<float>(d.shiftSt));
  d.latency = d.st->inputLatency() + d.st->outputLatency();
  d.tail = d.latency + d.maxBlock;
  // Pre-warm lazy tmp buffers with a silent block (off-RT).
  d.inCh[0] = d.warm.data();
  d.outCh[0] = d.warm.data();
  d.st->process(d.inCh, d.maxBlock, d.outCh, d.maxBlock);
  d.st->reset();
  d.st->setTransposeSemitones(static_cast<float>(d.shiftSt));
}

void BenchSignalsmith::processBlock(const float* input, float* output, int n)
{
  Impl& d = *impl_;
  if (n <= 0)
    return;
  // Equal counts => time rate 1; pitch comes from the transpose map.
  // process() consumes exactly n and produces exactly n (documented
  // block contract); n <= maxBlock by the prepare() bound.
  const float* inCh[1] = {input};
  float* outCh[1] = {output};
  d.st->process(inCh, n, outCh, n);
}

int BenchSignalsmith::latencySamples() const
{
  return impl_->latency;
}

int BenchSignalsmith::tailSamples() const
{
  return impl_->tail;
}
} // namespace bench
} // namespace tdm

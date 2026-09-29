// BenchRubberBand: see header. Research/benchmark use only (GPL).

#include "dsp/lab/bench/BenchRubberBand.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "rubberband/RubberBandLiveShifter.h"
#include "rubberband/RubberBandStretcher.h"

namespace tdm
{
namespace bench
{
namespace
{
constexpr int kBlockCap = 8192; // hard cap on processBlock n (FIFO guard)
} // namespace

struct BenchRubberBand::Impl
{
  Mode mode = Mode::R2Realtime;
  double sampleRate = 0.0;
  int maxBlock = 0;
  double shiftSt = 0.0;

  std::unique_ptr<RubberBand::RubberBandStretcher> st;
  std::unique_ptr<RubberBand::RubberBandLiveShifter> live;

  // Fixed-block adapter FIFO (preallocated in prepare; plain indices).
  std::vector<float> fifo;
  int fifoRead = 0;
  int fifoCount = 0;
  // Live-mode input assembler (batches to the fixed block size) and
  // preallocated shift I/O block (no allocation on the audio path).
  std::vector<float> asm_;
  std::vector<float> liveBlk_;
  int asmCount = 0;
  int liveBlock = 0;

  int latency = 0;
  int tail = 0;
  std::string configStr;
};

BenchRubberBand::BenchRubberBand(Mode mode) : impl_(new Impl())
{
  impl_->mode = mode;
}

BenchRubberBand::~BenchRubberBand() = default;

const char* BenchRubberBand::id() const
{
  return impl_->mode == Mode::R2Realtime ? "rb2" : "rblive";
}

const char* BenchRubberBand::config() const
{
  return impl_->configStr.c_str();
}

void BenchRubberBand::setShiftSemitones(double semitones)
{
  impl_->shiftSt = semitones > 0.0 ? 0.0 : semitones;
}

void BenchRubberBand::prepare(double sampleRate, int maxBlock)
{
  Impl& d = *impl_;
  d.sampleRate = sampleRate;
  d.maxBlock = maxBlock > kBlockCap ? kBlockCap : maxBlock;
  reset();
}

void BenchRubberBand::reset()
{
  Impl& d = *impl_;
  const double ratio = std::exp2(d.shiftSt / 12.0);
  d.fifoRead = 0;
  d.fifoCount = 0;
  d.asmCount = 0;
  if (d.mode == Mode::R2Realtime)
  {
    using Opt = RubberBand::RubberBandStretcher::Option;
    const int opts = Opt::OptionProcessRealTime | Opt::OptionEngineFaster | Opt::OptionTransientsMixed |
        Opt::OptionDetectorCompound | Opt::OptionPhaseLaminar | Opt::OptionThreadingNever |
        Opt::OptionWindowStandard | Opt::OptionSmoothingOff | Opt::OptionFormantShifted |
        Opt::OptionPitchHighQuality | Opt::OptionChannelsTogether;
    d.live.reset();
    d.st.reset(new RubberBand::RubberBandStretcher(
        static_cast<size_t>(d.sampleRate), 1,
        static_cast<RubberBand::RubberBandStretcher::Options>(opts), 1.0, ratio));
    d.st->setMaxProcessSize(static_cast<size_t>(d.maxBlock));
    d.latency = static_cast<int>(d.st->getStartDelay());
    // FIFO must cover the latency backlog plus a block either side.
    d.fifo.assign(static_cast<size_t>(d.latency + 2 * d.maxBlock + 64), 0.0f);
    d.tail = d.latency + d.maxBlock;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "RubberBand 4.0.0 R2 realtime (Faster/Mixed/Compound/Laminar/1-thread/StdWin/"
                  "noSmooth/shiftedFormant/HQresample), engine=%d",
                  d.st->getEngineVersion());
    d.configStr = buf;
  }
  else
  {
    using LOpt = RubberBand::RubberBandLiveShifter::Option;
    const int opts = LOpt::OptionFormantShifted;
    d.st.reset();
    d.live.reset(new RubberBand::RubberBandLiveShifter(
        static_cast<size_t>(d.sampleRate), 1,
        static_cast<RubberBand::RubberBandLiveShifter::Options>(opts)));
    d.live->setPitchScale(ratio);
    d.liveBlock = static_cast<int>(d.live->getBlockSize());
    d.asm_.assign(static_cast<size_t>(d.liveBlock), 0.0f);
    d.liveBlk_.assign(static_cast<size_t>(d.liveBlock), 0.0f);
    d.fifo.assign(static_cast<size_t>(d.liveBlock * 4 + 64), 0.0f);
    // Content latency = the library start delay exactly (block assembly
    // only batches; it does not shift content). Live output EMERGENCE
    // adds +0..blockSize samples by onset phase (measured separately).
    d.latency = static_cast<int>(d.live->getStartDelay());
    d.tail = d.latency + d.liveBlock;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "RubberBand 4.0.0 LiveShifter (R3Live, block=%d, shiftedFormant)", d.liveBlock);
    d.configStr = buf;
  }
  // NOTE: no silence pre-roll here by design. reset() = fresh
  // construction, matching our own studies' convention (fresh-state
  // renders everywhere; priming measurably changes rb2 startup-path
  // behavior). Live hosts pre-roll 2*(lat+tail) zeros in start()
  // (off-RT) so lazy FIFO growth never lands on an audible block.
}

void BenchRubberBand::processBlock(const float* input, float* output, int n)
{
  Impl& d = *impl_;
  if (n <= 0)
    return;
  if (n > static_cast<int>(d.fifo.size()) / 2)
    n = static_cast<int>(d.fifo.size()) / 2; // never overrun the FIFO
  const int cap = static_cast<int>(d.fifo.size());
  auto fifoPush = [&](float v) {
    if (d.fifoCount < cap)
    {
      d.fifo[static_cast<size_t>((d.fifoRead + d.fifoCount) % cap)] = v;
      ++d.fifoCount;
    }
  };
  auto fifoPop = [&]() {
    float v = 0.0f;
    if (d.fifoCount > 0)
    {
      v = d.fifo[static_cast<size_t>(d.fifoRead)];
      d.fifoRead = (d.fifoRead + 1) % cap;
      --d.fifoCount;
    }
    return v;
  };
  if (d.mode == Mode::R2Realtime)
  {
    const float* inCh[1] = {input};
    d.st->process(inCh, static_cast<size_t>(n), false);
    float tmp[8192];
    for (;;)
    {
      const int avail = d.st->available();
      if (avail <= 0)
        break;
      size_t want = static_cast<size_t>(avail);
      if (want > sizeof(tmp) / sizeof(tmp[0]))
        want = sizeof(tmp) / sizeof(tmp[0]);
      float* outCh[1] = {tmp};
      // Never request more than available: retrieve() warns and
      // short-returns otherwise (found by smoke test).
      const size_t got = d.st->retrieve(outCh, want);
      if (got == 0)
        break;
      for (size_t i = 0; i < got; ++i)
        fifoPush(tmp[i]);
    }
    for (int i = 0; i < n; ++i)
      output[i] = fifoPop();
  }
  else
  {
    for (int i = 0; i < n; ++i)
    {
      d.asm_[static_cast<size_t>(d.asmCount++)] = input[i];
      if (d.asmCount == d.liveBlock)
      {
        const float* inCh[1] = {d.asm_.data()};
        float* outCh[1] = {d.liveBlk_.data()};
        d.live->shift(inCh, outCh);
        for (int k = 0; k < d.liveBlock; ++k)
          fifoPush(d.liveBlk_[static_cast<size_t>(k)]);
        d.asmCount = 0;
      }
    }
    for (int i = 0; i < n; ++i)
      output[i] = fifoPop();
  }
}

int BenchRubberBand::latencySamples() const
{
  return impl_->latency;
}

int BenchRubberBand::tailSamples() const
{
  return impl_->tail;
}
} // namespace bench
} // namespace tdm

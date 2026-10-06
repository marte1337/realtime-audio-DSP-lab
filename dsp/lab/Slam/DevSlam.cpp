// DevSlam: DEV-only SLAM flavour router. See DevSlam.h.

#include "dsp/lab/Slam/DevSlam.h"

#include <cmath>

namespace tdm
{
namespace lab
{
namespace
{
float clamp01(float v)
{
  return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}
} // namespace

DevSlam::DevSlam()
{
  preTap_.bind(this, PreDrive);
  postNamTap_.bind(this, PostNam);
  postIrTap_.bind(this, PostIr);
}

void DevSlam::setEnabled(bool enabled)
{
  enabledTarget_.store(enabled, std::memory_order_relaxed);
}
void DevSlam::setFlavor(Flavor f)
{
  flavorTarget_.store(static_cast<int>(f), std::memory_order_relaxed);
}
void DevSlam::setAmount01(float v)
{
  amountTarget_.store(clamp01(v), std::memory_order_relaxed);
}

void DevSlam::TapAdapter::reset(double sampleRate, int maxBlockSize)
{
  if (owner_ != nullptr)
    owner_->resetAudio(sampleRate, maxBlockSize);
}
void DevSlam::TapAdapter::process(const float* input, float* output, int numFrames)
{
  if (owner_ != nullptr)
    owner_->processTap(tap_, input, output, numFrames);
}

void DevSlam::resetAudio(double sampleRate, int maxBlockSize)
{
  sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
  wet_.assign(static_cast<size_t>(maxBlockSize > 0 ? maxBlockSize : 1), 0.0f);
  // Park at the UI targets: post-Start state matches the UI immediately,
  // fresh zero-state branches (no stale transients).
  activeEnabled_ = enabledTarget_.load(std::memory_order_relaxed);
  activeFlavor_ = static_cast<Flavor>(flavorTarget_.load(std::memory_order_relaxed));
  amount_ = clamp01(amountTarget_.load(std::memory_order_relaxed));
  m_ = activeEnabled_ ? 1.0f : 0.0f;
  phase_ = Steady;
  adoptPending_ = false;
  rampLeft_ = 0;
  rampStep_ = 0.0f;
  preAbs_ = 0;
  postAbs_ = 0;
  push_.reset(sr_);
  push_.setBandHz(140.0f); // study A1 voice
  push_.setAmount(amount_);
  crush_.reset(sr_);
  crush_.setBandHz(200.0f); // study B voice
  crush_.setAmount(amount_);
  mass_.reset(sr_);
  mass_.setBandHz(220.0f); // study C voice
  mass_.setAmount(amount_);
  trig_.reset(sr_, 0.5f, 90.0f);
  burst_.reset(sr_, 70.0f, 48.0f, 130.0f, 1.5f, 0.75f); // study dhyb voice
  bloomLp_.reset();
  bloomLp_.setLowpass(sr_, 105.0f, 0.7f);
  bloomDecay_ = static_cast<float>(std::exp(-6.907755278982137 / (0.070 * sr_)));
  bloom_ = 0.0f;
  dipN_ = static_cast<int>(0.004 * sr_ + 0.5);
  riseN_ = static_cast<int>(0.008 * sr_ + 0.5);
  if (dipN_ < 1)
    dipN_ = 1;
  if (riseN_ < 1)
    riseN_ = 1;
  clearImpactPending();
  lastTrigCount_ = 0;
  uiFireCount_.store(0, std::memory_order_relaxed);
}

void DevSlam::clearImpactPending()
{
  pendCount_ = 0;
}

void DevSlam::adoptTargets()
{
  const bool tEn = enabledTarget_.load(std::memory_order_relaxed);
  const Flavor tFl = static_cast<Flavor>(flavorTarget_.load(std::memory_order_relaxed));
  const float tAm = clamp01(amountTarget_.load(std::memory_order_relaxed));
  // Amount glides toward target (halving per block: ~1% in 7 blocks).
  amount_ += (tAm - amount_) * 0.5f;
  if (std::fabs(tAm - amount_) < 1e-4f)
    amount_ = tAm;
  push_.setAmount(amount_);
  crush_.setAmount(amount_);
  mass_.setAmount(amount_);
  // Deferred dip-bottom adoption first: the switch always lands on a
  // block boundary (mid-block activation would blend the outgoing
  // flavour's whole-block render with the incoming ramp: click).
  if (adoptPending_)
  {
    adoptPending_ = false;
    adoptNow(tEn, tFl);
    return;
  }
  // Enable/flavour change dips through dry (adopt at the dip bottom, or
  // immediately when already dry).
  if (tEn != activeEnabled_ || tFl != activeFlavor_)
  {
    if (m_ == 0.0f)
      adoptNow(tEn, tFl);
    else if (phase_ != Dip)
    {
      phase_ = Dip;
      rampLeft_ = dipN_;
      rampStep_ = (0.0f - m_) / static_cast<float>(dipN_);
    }
  }
}

void DevSlam::adoptNow(bool tEn, Flavor tFl)
{
  activeEnabled_ = tEn;
  activeFlavor_ = tFl;
  if (tFl != Flavor::Impact)
    clearImpactPending(); // stale fires must not leak into other flavours
  // Fresh output state for the incoming flavour (masks nothing: m == 0,
  // rise covers the settle). Impact keeps its trigger floor warm (no
  // reset) so in-flight confirms and adaptation survive the switch.
  switch (tFl)
  {
  case Flavor::Push:
    push_.reset(sr_);
    push_.setAmount(amount_);
    break;
  case Flavor::Crush:
    crush_.reset(sr_);
    crush_.setAmount(amount_);
    break;
  case Flavor::Mass:
    mass_.reset(sr_);
    mass_.setAmount(amount_);
    break;
  case Flavor::Impact:
    burst_.reset(sr_, 70.0f, 48.0f, 130.0f, 1.5f, 0.75f);
    bloomLp_.reset();
    bloomLp_.setLowpass(sr_, 105.0f, 0.7f);
    bloom_ = 0.0f;
    break;
  }
  const float goal = activeEnabled_ ? 1.0f : 0.0f;
  if (m_ == goal)
  {
    phase_ = Steady;
    rampLeft_ = 0;
  }
  else
  {
    phase_ = Rise;
    rampLeft_ = riseN_;
    rampStep_ = (goal - m_) / static_cast<float>(riseN_);
  }
}

void DevSlam::advanceRamp()
{
  if (phase_ == Steady)
    return;
  if (--rampLeft_ <= 0)
  {
    if (phase_ == Dip)
    {
      m_ = 0.0f;
      phase_ = Steady; // parked dry; the next block adopts the switch
      adoptPending_ = true;
    }
    else // Rise
    {
      m_ = activeEnabled_ ? 1.0f : 0.0f;
      phase_ = Steady;
    }
  }
  else
  {
    m_ += rampStep_;
  }
}

bool DevSlam::detectorArmed() const
{
  const bool tEn = enabledTarget_.load(std::memory_order_relaxed);
  const Flavor tFl = static_cast<Flavor>(flavorTarget_.load(std::memory_order_relaxed));
  return (activeEnabled_ || tEn) && (activeFlavor_ == Flavor::Impact || tFl == Flavor::Impact);
}

void DevSlam::pushPending(int64_t absSample, float strength)
{
  if (pendCount_ >= kMaxPending)
  {
    for (int i = 1; i < kMaxPending; ++i)
    {
      pendAbs_[i - 1] = pendAbs_[i];
      pendStr_[i - 1] = pendStr_[i];
    }
    --pendCount_;
  }
  pendAbs_[pendCount_] = absSample;
  pendStr_[pendCount_] = strength;
  ++pendCount_;
}

float DevSlam::renderImpactSample(float dry, int64_t absSample)
{
  while (pendCount_ > 0 && pendAbs_[0] <= absSample)
  {
    burst_.trigger(pendStr_[0]);
    bloom_ = 2.0f * pendStr_[0];
    for (int i = 1; i < pendCount_; ++i)
    {
      pendAbs_[i - 1] = pendAbs_[i];
      pendStr_[i - 1] = pendStr_[i];
    }
    --pendCount_;
  }
  const float low = bloomLp_.process(dry);
  const float burst = burst_.process();
  float y = dry + amount_ * (burst + bloom_ * low);
  if (!std::isfinite(y))
    y = dry;
  bloom_ *= bloomDecay_;
  if (bloom_ < 1e-5f)
    bloom_ = 0.0f;
  return y;
}

void DevSlam::processTap(Tap tap, const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (tap == PreDrive)
    adoptTargets(); // first tap each block: block-boundary adoption
  // Post-IR timeline advances on EVERY post-IR block (any flavour), so
  // burst dues stay aligned with the detector across flavour switches.
  const int64_t postBase = postAbs_;
  if (tap == PostIr)
    postAbs_ += numFrames;
  // Impact detector observes the pre-drive tap (read-only; io untouched).
  if (tap == PreDrive)
  {
    if (detectorArmed())
    {
      for (int i = 0; i < numFrames; ++i)
      {
        const float s = trig_.feed(input[i]);
        if (s > 0.0f)
          pushPending(preAbs_ + i + 1, s); // +1: study scheduled timing
      }
      const int64_t now = trig_.fireCount();
      if (now != lastTrigCount_)
      {
        lastTrigCount_ = now;
        uiFireCount_.store(now, std::memory_order_relaxed); // LED telemetry
      }
    }
    preAbs_ += numFrames;
  }
  // Route: exactly one flavour renders output.
  const bool serves = (activeFlavor_ == Flavor::Push && tap == PreDrive)
      || (activeFlavor_ == Flavor::Crush && tap == PostNam)
      || ((activeFlavor_ == Flavor::Mass || activeFlavor_ == Flavor::Impact) && tap == PostIr);
  if (!serves || !activeEnabled_)
    return; // io untouched (bit-transparent)
  if (phase_ == Steady && m_ == 0.0f)
    return; // disabled steady: transparent, no CPU
  if (activeFlavor_ == Flavor::Impact)
  {
    for (int i = 0; i < numFrames; ++i)
    {
      const float dry = input[i];
      const float y = renderImpactSample(dry, postBase + i); // states always advance
      float out;
      if (m_ == 1.0f)
        out = y; // steady-state shortcut: study-exact render
      else if (m_ == 0.0f)
        out = dry;
      else
        out = dry + m_ * (y - dry);
      output[i] = out;
      advanceRamp();
    }
    return;
  }
  // Push / Crush / Mass: render into scratch, crossfade by m. Always
  // rendered (even at Amount 0, where the flavour is dry-exact by
  // contract): no stale filter state when Amount returns.
  switch (activeFlavor_)
  {
  case Flavor::Push:
    push_.processBlock(input, wet_.data(), numFrames);
    break;
  case Flavor::Crush:
    crush_.processBlock(input, wet_.data(), numFrames);
    break;
  default:
    mass_.processBlock(input, wet_.data(), numFrames);
    break;
  }
  for (int i = 0; i < numFrames; ++i)
  {
    float out;
    if (m_ == 1.0f)
      out = wet_[static_cast<size_t>(i)]; // steady-state: study-exact
    else if (m_ == 0.0f)
      out = input[i];
    else
      out = input[i] + m_ * (wet_[static_cast<size_t>(i)] - input[i]);
    output[i] = out;
    advanceRamp();
  }
}

} // namespace lab
} // namespace tdm

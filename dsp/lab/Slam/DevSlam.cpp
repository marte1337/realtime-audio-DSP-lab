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
float clampBoost(float v)
{
  return (v < 0.0f) ? 0.0f : ((v > 2.0f) ? 2.0f : v);
}
float clampDbg(float v)
{
  return (v < 1.0f) ? 1.0f : ((v > 8.0f) ? 8.0f : v);
}
SlamImpactV2::Voice mapVoice(DevSlam::ImpactVoice v)
{
  switch (v)
  {
  case DevSlam::ImpactVoice::Sub:
    return SlamImpactV2::Voice::Sub;
  case DevSlam::ImpactVoice::Punch:
    return SlamImpactV2::Voice::Punch;
  case DevSlam::ImpactVoice::Thump:
  default:
    return SlamImpactV2::Voice::Thump; // Legacy parks here (unused)
  }
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
void DevSlam::setImpactVoice(ImpactVoice v)
{
  impactVoiceTarget_.store(static_cast<int>(v), std::memory_order_relaxed);
}
void DevSlam::setImpactSensitivity(float v)
{
  impactSensTarget_.store(clamp01(v), std::memory_order_relaxed);
}
void DevSlam::setImpactTestBoost(float v)
{
  impactBoostTarget_.store(clampBoost(v), std::memory_order_relaxed);
}
void DevSlam::setImpactDebugSolo(bool solo)
{
  soloTarget_.store(solo, std::memory_order_relaxed);
}
void DevSlam::setImpactDebugGain(float v)
{
  dbgGainTarget_.store(clampDbg(v), std::memory_order_relaxed);
}
void DevSlam::setImpactTestRef(bool ref)
{
  refTarget_.store(ref, std::memory_order_relaxed);
}
void DevSlam::setImpactGainLaw(ImpactGainLaw l)
{
  lawTarget_.store(static_cast<int>(l), std::memory_order_relaxed);
}
void DevSlam::setImpactTargetDb(float db)
{
  targetDbTarget_.store(db, std::memory_order_relaxed); // voice clamps
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
  activeVoice_ = static_cast<ImpactVoice>(impactVoiceTarget_.load(std::memory_order_relaxed));
  amount_ = clamp01(amountTarget_.load(std::memory_order_relaxed));
  sens_ = clamp01(impactSensTarget_.load(std::memory_order_relaxed));
  boost_ = clampBoost(impactBoostTarget_.load(std::memory_order_relaxed));
  soloT_ = soloTarget_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
  soloM_ = soloT_; // snapped: post-Start matches the UI immediately
  dbgT_ = clampDbg(dbgGainTarget_.load(std::memory_order_relaxed));
  dbgG_ = dbgT_;
  ref_ = refTarget_.load(std::memory_order_relaxed);
  benv_ = 0.0f;
  teleBranch_.store(0.0f, std::memory_order_relaxed);
  teleGain_.store(0.0f, std::memory_order_relaxed);
  teleLim_.store(1.0f, std::memory_order_relaxed);
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
  trig_.reset(sr_, sens_, 90.0f);
  burst_.reset(sr_, 70.0f, 48.0f, 130.0f, 1.5f, 0.75f); // Legacy v1 (FROZEN)
  bloomLp_.reset();
  bloomLp_.setLowpass(sr_, 105.0f, 0.7f);
  v2_.reset(sr_);
  v2_.setVoice(mapVoice(activeVoice_));
  v2_.setAmount(amount_ * boost_);
  v2_.setTestRef(ref_);
  const int tLaw = lawTarget_.load(std::memory_order_relaxed);
  v2_.setGainLaw(tLaw == 1 ? SlamImpactV2::GainLaw::TargetRatio
                           : SlamImpactV2::GainLaw::LegacyAdaptive);
  v2_.setTargetDb(targetDbTarget_.load(std::memory_order_relaxed));
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
  const ImpactVoice tVoice =
      static_cast<ImpactVoice>(impactVoiceTarget_.load(std::memory_order_relaxed));
  const float tAm = clamp01(amountTarget_.load(std::memory_order_relaxed));
  // Amount glides toward target (halving per block: ~1% in 7 blocks).
  amount_ += (tAm - amount_) * 0.5f;
  if (std::fabs(tAm - amount_) < 1e-4f)
    amount_ = tAm;
  push_.setAmount(amount_);
  crush_.setAmount(amount_);
  mass_.setAmount(amount_);
  // Sensitivity is a threshold-only live update (no output discontinuity,
  // no dip; detector state preserved).
  const float tSens = clamp01(impactSensTarget_.load(std::memory_order_relaxed));
  if (tSens != sens_)
  {
    sens_ = tSens;
    trig_.setSensitivity(sens_);
  }
  // Test-only v2 boost (adopted directly; tests use it at reset boundaries).
  boost_ = clampBoost(impactBoostTarget_.load(std::memory_order_relaxed));
  v2_.setAmount(amount_ * boost_); // Legacy v1 path never sees boost_
  // DEV gain law + target (adopted directly, no dip: future fires use the
  // new law; in-flight rings keep their sampled gain).
  const int tLaw = lawTarget_.load(std::memory_order_relaxed);
  v2_.setGainLaw(tLaw == 1 ? SlamImpactV2::GainLaw::TargetRatio
                           : SlamImpactV2::GainLaw::LegacyAdaptive);
  v2_.setTargetDb(targetDbTarget_.load(std::memory_order_relaxed));
  // DEV diagnostics: solo/dbg targets glide per sample; ref toggles the
  // voice render path with fresh output state (both directions).
  soloT_ = soloTarget_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
  dbgT_ = clampDbg(dbgGainTarget_.load(std::memory_order_relaxed));
  const bool tRef = refTarget_.load(std::memory_order_relaxed);
  if (tRef != ref_)
  {
    ref_ = tRef;
    v2_.setTestRef(ref_);
    v2_.setVoice(mapVoice(activeVoice_)); // fresh v2/ref output state
    burst_.reset(sr_, 70.0f, 48.0f, 130.0f, 1.5f, 0.75f); // fresh Legacy too
    bloomLp_.reset();
    bloomLp_.setLowpass(sr_, 105.0f, 0.7f);
    bloom_ = 0.0f;
  }
  // Deferred dip-bottom adoption first: the switch always lands on a
  // block boundary (mid-block activation would blend the outgoing
  // flavour's whole-block render with the incoming ramp: click).
  if (adoptPending_)
  {
    adoptPending_ = false;
    adoptNow(tEn, tFl, tVoice);
    return;
  }
  // Enable/flavour/Impact-voice change dips through dry (adopt at the dip
  // bottom, or immediately when already dry). Staging a voice while a
  // non-Impact flavour is active never dips.
  const bool voiceSwitch = (tFl == Flavor::Impact && activeFlavor_ == Flavor::Impact
                            && tVoice != activeVoice_);
  if (tEn != activeEnabled_ || tFl != activeFlavor_ || voiceSwitch)
  {
    if (m_ == 0.0f)
      adoptNow(tEn, tFl, tVoice);
    else if (phase_ != Dip)
    {
      phase_ = Dip;
      rampLeft_ = dipN_;
      rampStep_ = (0.0f - m_) / static_cast<float>(dipN_);
    }
  }
}

void DevSlam::adoptNow(bool tEn, Flavor tFl, ImpactVoice tVoice)
{
  activeEnabled_ = tEn;
  activeFlavor_ = tFl;
  activeVoice_ = tVoice;
  if (tFl != Flavor::Impact)
    clearImpactPending(); // stale fires must not leak into other flavours
  // Fresh output state for the incoming flavour (masks nothing: m == 0,
  // rise covers the settle). Impact keeps its trigger floor warm (no
  // reset) so in-flight confirms and adaptation survive the switch; the
  // v2 rig-level tracker likewise stays warm across voice changes.
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
    burst_.reset(sr_, 70.0f, 48.0f, 130.0f, 1.5f, 0.75f); // Legacy (FROZEN)
    bloomLp_.reset();
    bloomLp_.setLowpass(sr_, 105.0f, 0.7f);
    bloom_ = 0.0f;
    v2_.setVoice(mapVoice(tVoice));
    v2_.setAmount(amount_ * boost_);
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

void DevSlam::publishTele(bool impactActive)
{
  // Called once per post-IR tap call (block rate): keeps the UI atoms
  // fresh without per-sample store traffic.
  if (!impactActive)
  {
    benv_ *= 0.8f; // fast meter fade when Impact isn't rendering
    if (benv_ < 1e-7f)
      benv_ = 0.0f;
    teleBranch_.store(benv_, std::memory_order_relaxed);
    teleGain_.store(0.0f, std::memory_order_relaxed);
    teleLim_.store(1.0f, std::memory_order_relaxed);
    return;
  }
  const bool v2render = (activeVoice_ != ImpactVoice::Legacy) || ref_;
  teleBranch_.store(benv_, std::memory_order_relaxed);
  // Legacy v1 has no adaptive gain or limiter: report honest zeros.
  teleGain_.store(v2render ? v2_.lastFireGain() : 0.0f, std::memory_order_relaxed);
  teleLim_.store(v2render ? v2_.limiterGain() : 1.0f, std::memory_order_relaxed);
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

float DevSlam::renderV2Sample(float dry, int64_t absSample)
{
  float s = 0.0f;
  while (pendCount_ > 0 && pendAbs_[0] <= absSample)
  {
    s = pendStr_[0]; // coalesce: latest due wins (retrigger restarts anyway)
    for (int i = 1; i < pendCount_; ++i)
    {
      pendAbs_[i - 1] = pendAbs_[i];
      pendStr_[i - 1] = pendStr_[i];
    }
    --pendCount_;
  }
  return v2_.render(dry, s);
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
  {
    if (tap == PostIr)
      publishTele(false);
    return; // io untouched (bit-transparent)
  }
  if (phase_ == Steady && m_ == 0.0f)
  {
    if (tap == PostIr)
      publishTele(false);
    return; // disabled steady: transparent, no CPU
  }
  if (activeFlavor_ == Flavor::Impact)
  {
    const bool legacy = (activeVoice_ == ImpactVoice::Legacy) && !ref_;
    for (int i = 0; i < numFrames; ++i)
    {
      const float dry = input[i];
      // states always advance (both renders are dry-exact without fires)
      const float y = legacy ? renderImpactSample(dry, postBase + i)
                             : renderV2Sample(dry, postBase + i);
      // Telemetry on the musical branch (pre-solo, pre-debug-gain).
      const float ab = std::fabs(y - dry);
      benv_ = (ab > benv_) ? ab : benv_ * 0.9999f;
      if (benv_ < 1e-7f)
        benv_ = 0.0f;
      // Debug glides (~5 ms one-pole + snap so Normal parks exactly).
      soloM_ += (soloT_ - soloM_) * 0.02f;
      if (std::fabs(soloT_ - soloM_) < 1e-6f)
        soloM_ = soloT_;
      dbgG_ += (dbgT_ - dbgG_) * 0.02f;
      if (std::fabs(dbgT_ - dbgG_) < 1e-6f)
        dbgG_ = dbgT_;
      float out;
      if (phase_ == Steady && m_ == 1.0f && soloM_ == 0.0f && dbgG_ == 1.0f)
        out = y; // fast path: bit-identical to the pre-diagnostic render
      else
      {
        const float br = dbgG_ * (y - dry);
        const float dm = dry * (1.0f - soloM_);
        if (m_ == 1.0f)
          out = dm + br;
        else if (m_ == 0.0f)
          out = dm;
        else
          out = dm + m_ * br;
      }
      output[i] = out;
      advanceRamp();
    }
    if (tap == PostIr)
      publishTele(true);
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
  // Push/Crush/Mass render path untouched by Impact diagnostics (Impact
  // telemetry simply idles while other flavours serve).
  if (tap == PostIr)
    publishTele(false);
}

} // namespace lab
} // namespace tdm

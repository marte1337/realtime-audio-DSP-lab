// GuitarTransposeV2: Doppler transpose with rare matched splices. See header.

#include "dsp/lab/Pitch/GuitarTransposeV2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tdm
{
namespace lab
{
namespace
{
constexpr double kPi = 3.14159265358979;

int nextPow2(int n)
{
  int s = 1;
  while (s < n)
    s <<= 1;
  return s;
}

double clamp01(double x)
{
  return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}
} // namespace

GuitarTransposeV2::GuitarTransposeV2() = default;

void GuitarTransposeV2::setConfig(const Config& cfg)
{
  config_ = cfg; // validated at reset()
}

void GuitarTransposeV2::setShiftSt(float semitones)
{
  const float clamped = semitones < kMinShiftSt ? kMinShiftSt : (semitones > kMaxShiftSt ? kMaxShiftSt : semitones);
  shiftSt_ = clamped;
  targetRatio_ = ratioFor(clamped);
}

void GuitarTransposeV2::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

double GuitarTransposeV2::ratioFor(float st) const
{
  return std::pow(2.0, static_cast<double>(st) / 12.0);
}

void GuitarTransposeV2::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("GuitarTransposeV2: sample rate out of range");
  const Config& c = config_;
  const auto inRange = [](double v, double lo, double hi) { return v >= lo && v <= hi; };
  if (!inRange(c.windowMs, 10.0, 120.0) || !inRange(c.floorMs, 0.5, 10.0) || c.floorMs >= c.windowMs
      || !inRange(c.corrMs, 5.0, 60.0) || !inRange(c.fadeMinMs, 4.0, 120.0) || !inRange(c.fadeMaxMs, c.fadeMinMs, 250.0)
      || !(c.fadeNccHi > c.fadeNccLo && c.fadeNccHi <= 1.0 && c.fadeNccLo >= 0.0) || !inRange(c.onsetFadeMs, 0.5, 10.0)
      || !inRange(c.onsetSpanMs, 1.0, 12.0) || !inRange(c.searchLeadMs, 1.0, 16.0)
      || !inRange(c.refractoryMs, 5.0, 200.0) || !inRange(c.detectorHpHz, 100.0, 4000.0)
      || !inRange(c.detectorSmoothMs, 0.5, 10.0) || !inRange(c.onsetOverMinDb, 3.0, 24.0)
      || !inRange(c.onsetOverMaxDb, 1.0, 18.0) || c.historyCells < 10 || c.historyCells > 200 || c.historySkip < 1
      || c.historySkip > 20 || c.historySkip >= c.historyCells)
    throw std::invalid_argument("GuitarTransposeV2: config out of range");

  sampleRate_ = sampleRate;
  dMin_ = std::max(8, static_cast<int>(sampleRate * c.floorMs * 0.001));
  dMax_ = std::max(dMin_ + 8, static_cast<int>(sampleRate * c.windowMs * 0.001));
  corrLen_ = std::max(16, std::min(dMax_, static_cast<int>(sampleRate * c.corrMs * 0.001)));
  fadeMinLen_ = std::max(8, static_cast<int>(sampleRate * c.fadeMinMs * 0.001));
  fadeMaxLen_ = std::max(fadeMinLen_, static_cast<int>(sampleRate * c.fadeMaxMs * 0.001));
  onsetFadeLen_ = std::max(8, static_cast<int>(sampleRate * c.onsetFadeMs * 0.001));
  onsetSpan_ = std::max(8, static_cast<int>(sampleRate * c.onsetSpanMs * 0.001));
  searchLead_ = std::max(1, static_cast<int>(sampleRate * c.searchLeadMs * 0.001));
  refractory_ = std::max(1, static_cast<int>(sampleRate * c.refractoryMs * 0.001));
  cellLen_ = std::max(1, static_cast<int>(sampleRate * 0.001));
  coarseStep_ = std::max(1, static_cast<int>(sampleRate / 12000.0 + 0.5));
  fadeHiUp_ = fadeMaxLen_;

  // Ring covers the deepest tap (dMax + fade overshoot + lead) plus the
  // correlation reach behind the farthest candidate, with margin.
  const int need = dMax_ + corrLen_ + fadeMaxLen_ + 2 * searchLead_ + 64;
  ring_.assign(static_cast<size_t>(nextPow2(need)), 0.0f);
  ringMask_ = static_cast<int>(ring_.size()) - 1;
  ref_.assign(static_cast<size_t>(corrLen_), 0.0f);
  minHist_.assign(static_cast<size_t>(c.historyCells), 1e9f);
  maxHist_.assign(static_cast<size_t>(c.historyCells), 0.0f);

  hpCoeff_ = std::exp(-2.0 * kPi * c.detectorHpHz / sampleRate);
  smoothCoeff_ = std::exp(-1.0 / (sampleRate * c.detectorSmoothMs * 0.001));
  overMin_ = std::pow(10.0, c.onsetOverMinDb / 10.0); // energy ratios: dB power
  overMax_ = std::pow(10.0, c.onsetOverMaxDb / 10.0);

  writePos_ = 0;
  tapA_ = tapB_ = static_cast<double>(-dMin_); // parked at the floor
  fading_ = false;
  fadePos_ = 0.0;
  fadeInc_ = 0.0;
  fadeNcc_ = 1.0;
  search_ = Search{};
  refEnergy_ = 0.0;
  hpState_ = 0.0;
  hpPrevX_ = 0.0;
  energy_ = 0.0;
  cellMin_ = 1e9;
  cellMax_ = 0.0;
  lastOnset_ = -(1LL << 40);
  triggerWasHigh_ = false;
  lastDropPos_ = -(1LL << 40);

  ratio_ = targetRatio_ = ratioFor(shiftSt_);
  updateUpshiftCap();
  bypass0_ = (shiftSt_ == 0.0f);
  latency_ = bypass0_ ? 0 : (dMin_ + dMax_) / 2;

  telemetry_ = Telemetry{};
  spliceLog_.assign(kEventLogSize, SpliceEvent{});
  onsetLog_.assign(kEventLogSize, OnsetEvent{});
  spliceCount_ = 0;
  onsetCount_ = 0;
  traceEnv_.clear();
  traceOverMin_.clear();
  traceOverMax_.clear();
  traceFired_.clear();
}

void GuitarTransposeV2::updateUpshiftCap()
{
  // An upshift tap gains on the write head, so every fade and every
  // search lead spends buffer at the drift rate. The longest fade is
  // held to about a sixth of the range, and where one cycle (landing
  // fade + next lead + following fade) would leave less than half the
  // buffer to land in, fade and lead scale down together until it does.
  // Without this the landing range collapses to a point at large
  // ratios and splices stop finding whole-period jumps (off-pitch).
  fadeHiUp_ = fadeMaxLen_;
  leadNow_ = searchLead_;
  if (ratio_ > 1.0)
  {
    const double drift = ratio_ - 1.0;
    double hi =
        std::max<double>(fadeMinLen_, std::min<double>(fadeMaxLen_, static_cast<double>(dMax_ - dMin_) / (6.0 * drift)));
    double lead = static_cast<double>(searchLead_);
    int floor = fadeMinLen_;
    const double cost = drift * (2.0 * hi + lead + 4.0) + 6.0;
    const double budget = static_cast<double>(dMax_) * 0.5;
    if (cost > budget)
    {
      const double s = budget / cost;
      hi *= s;
      lead *= s;
      floor = 8;
    }
    fadeHiUp_ = std::max(floor, std::min(fadeMaxLen_, static_cast<int>(hi)));
    leadNow_ = std::max(1, static_cast<int>(lead));
  }
}

int GuitarTransposeV2::onsetLatencySamples() const
{
  if (bypass0_ || sampleRate_ <= 0.0)
    return 0;
  return dMin_ + onsetSpan_ + onsetFadeLen_;
}

float GuitarTransposeV2::readTap(double pos) const
{
  // House cubic Lagrange kernel (same as LabPitchShift/LabWsolaShift).
  const long long i = static_cast<long long>(std::floor(pos));
  const float f = static_cast<float>(pos - static_cast<double>(i));
  const auto at = [&](long long k) { return ring_[static_cast<size_t>(static_cast<uint32_t>(k) & static_cast<uint32_t>(ringMask_))]; };
  const float ym1 = at(i - 1);
  const float y0 = at(i);
  const float y1 = at(i + 1);
  const float y2 = at(i + 2);
  const float c1 = 0.5f * (y1 - ym1);
  const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  const float c3 = -0.5f * ym1 + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
  return y0 + f * (c1 + f * (c2 + f * c3));
}

double GuitarTransposeV2::nccAt(long long windowEnd) const
{
  double sxy = 0.0;
  double syy = 0.0;
  for (int i = 0; i < corrLen_; ++i)
  {
    const float y = ring_[static_cast<size_t>(static_cast<uint32_t>(windowEnd - corrLen_ + i) & static_cast<uint32_t>(ringMask_))];
    const float x = ref_[static_cast<size_t>(i)];
    sxy += static_cast<double>(x) * y;
    syy += static_cast<double>(y) * y;
  }
  const double ncc = sxy / (std::sqrt(refEnergy_ * syy) + 1e-9);
  return ncc < -1.0 ? -1.0 : (ncc > 1.0 ? 1.0 : ncc);
}

void GuitarTransposeV2::beginSearch(int lo, int hi, int lead)
{
  search_.active = true;
  search_.ready = false;
  search_.planPos = writePos_;
  search_.tapAtPlan = static_cast<long long>(std::floor(tapA_));
  search_.lo = lo;
  search_.hi = hi > lo ? hi : lo;
  search_.next = lo;
  search_.bestScore = -1e9;
  search_.bestNcc = 0.0;
  search_.bestDelay = lo;
  // Reference: the tap's recent waveform (fully causal).
  for (int i = 0; i < corrLen_; ++i)
    ref_[static_cast<size_t>(i)] =
        ring_[static_cast<size_t>(static_cast<uint32_t>(search_.tapAtPlan - corrLen_ + i) & static_cast<uint32_t>(ringMask_))];
  refEnergy_ = 0.0;
  for (int i = 0; i < corrLen_; ++i)
    refEnergy_ += static_cast<double>(ref_[static_cast<size_t>(i)]) * ref_[static_cast<size_t>(i)];
  const int candidates = (search_.hi - search_.lo) / coarseStep_ + 1;
  search_.perSample = lead > 0 ? (candidates + lead - 1) / lead : candidates;
}

void GuitarTransposeV2::considerCandidate(int d)
{
  const double curDelay = static_cast<double>(search_.planPos - search_.tapAtPlan);
  const double jump = std::max(1.0, std::fabs(curDelay - static_cast<double>(d)));
  const double ncc = nccAt(search_.planPos - d);
  // Rate-cost objective: damage per splice over time the jump buys. A
  // long near-perfect jump beats a short perfect one, so splices stay
  // rare; on periodic locks the common-period lag wins outright.
  const double score = -(1.0 - ncc) / jump;
  if (score > search_.bestScore)
  {
    search_.bestScore = score;
    search_.bestDelay = d;
    search_.bestNcc = ncc;
  }
}

void GuitarTransposeV2::stepSearch(int count)
{
  while (count-- > 0 && search_.next <= search_.hi)
  {
    considerCandidate(search_.next);
    search_.next += coarseStep_;
  }
  if (search_.next > search_.hi)
  {
    if (coarseStep_ > 1)
    {
      const int c = search_.bestDelay;
      const int rlo = std::max(search_.lo, c - coarseStep_ + 1);
      const int rhi = std::min(search_.hi, c + coarseStep_ - 1);
      for (int d = rlo; d <= rhi; ++d)
        if (d != c)
          considerCandidate(d);
    }
    search_.resultJump = (search_.planPos - search_.bestDelay) - search_.tapAtPlan;
    search_.active = false;
    search_.ready = true;
  }
}

int GuitarTransposeV2::fadeLenFor(double ncc, double room) const
{
  const double t = clamp01((config_.fadeNccHi - ncc) / (config_.fadeNccHi - config_.fadeNccLo));
  const double len = static_cast<double>(fadeMinLen_) + t * static_cast<double>(fadeMaxLen_ - fadeMinLen_);
  const double capped = len < room ? len : room;
  return std::max(8, static_cast<int>(capped));
}

bool GuitarTransposeV2::startFade(long long jump, int fadeLen, int kind)
{
  search_.ready = false;
  const double destDelay = static_cast<double>(writePos_) - (tapA_ + static_cast<double>(jump));
  if (!(destDelay >= 2.0 && destDelay <= static_cast<double>(ring_.size()) - 8.0))
  {
    ++telemetry_.droppedFades;
    lastDropPos_ = writePos_;
    return false; // drift logic replans (throttled); never jump off the ring
  }
  tapB_ = tapA_ + static_cast<double>(jump); // tapA fraction kept: integer lag
  fading_ = true;
  fadePos_ = 0.0;
  fadeInc_ = 1.0 / static_cast<double>(fadeLen > 0 ? fadeLen : 1);
  fadeNcc_ = clamp01(search_.bestNcc);
  // Telemetry (magnitudes; events keep the sign).
  const long long mag = jump >= 0 ? jump : -jump;
  ++telemetry_.jumpCount;
  telemetry_.jumpSum += static_cast<double>(mag);
  if (telemetry_.jumpCount == 1 || mag < telemetry_.jumpMin)
    telemetry_.jumpMin = mag;
  if (mag > telemetry_.jumpMax)
    telemetry_.jumpMax = mag;
  telemetry_.nccSum += search_.bestNcc;
  telemetry_.fadeSum += fadeLen;
  telemetry_.lastJump = jump;
  telemetry_.lastNcc = search_.bestNcc;
  telemetry_.lastFadeLen = fadeLen;
  SpliceEvent e;
  e.pos = writePos_;
  e.kind = kind;
  e.jump = jump;
  e.ncc = search_.bestNcc;
  e.fadeLen = fadeLen;
  e.tapDelay = static_cast<int>(static_cast<double>(writePos_) - tapA_);
  spliceLog_[spliceCount_ % kEventLogSize] = e;
  ++spliceCount_;
  return true;
}

bool GuitarTransposeV2::detectorStep(float x)
{
  const long long now = writePos_;
  // First-order highpass: picks are HF-rich, steady lows sit below it.
  const double hp = hpCoeff_ * (hpState_ + static_cast<double>(x) - hpPrevX_);
  hpPrevX_ = x;
  hpState_ = hp;
  energy_ = smoothCoeff_ * energy_ + (1.0 - smoothCoeff_) * hp * hp;
  const long long cellNow = cellLen_ > 0 ? now / cellLen_ : now;
  if (cellLen_ > 0 && now % cellLen_ == 0 && now > 0)
  {
    const size_t prev = static_cast<size_t>((cellNow - 1) % config_.historyCells);
    minHist_[prev] = static_cast<float>(cellMin_);
    maxHist_[prev] = static_cast<float>(cellMax_);
    cellMin_ = 1e9;
    cellMax_ = 0.0;
  }
  if (energy_ < cellMin_)
    cellMin_ = energy_;
  if (energy_ > cellMax_)
    cellMax_ = energy_;
  double recentMin = 1e9;
  double recentMax = 0.0;
  for (int back = config_.historySkip; back < config_.historyCells; ++back)
  {
    const size_t c = static_cast<size_t>((cellNow - back + 4LL * config_.historyCells) % config_.historyCells);
    if (minHist_[c] < recentMin)
      recentMin = minHist_[c];
    if (maxHist_[c] > recentMax)
      recentMax = maxHist_[c];
  }
  const bool fired =
      energy_ > 1e-7 && energy_ > recentMin * overMin_ && energy_ > recentMax * overMax_;
  const bool edge = fired && !triggerWasHigh_;
  triggerWasHigh_ = fired;
  if (edge)
    ++telemetry_.detectorEdges;
  if (traceOn_)
  {
    traceEnv_.push_back(static_cast<float>(energy_));
    traceOverMin_.push_back(static_cast<float>(energy_ / (recentMin * overMin_ + 1e-12)));
    traceOverMax_.push_back(static_cast<float>(energy_ / (recentMax * overMax_ + 1e-12)));
    traceFired_.push_back(fired ? 1 : 0);
  }
  return edge;
}

void GuitarTransposeV2::detectorResetHistory(double level)
{
  const float f = static_cast<float>(level);
  std::fill(minHist_.begin(), minHist_.end(), f);
  std::fill(maxHist_.begin(), maxHist_.end(), f);
  cellMin_ = cellMax_ = level;
}

void GuitarTransposeV2::processBlock(const float* input, float* output, int numFrames)
{
  if (numFrames <= 0)
    return;
  if (!enabled_ || bypass0_)
  {
    if (output != input)
      std::memcpy(output, input, static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }
  if (sampleRate_ <= 0.0)
  {
    for (int i = 0; i < numFrames; ++i)
      output[i] = 0.0f;
    return;
  }

  for (int i = 0; i < numFrames; ++i)
  {
    // Live ratio adoption (per sample: block-size deterministic). A
    // pending plan was built for the old drift, so it is dropped; the
    // boundary logic below falls back to a synchronous search.
    if (targetRatio_ != ratio_)
    {
      ratio_ = targetRatio_;
      search_ = Search{};
      updateUpshiftCap();
    }
    const double ratio = ratio_;
    const double drift = ratio < 1.0 ? 1.0 - ratio : (ratio > 1.0 ? ratio - 1.0 : 0.0);

    const long long now = writePos_;
    const float x = input[i];
    ring_[static_cast<size_t>(static_cast<uint32_t>(now) & static_cast<uint32_t>(ringMask_))] = x;

    const double delay = static_cast<double>(now) - tapA_;
    const bool onsetEdge = detectorStep(x);

    // Onset re-sync first: a genuine attack with the tap deep jumps to
    // the floor region at once (small bounded search, quick fade).
    bool resynced = false;
    if (onsetEdge && config_.enableResync)
    {
      OnsetEvent oe;
      oe.pos = now;
      oe.tapDelay = static_cast<int>(delay);
      if (fading_)
      {
        oe.action = 1;
        ++telemetry_.resyncBlockedFade;
      }
      else if (now - lastOnset_ <= refractory_)
      {
        oe.action = 2;
        ++telemetry_.resyncBlockedRefr;
      }
      else if (!(delay > static_cast<double>(dMin_ + onsetSpan_)))
      {
        oe.action = 3;
        ++telemetry_.resyncBlockedDepth;
      }
      else
      {
        lastOnset_ = now;
        const int hi = std::min(dMin_ + onsetSpan_, dMax_);
        beginSearch(dMin_, hi, 0);
        stepSearch(search_.perSample); // completes synchronously
        if (startFade(search_.resultJump, onsetFadeLen_, 2))
        {
          oe.action = 0;
          ++telemetry_.resyncs;
          resynced = true;
        }
        else
        {
          oe.action = 4;
        }
        detectorResetHistory(energy_); // the attack is the new reference
      }
      onsetLog_[onsetCount_ % kEventLogSize] = oe;
      ++onsetCount_;
    }

    if (!fading_ && !resynced)
    {
      if (ratio < 1.0 && drift > 0.0)
      {
        // Drift splice, downshift: the tap falls back toward dMax. The
        // search plans a lead early; candidates sit leadDrift nearer the
        // head than where they land at fire time.
        const double leadDrift = drift * static_cast<double>(searchLead_ + 4);
        const int leadDriftI = static_cast<int>(std::ceil(leadDrift));
        const int fadeDrift = static_cast<int>(std::ceil(drift * static_cast<double>(fadeMinLen_)));
        const int landLo = dMin_;
        const int landHi = std::max(dMin_, dMax_ - fadeDrift - leadDriftI - 2);
        if (!search_.active && !search_.ready && delay >= static_cast<double>(dMax_) - leadDrift
            && now - lastDropPos_ > 64)
        {
          const int lo = std::max(4, landLo - leadDriftI);
          beginSearch(lo, std::max(lo, landHi - leadDriftI), searchLead_);
        }
        if (delay >= static_cast<double>(dMax_))
        {
          if (!search_.ready)
          {
            // Unplanned boundary (ratio just changed): synchronous
            // fallback over the true landing range. Bounded, rare.
            if (!search_.active)
              beginSearch(landLo, landHi, 0);
            stepSearch(1 << 28);
            ++telemetry_.syncFallbacks;
          }
          if (search_.ready)
          {
            const double dest = delay - static_cast<double>(search_.resultJump);
            const double room = (static_cast<double>(dMax_) - dest - leadDrift - 2.0) / drift;
            if (startFade(search_.resultJump, fadeLenFor(search_.bestNcc, room), 0))
              ++telemetry_.driftSplicesDown;
          }
        }
      }
      else if (ratio > 1.0 && drift > 0.0)
      {
        // Upshift mirror: the tap gains toward a guard; candidates sit
        // deeper than where they land. Fade and lead budgeted (see
        // updateUpshiftCap) so a landing range survives.
        const int guard = std::max(dMin_, static_cast<int>(std::ceil(drift * static_cast<double>(fadeHiUp_))) + 4);
        const double leadDrift = drift * static_cast<double>(leadNow_ + 4);
        const int leadDriftI = static_cast<int>(std::ceil(leadDrift));
        const int fadeDrift = static_cast<int>(std::ceil(drift * static_cast<double>(fadeHiUp_)));
        const int landLo = std::min(guard + fadeDrift + leadDriftI + 2, dMax_);
        const int landHi = dMax_;
        if (!search_.active && !search_.ready && delay <= static_cast<double>(guard) + leadDrift
            && now - lastDropPos_ > 64)
          beginSearch(landLo + leadDriftI, landHi + leadDriftI, leadNow_);
        if (delay <= static_cast<double>(guard))
        {
          if (!search_.ready)
          {
            if (!search_.active)
              beginSearch(landLo, landHi, 0);
            stepSearch(1 << 28);
            ++telemetry_.syncFallbacks;
          }
          if (search_.ready)
          {
            if (startFade(search_.resultJump, fadeLenFor(search_.bestNcc, static_cast<double>(fadeHiUp_)), 1))
              ++telemetry_.driftSplicesUp;
          }
        }
      }
    }
    if (search_.active)
      stepSearch(search_.perSample);

    // Read. Raised-cosine crossfade, gains normalized by the taps'
    // measured correlation: r = 1 keeps the complementary fade, r = 0
    // the equal-power one, so level holds either way.
    float gainA = 1.0f;
    float gainB = 0.0f;
    if (fading_)
    {
      gainB = static_cast<float>(0.5 - 0.5 * std::cos(kPi * fadePos_));
      gainA = 1.0f - gainB;
      const float r = static_cast<float>(fadeNcc_);
      const float norm = std::sqrt(gainA * gainA + gainB * gainB + 2.0f * gainA * gainB * r);
      if (norm > 1e-6f)
      {
        gainA /= norm;
        gainB /= norm;
      }
      fadePos_ += fadeInc_;
    }
    float wet = readTap(tapA_);
    if (fading_)
      wet = wet * gainA + gainB * readTap(tapB_);
    output[i] = wet;
    if (fading_ && fadePos_ >= 1.0)
    {
      tapA_ = tapB_;
      fading_ = false;
    }
    tapA_ += ratio;
    tapB_ += ratio;
    ++writePos_;
    ++telemetry_.samples;
  }
}

GuitarTransposeV2::SpliceEvent GuitarTransposeV2::spliceEvent(size_t i) const
{
  const size_t kept = spliceCount_ < kEventLogSize ? spliceCount_ : kEventLogSize;
  if (i >= kept)
    return SpliceEvent{};
  const size_t base = spliceCount_ > kEventLogSize ? spliceCount_ - kEventLogSize : 0;
  return spliceLog_[(base + i) % kEventLogSize];
}

GuitarTransposeV2::OnsetEvent GuitarTransposeV2::onsetEvent(size_t i) const
{
  const size_t kept = onsetCount_ < kEventLogSize ? onsetCount_ : kEventLogSize;
  if (i >= kept)
    return OnsetEvent{};
  const size_t base = onsetCount_ > kEventLogSize ? onsetCount_ - kEventLogSize : 0;
  return onsetLog_[(base + i) % kEventLogSize];
}

void GuitarTransposeV2::enableTrace(bool on)
{
  traceOn_ = on;
  traceEnv_.clear();
  traceOverMin_.clear();
  traceOverMax_.clear();
  traceFired_.clear();
}
} // namespace lab
} // namespace tdm

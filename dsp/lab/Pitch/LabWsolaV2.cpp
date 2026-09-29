// LabWsolaV2: E2 research fork of LabWsolaShift. See header. Marked E2
// branches only; everything else is the baseline algorithm verbatim.

#include "dsp/lab/Pitch/LabWsolaV2.h"

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
constexpr double kTwoPi = 2.0 * 3.14159265358979;
constexpr double kSilenceEnergy = 1e-12; // tail energy floor: skip search
constexpr double kScoreEps = 1e-18; // correlation denominator guard
// Tie-break bands (see placeFrame), probe-grounded: during drift runs
// exact continuation scores exactly 1.0000 (target is verbatim-
// dominated, blend cost ~0), so the 1e-3 stationary band admits it and
// drift sustains. A 0.03 band was tried and REJECTED by probe: it
// admits off-tie edges (systematic phase errors, broke the sine spots)
// and lets pegs stick (dry dilution). Pegged frames (continuation out
// of range) and transients use 1e-6 (outright max): at the peg the max
// is the skip-back (drift resumes after); closest-to-continuation there
// would pin the edge forever (found by probe: permanent -D peg = dry).
// Transient outright-max is floored at -(Lov-1) (see placeFrame): with
// asymmetric Dm > Lov the unfloored max prefers attack-excluding
// sustain windows and deletes pick transients (measured, -1 chugs).
constexpr double kTieEps = 1e-3;
constexpr double kTieEpsTransient = 1e-6;
constexpr double kFluxRise = 6.0; // HP-energy rise vs trailing average
constexpr double kFluxTrailRate = 0.25; // trailing-average update per frame
// (E2 validity band lives in skipBand_, set via setSkipBand(); the study
// sweeps it. Default kSkipBandDefault; see header.)

int ceilPow2(int v)
{
  int p = 32;
  while (p < v)
    p <<= 1;
  return p;
}
} // namespace

LabWsolaV2::LabWsolaV2() = default;

void LabWsolaV2::setConfig(double windowMs)
{
  if (windowMs != 14.0 && windowMs != 16.0 && windowMs != 20.0 && windowMs != 30.0 && windowMs != 40.0)
    throw std::invalid_argument("LabWsolaV2: windowMs must be one of {14.0, 16.0, 20.0, 30.0, 40.0}");
  windowMs_ = windowMs;
}

void LabWsolaV2::setSearch(int tolMinus, int tolPlus)
{
  // E2: tolPlus == 0 with tolMinus >= 1 is the causal request (fully
  // history-side search). (0, 0) stays the symmetric default; (0, Dp>0)
  // is still meaningless and still throws.
  if (tolMinus == 0 && tolPlus != 0)
    throw std::invalid_argument("LabWsolaV2: setSearch takes (0, 0) for default or (Dm >= 1, Dp >= 0)");
  if (tolMinus < 0 || tolPlus < 0)
    throw std::invalid_argument("LabWsolaV2: setSearch tolerances must be non-negative");
  wantTolMinus_ = tolMinus;
  wantTolPlus_ = tolPlus;
}

void LabWsolaV2::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("LabWsolaV2: sample rate out of range");
  sampleRate_ = sampleRate;
  bypass0_ = (shiftSt_ == 0.0f); // exact equality, like LabPitchShift

  frameLen_ = static_cast<int>(windowMs_ * sampleRate / 1000.0 + 0.5);
  if (frameLen_ < 64)
    frameLen_ = 64; // unreachable at >= 8 kHz, kept as a guard
  hopA_ = frameLen_ / 2; // 50% nominal analysis overlap
  // Search tolerance: symmetric W/2 by default (the accepted baseline:
  // search SPAN 2D = W covers a strong period of the lowest content for
  // skip-back coherence - probed span rule, low-B fund needs 778
  // samples @48k, so W/4 (span 480/720) fails low-B at wms20/30 while
  // W/2 (span = W) clears it). The latency study may request asymmetric
  // (Dm, Dp): only Dp reaches into the future, so latency is W+Dp+C
  // while span Dm+Dp keeps the low-string coherence (re-probed, not
  // assumed - see the latency study report). E2 admits Dp = 0 (causal);
  // bounds Dm in [1, W], Dp in [0, W] keep the search inside the frame
  // pair the rings are sized for.
  if (wantTolMinus_ == 0 && wantTolPlus_ == 0)
  {
    tolMinus_ = frameLen_ / 2;
    tolPlus_ = frameLen_ / 2;
  }
  else
  {
    // E2: Dp in [0, W] (0 = causal); Dm stays in [1, W].
    if (wantTolMinus_ < 1 || wantTolMinus_ > frameLen_ || wantTolPlus_ < 0 || wantTolPlus_ > frameLen_)
      throw std::invalid_argument("LabWsolaV2: search tolerances must be Dm in [1, W], Dp in [0, W]");
    tolMinus_ = wantTolMinus_;
    tolPlus_ = wantTolPlus_;
  }
  const double ratio = std::exp2(static_cast<double>(shiftSt_) / 12.0);
  hopS_ = static_cast<int>(ratio * hopA_ + 0.5);
  if (hopS_ < 1)
    hopS_ = 1; // unreachable at r >= 2^-2, kept as a guard
  if (hopS_ > hopA_)
    hopS_ = hopA_; // unreachable at r <= 1, kept as a guard
  actualRatio_ = static_cast<double>(hopS_) / hopA_;
  overlap_ = frameLen_ - hopS_; // Lov >= W/2 always (Hs <= Ha <= W/2)

  // Latency: W (frame fill) + Dp (search lookahead: the +Dp candidate
  // reaches Dp past the nominal frame end, so frame 0 places once input
  // [0, W+Dp) has arrived; the -Dm side reads history already in the
  // ring and costs nothing) + C.
  // Pops are HELD until tick L (see processBlock), so the FIFO prefills
  // and output o emits at exactly tick o+L. Starvation-free by proof:
  // output o needs compressed through floor(o*ar)+2, produced by frame
  // j* with (j*+1)*Hs > floor(o*ar)+2, placed at tick j**Ha+Dp+W-1 <=
  // o+3/ar+Dp+W-1; holding pops to o+W+Dp+C never starves iff
  // C >= 3/ar-1, and C = ceil(3/ar-1) is the smallest integer that does.
  // The proof reads Dp wherever the placement schedule does, so it
  // covers asymmetric search unchanged. (An earlier C = ceil(2/ar)+2
  // with no hold starved intermittently at -24 - caught by onset probe,
  // fixed by construction, pinned by the grid-wide DC starvation test.)
  // NOTE L is NOT an onset-alignment claim: input at sub-frame offset
  // j0 legitimately lands j0*(1/ar-1) late in the output (the slow read
  // stretches within-frame positions; the correlation search usually
  // but not always compensates). Transient placement slop up to
  // ~W*(1/ar-1) is inherent WSOLA behavior, measured honestly in
  // analysis, never tuned into L.
  const int c = static_cast<int>(std::ceil(3.0 / actualRatio_ - 1.0));
  latency_ = bypass0_ ? 0 : frameLen_ + tolPlus_ + c;
  tail_ = frameLen_ + hopA_; // conservative flush (verified by tail test)

  scoreBuf_.assign(static_cast<size_t>(tolMinus_ + tolPlus_ + 1), -2.0);
  prevDelta_ = 0;
  fluxTrail_ = 0.0;
  framesPlaced_ = 0;
  telemetry_ = Telemetry{};
  trace_.clear(); // buffer cleared; the armed flag survives reset
  fadeUp_.assign(static_cast<size_t>(overlap_), 0.0f);
  fadeDown_.assign(static_cast<size_t>(overlap_), 0.0f);
  for (int j = 0; j < overlap_; ++j)
  {
    const double a = 0.5 * kTwoPi * 0.5 * (j + 1) / (overlap_ + 1); // 0..pi/2
    const double s = std::sin(a);
    fadeUp_[static_cast<size_t>(j)] = static_cast<float>(s * s);
    fadeDown_[static_cast<size_t>(j)] = static_cast<float>(1.0 - s * s);
  }

  inRing_.assign(static_cast<size_t>(ceilPow2(tolMinus_ + tolPlus_ + frameLen_ + hopA_ + 16)), 0.0f);
  inMask_ = static_cast<int>(inRing_.size()) - 1;
  inCount_ = 0;
  nextFrameAt_ = tolPlus_ + frameLen_; // frame 0 places once its span arrives

  outAcc_.assign(static_cast<size_t>(ceilPow2(2 * frameLen_ + 16)), 0.0f);
  outMask_ = static_cast<int>(outAcc_.size()) - 1;
  olaPos_ = 0;
  compEmitted_ = 0;

  int compSize = 32;
  while (compSize < hopS_ + 16)
    compSize <<= 1;
  compRing_.assign(static_cast<size_t>(compSize), 0.0f);
  compMask_ = compSize - 1;
  resampOut_ = 0;

  outFifo_.assign(static_cast<size_t>(4 * hopA_ + 64), 0.0f);
  fifoRead_ = 0;
  fifoWrite_ = 0;
  fifoCount_ = 0;
  outCount_ = 0;

  firstFrame_ = true;
}

void LabWsolaV2::setShiftSt(float semitones)
{
  shiftSt_ = semitones < kMinShiftSt ? kMinShiftSt : (semitones > kMaxShiftSt ? kMaxShiftSt : semitones);
}

void LabWsolaV2::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

void LabWsolaV2::enableTrace(bool on)
{
  traceOn_ = on;
  trace_.clear();
}

std::vector<double> LabWsolaV2::landscape() const
{
  return scoreBuf_;
}

float LabWsolaV2::compressedAt(long long pos) const
{
  if (pos < 0)
    return 0.0f; // zero pre-roll before the stream starts
  return compRing_[static_cast<size_t>(pos & compMask_)];
}

void LabWsolaV2::pushOutput(float v)
{
  const int fifoCap = static_cast<int>(outFifo_.size());
  outFifo_[static_cast<size_t>(fifoWrite_)] = v;
  fifoWrite_ = (fifoWrite_ + 1) % fifoCap;
  ++fifoCount_;
}

void LabWsolaV2::placeFrame()
{
  const int w = frameLen_;
  const int hs = hopS_;
  const int lov = overlap_;
  const long long nominal = olaPos_ / hs * hopA_; // frame k nominal: k*Ha

  // Transient detector (input-side, ghost-free): highpassed-frame energy
  // vs trailing average. Attacks are 10-20 dB HP rises on guitar; the
  // trailing average updates only on non-transient frames so it cannot
  // self-trigger into lockup. Detector errors are graceful: a missed
  // soft onset gets servo treatment (mild), a false alarm takes pure-max
  // for one frame (bounded debt, repaid after).
  double hpE = 0.0;
  {
    float prev = 0.0f;
    {
      const long long ip0 = nominal - 1;
      prev = (ip0 < 0 || ip0 >= inCount_) ? 0.0f : inRing_[static_cast<size_t>(ip0 & inMask_)];
    }
    for (int j = 0; j < w; ++j)
    {
      const long long ip = nominal + j;
      const float c = (ip < 0 || ip >= inCount_) ? 0.0f : inRing_[static_cast<size_t>(ip & inMask_)];
      const double hp = static_cast<double>(c) - prev;
      hpE += hp * hp;
      prev = c;
    }
  }
  const double fluxFloor = 1e-9 * frameLen_;
  // Trail seeds unconditionally on the first two frames: otherwise a
  // hot start (signal from sample 0) reads as an infinite rise, every
  // frame flags transient, the trail never learns, and the stationary
  // servo band never engages (found by probe: stuck isTransient=1).
  const bool seedTrail = framesPlaced_ < 2;
  const bool isTransient = !seedTrail && hpE > kFluxRise * fluxTrail_ && hpE > fluxFloor;
  if (!isTransient || seedTrail)
    fluxTrail_ += kFluxTrailRate * (hpE - fluxTrail_);

  // Transient search floor (latency-study fix): attack frames take the
  // outright max, and against a sustain tail the outright max prefers
  // windows that EXCLUDE the attack (pure sustain correlates ~1.0,
  // attack content does not) - so with Dm > Lov the attack frame jumps
  // back to a sustain lag, repeats sustain over the attack, and the
  // pick transient is deleted when the map jumps forward again
  // (measured: full-span-960 configs ate chug pick attacks at -1 while
  // Dm <= Lov configs rendered them bit-identically to the baseline).
  // Clamping transient frames to d >= -(Lov-1) keeps windows overlapping
  // the frame start, so the backward jump - and the sustain repeat - is
  // bounded by the correlation length instead of the search span.
  // Provable no-op for symmetric search (Dm = W/2 <= Lov always, since
  // Hs <= Ha): the accepted baseline renders bit-identically with or
  // without this clamp. Pegged-but-not-transient frames (skip-backs in
  // sustain) keep the full span - the landing range IS the span rule.
  const long long transLo = (isTransient && tolMinus_ > lov - 1) ? -(lov - 1) : -tolMinus_;

  long long best = 0;
  long long cont = 0;
  bool pegged = false, searched = false;
  double bestScore = -2.0;
  if (!firstFrame_)
  {
    // Target: the CURRENT overlap-region content [olaPos, olaPos+Lov) -
    // exactly what the candidate's first Lov samples will blend WITH in
    // the OLA below (fully written by the previous frame: its span ends
    // at (k-1)*Hs+W = k*Hs+Lov). Correlating against the tail BEFORE the
    // overlap instead is a real bug found by probe (systematic fractional
    // misalignment, ~7% flat pitch on sines): the blend partner is the
    // only window that can judge join coherence.
    double tailE = 0.0;
    for (int j = 0; j < lov; ++j)
    {
      const float t = outAcc_[static_cast<size_t>((olaPos_ + j) & outMask_)];
      tailE += static_cast<double>(t) * t;
    }
    if (tailE >= kSilenceEnergy)
    {
      // Pass 1: normalized cross-correlation over [-Dm, +Dp], 0-outward
      // (0, -1, +1, -2, +2, ..., skipping whichever side exhausts first)
      // into scoreBuf_. Symmetric Dm == Dp visits lags in exactly the
      // baseline order, so the default config renders bit-identically.
      // Pre-fill: unvisited lags (transient floor) read -2.0 instead of
      // a stale frame's scores. Behavior-neutral (pass 2 only reads
      // visited lags) and keeps landscape()/trace honest.
      searched = true;
      std::fill(scoreBuf_.begin(), scoreBuf_.end(), -2.0);
      const long long maxM = tolMinus_ > tolPlus_ ? tolMinus_ : tolPlus_;
      for (long long m = 0; m <= maxM; ++m)
        for (int side = 0; side < 2; ++side)
        {
          if (m == 0 && side == 1)
            continue; // d = 0 visited once
          const long long d = (side == 0) ? -m : m;
          if (d < transLo || d > tolPlus_)
            continue;
          double num = 0.0, candE = 0.0;
          for (int j = 0; j < lov; ++j)
          {
            const long long ip = nominal + d + j;
            const float c =
                (ip < 0 || ip >= inCount_) ? 0.0f : inRing_[static_cast<size_t>(ip & inMask_)];
            const float t = outAcc_[static_cast<size_t>((olaPos_ + j) & outMask_)];
            num += static_cast<double>(c) * t;
            candE += static_cast<double>(c) * c;
          }
          const double score = num / std::sqrt(candE * tailE + kScoreEps);
          scoreBuf_[static_cast<size_t>(d + tolMinus_)] = score;
          if (score > bestScore)
            bestScore = score;
        }
      // Pass 2: drift-seeking tie-break with transient guard. Among
      // lags within the tie band of the max, take the one closest to
      // exact continuation (drift step -(Ha-Hs)): frame advance Hs makes
      // the compressed stream 1:1 with the input, so the slow read yields
      // input[r*t] (pitch x r, proven); pinned lags (advance Ha) yield
      // dry. Continuation always scores ~1.0 (it IS the target samples),
      // so argmax drifts unaided - the tie-break only keeps periodic
      // ties drifting instead of center-pinning (pin = dry, the -1
      // collapse). At the -Dm peg the score slides off and the max jumps
      // back to a high-score lag (aligned skip-back); drift+jump cycles
      // are textbook WSOLA pitch shifting. Transients use the 1e-6 band
      // (outright max: attacks align, never smear through drift).
      // Strict < keeps the first on exact distance ties: deterministic.
      cont = prevDelta_ - (hopA_ - hopS_);
      // Peg rule: cont can only exit below -Dm (drift step Ha-Hs >= 0
      // for downshift, so cont <= prevDelta <= +Dp always - no upper
      // peg exists, symmetric or asymmetric). When pegged the drift run
      // is over and closest-to-continuation would pin the -Dm edge
      // whenever it scores within the band (a stuck peg pins dry -
      // found by probe); instead take the outright max like a transient
      // (the skip-back; drift resumes from the landing).
      pegged = (cont < -tolMinus_);
      const double tieEps = (isTransient || pegged) ? kTieEpsTransient : kTieEps;
      // E2: SmallSkip landing rule (pegged NON-transient frames only -
      // transients keep outright max: the guard is ahead of SoundTouch
      // and stays out of this A/B). Among VALID lags (score within
      // kSkipBand of the frame max), take the minimal time-map jump
      // |d - prevDelta|, excluding d == prevDelta (J = 0 pins dry -
      // the anti-stick exclusion). Search order: jump size m = 1, 2,
      // ...; forward (+m, debt-repaying) before backward (-m): first
      // valid wins, fully deterministic. No valid lag off the peg falls
      // through to the standard rule below (outright max - degenerates,
      // never stuck). Drift (non-pegged) frames always use the standard
      // rule: drift-seeking already minimizes skip size among
      // near-perfect lags, so the landing rule is the whole A/B.
      bool e2Found = false;
      if (mode_ == Mode::SmallSkip && pegged && !isTransient)
      {
        for (long long m = 1; m <= tolPlus_ - transLo && !e2Found; ++m)
          for (int side = 0; side < 2; ++side)
          {
            const long long d = (side == 0) ? (prevDelta_ + m) : (prevDelta_ - m);
            if (d < transLo || d > tolPlus_)
              continue;
            if (scoreBuf_[static_cast<size_t>(d + tolMinus_)] < bestScore - skipBand_)
              continue;
            best = d;
            e2Found = true;
            break;
          }
      }
      if (!e2Found)
      {
        double bestDist = 1e300;
        for (long long m = 0; m <= maxM; ++m)
          for (int side = 0; side < 2; ++side)
          {
            if (m == 0 && side == 1)
              continue;
            const long long d = (side == 0) ? -m : m;
            if (d < transLo || d > tolPlus_)
              continue;
            if (scoreBuf_[static_cast<size_t>(d + tolMinus_)] < bestScore - tieEps)
              continue;
            const double dist = static_cast<double>((d > cont) ? (d - cont) : (cont - d));
            if (dist < bestDist)
            {
              bestDist = dist;
              best = d;
            }
          }
      }
    }
    // else: silent tail - keep nominal (best = 0), no garbage alignment.
  }
  if (traceOn_)
  {
    FrameTrace r;
    r.frame = framesPlaced_;
    r.nominal = nominal;
    r.best = best;
    r.cont = cont;
    r.prevDelta = prevDelta_;
    r.isTransient = isTransient;
    r.pegged = pegged;
    r.first = firstFrame_;
    r.searched = searched;
    if (searched)
    {
      // Top-3 over ascending lag (strict > : deterministic ties).
      for (long long d = -tolMinus_; d <= tolPlus_; ++d)
      {
        const double s = scoreBuf_[static_cast<size_t>(d + tolMinus_)];
        for (int t = 0; t < 3; ++t)
          if (s > r.topScore[t])
          {
            for (int u = 2; u > t; --u)
            {
              r.topScore[u] = r.topScore[u - 1];
              r.topLag[u] = r.topLag[u - 1];
            }
            r.topScore[t] = s;
            r.topLag[t] = d;
            break;
          }
      }
      r.selScore = scoreBuf_[static_cast<size_t>(best + tolMinus_)];
    }
    trace_.push_back(r);
  }
  firstFrame_ = false;
  const long long churn =
      (best > prevDelta_) ? (best - prevDelta_) : (prevDelta_ - best);
  telemetry_.lagChurn += churn;
  telemetry_.transientFrames += isTransient ? 1 : 0;
  ++telemetry_.frames;
  prevDelta_ = best;
  ++framesPlaced_;

  // Overlap-add the chosen frame at olaPos_: raised-cosine crossfade over
  // Lov, verbatim copy for the fresh Hs tail. Sequential complementary
  // crossfades preserve unity gain by induction (down + up == 1). The
  // first frame has no prior output to blend with, so it goes verbatim
  // (no gratuitous fade-in; deterministic start, no burst either way).
  const bool isFirst = (olaPos_ == 0);
  const long long start = nominal + best;
  for (int j = 0; j < lov; ++j)
  {
    const long long ip = start + j;
    const float c = (ip < 0 || ip >= inCount_) ? 0.0f : inRing_[static_cast<size_t>(ip & inMask_)];
    const size_t idx = static_cast<size_t>((olaPos_ + j) & outMask_);
    outAcc_[idx] = isFirst ? c : fadeDown_[static_cast<size_t>(j)] * outAcc_[idx] +
                                        fadeUp_[static_cast<size_t>(j)] * c;
  }
  for (int j = lov; j < w; ++j)
  {
    const long long ip = start + j;
    const float c = (ip < 0 || ip >= inCount_) ? 0.0f : inRing_[static_cast<size_t>(ip & inMask_)];
    outAcc_[static_cast<size_t>((olaPos_ + j) & outMask_)] = c;
  }

  // Frame k finalizes compressed [k*Hs, (k+1)*Hs): hand Hs samples to the
  // resampler ring.
  for (int j = 0; j < hs; ++j)
  {
    compRing_[static_cast<size_t>(compEmitted_ & compMask_)] =
        outAcc_[static_cast<size_t>((olaPos_ + j) & outMask_)];
    ++compEmitted_;
  }
  olaPos_ += hs;

  // Slow fractional read at the TRUE rounded ratio (self-consistent pitch
  // + duration, like LabPitchShift): output o samples o*ar.
  for (;;)
  {
    const double p = resampOut_ * actualRatio_;
    const long long q = static_cast<long long>(std::floor(p));
    if (q + 2 > compEmitted_ - 1)
      break;
    const double f = p - static_cast<double>(q);
    const double ym1 = compressedAt(q - 1);
    const double y0 = compressedAt(q);
    const double y1 = compressedAt(q + 1);
    const double y2 = compressedAt(q + 2);
    const double c1 = 0.5 * (y1 - ym1);
    const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
    const double c3 = -0.5 * ym1 + 1.5 * y0 - 1.5 * y1 + 0.5 * y2;
    pushOutput(static_cast<float>(y0 + f * (c1 + f * (c2 + f * c3))));
    ++resampOut_;
  }
}

void LabWsolaV2::processBlock(const float* input, float* output, int numFrames)
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
      output[i] = 0.0f; // enabled but never reset: silence, like LabPitchShift
    return;
  }
  const int fifoCap = static_cast<int>(outFifo_.size());
  for (int i = 0; i < numFrames; ++i)
  {
    const float x = input[i]; // read first: in-place safe
    inRing_[static_cast<size_t>(inCount_ & inMask_)] = x;
    ++inCount_;
    if (inCount_ >= nextFrameAt_)
    {
      placeFrame();
      nextFrameAt_ += hopA_;
    }
    // Pops held until tick L: the FIFO prefills during startup, so output
    // o emits at exactly tick o+L and mid-stream starvation is
    // impossible by the margin proof in reset() (not by luck).
    if (outCount_ >= latency_ && fifoCount_ > 0)
    {
      output[i] = outFifo_[static_cast<size_t>(fifoRead_)];
      fifoRead_ = (fifoRead_ + 1) % fifoCap;
      --fifoCount_;
    }
    else
    {
      output[i] = 0.0f;
    }
    ++outCount_;
  }
}
} // namespace lab
} // namespace tdm

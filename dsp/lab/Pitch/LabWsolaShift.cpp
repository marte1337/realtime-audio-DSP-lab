// LabWsolaShift: WSOLA time compressor + cubic slow read. See header.

#include "dsp/lab/Pitch/LabWsolaShift.h"

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
constexpr double kTieEps = 1e-3;
constexpr double kTieEpsTransient = 1e-6;
constexpr double kFluxRise = 6.0; // HP-energy rise vs trailing average
constexpr double kFluxTrailRate = 0.25; // trailing-average update per frame

int ceilPow2(int v)
{
  int p = 32;
  while (p < v)
    p <<= 1;
  return p;
}
} // namespace

LabWsolaShift::LabWsolaShift() = default;

void LabWsolaShift::setConfig(double windowMs)
{
  if (windowMs != 20.0 && windowMs != 30.0 && windowMs != 40.0)
    throw std::invalid_argument("LabWsolaShift: windowMs must be one of {20.0, 30.0, 40.0}");
  windowMs_ = windowMs;
}

void LabWsolaShift::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("LabWsolaShift: sample rate out of range");
  sampleRate_ = sampleRate;
  bypass0_ = (shiftSt_ == 0.0f); // exact equality, like LabPitchShift

  frameLen_ = static_cast<int>(windowMs_ * sampleRate / 1000.0 + 0.5);
  if (frameLen_ < 64)
    frameLen_ = 64; // unreachable at >= 8 kHz, kept as a guard
  hopA_ = frameLen_ / 2; // 50% nominal analysis overlap
  // Symmetric search tolerance W/2 (NOT W/4): the search SPAN (2D) must
  // cover a strong period of the lowest content for skip-back coherence
  // (probed span rule: span < fund-period pins the time map (dry) or
  // mushes jumps (warble) - low-B fund needs 778 samples @48k, so W/4
  // (span 480/720) fails low-B at wms20/30 while W/2 (span = W) clears
  // it. Costs latency (L = W+D+C = 1.5W+C: 30/45/60 ms) and hidden lag
  // (<= D); wms40 thus loses to PV-D on latency (report positioning:
  // wms40-vs-A on LF quality, wms20/30-vs-D on latency).
  tol_ = frameLen_ / 2;
  const double ratio = std::exp2(static_cast<double>(shiftSt_) / 12.0);
  hopS_ = static_cast<int>(ratio * hopA_ + 0.5);
  if (hopS_ < 1)
    hopS_ = 1; // unreachable at r >= 2^-2, kept as a guard
  if (hopS_ > hopA_)
    hopS_ = hopA_; // unreachable at r <= 1, kept as a guard
  actualRatio_ = static_cast<double>(hopS_) / hopA_;
  overlap_ = frameLen_ - hopS_; // Lov >= W/2 always (Hs <= Ha <= W/2)

  // Latency: W (frame fill) + D (symmetric search lookahead) + C.
  // Pops are HELD until tick L (see processBlock), so the FIFO prefills
  // and output o emits at exactly tick o+L. Starvation-free by proof:
  // output o needs compressed through floor(o*ar)+2, produced by frame
  // j* with (j*+1)*Hs > floor(o*ar)+2, placed at tick j**Ha+D+W-1 <=
  // o+3/ar+D+W-1; holding pops to o+W+D+C never starves iff
  // C >= 3/ar-1, and C = ceil(3/ar-1) is the smallest integer that does.
  // (An earlier C = ceil(2/ar)+2 with no hold starved intermittently at
  // -24 - caught by onset probe, fixed by construction, pinned by the
  // grid-wide DC starvation test.) NOTE L is NOT an onset-alignment
  // claim: input at sub-frame offset j0 legitimately lands j0*(1/ar-1)
  // late in the output (the slow read stretches within-frame positions;
  // the correlation search usually but not always compensates).
  // Transient placement slop up to ~W*(1/ar-1) is inherent WSOLA
  // behavior, measured honestly in analysis, never tuned into L.
  const int c = static_cast<int>(std::ceil(3.0 / actualRatio_ - 1.0));
  latency_ = bypass0_ ? 0 : frameLen_ + tol_ + c;
  tail_ = frameLen_ + hopA_; // conservative flush (verified by tail test)

  scoreBuf_.assign(static_cast<size_t>(2 * tol_ + 1), -2.0);
  prevDelta_ = 0;
  fluxTrail_ = 0.0;
  framesPlaced_ = 0;
  telemetry_ = Telemetry{};
  fadeUp_.assign(static_cast<size_t>(overlap_), 0.0f);
  fadeDown_.assign(static_cast<size_t>(overlap_), 0.0f);
  for (int j = 0; j < overlap_; ++j)
  {
    const double a = 0.5 * kTwoPi * 0.5 * (j + 1) / (overlap_ + 1); // 0..pi/2
    const double s = std::sin(a);
    fadeUp_[static_cast<size_t>(j)] = static_cast<float>(s * s);
    fadeDown_[static_cast<size_t>(j)] = static_cast<float>(1.0 - s * s);
  }

  inRing_.assign(static_cast<size_t>(ceilPow2(2 * tol_ + frameLen_ + hopA_ + 16)), 0.0f);
  inMask_ = static_cast<int>(inRing_.size()) - 1;
  inCount_ = 0;
  nextFrameAt_ = tol_ + frameLen_; // frame 0 places once its span arrives

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

void LabWsolaShift::setShiftSt(float semitones)
{
  shiftSt_ = semitones < kMinShiftSt ? kMinShiftSt : (semitones > kMaxShiftSt ? kMaxShiftSt : semitones);
}

void LabWsolaShift::setEnabled(bool enabled)
{
  enabled_ = enabled;
}

float LabWsolaShift::compressedAt(long long pos) const
{
  if (pos < 0)
    return 0.0f; // zero pre-roll before the stream starts
  return compRing_[static_cast<size_t>(pos & compMask_)];
}

void LabWsolaShift::pushOutput(float v)
{
  const int fifoCap = static_cast<int>(outFifo_.size());
  outFifo_[static_cast<size_t>(fifoWrite_)] = v;
  fifoWrite_ = (fifoWrite_ + 1) % fifoCap;
  ++fifoCount_;
}

void LabWsolaShift::placeFrame()
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

  long long best = 0;
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
      // Pass 1: normalized cross-correlation over the symmetric
      // tolerance, 0-outward (0, -1, +1, ...) into scoreBuf_.
      double bestScore = -2.0;
      for (long long step = 0; step <= 2 * tol_; ++step)
      {
        const long long m = (step + 1) / 2;
        const long long d = (step == 0) ? 0 : ((step & 1) ? -m : m);
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
        scoreBuf_[static_cast<size_t>(d + tol_)] = score;
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
      // collapse). At the -D peg the score slides off and the max jumps
      // back to a high-score lag (aligned skip-back); drift+jump cycles
      // are textbook WSOLA pitch shifting. Transients use the 1e-6 band
      // (outright max: attacks align, never smear through drift).
      // Strict < keeps the first on exact distance ties: deterministic.
      const long long cont = prevDelta_ - (hopA_ - hopS_);
      // Peg rule: cont can only exit below -D (drift step >= 0 for
      // downshift, so cont <= prevDelta <= +D always). When pegged the
      // drift run is over and closest-to-continuation would pin the -D
      // edge whenever it scores within the band (a stuck peg pins dry -
      // found by probe); instead take the outright max like a transient
      // (the skip-back; drift resumes from the landing).
      const bool pegged = (cont < -tol_);
      const double tieEps = (isTransient || pegged) ? kTieEpsTransient : kTieEps;
      double bestDist = 1e300;
      for (long long step = 0; step <= 2 * tol_; ++step)
      {
        const long long m = (step + 1) / 2;
        const long long d = (step == 0) ? 0 : ((step & 1) ? -m : m);
        if (scoreBuf_[static_cast<size_t>(d + tol_)] < bestScore - tieEps)
          continue;
        const double dist =
            static_cast<double>((d > cont) ? (d - cont) : (cont - d));
        if (dist < bestDist)
        {
          bestDist = dist;
          best = d;
        }
      }
    }
    // else: silent tail - keep nominal (best = 0), no garbage alignment.
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

void LabWsolaShift::processBlock(const float* input, float* output, int numFrames)
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

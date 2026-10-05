// Tuner: YIN-style chromatic analyzer. See header for the design contract.

#include "dsp/Tuner/Tuner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tdm
{
namespace
{
constexpr double kPiTuner = 3.14159265358979;

int nextPow2(int n)
{
  int s = 1;
  while (s < n)
    s <<= 1;
  return s;
}

float clamp01(float x)
{
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}
} // namespace

Tuner::Tuner() = default;

void Tuner::reset(double sampleRate)
{
  if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0))
    throw std::invalid_argument("Tuner: sample rate out of range");
  sampleRate_ = sampleRate;
  // Full-rate refine stage: window holds ~2+ periods of the lowest target.
  window_ = nextPow2(static_cast<int>(std::ceil(2.0 * sampleRate / kMinHz)));
  tauMin_ = std::max(2, static_cast<int>(sampleRate / kMaxHz));
  tauMax_ = std::max(tauMin_ + 8, static_cast<int>(sampleRate / kMinHz));
  // Decimated coarse stage (~12 kHz internal): same time spans.
  decFactor_ = std::max(1, static_cast<int>(sampleRate / 12000.0 + 0.5));
  const double srDec = sampleRate / decFactor_;
  windowDec_ = nextPow2(static_cast<int>(std::ceil(2.0 * srDec / kMinHz)));
  hopDec_ = windowDec_ / 2;
  hop_ = hopDec_ * decFactor_;
  tauMinDec_ = std::max(2, static_cast<int>(srDec / kMaxHz));
  tauMaxDec_ = std::max(tauMinDec_ + 8, static_cast<int>(srDec / kMinHz));
  const int ringSize = nextPow2(window_ + tauMax_ + hop_);
  ring_.assign(static_cast<size_t>(ringSize), 0.0f);
  ringMask_ = ringSize - 1;
  const int ringDecSize = nextPow2(windowDec_ + tauMaxDec_ + hopDec_);
  ringDec_.assign(static_cast<size_t>(ringDecSize), 0.0f);
  ringDecMask_ = ringDecSize - 1;
  yin_.assign(static_cast<size_t>(tauMax_) + 1, 0.0f);
  linear_.assign(static_cast<size_t>(window_ + tauMax_), 0.0f);
  // Anti-alias biquad: 2nd-order Butterworth lowpass at 3 kHz (RBJ).
  {
    const double w0 = 2.0 * kPiTuner / sampleRate * 3000.0;
    const double cosW = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    lpB0_ = (1.0 - cosW) / 2.0 / a0;
    lpB1_ = (1.0 - cosW) / a0;
    lpB2_ = (1.0 - cosW) / 2.0 / a0;
    lpA1_ = -2.0 * cosW / a0;
    lpA2_ = (1.0 - alpha) / a0;
  }
  writePos_ = 0;
  writeDec_ = 0;
  decPhase_ = 0;
  lpZ1_ = lpZ2_ = 0.0;
  sinceAnalysis_ = 0;
  analysesRun_ = 0;
  published_ = TunerResult{};
  seq_.store(0, std::memory_order_relaxed);
}

void Tuner::setEnabled(bool enabled)
{
  if (enabled == enabled_)
    return;
  enabled_ = enabled;
  if (enabled)
  {
    // Fresh start: stale ring audio must never produce a reading.
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(ringDec_.begin(), ringDec_.end(), 0.0f);
    lpZ1_ = lpZ2_ = 0.0;
    decPhase_ = 0;
    sinceAnalysis_ = 0;
  }
  else
  {
    // Park an invalid result so the UI blanks promptly.
    seq_.fetch_add(1, std::memory_order_release);
    published_ = TunerResult{};
    seq_.fetch_add(1, std::memory_order_release);
  }
}

void Tuner::feedBlock(const float* mono, int numFrames)
{
  if (!enabled_ || mono == nullptr || numFrames <= 0 || sampleRate_ <= 0.0)
    return;
  for (int i = 0; i < numFrames; ++i)
  {
    const float s = std::isfinite(mono[i]) ? mono[i] : 0.0f;
    ring_[static_cast<size_t>(writePos_ & ringMask_)] = s;
    ++writePos_;
    // Anti-alias biquad (transposed direct form II) + decimate.
    const double y = lpB0_ * s + lpZ1_;
    lpZ1_ = lpB1_ * s - lpA1_ * y + lpZ2_;
    lpZ2_ = lpB2_ * s - lpA2_ * y;
    if (std::fabs(lpZ1_) < 1e-12)
      lpZ1_ = 0.0; // denormal guard (house pattern)
    if (std::fabs(lpZ2_) < 1e-12)
      lpZ2_ = 0.0;
    if (++decPhase_ >= decFactor_)
    {
      decPhase_ = 0;
      ringDec_[static_cast<size_t>(writeDec_ & ringDecMask_)] = static_cast<float>(y);
      ++writeDec_;
      ++sinceAnalysis_;
    }
  }
  // At most one bounded analysis per call (strict RT bound); cadence
  // slips by a block under oversized blocks, harmless for a tuner.
  if (sinceAnalysis_ >= hopDec_)
  {
    sinceAnalysis_ = 0;
    analyze();
  }
}

void Tuner::result(TunerResult& out) const
{
  // Seqlock read: retry while the writer is mid-publish.
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    const unsigned a = seq_.load(std::memory_order_acquire);
    if ((a & 1u) != 0u)
      continue;
    const TunerResult copy = published_;
    const unsigned b = seq_.load(std::memory_order_acquire);
    if (a == b)
    {
      out = copy;
      return;
    }
  }
  out = published_; // best effort (writer constantly ahead; still safe)
}

void Tuner::analyze()
{
  ++analysesRun_;
  auto publish = [&](const TunerResult& r) {
    seq_.fetch_add(1, std::memory_order_release);
    published_ = r;
    seq_.fetch_add(1, std::memory_order_release);
  };
  // Stage 1 (coarse): linearize the decimated span and gate on its level.
  const int spanDec = windowDec_ + tauMaxDec_;
  const long long baseDec = writeDec_ - spanDec;
  double sumSq = 0.0;
  for (int i = 0; i < spanDec; ++i)
  {
    const float s = ringDec_[static_cast<size_t>((baseDec + i) & ringDecMask_)];
    linear_[static_cast<size_t>(i)] = std::isfinite(s) ? s : 0.0f;
    if (i < windowDec_)
      sumSq += static_cast<double>(linear_[static_cast<size_t>(i)]) * linear_[static_cast<size_t>(i)];
  }
  // Level gate: silence/low rumble yields invalid (and skips the YIN pass).
  const double gate = std::pow(10.0, kLevelGateDb / 20.0);
  if (!(sumSq > gate * gate * windowDec_)) // also catches NaN
  {
    publish(TunerResult{});
    return;
  }
  // Coarse YIN difference + CMND over the decimated lag range.
  const float* xd = linear_.data();
  for (int tau = 0; tau <= tauMaxDec_; ++tau)
  {
    double d = 0.0;
    for (int j = 0; j < windowDec_; ++j)
    {
      const double e = static_cast<double>(xd[j]) - xd[j + tau];
      d += e * e;
    }
    yin_[static_cast<size_t>(tau)] = static_cast<float>(d);
  }
  yin_[0] = 1.0f;
  double runSum = 0.0;
  for (int tau = 1; tau <= tauMaxDec_; ++tau)
  {
    runSum += yin_[static_cast<size_t>(tau)];
    const float cmnd =
        (runSum > 1e-12) ? static_cast<float>(yin_[static_cast<size_t>(tau)] * tau / runSum) : 1.0f;
    yin_[static_cast<size_t>(tau)] = cmnd;
  }
  // Absolute threshold: first dip below, then the local minimum.
  // Shortest-period-first scanning is the octave guard.
  int tauDec = -1;
  for (int t = tauMinDec_; t <= tauMaxDec_; ++t)
  {
    if (yin_[static_cast<size_t>(t)] < kYinThreshold)
    {
      tauDec = t;
      break;
    }
  }
  if (tauDec < 0)
  {
    publish(TunerResult{}); // aperiodic content (noise/chords/silence gaps)
    return;
  }
  while (tauDec + 1 <= tauMaxDec_ && yin_[static_cast<size_t>(tauDec + 1)] < yin_[static_cast<size_t>(tauDec)])
    ++tauDec;
  // Subharmonic guard: a bright attack can pull the threshold scan onto a
  // sub-period (octave-up error). A true sub-period dip is shallow next to
  // the real period's dip, while a genuine high note shows comparable
  // depth at its multiples (any T-periodic signal repeats at 2T) -- so a
  // multiple that is MUCH deeper wins. Iterate to a fixpoint (bounded:
  // the lag only grows toward tauMaxDec_).
  for (;;)
  {
    int better = -1;
    const float here = yin_[static_cast<size_t>(tauDec)];
    for (int mult = 2; mult <= 4; ++mult)
    {
      const int cand = tauDec * mult;
      if (cand > tauMaxDec_)
        break;
      if (yin_[static_cast<size_t>(cand)] < here * 0.25f)
      {
        better = cand; // lowest much-deeper multiple; iterate from there
        break;
      }
    }
    if (better < 0)
      break;
    tauDec = better;
  }
  const float confidence = clamp01(1.0f - yin_[static_cast<size_t>(tauDec)]);
  // Stage 2 (refine): full-rate difference in a narrow band around the
  // coarse lag, minimum + parabolic refinement for cents accuracy. The
  // band is generous (the coarse dip family is what matters, not its
  // exact subsample center).
  const int halfBand = 2 * decFactor_ + 4;
  const int bandLo = std::max(tauMin_, tauDec * decFactor_ - halfBand);
  const int bandHi = std::min(tauMax_, tauDec * decFactor_ + halfBand);
  const int span = window_ + bandHi;
  const long long base = writePos_ - span;
  for (int i = 0; i < span; ++i)
    linear_[static_cast<size_t>(i)] = ring_[static_cast<size_t>((base + i) & ringMask_)];
  const float* x = linear_.data();
  int tauBest = bandLo;
  double dBest = 1e300;
  for (int tau = bandLo; tau <= bandHi; ++tau)
  {
    double d = 0.0;
    for (int j = 0; j < window_; ++j)
    {
      const double e = static_cast<double>(x[j]) - x[j + tau];
      d += e * e;
    }
    if (d < dBest)
    {
      dBest = d;
      tauBest = tau;
    }
  }
  // Full-rate self-similarity validation: the coarse (lowpassed) stage
  // can lock onto residue periodicity that the raw input does not share
  // (broadband pick noise). A genuine period repeats: normalized
  // difference << 1. Noise sits near 1.0; 0.35 splits them with margin.
  {
    double eBest = 1e-12;
    for (int j = 0; j < window_; ++j)
      eBest += static_cast<double>(x[j]) * x[j] + static_cast<double>(x[j + tauBest]) * x[j + tauBest];
    if (!(dBest / eBest <= 0.35))
    {
      publish(TunerResult{});
      return;
    }
  }
  // Parabolic refinement of the difference minimum (recompute the two
  // neighbors; bounded: two extra O(window) passes).
  double refined = tauBest;
  if (tauBest > bandLo && tauBest < bandHi)
  {
    auto diffAt = [&](int tau) {
      double d = 0.0;
      for (int j = 0; j < window_; ++j)
      {
        const double e = static_cast<double>(x[j]) - x[j + tau];
        d += e * e;
      }
      return d;
    };
    const double y1 = diffAt(tauBest - 1);
    const double y3 = diffAt(tauBest + 1);
    const double denom = y1 - 2.0 * dBest + y3;
    if (std::fabs(denom) > 1e-9 * (1.0 + dBest))
      refined = tauBest + std::clamp(0.5 * (y1 - y3) / denom, -0.5, 0.5);
  }
  const double freq = sampleRate_ / refined;
  if (!(freq >= kMinHz - 5.0 && freq <= kMaxHz + 100.0) || !std::isfinite(freq))
  {
    publish(TunerResult{});
    return;
  }
  const TunerNote note = noteFor(static_cast<float>(freq), kDefaultRefHz);
  TunerResult r;
  r.valid = true;
  r.frequencyHz = static_cast<float>(freq);
  r.cents = note.cents;
  r.midiNote = note.midi;
  r.confidence = confidence;
  publish(r);
}

TunerNote Tuner::noteFor(float freqHz, float refHz)
{
  TunerNote note;
  const double f = (std::isfinite(freqHz) && freqHz > 0.0f) ? freqHz : 440.0;
  const double ref = (std::isfinite(refHz) && refHz > 0.0f) ? refHz : 440.0;
  const double midiFloat = 69.0 + 12.0 * (std::log(f / ref) / std::log(2.0));
  const int midi = static_cast<int>(std::floor(midiFloat + 0.5));
  static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
  const int pc = ((midi % 12) + 12) % 12;
  const char* nm = kNames[pc];
  note.name[0] = nm[0];
  note.name[1] = nm[1]; // '\0' for naturals, '#' otherwise
  note.name[2] = '\0';
  note.name[3] = '\0';
  note.midi = midi;
  note.octave = midi / 12 - 1; // MIDI 69 -> 4 (A4)
  note.cents = static_cast<float>(100.0 * (midiFloat - midi)); // [-50, +50)
  return note;
}

TunerDisplay::TunerDisplay() = default;

void TunerDisplay::reset()
{
  state_ = State{};
  pendingMidi_ = -1;
  pendingCount_ = 0;
  invalidCount_ = 0;
  smoothCents_ = 0.0f;
  haveSmooth_ = false;
}

const char* TunerDisplay::nameFor(int midi)
{
  static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
  return kNames[((midi % 12) + 12) % 12];
}

void TunerDisplay::update(const TunerResult& raw)
{
  if (!raw.valid)
  {
    pendingMidi_ = -1;
    pendingCount_ = 0;
    if (++invalidCount_ >= 3)
    {
      state_.valid = false; // blank after ~300 ms of no signal at 10 Hz
      haveSmooth_ = false;
    }
    return; // brief invalids hold the last display state
  }
  invalidCount_ = 0;
  // Fresh attack from blank: needs one confirmation tick, like any note
  // change (kills single-tick transient flashes; still fast at 10 Hz).
  if (!state_.valid)
  {
    if (raw.midiNote == pendingMidi_ && ++pendingCount_ >= 1)
    {
      state_.valid = true;
      state_.midiNote = raw.midiNote;
      state_.name = nameFor(raw.midiNote);
      state_.octave = raw.midiNote / 12 - 1;
      smoothCents_ = raw.cents;
      haveSmooth_ = true;
      state_.cents = smoothCents_;
      state_.frequencyHz = raw.frequencyHz;
      pendingCount_ = 0;
    }
    else if (raw.midiNote != pendingMidi_)
    {
      pendingMidi_ = raw.midiNote;
      pendingCount_ = 0;
    }
    return;
  }
  if (raw.midiNote == state_.midiNote)
  {
    pendingMidi_ = raw.midiNote;
    pendingCount_ = 0;
    // Light cents smoothing (fast follower, not sluggish).
    smoothCents_ = haveSmooth_ ? 0.5f * smoothCents_ + 0.5f * raw.cents : raw.cents;
    haveSmooth_ = true;
    state_.cents = smoothCents_;
    state_.frequencyHz = raw.frequencyHz;
    return;
  }
  // Note change needs one confirmation tick (kills semitone flicker).
  if (raw.midiNote == pendingMidi_)
  {
    if (++pendingCount_ >= 1)
    {
      state_.midiNote = raw.midiNote;
      state_.name = nameFor(raw.midiNote);
      state_.octave = raw.midiNote / 12 - 1;
      smoothCents_ = raw.cents;
      state_.cents = smoothCents_;
      state_.frequencyHz = raw.frequencyHz;
      pendingCount_ = 0;
    }
    return;
  }
  pendingMidi_ = raw.midiNote;
  pendingCount_ = 0;
}
} // namespace tdm

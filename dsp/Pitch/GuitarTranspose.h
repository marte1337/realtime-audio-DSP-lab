#pragma once

// GuitarTranspose: production guitar transpose engine.
//
// Promoted verbatim from the accepted lab baseline (GuitarTransposeV2):
// same algorithm, same defaults, same renders (see the golden regression
// in tests/TestGuitarTranspose.cpp). Wired into TechDeathRig between
// Gate and TightDrive; the validated primary use is fixed detune -1/-2,
// with -12..+12 available. Do not retune here: this file is the frozen
// acoustic baseline, and DEV experiments override Config off-RT only
// (see dsp/lab/Pitch/DevTranspose.h), never by editing these defaults.
//
// Algorithm: Doppler pitch shifting with rare waveform-matched splices.
// One fractional read tap runs through a delay ring at the pitch ratio,
// so its delay behind the write head drifts between a floor (a few ms)
// and the buffer size (30 ms class). Drift IS the shift: a downshift tap
// falls back through history and reads the past progressively slower.
// When the tap approaches the range end it splices (jumps) to a causal
// landing nearer the floor; the landing is chosen by normalized
// cross-correlation of the composite waveform, scored to balance match
// quality against the drift time the jump buys, so splices are large
// (nearly the full range) and rare (a few per second at -1/-2) instead
// of the per-frame micro-joins of WSOLA. Old and new taps crossfade
// with a raised-cosine whose length follows the match (short for clean
// periodic locks, long for chord compromises) and whose gains are
// normalized by the measured tap correlation so level holds through
// long fades. A guitar onset detector (HF energy vs recent floor and
// ceiling, with refractory period) re-syncs the tap to the floor region
// on pick attacks, so attacks emerge a few ms late wherever the tap had
// drifted. Full-band: no tonality crossover. Independent implementation:
// own code, own constants-as-parameters, no copied source.
//
// Baseline configuration: the Config defaults below ARE the accepted
// baseline (30 ms window, 2 ms floor, 25 ms correlation, 30-120 ms
// match-mapped fades, 2 ms / 4 ms re-sync, 600 Hz detector, resync on).
// Production never calls setConfig: the rig uses a default-constructed
// engine, so production output cannot drift from the baseline. The DEV
// transpose stage may install validated Config overrides off-RT (audio
// stopped, before reset); "Restore Baseline" restores Config{} exactly.
//
// Shift range: the engine accepts [kMinShiftSt, kMaxShiftSt] (-24/+12);
// production exposes -12..+12 through RigParams (clamped at the rig).
// Downshift is primary; dives adopt per sample (a pending search is
// dropped on change, with a synchronous fallback search if the tap then
// reaches the boundary unplanned). Upshift uses landing-range budgeting
// (validated to +12, beyond untested). reset() clears state
// deterministically. Caveat: reset() at exactly 0.0 st selects a
// zero-latency wire (bypass0_) that ignores later live shifts until the
// next reset; the production rig avoids it by priming the engine at a
// nonzero shift when the requested shift is 0 (inaudible: the rig's
// bypass wrapper outputs latency-matched dry at shift 0 regardless).
//
// Latency (all post-reset figures, baseline 30 ms window @ 48 kHz):
// - nominal reported latency: (floor + window) / 2 = 768 samples
//   (16 ms). THE stable host/rig figure (latencySamples()); it never
//   varies per note.
// - instantaneous tap delay: rides between floor (96) and window (1440)
//   as sustains drift; this is where any single output sample sat, not
//   a reportable number.
// - onset re-sync emergence: about floor + span + fade = 384 samples
//   (8 ms, see onsetLatencySamples()); attacks arrive earlier than the
//   nominal figure, perceptually only.
// - engine bypass (disabled, or exact-0-st reset): bit-exact zero-
//   latency wire. The RIG never uses this live: its bypass wrapper
//   outputs dry delayed by the nominal latency (constant feel) with a
//   128-sample ramp, and an exact wire only while fully disengaged.
// tailSamples() is 0: fully causal, no flush requirement. Offline
// callers feed latencySamples() extra zeros and drop the first
// latencySamples() outputs; attacks inside the dropped head start are
// affected by priming like any delay line.
//
// Realtime contract: setConfig/reset are off-RT (reset allocates the
// rings). setShiftSt/setEnabled are RT-safe (clamp + one bounded pow /
// plain store; the rig calls them at block boundaries). processBlock is
// RT-safe (no allocation, no locks, no I/O, deterministic). Worst-case
// per-sample cost is a bounded NCC slice; drift searches spread over a
// lead, onset searches run at once over a small span (bounded burst).
// Telemetry/event counters update on the audio path (plain member
// writes, no allocation); the diagnostic trace stays off unless an
// offline instrument enables it.
//
// Expected signal range: finite floats, nominally DI guitar in [-1, 1].
// Output peak is bounded by roughly the input peak (crossfades only
// blend existing samples; the interpolator never boosts).

#include <cstddef>
#include <vector>

namespace tdm
{
class GuitarTranspose
{
public:
  static constexpr float kMinShiftSt = -24.0f;
  static constexpr float kMaxShiftSt = 12.0f; // upshift budgeted to +12; beyond untested
  static constexpr float kDefaultShiftSt = 0.0f;
  // Production product range (enforced by TechDeathRig/RigParams; the wider
  // engine range above stays available to DEV/study harnesses only).
  static constexpr float kProductionMinShiftSt = -12.0f;
  static constexpr float kProductionMaxShiftSt = 12.0f;

  // Timing/geometry configuration. All times in ms (rate-independent);
  // sample counts derive at reset(). Every field is validated there
  // (throws std::invalid_argument) so DEV/study harnesses can sweep them.
  // The defaults ARE the accepted production baseline: 30 ms-class
  // buffer (the configuration that passed hardware validation), floor a
  // few ms, correlation over ~25 ms, fades 30-120 ms by match, 2 ms
  // re-sync fades. Production constructs Config{} and never changes it.
  struct Config
  {
    double windowMs = 30.0; // max tap delay (dMax), [10, 120]
    double floorMs = 2.0; // min tap delay (dMin), [0.5, 10]
    double corrMs = 25.0; // NCC window, capped at dMax, [5, 60]
    double fadeMinMs = 30.0; // drift fade at strong match, [4, 120]
    double fadeMaxMs = 120.0; // drift fade at poor match, [fadeMin, 250]
    double fadeNccHi = 0.95; // match mapping to fadeMin, (fadeNccLo, 1]
    double fadeNccLo = 0.60; // match mapping to fadeMax, [0, fadeNccHi)
    double onsetFadeMs = 2.0; // re-sync fade, [0.5, 10]
    double onsetSpanMs = 4.0; // re-sync landing span above floor, [1, 12]
    double searchLeadMs = 4.0; // drift search lead before boundary, [1, 16]
    double refractoryMs = 40.0; // onset refractory, [5, 200]
    double detectorHpHz = 600.0; // onset HPF corner, [100, 4000]
    double detectorSmoothMs = 2.0; // onset energy smoothing, [0.5, 10]
    double onsetOverMinDb = 9.0; // trigger over recent floor, [3, 24]
    double onsetOverMaxDb = 6.0; // trigger over recent ceiling, [1, 18]
    int historyCells = 50; // 1 ms detector cells, [10, 200]
    int historySkip = 5; // newest cells excluded from reference, [1, 20]
    bool enableResync = true; // false: detector runs+traces, never acts
  };

  GuitarTranspose();

  // Off-RT config override (DEV/study only; production never calls this):
  // validated at reset(), not here, so fields may be set in any order.
  // Takes effect on the next reset().
  void setConfig(const Config& cfg);
  const Config& config() const { return config_; }

  // Off-RT: validates rate + config, derives geometry, (re)allocates
  // rings/reference/history, clears all state. Deterministic start:
  // zero history, tap parked at the floor.
  void reset(double sampleRate);

  // Shift in semitones, clamped to [kMinShiftSt, kMaxShiftSt].
  // Adopted per sample (a pending search is dropped on change; dives
  // work but are unoptimized in v1). Exactly 0.0f at reset() selects
  // the bit-exact zero-latency bypass.
  void setShiftSt(float semitones);
  void setEnabled(bool enabled);

  float shiftSt() const { return shiftSt_; }
  bool isEnabled() const { return enabled_; }
  // Nominal (mean-delay) latency in samples, valid post-reset: the
  // host-reportable figure. 0 when the exact-0-st bypass is active.
  int latencySamples() const { return latency_; }
  // Expected attack emergence in samples (floor + span + fade), valid
  // post-reset: what an onset re-sync delivers. 0 when bypassed.
  int onsetLatencySamples() const;
  int tailSamples() const { return 0; }

  // Derived geometry in samples (valid post-reset; study introspection).
  int floorSamples() const { return dMin_; }
  int windowSamples() const { return dMax_; }
  int fadeMinSamples() const { return fadeMinLen_; }
  int fadeMaxSamples() const { return fadeMaxLen_; }

  // Mono float processing, in-place safe. Disabled bypass and the
  // exact-0-st bypass copy exactly. Otherwise emits exactly numFrames
  // samples; sustains lag by the riding tap (see latencySamples),
  // re-synced attacks by about onsetLatencySamples().
  void processBlock(const float* input, float* output, int numFrames);

  // Telemetry (cheap counters + fixed event logs, no allocation,
  // updated on the audio path, read off-RT; used by studies + DEV).
  struct Telemetry
  {
    long long samples = 0;
    long long driftSplicesDown = 0;
    long long driftSplicesUp = 0;
    long long syncFallbacks = 0; // boundary reached with no ready plan
    long long droppedFades = 0; // planned landing failed validation
    long long detectorEdges = 0; // rising edges of the trigger predicate
    long long resyncs = 0; // re-syncs actioned
    long long resyncBlockedFade = 0; // genuine onsets arriving mid-fade
    long long resyncBlockedRefr = 0; // onsets inside refractory
    long long resyncBlockedDepth = 0; // onsets with tap already shallow
    // Jump/NCC/fade aggregates over actioned drift splices + re-syncs.
    long long jumpCount = 0;
    long long jumpMin = 0;
    long long jumpMax = 0;
    double jumpSum = 0.0;
    double nccSum = 0.0;
    long long fadeSum = 0;
    // Last actioned splice (any kind).
    long long lastJump = 0;
    double lastNcc = 0.0;
    int lastFadeLen = 0;
  };
  Telemetry telemetry() const { return telemetry_; }

  // Fixed event logs (ring, newest overwrites oldest past capacity).
  struct SpliceEvent
  {
    long long pos = 0; // absolute output sample index at fire
    int kind = 0; // 0 drift-down, 1 drift-up, 2 re-sync
    long long jump = 0; // tap advance in samples (signed)
    double ncc = 0.0; // landing match score
    int fadeLen = 0; // actioned fade length in samples
    int tapDelay = 0; // tap delay at fire in samples
  };
  struct OnsetEvent
  {
    long long pos = 0;
    int tapDelay = 0;
    int action = 0; // 0 actioned, 1 blocked-fade, 2 blocked-refr, 3 blocked-depth, 4 plan-dropped
  };
  static constexpr size_t kEventLogSize = 1024;
  size_t spliceEventCount() const { return spliceCount_; }
  size_t onsetEventCount() const { return onsetCount_; }
  // i in [0, count): 0 is oldest retained.
  SpliceEvent spliceEvent(size_t i) const;
  OnsetEvent onsetEvent(size_t i) const;

  // DIAGNOSTIC detector trace (removable; default OFF). When
  // enabled (off-RT, before processing), each sample appends envelope +
  // threshold multiples + fired flag. Memory grows per sample: offline
  // instrument only, never on the audio thread. Read-only w.r.t. DSP
  // state: enabling cannot change the render. Cleared on enable/reset.
  void enableTrace(bool on);
  const std::vector<float>& traceEnv() const { return traceEnv_; }
  const std::vector<float>& traceOverMin() const { return traceOverMin_; } // e/(omin*min)
  const std::vector<float>& traceOverMax() const { return traceOverMax_; } // e/(omax*max)
  const std::vector<char>& traceFired() const { return traceFired_; }

private:
  double ratioFor(float st) const;
  float readTap(double pos) const; // house cubic kernel over the ring
  double nccAt(long long windowEnd) const; // ref vs ring window, [-1, 1]
  void beginSearch(int lo, int hi, int lead); // lo/hi candidate delays
  void considerCandidate(int d); // score one coarse candidate
  void stepSearch(int count); // score count candidates; finalize at end
  int fadeLenFor(double ncc, double room) const; // match-mapped fade
  bool startFade(long long jump, int fadeLen, int kind); // validated; logs; false if dropped
  void updateUpshiftCap(); // sixth-of-range fade cap for ratio > 1
  bool detectorStep(float x); // one HPF/energy/history step; returns fired edge
  void detectorResetHistory(double level);

  Config config_;
  double sampleRate_ = 0.0;
  float shiftSt_ = kDefaultShiftSt;
  bool enabled_ = false;

  double ratio_ = 1.0; // adopted ratio (per-sample adoption)
  double targetRatio_ = 1.0; // setShiftSt target
  bool bypass0_ = false; // exact-0-st bypass, derived at reset
  int latency_ = 0; // (dMin+dMax)/2, derived at reset

  // Derived geometry (samples).
  int dMin_ = 0;
  int dMax_ = 0;
  int corrLen_ = 0;
  int fadeMinLen_ = 0;
  int fadeMaxLen_ = 0;
  int fadeHiUp_ = 0; // upshift fade cap (sixth-of-range rule)
  int onsetFadeLen_ = 0;
  int onsetSpan_ = 0;
  int searchLead_ = 0;
  int leadNow_ = 0; // upshift-shrunk lead (== searchLead_ unless budgeted)
  int refractory_ = 0;
  int cellLen_ = 1;
  int coarseStep_ = 1;

  // Ring + taps (absolute positions).
  std::vector<float> ring_;
  int ringMask_ = 0;
  long long writePos_ = 0;
  double tapA_ = 0.0;
  double tapB_ = 0.0;
  bool fading_ = false;
  double fadePos_ = 0.0;
  double fadeInc_ = 0.0;
  double fadeNcc_ = 1.0;

  // Incremental lag search.
  struct Search
  {
    bool active = false;
    bool ready = false;
    int lo = 0;
    int hi = 0;
    int next = 0;
    int perSample = 1;
    double bestScore = -1e9;
    double bestNcc = 0.0;
    int bestDelay = 0;
    long long planPos = 0; // writePos at plan time
    long long tapAtPlan = 0; // floor(tapA) at plan time
    long long resultJump = 0;
  };
  Search search_;
  std::vector<float> ref_;
  double refEnergy_ = 0.0;

  // Onset detector: one-pole HPF + energy smoother + 1 ms min/max cells.
  double hpState_ = 0.0;
  double hpPrevX_ = 0.0;
  double hpCoeff_ = 0.0;
  double smoothCoeff_ = 0.0;
  double energy_ = 0.0;
  std::vector<float> minHist_;
  std::vector<float> maxHist_;
  double cellMin_ = 1e9;
  double cellMax_ = 0.0;
  double overMin_ = 1.0; // linear trigger ratios from dB config
  double overMax_ = 1.0;
  long long lastOnset_ = 0;
  bool triggerWasHigh_ = false;

  long long lastDropPos_ = 0; // replan throttle after a dropped fade
  Telemetry telemetry_;
  std::vector<SpliceEvent> spliceLog_;
  std::vector<OnsetEvent> onsetLog_;
  size_t spliceCount_ = 0;
  size_t onsetCount_ = 0;

  bool traceOn_ = false;
  std::vector<float> traceEnv_;
  std::vector<float> traceOverMin_;
  std::vector<float> traceOverMax_;
  std::vector<char> traceFired_;
};
} // namespace tdm

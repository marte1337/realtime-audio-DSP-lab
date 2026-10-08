#pragma once

// SlamImpactV2: Impact v2 lab voices (LAB ONLY, dsp/lab/Slam). The v1
// diagnosis (see DevSlam/Impact work): the dhyb burst is electrically LARGE
// (branch peak ~ main peak) but ~93% of its energy sits below 160 Hz, half
// below 65 Hz — through typical guitar monitoring (70 Hz HPF sim) the
// surviving branch is -5 dB peak / -9 dB RMS under the guitar, i.e. masked.
// Equal-loudness (≈ +15 dB needed at 55 Hz vs 150 Hz) finishes it. So v1's
// failure is frequency placement + masking, not level, timing, or a bug.
//
// v2 keeps the v1 architecture (SlamTrigger pre-drive, scheduled post-IR
// render, burst + dry-derived bloom) and changes ONLY the generated event:
//   Sub   ≈ v1 topology (70->48 Hz burst, 105 Hz bloom), adaptive gain.
//           The intentional sub voice; still monitor-dependent by physics.
//   Thump shifts the burst to 140->85 Hz with mild saturation harmonics
//           and a 160 Hz bloom: palm-mute thump band.
//   Punch adds a short saturated 200->130 Hz mid burst over a smaller
//           90->55 Hz body burst: most audible / aggressive.
//
// Adaptive gain (replaces v1's fixed 0.75 burst gain, which the study showed
// mistracking quiet rigs by +4 dB): a peak-hold rig-level tracker on the
// post-IR dry's LOW BAND (per-voice LP: the burst competes with the guitar's
// lows, not its broadband peak — broadband normalization drifts with rig
// crest factor: the 6505's lows are nearly as hot as the 5150's while its
// peak is 4 dB lower) is sampled at each fire, so the burst stands a
// constant ratio above the low band it must punch through on ANY rig. The
// tracker release (70 ms) keeps every fire per-hit fresh: slower releases
// leak the previous section's lows into loud->soft transitions (measured:
// hot burst on a thin high-riff hit). NAM/IR smear plus the tracker's LP
// delay can push the low-band peak a few ms AFTER the fire sample (loose
// rigs peak later), so the gain GLIDES up toward the tracker over the first
// 6 ms post-fire (fast one-pole, up-only: smooth, never steps). The bloom
// term is dry-derived and already rig-relative, so it is NOT scaled (scaling
// it would make it rig-level-squared). Per-hit dynamics still come from the
// trigger velocity (burst env starts at s), so harder hits slam relatively
// harder; the gain fades to 0 below -40 dBFS rig level (never thumps on
// silence), hard-capped at 1.6. Clip safety comes from a dedicated branch
// limiter (not the gain law): the ADDED branch (burst + bloom, post-Amount)
// is limited against the headroom left by the current dry envelope
// (ceiling = 0.95 − |dry|env, instant attack, 30 ms release, 0.25 floor),
// so burst/dry phase coincidences on sustained material can never clip.
// The limiter only ever engages when headroom is genuinely scarce — normal
// palm mutes never touch it — and saturated bursts are normalized by the
// fire velocity before saturation (constant harmonics, preserved dynamics).
//
// Contract (mono float): reset(sr) off-RT; setVoice() at block boundaries
// (resets output state, keeps the rig-level tracker warm); render(dry,
// fireStrength) RT-safe, deterministic, finite-out; amount 0 = dry-exact.
// The caller (DevSlam) owns detection/pending-queue/dip-ramp and passes the
// due fire strength (or 0) per sample.
//
// Gain laws (DEV research): LegacyAdaptive (default, above) scales the burst
// WITH the rig's low-band level, so low-poor rigs get a quieter branch —
// arguably backwards (hardware: live teleG 0.85 vs 1.60 cap offline, branch
// faint). TargetRatio instead aims the burst peak at a fixed ratio below the
// recent BROADBAND post-IR level: gain = main*ratio/(shape*s), where shape
// is the per-voice calibrated raw-burst peak at s=1 (linear in s to
// 0.24 dB). Per-hit velocity then flows through the main level (not the
// trigger strength): every fire lands the target ratio for the BURST
// component; end-to-end (burst + dry-phase bloom interference) holds about
// +/-2.5 dB, voice-inherent — DEV target steps must be spaced accordingly.
// Same silence convention (-40 dBFS), same 6 ms up-only glide, same
// limiter. Telemetry records (fireRig_/fireMain_/reqGain_/reqLim_) are
// passive: they never feed back into the audio.

#include "dsp/lab/Slam/SlamDsp.h"

namespace tdm
{
namespace lab
{

class SlamImpactV2
{
public:
  enum class Voice
  {
    Sub = 0,
    Thump = 1,
    Punch = 2
  };
  enum class GainLaw
  {
    LegacyAdaptive = 0, // burst scales WITH rig low-band (default, unchanged)
    TargetRatio = 1 // burst peak aims at a ratio below recent broadband level
  };

  void reset(double sampleRate)
  {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    sr_ = sr;
    rigLp_.reset();
    rigEnv_.reset(sr, 0.0f, 70.0f); // instant attack, 70 ms release (per-hit fresh)
    mainEnv_.reset(sr, 0.0f, 250.0f); // recent broadband level (target law)
    dryEnv_.reset(sr, 0.0f, 5.0f); // headroom tracker: instant up, 5 ms hold
    glideCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.001 * sr))); // 1 ms
    limRel_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * sr))); // 30 ms
    limGain_ = 1.0f;
    limMin_ = 1.0f;
    reqLim_ = 1.0f;
    applyVoice(); // (re)derives coefficients + clears output state
  }

  void setVoice(Voice v)
  {
    voice_ = v;
    applyVoice(); // output state fresh; rig tracker stays warm
  }
  Voice voice() const { return voice_; }

  void setAmount(float a) { amount_ = a; } // 0..1 (caller may scale for tests)
  // Test/DEV-only 180 Hz reference burst (routing proof): replaces the
  // voice contribution with a fixed loud burst (strength 0.9, gain 0.5,
  // NO adaptive scaling, still through the limiter). If the ref is
  // audible where voices are not, routing is proven and the voice level
  // law is the suspect. Never on in normal behavior.
  void setTestRef(bool ref) { testRef_ = ref; }
  void setGainLaw(GainLaw l) { law_ = l; } // live-safe: future fires (+glide)
  void setTargetDb(float db) // target burst/main peak ratio (target law)
  {
    const float c = (db < -30.0f) ? -30.0f : ((db > 0.0f) ? 0.0f : db);
    targetDb_ = c;
    targetRatio_ = std::pow(10.0f, c / 20.0f);
  }

  // One post-IR dry sample; fireStrength in (0, 1] fires first (gain is
  // sampled from the rig tracker, which has already seen this sample).
  float render(float dry, float fireStrength)
  {
    const float rig = rigEnv_.process(rigLp_.process(dry));
    const float denv = dryEnv_.process(dry);
    const float main = mainEnv_.process(dry); // always warm: law switches safe
    if (fireStrength > 0.0f)
    {
      fireRig_ = rig;
      fireMain_ = main;
      fireGain_ = gainFrom(rig, main, fireStrength, true);
      lastS_ = fireStrength;
      glideLeft_ = glideN_;
      if (testRef_)
        refBurst_.trigger(0.9f); // fixed: routing proof independent of velocity
      else
      {
        burst_.trigger(fireStrength);
        if (hasMid_)
          burstMid_.trigger(fireStrength);
      }
      bloom_ = bloomAmt_ * fireStrength;
    }
    else if (glideLeft_ > 0)
    {
      --glideLeft_;
      const float g = gainFrom(rig, main, lastS_, false);
      if (g > fireGain_)
        fireGain_ += (g - fireGain_) * glideCoef_; // smooth up-only catch-up
    }
    float add;
    if (testRef_)
    {
      bloomLp_.process(dry); // keep the LP warm for the return to voice mode
      add = amount_ * refBurst_.process();
    }
    else
    {
      // Saturated bursts normalize by the fire velocity first: the saturated
      // shape (hence harmonics) is velocity-independent, then ×s restores
      // dynamics (raw saturation would squash soft hits upward).
      const float sn = (lastS_ > 1e-6f) ? lastS_ : 1e-6f;
      float b = burst_.process();
      if (satDrive_ > 0.0f)
        b = lastS_ * slamSoftSat(b / sn, satDrive_);
      float m = 0.0f;
      if (hasMid_)
      {
        m = burstMid_.process();
        if (satDriveMid_ > 0.0f)
          m = lastS_ * slamSoftSat(m / sn, satDriveMid_);
      }
      const float low = bloomLp_.process(dry);
      add = amount_ * (fireGain_ * (loW_ * b + midW_ * m) + bloom_ * low);
    }
    float y = dry + limitAdd(denv, add);
    if (!std::isfinite(y))
      y = dry;
    bloom_ *= bloomDecay_;
    if (bloom_ < 1e-5f)
      bloom_ = 0.0f;
    return y;
  }

  float rigLevel() const { return rigEnv_.value(); } // tests/telemetry
  float lastFireGain() const { return fireGain_; }
  float limiterMin() const { return limMin_; } // 1.0 = never engaged
  float limiterGain() const { return limGain_; } // current GR (telemetry)
  // Snapshot records (passive telemetry; set at each fire / engagement).
  float lastFireRig() const { return fireRig_; } // rig low-band env at fire
  float lastFireMain() const { return fireMain_; } // broadband env at fire
  float lastReqGain() const { return reqGain_; } // requested gain pre-cap
  float lastLimiterReq() const { return reqLim_; } // deepest GR request (pre-floor)
  float gainK() const { return kGain_; } // legacy law constant (this voice)
  float shapePeak() const { return shapePeak_; } // calibrated burst peak, s=1
  GainLaw gainLaw() const { return law_; }
  float targetDb() const { return targetDb_; }

private:
  float gainFrom(float rig, float main, float s, bool recordReq)
  {
    float req;
    float cap;
    if (law_ == GainLaw::TargetRatio)
    {
      const float sh = (shapePeak_ > 1e-6f) ? shapePeak_ : 1.0f;
      const float ss = (s > 1e-4f) ? s : 1e-4f;
      req = (main * targetRatio_) / (sh * ss); // every fire lands the ratio
      if (main < 0.01f)
        req *= main / 0.01f; // same -40 dBFS silence convention
      cap = 4.0f; // backstop: tiny-s fires stay graceful (limiter guards)
    }
    else
    {
      req = rig * kGain_;
      if (rig < 0.01f)
        req *= rig / 0.01f; // fade below -40 dBFS: never thump on silence
      cap = 1.6f; // backstop (clip safety itself is the branch limiter's job)
    }
    if (recordReq)
      reqGain_ = req;
    float g = (req > cap) ? cap : req;
    if (g < 0.0f)
      g = 0.0f;
    return g;
  }

  // Branch limiter: fill the headroom the dry leaves, never exceed it.
  float limitAdd(float denv, float add)
  {
    const float ceiling = 0.95f - denv;
    const float aa = std::fabs(add);
    if (aa > ceiling && aa > 1e-6f)
    {
      const float req = ceiling / aa; // pre-floor request (telemetry)
      if (req < reqLim_)
        reqLim_ = req; // deepest request since reset (pairs with limMin_)
      float target = (req < 0.25f) ? 0.25f : req; // floor: slammed stays slammed
      // Instant attack down: strict clip safety (no overshoot sample can
      // slip). The resulting step is bounded by the excess — small in
      // practice (study material: max 1 dB GR) and masked at burst peaks.
      if (target < limGain_)
        limGain_ = target;
    }
    else
    {
      limGain_ += (1.0f - limGain_) * limRel_; // 30 ms release toward unity
      if (limGain_ > 1.0f)
        limGain_ = 1.0f;
    }
    if (limGain_ < limMin_)
      limMin_ = limGain_;
    return add * limGain_;
  }

  void applyVoice()
  {
    // Per-voice event design (burst topology only; detector untouched).
    float f0 = 70.0f, f1 = 48.0f, dec = 130.0f, atk = 1.5f, sat = 0.0f;
    float mf0 = 200.0f, mf1 = 130.0f, mdec = 70.0f, matk = 0.7f, msat = 3.5f;
    float bloHz = 105.0f, bloDec = 70.0f, trkHz = 150.0f;
    hasMid_ = false;
    loW_ = 1.0f;
    midW_ = 0.0f;
    bloomAmt_ = 2.0f;
    kGain_ = 13.0f; // Sub: Legacy-parity burst (low-band-normalized; see §4)
    switch (voice_)
    {
    case Voice::Sub:
      break; // v1 topology, adaptive gain (defaults above)
    case Voice::Thump:
      f0 = 140.0f;
      f1 = 85.0f;
      dec = 110.0f;
      atk = 1.0f;
      sat = 1.5f;
      bloHz = 160.0f;
      bloDec = 60.0f;
      bloomAmt_ = 1.5f;
      trkHz = 250.0f;
      kGain_ = 7.0f;
      break;
    case Voice::Punch:
      f0 = 90.0f;
      f1 = 55.0f;
      dec = 100.0f;
      atk = 1.0f;
      hasMid_ = true;
      loW_ = 0.55f;
      midW_ = 0.8f;
      bloHz = 220.0f;
      bloDec = 50.0f;
      bloomAmt_ = 0.8f;
      trkHz = 350.0f;
      kGain_ = 5.6f;
      break;
    }
    rigLp_.setLowpass(sr_, trkHz, 0.7f); // tracker follows the burst's band
    satDrive_ = sat;
    satDriveMid_ = msat;
    burst_.reset(sr_, f0, f1, dec, atk, 1.0f); // unity: fireGain_ scales
    burstMid_.reset(sr_, mf0, mf1, mdec, matk, 1.0f);
    bloomLp_.reset();
    bloomLp_.setLowpass(sr_, bloHz, 0.7f);
    bloomDecay_ = static_cast<float>(std::exp(-6.907755278982137 / (bloDec * 0.001 * sr_)));
    refBurst_.reset(sr_, 180.0f, 180.0f, 150.0f, 1.0f, 0.5f); // routing proof
    // Calibrate the raw burst shape peak at s=1 (target-ratio law
    // normalization): local bursts, same topology, one decay scan. One
    // constant suffices: the burst peak is linear in s to 0.24 dB (the
    // f0->f1 sweep keys off absolute env but the peak is set by the attack
    // ramp + first cycles). Off-RT only (applyVoice runs at reset /
    // voice-switch / ref-toggle edges); the scan breaks when both bursts go
    // inactive (~130 ms, not the full second).
    {
      SlamBurst cb0, cb1;
      cb0.reset(sr_, f0, f1, dec, atk, 1.0f);
      cb1.reset(sr_, mf0, mf1, mdec, matk, 1.0f);
      cb0.trigger(1.0f);
      if (hasMid_)
        cb1.trigger(1.0f);
      float pk = 0.0f;
      const int nn = static_cast<int>(sr_);
      for (int i = 0; i < nn; ++i)
      {
        float x = cb0.process();
        if (sat > 0.0f)
          x = slamSoftSat(x, sat); // s=1: lastS_*sat(b/sn) == sat(b)
        float m = 0.0f;
        if (hasMid_)
        {
          m = cb1.process();
          if (msat > 0.0f)
            m = slamSoftSat(m, msat);
        }
        const float ax = std::fabs(loW_ * x + midW_ * m);
        pk = (ax > pk) ? ax : pk;
        if (!cb0.active() && !cb1.active())
          break;
      }
      shapePeak_ = (pk > 1e-6f) ? pk : 1.0f;
    }
    bloom_ = 0.0f;
    fireGain_ = 0.0f;
    fireRig_ = 0.0f;
    fireMain_ = 0.0f;
    reqGain_ = 0.0f;
    lastS_ = 0.0f;
    glideLeft_ = 0;
    glideN_ = static_cast<int>(0.006 * sr_ + 0.5);
    if (glideN_ < 1)
      glideN_ = 1;
  }

  double sr_ = 48000.0;
  Voice voice_ = Voice::Thump;
  float amount_ = 1.0f;
  SlamBiquad rigLp_; // tracker prefilter: burst's own low band
  SlamPeakEnv rigEnv_;
  SlamPeakEnv dryEnv_; // headroom tracker for the branch limiter
  SlamBurst burst_; // Sub/Thump main, Punch body
  SlamBurst burstMid_; // Punch mid knock only
  SlamBurst refBurst_; // test-only 180 Hz routing proof
  bool testRef_ = false;
  SlamBiquad bloomLp_;
  float bloom_ = 0.0f, bloomDecay_ = 1.0f, bloomAmt_ = 2.0f;
  float fireGain_ = 0.0f; // sampled per fire, glides up 6 ms, then held
  float lastS_ = 0.0f; // fire velocity (saturation normalization)
  float glideCoef_ = 0.02f; // 1 ms one-pole (derived per rate)
  int glideLeft_ = 0, glideN_ = 288;
  float limGain_ = 1.0f; // branch limiter GR (instant down, 30 ms up)
  float limRel_ = 0.001f; // 30 ms one-pole (derived per rate)
  float limMin_ = 1.0f; // deepest GR since reset (tests)
  float kGain_ = 7.0f; // always set by applyVoice(); init matches Thump
  float satDrive_ = 0.0f, satDriveMid_ = 0.0f;
  float loW_ = 1.0f, midW_ = 0.0f;
  bool hasMid_ = false;
  GainLaw law_ = GainLaw::LegacyAdaptive;
  float targetRatio_ = 0.7943282f; // -2 dB High (target law; UI picks the step)
  float targetDb_ = -2.0f;
  SlamPeakEnv mainEnv_; // recent broadband post-IR level (target law)
  float shapePeak_ = 1.0f; // calibrated raw-burst peak at s=1 (target law)
  float fireRig_ = 0.0f; // snapshot: rig low-band env at last fire
  float fireMain_ = 0.0f; // snapshot: broadband env at last fire
  float reqGain_ = 0.0f; // snapshot: requested gain pre-cap at last fire
  float reqLim_ = 1.0f; // snapshot: deepest limiter request (pre-floor)
};

} // namespace lab
} // namespace tdm

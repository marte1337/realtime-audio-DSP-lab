#include "tests/Assert.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "dsp/TechDeathRig.h"
#include "dsp/Tuner/Tuner.h"
#include "dsp/WavFile.h"

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979;

double midiToFreq(int midi, double cents = 0.0)
{
  return 440.0 * std::pow(2.0, (midi - 69 + cents / 100.0) / 12.0);
}

std::vector<float> sine(float peak, double freqHz, int n)
{
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * static_cast<float>(std::sin(kPi * 2.0 * freqHz * i / kSr));
  return out;
}

// Harmonic stack with 1/k rolloff (guitar-ish steady spectrum).
std::vector<float> harmonic(float peak, double freqHz, int n, int firstHarm = 1, int lastHarm = 6)
{
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (int h = firstHarm; h <= lastHarm; ++h)
  {
    const float a = peak / static_cast<float>(h);
    for (int i = 0; i < n; ++i)
      out[static_cast<size_t>(i)] += a * static_cast<float>(std::sin(kPi * 2.0 * freqHz * h * i / kSr));
  }
  return out;
}

uint32_t lcg(uint32_t& s)
{
  s = s * 1664525u + 1013904223u;
  return s;
}

std::vector<float> noise(float peak, int n, uint32_t seed = 0x12345678u)
{
  std::vector<float> out(static_cast<size_t>(n));
  uint32_t x = seed;
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] = peak * (2.0f * float((lcg(x) >> 8) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f);
  return out;
}

// Decaying Karplus-Strong pluck (pick transient + sustain + release).
std::vector<float> pluck(float peak, double freqHz, int n, uint32_t seed = 0x51ab3u)
{
  const int period = std::max(2, static_cast<int>(kSr / freqHz + 0.5));
  uint32_t s = seed;
  std::vector<float> line(static_cast<size_t>(period));
  for (int i = 0; i < period; ++i)
    line[static_cast<size_t>(i)] = 2.0f * (lcg(s) & 0xFFFFFF) / float(0xFFFFFF) - 1.0f;
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  int idx = 0;
  float prev = 0.0f;
  for (int i = 0; i < n; ++i)
  {
    const float cur = line[static_cast<size_t>(idx)];
    const float v = 0.9995f * 0.5f * (cur + prev);
    prev = cur;
    line[static_cast<size_t>(idx)] = v;
    idx = (idx + 1) % period;
    out[static_cast<size_t>(i)] = peak * cur;
  }
  return out;
}

// Feed a buffer through a fresh tuner in fixed blocks; collect one
// snapshot per block plus the final result.
struct TunerRun
{
  std::vector<tdm::TunerResult> perBlock;
  tdm::TunerResult last;
  int analyses = 0;
};

TunerRun runTuner(const std::vector<float>& in, int block = 256, double sr = kSr)
{
  TunerRun r;
  tdm::Tuner t;
  t.reset(sr);
  t.setEnabled(true);
  for (size_t off = 0; off < in.size(); off += static_cast<size_t>(block))
  {
    const int m = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - off));
    t.feedBlock(in.data() + off, m);
    tdm::TunerResult snap;
    t.result(snap);
    r.perBlock.push_back(snap);
  }
  t.result(r.last);
  r.analyses = t.analysesRun();
  return r;
}

bool snapshotsEqual(const tdm::TunerResult& a, const tdm::TunerResult& b)
{
  return a.valid == b.valid && a.midiNote == b.midiNote && a.frequencyHz == b.frequencyHz
      && a.cents == b.cents && a.confidence == b.confidence;
}
} // namespace

void runTunerTests()
{
  // ---- note math (pure, exact) ----
  {
    struct Expect
    {
      int midi;
      const char* name;
      int octave;
    };
    const Expect table[] = {{30, "F#", 1}, {35, "B", 1}, {40, "E", 2}, {45, "A", 2}, {50, "D", 3},
                            {55, "G", 3},  {59, "B", 3}, {64, "E", 4}, {69, "A", 4}};
    for (const auto& e : table)
    {
      const tdm::TunerNote n = tdm::Tuner::noteFor(static_cast<float>(midiToFreq(e.midi)));
      TDM_CHECK(n.midi == e.midi, "note midi exact");
      TDM_CHECK(std::string(n.name) == e.name, "note name exact");
      TDM_CHECK(n.octave == e.octave, "note octave exact");
      TDM_CHECK(std::fabs(n.cents) < 0.01f, "exact freq centers");
    }
    const tdm::TunerNote sharp = tdm::Tuner::noteFor(static_cast<float>(midiToFreq(69, 20.0)));
    TDM_CHECK(sharp.midi == 69 && std::fabs(sharp.cents - 20.0f) < 0.01f, "+20c maps");
    // Calibration path stays general (no UI yet).
    const tdm::TunerNote cal = tdm::Tuner::noteFor(442.0f, 442.0f);
    TDM_CHECK(cal.midi == 69 && std::fabs(cal.cents) < 0.01f, "refHz calibrates");
  }
  // ---- pure-sine corpus: exact note, sub-cent accuracy ----
  {
    const int midis[] = {30, 35, 40, 45, 50, 55, 59, 64, 69}; // F#1..A4
    for (int m : midis)
    {
      const TunerRun r = runTuner(sine(0.5f, midiToFreq(m), 96000));
      TDM_CHECK(r.last.valid, "sine valid");
      TDM_CHECK(r.last.midiNote == m, "sine exact midi (no octave error)");
      TDM_CHECK_CLOSE(r.last.cents, 0.0f, 1.0f, "sine sub-cent");
      TDM_CHECK(r.last.confidence >= 0.9f, "sine high confidence");
    }
  }
  // ---- detune offsets track ----
  for (double off : {5.0, -5.0, 20.0, -20.0})
  {
    for (int m : {40, 69}) // E2, A4
    {
      const TunerRun r = runTuner(sine(0.5f, midiToFreq(m, off), 96000));
      TDM_CHECK(r.last.valid && r.last.midiNote == m, "offset keeps nearest note");
      TDM_CHECK_CLOSE(r.last.cents, static_cast<float>(off), 1.5f, "offset tracks");
    }
  }
  // ---- semitone boundary behavior (raw detector, no display hysteresis) ----
  {
    const TunerRun below = runTuner(sine(0.5f, midiToFreq(40, 49.0), 96000));
    TDM_CHECK(below.last.valid && below.last.midiNote == 40, "+49c stays E2");
    TDM_CHECK_CLOSE(below.last.cents, 49.0f, 2.0f, "+49c measures");
    const TunerRun above = runTuner(sine(0.5f, midiToFreq(40, 51.0), 96000));
    TDM_CHECK(above.last.valid && above.last.midiNote == 41, "+51c flips to F2");
    TDM_CHECK_CLOSE(above.last.cents, -49.0f, 2.0f, "+51c measures from F2");
  }
  // ---- harmonic-rich tone + missing fundamental ----
  {
    const TunerRun rich = runTuner(harmonic(0.5f, midiToFreq(40), 96000));
    TDM_CHECK(rich.last.valid && rich.last.midiNote == 40, "harmonic stack exact");
    TDM_CHECK_CLOSE(rich.last.cents, 0.0f, 3.0f, "harmonic stack cents");
    const TunerRun noFund = runTuner(harmonic(0.5f, midiToFreq(40), 96000, 2, 6));
    TDM_CHECK(noFund.last.valid && noFund.last.midiNote == 40, "missing fundamental still E2");
    TDM_CHECK_CLOSE(noFund.last.cents, 0.0f, 5.0f, "missing fundamental cents");
  }
  // ---- level extremes ----
  {
    const TunerRun quiet = runTuner(sine(0.01f, 440.0, 96000)); // -40 dBFS peak
    TDM_CHECK(quiet.last.valid && quiet.last.midiNote == 69, "quiet tone valid");
    TDM_CHECK_CLOSE(quiet.last.cents, 0.0f, 1.5f, "quiet tone cents");
    const TunerRun silent = runTuner(std::vector<float>(96000, 0.0f));
    TDM_CHECK(!silent.last.valid, "silence invalid");
    TDM_CHECK(silent.analyses > 0, "silence still ticks analyses");
    const TunerRun hiss = runTuner(noise(0.5f, 96000));
    TDM_CHECK(!hiss.last.valid, "white noise invalid");
    const TunerRun tiny = runTuner(sine(1e-4f, 440.0, 96000)); // below the gate
    TDM_CHECK(!tiny.last.valid, "sub-gate tone invalid");
  }
  // ---- pick transient + sustain (KS pluck) ----
  {
    const TunerRun r = runTuner(pluck(0.8f, midiToFreq(40), 96000));
    // Settle past the attack: snapshots from ~300 ms on must be E2.
    bool settled = true;
    for (size_t b = 300 * 48 / 256; b < r.perBlock.size() && settled; ++b)
    {
      // Pluck decays below the gate near the tail; only require the body.
      if (b * 256 > 60000)
        break;
      settled = r.perBlock[b].valid && r.perBlock[b].midiNote == 40;
    }
    TDM_CHECK(settled, "pluck settles to E2");
    TDM_CHECK_CLOSE(r.perBlock[40].cents, 0.0f, 3.0f, "pluck sustain cents");
  }
  // ---- note change mid-stream ----
  {
    std::vector<float> in = sine(0.5f, midiToFreq(45), 48000); // A2 1 s
    const auto d3 = sine(0.5f, midiToFreq(50), 48000); // D3 1 s
    in.insert(in.end(), d3.begin(), d3.end());
    const TunerRun r = runTuner(in);
    const tdm::TunerResult late = r.perBlock[r.perBlock.size() - 4];
    TDM_CHECK(late.valid && late.midiNote == 50, "note change lands D3");
  }
  // ---- acquisition time from reset ----
  {
    auto acquire = [&](double freq, int midi) {
      tdm::Tuner t;
      t.reset(kSr);
      t.setEnabled(true);
      const auto in = sine(0.5f, freq, 48000);
      int at = -1;
      for (int off = 0; off < 48000; off += 256)
      {
        const int m = std::min(256, 48000 - off);
        t.feedBlock(in.data() + off, m);
        tdm::TunerResult s;
        t.result(s);
        if (at < 0 && s.valid && s.midiNote == midi && std::fabs(s.cents) < 3.0f)
          at = off + 256;
      }
      return at;
    };
    const int e2at = acquire(midiToFreq(40), 40);
    TDM_CHECK(e2at > 0 && e2at <= 9600, "E2 acquires within 200 ms");
    const int fsat = acquire(midiToFreq(30), 30);
    TDM_CHECK(fsat > 0 && fsat <= 14400, "F#1 acquires within 300 ms");
  }
  // ---- sustain stability (no flicker, no dropout, tight cents) ----
  {
    const TunerRun r = runTuner(harmonic(0.5f, midiToFreq(40), 96000));
    float lo = 1e9f, hi = -1e9f;
    bool allValid = true;
    for (size_t b = 40; b < r.perBlock.size(); ++b) // past acquisition
    {
      allValid = allValid && r.perBlock[b].valid && r.perBlock[b].midiNote == 40;
      lo = std::min(lo, r.perBlock[b].cents);
      hi = std::max(hi, r.perBlock[b].cents);
    }
    TDM_CHECK(allValid, "sustain never drops");
    TDM_CHECK(hi - lo <= 2.0f, "sustain cents spread tight");
  }
  // ---- other sample rates ----
  for (double sr : {44100.0, 96000.0})
  {
    for (int m : {40, 69})
    {
      const int n = static_cast<int>(sr * 2.0);
      std::vector<float> in(static_cast<size_t>(n));
      const double f = midiToFreq(m);
      for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(kPi * 2.0 * f * i / sr));
      const TunerRun r = runTuner(in, 256, sr);
      TDM_CHECK(r.last.valid && r.last.midiNote == m, "rate holds note");
      TDM_CHECK_CLOSE(r.last.cents, 0.0f, 2.0f, "rate holds cents");
    }
  }
  // ---- non-finite input: invalid, finite fields, recovers ----
  {
    tdm::Tuner t;
    t.reset(kSr);
    t.setEnabled(true);
    std::vector<float> bad(4096, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> inf(4096, std::numeric_limits<float>::infinity());
    t.feedBlock(bad.data(), 4096);
    t.feedBlock(inf.data(), 4096);
    tdm::TunerResult s;
    t.result(s);
    TDM_CHECK(!s.valid, "non-finite yields invalid");
    TDM_CHECK(std::isfinite(s.frequencyHz) && std::isfinite(s.cents) && std::isfinite(s.confidence),
              "published fields stay finite");
    const auto clean = sine(0.5f, 440.0, 48000);
    for (int off = 0; off < 48000; off += 256)
      t.feedBlock(clean.data() + off, std::min(256, 48000 - off));
    t.result(s);
    TDM_CHECK(s.valid && s.midiNote == 69, "recovers after garbage");
  }
  // ---- enable/disable: no work, no stale readings ----
  {
    tdm::Tuner t;
    t.reset(kSr);
    const auto in = sine(0.5f, 440.0, 48000);
    for (int off = 0; off < 48000; off += 256)
      t.feedBlock(in.data() + off, std::min(256, 48000 - off));
    TDM_CHECK(t.analysesRun() == 0, "disabled runs no analyses");
    tdm::TunerResult s;
    t.result(s);
    TDM_CHECK(!s.valid, "disabled publishes invalid");
    t.setEnabled(true);
    for (int off = 0; off < 48000; off += 256)
      t.feedBlock(in.data() + off, std::min(256, 48000 - off));
    TDM_CHECK(t.analysesRun() > 0, "enabled runs analyses");
    t.result(s);
    TDM_CHECK(s.valid && s.midiNote == 69, "enabled detects");
    t.setEnabled(false);
    t.result(s);
    TDM_CHECK(!s.valid, "disable blanks promptly");
  }
  // ---- seqlock under a concurrent reader (hammer, join) ----
  {
    tdm::Tuner t;
    t.reset(kSr);
    t.setEnabled(true);
    const auto in = sine(0.5f, 220.0f, 96000);
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::thread reader([&] {
      tdm::TunerResult s;
      while (!stop.load())
      {
        t.result(s);
        ++reads;
      }
    });
    for (int off = 0; off < 96000; off += 256)
      t.feedBlock(in.data() + off, std::min(256, 96000 - off));
    stop.store(true);
    reader.join();
    TDM_CHECK(reads.load() > 1000, "concurrent reads served");
    tdm::TunerResult s;
    t.result(s);
    TDM_CHECK(s.valid && s.midiNote == 57, "A3 under load");
  }
  // ---- display smoother: adopt / hysteresis / blank / smoothing ----
  {
    tdm::TunerDisplay d;
    auto snap = [](bool v, int m, float c) {
      tdm::TunerResult r;
      r.valid = v;
      r.midiNote = m;
      r.cents = c;
      r.frequencyHz = static_cast<float>(midiToFreq(m, c));
      r.confidence = 0.95f;
      return r;
    };
    d.update(snap(true, 40, 3.0f));
    TDM_CHECK(!d.state().valid, "single tick from blank holds");
    d.update(snap(true, 40, 3.0f));
    TDM_CHECK(d.state().valid && d.state().midiNote == 40, "display adopts on confirm");
    TDM_CHECK(std::string(d.state().name) == "E" && d.state().octave == 2, "display name/octave");
    // Semitone flicker around the boundary never reaches stock: holds E2.
    d.update(snap(true, 41, -48.0f));
    d.update(snap(true, 40, 3.0f));
    d.update(snap(true, 41, -48.0f));
    d.update(snap(true, 40, 3.0f));
    TDM_CHECK(d.state().midiNote == 40, "flicker holds note");
    // A real move confirms and adopts.
    d.update(snap(true, 41, -48.0f));
    d.update(snap(true, 41, -47.0f));
    TDM_CHECK(d.state().midiNote == 41, "confirmed move adopts");
    // Brief invalids hold; three in a row blanks.
    d.update(snap(false, 0, 0.0f));
    d.update(snap(false, 0, 0.0f));
    TDM_CHECK(d.state().valid, "brief dropout holds");
    d.update(snap(false, 0, 0.0f));
    TDM_CHECK(!d.state().valid, "sustained silence blanks");
    // Cents smoothing follows without jumping.
    tdm::TunerDisplay d2;
    d2.update(snap(true, 69, 0.0f));
    d2.update(snap(true, 69, 0.0f)); // confirm from blank
    d2.update(snap(true, 69, 10.0f));
    TDM_CHECK_CLOSE(d2.state().cents, 5.0f, 0.01f, "cents smooth halfway");
  }
  // ---- CPU budget: mean analysis cost ----
  {
    tdm::Tuner t;
    t.reset(kSr);
    t.setEnabled(true);
    const auto in = harmonic(0.5f, midiToFreq(40), 96000);
    // Warm up, then time a steady run.
    for (int off = 0; off < 48000; off += 512)
      t.feedBlock(in.data() + off, std::min(512, 48000 - off));
    const int a0 = t.analysesRun();
    const auto t0 = std::chrono::steady_clock::now();
    for (int off = 48000; off < 96000; off += 512)
      t.feedBlock(in.data() + off, std::min(512, 96000 - off));
    const auto t1 = std::chrono::steady_clock::now();
    const int runs = t.analysesRun() - a0;
    const double us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / std::max(1, runs);
    std::printf("[tuner-cpu] mean %.1f us/analysis (%d runs, E2 harmonic @48k)\n", us, runs);
    TDM_CHECK(us < 2000.0, "analysis under 2 ms mean");
  }
  // ---- rig transparency: tuner on/off bit-identical ----
  {
    const auto in = noise(0.6f, 96000);
    auto runRig = [&](bool tuner, bool fullChain) {
      tdm::TechDeathRig rig;
      rig.reset(kSr, 512);
      if (fullChain)
      {
        rig.setInputTrimDb(6.0f);
        rig.setGateEnabled(true);
        rig.setTransposeEnabled(true);
        rig.setTransposeSemitones(-2.0f);
        rig.setDriveEnabled(true);
        rig.setTight(0.8f);
        rig.setDrive(0.5f);
        rig.setBite(0.6f);
        rig.setShapeEnabled(true);
        rig.setDelayEnabled(true);
        rig.setReverbEnabled(true);
        rig.setOutputTrimDb(-3.0f);
      }
      rig.setTunerEnabled(tuner);
      std::vector<float> out(in.size(), 0.0f);
      for (size_t off = 0; off < in.size(); off += 512)
      {
        const int m = static_cast<int>(std::min<size_t>(512, in.size() - off));
        const float* bi[1] = {in.data() + off};
        float* bo[1] = {out.data() + off};
        rig.processBlock(bi, 1, bo, 1, m);
      }
      return out;
    };
    TDM_CHECK(runRig(false, false) == runRig(true, false), "transparency, dry chain");
    TDM_CHECK(runRig(false, true) == runRig(true, true), "transparency, full chain");
  }
  // ---- tap location: downstream cannot move the tuner ----
  {
    const auto in = pluck(0.7f, midiToFreq(40), 96000);
    auto snapshots = [&](bool hot, bool assets) {
      tdm::TechDeathRig rig;
      rig.reset(kSr, 512);
      if (assets)
      {
        std::vector<float> taps(1024, 0.0f);
        for (int k = 0; k < 1024; ++k)
          taps[static_cast<size_t>(k)] = 0.9f * std::exp(-3.0f * k / 1024.0f);
        const float* ch[1] = {taps.data()};
        tdm::writeWavFloat32("/tmp/tdm_test_tuner_ir.wav", ch, 1, 1024, kSr);
        rig.loadIr("/tmp/tdm_test_tuner_ir.wav");
        rig.loadNam("assets/nam/test_amp.nam");
        std::remove("/tmp/tdm_test_tuner_ir.wav");
      }
      if (hot)
      {
        rig.setInputTrimDb(12.0f);
        rig.setGateEnabled(true);
        rig.setGateThresholdDb(-40.0f);
        rig.setTransposeEnabled(true);
        rig.setTransposeSemitones(-7.0f);
        rig.setDriveEnabled(true);
        rig.setTight(1.0f);
        rig.setDrive(1.0f);
        rig.setShapeEnabled(true);
        rig.setWeight(0.9f);
        rig.setDelayEnabled(true);
        rig.setReverbEnabled(true);
        rig.setOutputTrimDb(6.0f);
      }
      rig.setTunerEnabled(true);
      std::vector<tdm::TunerResult> snaps;
      std::vector<float> out(512, 0.0f);
      for (size_t off = 0; off < in.size(); off += 512)
      {
        const int m = static_cast<int>(std::min<size_t>(512, in.size() - off));
        const float* bi[1] = {in.data() + off};
        float* bo[1] = {out.data()};
        rig.processBlock(bi, 1, bo, 1, m);
        tdm::TunerResult s;
        rig.tunerResult(s);
        snaps.push_back(s);
      }
      return snaps;
    };
    const auto base = snapshots(false, false);
    const auto hot = snapshots(true, false);
    const auto loaded = snapshots(true, true);
    bool sameHot = base.size() == hot.size();
    for (size_t i = 0; sameHot && i < base.size(); ++i)
      sameHot = snapshotsEqual(base[i], hot[i]);
    TDM_CHECK(sameHot, "downstream params cannot move tuner");
    bool sameLoaded = base.size() == loaded.size();
    for (size_t i = 0; sameLoaded && i < base.size(); ++i)
      sameLoaded = snapshotsEqual(base[i], loaded[i]);
    TDM_CHECK(sameLoaded, "NAM/IR cannot move tuner");
    TDM_CHECK(base.back().valid && base.back().midiNote == 40, "rig tap detects E2");
  }
}

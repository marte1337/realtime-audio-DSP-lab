#pragma once

// TdmEngine: GUI-independent live-audio owner for TechDeathMachine.
//
// Owns one TechDeathRig plus CoreAudio duplex IO (default input -> lock-free
// ring -> rig -> default output). Extracted verbatim in behavior from the
// original tdm_live runner: same mono-averaging input, same interface clamp
// on output, same "devices must share one rate" rule, same chunking.
//
// Threading / lifecycle contract (v0):
// - rig() exposes the realtime-safe TechDeathRig facade: parameter setters
//   may be called from any thread, including while audio runs. They apply at
//   audio block boundaries (see dsp/TechDeathRig.h).
// - loadNam()/loadIr() are NOT realtime-safe (allocation + file IO) and
//   require stopped audio: they refuse with an error string while running.
//   v0 policy is Stop -> load -> Start; there is no async model swapping.
// - start() resets DSP state at the device rate and re-validates loaded
//   assets against that rate, so a sample-rate change in Audio MIDI Setup
//   between load and start fails loudly instead of playing wrong.
// - No GUI/host toolkit types here: this header is plain C++ and is shared
//   by tdm_live, the developer app, and any future Standalone/VST3/AU shell.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/TechDeathRig.h"

class TdmEngine
{
public:
  TdmEngine();
  ~TdmEngine();
  TdmEngine(const TdmEngine&) = delete;
  TdmEngine& operator=(const TdmEngine&) = delete;

  // Direct rig access for parameters (any-thread setters, see contract).
  tdm::TechDeathRig& rig() { return rig_; }
  const tdm::TechDeathRig& rig() const { return rig_; }

  // Currently loaded asset paths (empty = none). Set on successful load.
  const std::string& namPath() const { return namPath_; }
  const std::string& irPath() const { return irPath_; }

  bool isRunning() const { return running_.load(std::memory_order_acquire); }
  // Device rate of the last successful start, or 0 when never started.
  double sampleRate() const { return sampleRate_; }

  uint64_t blocks() const { return blocks_.load(std::memory_order_relaxed); }
  uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
  uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

  // Start duplex audio. Resets DSP state at the device rate. Returns false
  // with a human-readable error on any failure (no devices, rate mismatch
  // between input/output, asset rate mismatch, HAL errors).
  bool start(std::string& error);
  // Stop audio. Idempotent; safe to call when stopped.
  void stop();

  // Load assets. Require stopped audio (v0 policy); return false + error
  // while running or when the loader rejects the file (missing file,
  // corrupt asset, sample-rate mismatch vs the current device rate).
  bool loadNam(const std::string& path, std::string& error);
  bool loadIr(const std::string& path, std::string& error);

  // One-line-per-device inventory for CLIs ("id=.. name=.. in=..Hz out=..Hz").
  static bool listDevices(std::string& out, std::string& error);

private:
  friend struct TdmEngineAudio; // IO procs (defined in TdmEngine.cpp)

  // Reset the rig at the default-output rate so loaders have a known rate
  // to validate against. Preserves already-loaded assets (Rig::reset keeps
  // models/weights); start() re-resets at the true rate and re-validates.
  void ensureResetForLoad();

  tdm::TechDeathRig rig_;
  std::string namPath_;
  std::string irPath_;
  double sampleRate_ = 0.0;

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> overruns_{0};

  // Live HAL state. Touched only on the owning (main/control) thread during
  // start()/stop(), except the scratch/ring/rig which the IO threads use.
  struct Hal;
  std::unique_ptr<Hal> hal_;
};

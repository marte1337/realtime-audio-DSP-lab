# TechDeathMachine roadmap

## Current / core

- NAM A2 amp modeling stage
- Cabinet IR stage
- Input Trim
- Output Trim
- TechDeathGate v1.2 (CLOSED → OPEN → CLOSING state machine, ~52 ms feel)
- TightDrive v1 (pre-NAM conditioning: Tight / Drive / Bite)
- ToneShape v1 (post-cab Weight / Contour / Presence, `dsp/ToneShape/`)
- Space v1 (post-ToneShape Delay + Reverb, `dsp/Space/`; first stereo
  stage — everything through ToneShape stays mono, Space emits L/R;
  series routing with additive mixes so dry is never scaled)
- Reference NAM auditioning in progress (6505 unboosted + Mesa V30 lead)
- Developer Control App v0 (`build/tdm_dev`, see `docs/dev-control-app.md`):
  audition NAMs/IRs and all current DSP params from a small native UI
- Realtime-safe parameter handoff: GUI/control thread stores lock-free
  atomics on `TechDeathRig`; the audio thread adopts them at block
  boundaries (`dsp/RigParams.h`, TSan-verified, no DSP changes)
- Shared live-audio host layer (`host/TdmEngine`): CoreAudio duplex engine
  used by both `tdm_live` and the developer app; v0 policy is
  Stop → load NAM/IR → Start (no async model swapping yet)

## Host / UI direction (decided 2026-09-16)

Target product shapes remain macOS Standalone, VST3, likely AU later.

- Chosen for v0: a small native AppKit developer app (`host/dev/`,
  Objective-C++ with ARC) on top of the shared `host/TdmEngine`. Zero new
  dependencies, same plain Makefile, builds in seconds. This is not a
  throwaway: a macOS Standalone product is literally this shell plus polish
  and packaging, and the DSP-side contract it establishes (`TechDeathRig` +
  `RigParams` + engine start/stop/load lifecycle) ports 1:1 into any plugin
  framework later.
- JUCE (the default Standalone+VST3+AU-from-one-codebase route) was
  evaluated and deferred: adopting it now means a large third-party
  dependency, a Makefile→CMake migration, and license implications — a
  repo-scale migration disproportionate to "stop typing CLI flags". No
  JUCE groundwork was laid; nothing in the v0 design blocks it later.
- iPlug2 is the other serious candidate: it is the exact framework the
  upstream NAM plugin ships Standalone/VST3/AU with, it is already on disk
  as a sibling checkout, and it is lighter than JUCE. Same verdict as JUCE:
  evaluate at product-layer time, not during developer tooling.
- Rejected for v0: Dear ImGui/SDL (new dependency, contributes nothing to
  Standalone/VST3/AU) and a web/localhost UI (realtime param path becomes
  HTTP, unusable in a plugin).
- Explicit non-goal, unchanged: do not reuse or fork the upstream
  NeuralAmpModelerPlugin application architecture. TechDeathMachine owns
  its host/UI layer; only NeuralAmpModelerCore DSP is shared.

## Next

- Establish a small set of reference NAMs (3 curated amp sounds)
- Audition ToneShape v1 starting points per reference amp (see experiments log)
- Audition Space v1 starting takes (subtle rhythm / technical lead /
  atmospheric lead settings in experiments log; listening validation pending)
- Product layer: harden `host/` toward Standalone packaging, then pick
  JUCE vs iPlug2 for VST3 (+AU) and wrap the unchanged `TechDeathRig`

## Experimental backlog

These must not delay the core playable product. They belong under
`dsp/lab/` and stay out of the core signal path until proven.

- SLAM: performance button/macro, likely parallel low-end/distortion
  branch, possibly sub/octave reinforcement
- Stutter
- Pitch Dive
- Fracture

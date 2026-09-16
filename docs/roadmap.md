# TechDeathMachine roadmap

## Current / core

- NAM A2 amp modeling stage
- Cabinet IR stage
- Input Trim
- Output Trim
- TechDeathGate v1.2 (CLOSED → OPEN → CLOSING state machine, ~52 ms feel)
- TightDrive v1 (pre-NAM conditioning: Tight / Drive / Bite)
- Reference NAM auditioning in progress (6505 unboosted + Mesa V30 lead)

## Next

- Establish a small set of reference NAMs (3 curated amp sounds)
- Design ToneShape after multi-amp auditioning (`dsp/ToneShape/`)
- Space: delay / reverb (`dsp/Space/`)
- Standalone + VST3 product layer

## Experimental backlog

These must not delay the core playable product. They belong under
`dsp/lab/` and stay out of the core signal path until proven.

- SLAM: performance button/macro, likely parallel low-end/distortion
  branch, possibly sub/octave reinforcement
- Stutter
- Pitch Dive
- Fracture

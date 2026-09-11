# TechDeathMachine

TechDeathMachine is a lightweight guitar DSP/NAM suite built specifically for tight, articulate technical-death-metal guitar tones.

Its tonal direction takes inspiration from bands such as Necrophagist, Beneath The Massacre, early The Faceless, and The Zenith Passage while developing its own sound and processing approach.

## Project goals

TechDeathMachine should be:

- fast
- tight
- articulate
- aggressive
- easy to dial in
- intentionally small
- experimental where useful

The project also serves as a compact DSP laboratory for ideas that may later inform larger guitar-processing projects.

## Planned signal chain

```text
Input
  ↓
Gate
  ↓
TightDrive
  ↓
NAM A2
  ↓
Cabinet IR
  ↓
ToneShape
  ↓
Experimental FX
  ↓
Delay / Reverb
  ↓
Output
```

## v0.1 target

The initial suite is intentionally limited to:

- 3 curated NAM amp sounds
- 4 curated cabinet IRs
- noise gate
- tight overdrive
- post-cab tone shaping
- delay
- reverb

Experimental processors will be developed only after the core guitar path is working well.

## Experimental Lab

Future ideas include:

- Stutter
- Pitch Dive
- Fracture
- micro-buffer manipulation
- transient-triggered glitches
- reverse fragments
- unusual pitch effects
- experimental delays

These processors live conceptually under `dsp/lab`.

## Development philosophy

Prefer small DSP modules, focused tests, realtime safety, and frequent listening validation.

Do not prematurely build a shared framework with other projects.

Reusable technology can be extracted later after it has proven useful.

## Current milestone

### Milestone 0 — Skeleton

Initial target:

```text
Input
→ NAM A2
→ Cabinet IR
→ Output
```

The goal is simply to establish a clean, playable, tested project foundation before implementing original DSP.

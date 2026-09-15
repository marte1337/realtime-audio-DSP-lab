# Milestone 0 — manual guitar smoke test

Goal: prove `Input → NAM A2 → Cabinet IR → Output` feels alive with a real
guitar before any Gate/TightDrive/ToneShape work starts.

## What the processors do (physically)

- **NAM A2**: a 23-layer dilated WaveNet emulation of a full amp (preamp +
  power amp + sag). Nonlinear, level-dependent: picking harder must distort
  more, like the real amp. Our `NamStage` runs the proven
  NeuralAmpModelerCore inference with the A2 fast path.
- **Cab IR**: linear FIR convolution with a cabinet impulse response. Adds
  the speaker's resonances and rolls off harsh highs. Our `CabIrStage` uses
  direct-form convolution with the same -18 dB staging gain as the upstream
  NAM plugin, so shared IRs read at the same loudness.

## Setup (constant for every audition)

1. `make` then `./build/tdm_live --list`; note your interface.
2. Set input + output device to the **same sample rate** in Audio MIDI
   Setup (48 kHz preferred: most `.nam` files are 48 kHz and M0 has no
   resampler — mismatches fail loudly at load time by design).
3. Copy one `.nam` to `assets/nam/` and one cab WAV to `assets/ir/`
   (both git-ignored by design).
4. `./build/tdm_live --nam assets/nam/<amp>.nam --ir assets/ir/<cab>.wav`
5. Play with the guitar volume at 10, interface gain so hard picking peaks
   just below clipping on the dry input.

## Varying one thing at a time

- **Bypass reference**: run once with no `--nam`/`--ir` flags. Dry DI
  should sound thin and clean; note the noise floor.
- **NAM only** (`--nam`, no `--ir`): expect aggressive amp distortion with
  fizzy, harsh top end (no speaker filtering yet). That fizz is correct —
  it is what the IR removes.
- **NAM + IR**: expect tight low end, present mids, no fizz. Palm mutes
  should "thunk" and stop, not boom.

## Bug / design smells

- Silence with assets loaded: wrong rate, wrong path, or crashed loader —
  the runner prints the reason; do not turn the guitar up to compensate.
- Machine-gun stutter / dropouts with `underrunFrames` climbing: block
  size or device mismatch, not the amp model. Try 256-sample buffers.
- Boom that grows note after note: IR history bug (report it).
- Harshness that EQ can't fix or weak pick attack: note the exact
  `.nam` + IR + rate; that combination may just be wrong for tech-death.

## Risks / known M0 limits (not bugs)

No sample-rate conversion, no live preset swapping, no DC blocker, no gate.
See the final M0 report for the senior-review list.

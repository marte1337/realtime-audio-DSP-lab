# Developer Control App v0 (`tdm_dev`)

Small graphical engineering interface for the current playable chain:

```text
Input Trim → Gate → Transpose → TightDrive → NAM → IR → ToneShape → Output Trim
```

"Transpose" here is the DEV A/B stage: OUR production `GuitarTranspose`
engine vs the frozen TONE3000 reference, substituted at the rig's
transpose position. (`tdm_live` / `tdm_render` run the same production
engine directly, without the A/B wrapper or the reference.)

It replaces long `tdm_live` command lines while auditioning amps. It is
infrastructure, not a product UI: no presets, no ToneShape, no meters,
no experimental FX.

## Build / run

```sh
make            # builds tdm_dev alongside tdm_live / tdm_render / tests
./build/tdm_dev # run from the repo root (reference NAM preload is relative)
```

`tdm_dev` hosts the DEV transpose A/B section, so `make` now needs the
TONE3000/JUCE research checkouts for this target only
(`TDM_T3K_DIR`/`TDM_JUCE_DIR`, same override style as `NAM_CORE_DIR`).
`tdm_live` and `tdm_render` never link the reference.

Headless checks (no GUI, no audio hardware):

```sh
./build/tdm_dev --smoke-test  # offline rig/param/NAM checks, exit code
./build/tdm_dev --smoke-ui    # build the full UI and auto-quit after 1 s
```

`tdm_live` and `tdm_render` are unchanged tools and still build.

## What the UI exposes

- NAM file selection (any `.nam`; reference `assets/nam/6505_unboost.nam`
  preloads best-effort at launch)
- IR file selection (`.wav` / `.aif` / `.aiff`)
- Audio Start / Stop (+ running rate, block count, under/overrun counters)
- Input Trim (-12..+18 dB, starts 0)
- Gate enable + Threshold (-80..-35 dB, starts -55) + Release (10..500 ms,
  starts 52)
- TightDrive enable + Tight / Drive / Bite (0..1, start 0.85 / 0.50 / 0.70)
- ToneShape enable + Weight / Contour / Presence (0..1, start neutral 0.50)
- Output Trim (-24..+24 dB, starts 0)
- Transpose section (Gate → Transpose → Drive position): enable checkbox,
  engine segmented control (Our GT2 / T3K Ref) + one-click A/B button,
  shift slider (−12…+12 st, float) + integer stepper, live status line
  (engine / shift / latency / rate / buffer)
- GT2 Advanced disclosure (17 config rows + re-sync checkbox, from the
  shared adapter table; edits validate eagerly and apply on next Start),
  baseline/CUSTOM badge, restart badge, Restore GT2 Baseline
- TONE3000 Reference disclosure (window 20/30/40/60, tonality Off/Hz —
  all live), Restore T3K Reference (30 ms / Off)
- Loaded NAM/IR basenames and live parameter values (10 Hz refresh)
- Double-click any slider to reset it to its canonical DSP default: Input
  0 dB, Gate −55 dB / 50 ms, Tight 0.50, Drive 0.30, Bite 0.50, Output
  0 dB. The audition starting points (e.g. TightDrive 0.85 / 0.50 / 0.70)
  are intentionally NOT the reset values. ToneShape resets to neutral
  (Weight 0.50, Contour 0.50, Presence 0.50).

- Transpose audition default: enabled, Our GT2, −2.0 st, T3K at 30 ms /
  Tonality Off, GT2 at the known-good baseline. Double-click shift resets
  to 0 st (live-safe unity, not bypass — use the enable checkbox for dry).
- No setting persistence (same as every other section): audition state is
  hardcoded, restores are always one click away.
- No live GT2 telemetry (splice/NCC counters are audio-thread plain data;
  reading them from the UI would race). The offline `tdm_gt2_study`
  harness remains the metrics source.

## Threading rules (v0)

- Parameter sliders/checkboxes are safe while audio runs: the control
  thread does a clamped lock-free store and the audio thread adopts it at
  the next block boundary. No mutexes, no allocation in the callback.
- NAM/IR loading is NOT realtime-safe: choosing a file while running shows
  "Stop audio first". Policy: Stop → load → Start.
- Transpose engine/shift/enable/T3K-window/tonality are live (lock-free
  atomics, block-boundary adoption, same as rig sliders). GT2 advanced
  config follows the NAM/IR policy: edits validate on the main thread and
  the running engine adopts them on the next Start (restart badge shows
  when stored ≠ running).
- `Start` re-validates loaded assets against the device rate and fails
  loudly on mismatch (no resampler). Changing the rate in Audio MIDI Setup
  between load and start therefore errors at Start, by design.

## Known v0 limitations

- Fixed-size window, no keyboard control, no presets, no metering.
- No NAM/IR clearing (pick a different file), no sample-rate conversion,
  no realtime model swapping/crossfading.
- Loading needs a CoreAudio device present (the rate validates the asset);
  headless machines can still run `--smoke-test`.
- `TechDeathRig` knows nothing about GUI code; all AppKit lives in
  `host/dev/`. The reusable product surface is `TechDeathRig` +
  `dsp/RigParams.h` + `host/TdmEngine` (see roadmap "Host / UI direction").

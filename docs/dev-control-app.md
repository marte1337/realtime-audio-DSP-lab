# Developer Control App v0 (`tdm_dev`)

Small graphical engineering interface for the current playable chain:

```text
Input Trim → Gate → TightDrive → NAM → IR → Output Trim
```

It replaces long `tdm_live` command lines while auditioning amps. It is
infrastructure, not a product UI: no presets, no ToneShape, no meters,
no experimental FX.

## Build / run

```sh
make            # builds tdm_dev alongside tdm_live / tdm_render / tests
./build/tdm_dev # run from the repo root (reference NAM preload is relative)
```

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
- Output Trim (-24..+24 dB, starts 0)
- Loaded NAM/IR basenames and live parameter values (10 Hz refresh)

## Threading rules (v0)

- Parameter sliders/checkboxes are safe while audio runs: the control
  thread does a clamped lock-free store and the audio thread adopts it at
  the next block boundary. No mutexes, no allocation in the callback.
- NAM/IR loading is NOT realtime-safe: choosing a file while running shows
  "Stop audio first". Policy: Stop → load → Start.
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

# TDM MIDI Control — Architecture & Hardware Guide

GENERIC middleware, not FCB1010-specific. Lives in `host/midi/` and is linked
only into DEV/test targets (`tdm_dev`, `tdm_tests`). Production targets
(`tdm_live`, `tdm_render`) contain no MIDI code (link-verified).

Chain:

	CoreMIDI → MidiInput → parser → MappingTable → Dispatcher → ISink
	                                                        → gesture API

## Files

- `host/midi/MidiTypes.h` — `Message`, `StreamParser` (running status,
  NoteOn-vel0→NoteOff, sysex skip/abort).
- `host/midi/MidiMapping.h` — `MappingTable` (trigger→target, OMNI/channel/
  source filter, continuous range+invert, action mode) + `LearnManager`
  (armed capture, conflict replace, 2.5 s calibration window for continuous).
- `host/midi/MidiDispatch.h` — `Dispatcher` (momentary/one-shot/toggle,
  presses map NoteOff→NoteOn by key so one mapping gives press+release;
  PC has no fabricated release), `ISink` interface (DSP-agnostic).
- `host/midi/MidiPersist.h/.cpp` — hand-rolled versioned JSON DOM
  (mappings + slot configs + source UIDs): `saveJson/loadJson`,
  `saveFile/loadFile`, `defaultPath`. Missing → defaults+OK,
  corrupt → defaults+error.
- `host/midi/MidiInput.h/.cpp` — CoreMIDI client/port/source discovery by UID
  (never index), multi-source connect, hot-plug notify; `ingest` parses
  each packet and calls the app handler synchronously on the CoreMIDI
  input thread (no queue, no deferral); callback-safe (no alloc/lock/IO).
- `tests/TestMidi.cpp` — 17 groups: parsing, running status, mapping,
  learn, dispatch modes, gestures by ID 0–7, JSON round-trip, CoreMIDI
  graceful no-server path. Invoked from `tests/TestMain.cpp`.

## Targets

- `Target::Expression`, `Target::Mix` — continuous 0–127 → normalized range.
- `Target::ExpressionEnable`, `Target::GesturesEnable` — toggle.
- `Target::GestureSlot0..7` — stable IDs 0–7 → `pressSlot/releaseSlot/
  oneShotSlot`; work with Expression OFF; no fabricated releases.
- Registry is a plain enum + sink adapter: adding Transpose/SLAM/Drive
  targets later is one enum entry + one sink case. No DSP change needed.

## Threading

CoreMIDI readProc → parse → Learn offer → Dispatcher → sink adapter
(atomics / `pressSlot` etc.), ALL synchronous on the CoreMIDI input
thread — measured <0.1 us typical per message, so MIDI-to-atomic
latency is negligible and DSP adopts at the next block boundary.
The 10 Hz UI tick does presentation ONLY (monitor label, Learn commit,
hot-plug refresh, slider reflection); it is not in the control path,
and expression sweeps cannot step at 10 Hz. No audio-thread MIDI work;
no UI calls from MIDI/audio threads.

## DEV UI

`MIDI CONTROL` section (last in the scroll doc): source popup, OMNI toggle,
recent-input monitor, one Learn row per target, Clear, Save.
`./build/tdm_dev --list-midi` prints CoreMIDI sources (exit 1 when no server).
Monitor + Learn are mouse-editable with no device attached.

## FCB1010 via Scarlett 6i6 (first hardware)

1. FCB1010 MIDI OUT → Scarlett 6i6 MIDI IN (DIN) → Mac USB.
2. The FCB1010 will NOT appear as its own CoreMIDI device; select the
   Scarlett MIDI source in the popup (or `--list-midi` to confirm).
3. No channel/CC/PC is assumed — trust the monitor, not factory defaults.
4. Move Expression A/B, watch the monitor for actual CC numbers,
   Learn → Expression / Mix.
5. Press footswitches, watch types: with press+release (CC/Note) use
   Momentary; single-fire PC → One-Shot; enables → Toggle.
6. Save writes JSON to the app-support path (`defaultPath`); restart and
   confirm mappings + slots persist; disconnect/reconnect is safe (UID keyed).

## VST3/AU note

`ISink` is the automation seam: a future host adapter posts the same
commands (expression value, slot press/release) from plugin parameters.
CoreMIDI code never touches DSP; DSP never touches MIDI.

## Status

Automated: 2971 checks green incl. 18 MIDI groups (burst/ordering);
smoke-test, smoke-ui green; production quarantine verified
(0 MIDI/CoreMIDI, 0 TONE3000/JUCE symbols in tdm_live/tdm_render).

Hardware (FCB1010 → Scarlett 6i6 DIN → Mac, verified on real rig):
- Scarlett 6i6 MIDI input works; source selectable, no FCB1010 ID needed.
- MIDI Learn works; both expression pedals map and respond.
- Whammy response is smooth and playable (no 10 Hz stepping, as designed).
- All eight gesture footswitches trigger their slots correctly.
- Mappings + gesture configurations persist across restarts.
- A little latency is perceptible but acceptable; pitch DSP and MIDI
  response deliberately left unchanged — revisit only if it becomes
  a musical problem.

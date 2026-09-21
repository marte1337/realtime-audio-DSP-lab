# Realtime Guitar DSP Engine

An experimental C++ audio/DSP platform combining neural amplifier modelling,
custom real-time signal processing, and DSP algorithm research for electric guitar.

Originally started as a guitar-processing suite for highly articulate modern
metal tones, the project has evolved into a broader real-time DSP research
platform covering dynamics, nonlinear processing, tone shaping, stereo effects,
neural amp modelling, and low-latency polyphonic pitch shifting.

## Overview

The current processing concept is built around a hybrid approach:

- neural amplifier processing via NAM / NeuralAmpModelerCore
- custom real-time C++ DSP before and after the amp model
- cabinet impulse-response processing
- native low-latency macOS audio for development and auditioning
- a lightweight developer UI for live parameter testing
- offline render and test tools for controlled DSP evaluation

A typical signal path currently looks approximately like:

`Input → Gate → Preamp Processing → NAM → Cabinet IR → Tone Shaping → Stereo Space → Output`

Individual stages are intentionally kept modular so algorithms can be tested independently before becoming part of the main signal chain.

## DSP implemented so far

### Gate

A guitar-oriented noise gate designed around fast technical playing.

The implementation evolved through several iterations after real playing tests exposed problems that were not obvious from unit tests alone, including sustained-note re-triggering and overly abrupt closing behaviour.

The current design uses an explicit state machine and separate behaviour for opening, closing and fully closed states.

### TightDrive

A custom pre-amplifier drive stage intended to tighten low frequencies and increase articulation before the neural amp model.

It combines:

- pre-drive high-pass filtering
- controllable nonlinear saturation
- presence shaping
- high-frequency control

Unlike a generic distortion effect, it is designed specifically as a front-end for high-gain amplifier models.

### ToneShape

A post-cabinet tone-shaping stage providing broad musical control over:

- low-frequency weight
- midrange contour
- presence

The controls are designed to remain simple while still allowing significantly different voicings from the same amplifier/cabinet combination.

### Space

The first stereo stage in the signal chain.

It currently contains:

- true stereo ping-pong delay
- custom algorithmic plate-style reverb

The reverb went through several topology changes during development, including Schroeder/Moorer and FDN-based prototypes, before moving toward a dual-tank, modulated plate architecture.

This was driven primarily by listening tests rather than numerical metrics alone: earlier versions were technically stable and passed objective tests, but still sounded metallic, narrow or overly center-focused.

### Pitch-shifting research

Pitch shifting has become a larger research topic within the project.

Several approaches have been prototyped and compared:

- variable-delay / Doppler-style shifting
- STFT phase-vocoder shifting
- multi-resolution phase-vocoder processing
- WSOLA-based time-domain shifting

The first implementation worked mathematically but produced obvious fragmentation on real guitar input and was rejected.

The phase-vocoder version significantly improved stability and polyphonic behaviour, but exposed the classic time/frequency-resolution trade-off: long FFT windows preserve low-frequency structure but introduce large latency and transient smearing, while shorter windows improve timing but weaken low fundamentals and chord partials.

A multi-resolution prototype successfully combined the tonal strengths of long windows with the transient behaviour of shorter ones, but inherited the latency of the slowest path.

The current most promising direction is a WSOLA-based time-domain approach, which has so far preserved low-frequency content, chord structure and pitch accuracy while reducing algorithmic latency to roughly the 30 ms range.

This work is still experimental and is deliberately kept separate from the production signal path until real-time playing tests justify integration.

## Engineering approach

A major goal of the project is to treat DSP development as an empirical engineering process rather than just implementing textbook algorithms.

Features typically go through the following cycle:

1. implement the smallest viable DSP topology
2. build deterministic tests and diagnostic renders
3. audition with real guitar input
4. identify audible failure modes
5. use measurements to explain those failure modes
6. refine or reject the design

Several algorithms in this project have intentionally been discarded after real-world testing, even when their automated tests were green.

That distinction between **correct signal processing** and **musically useful signal processing** is one of the main things I am exploring with the project.

## Testing and validation

The project includes a growing automated DSP test suite covering areas such as:

- signal-path bypass correctness
- parameter boundaries
- sample-rate behaviour
- deterministic reset
- stability and finite output
- stereo balance and decorrelation
- pitch accuracy
- transient continuity
- feedback stability
- latency
- block-boundary behaviour
- real-time safety assumptions

Offline diagnostic renders are also used to compare algorithms against the same source material.

For pitch-shifting research in particular, I have been measuring:

- fundamental-frequency accuracy
- harmonic retention
- chord root/fifth balance
- transient-envelope correlation
- spectral deviation
- dropout detection
- latency
- CPU cost

These measurements are used as diagnostic tools, not as substitutes for listening tests.

## Real-time considerations

The DSP code is designed with real-time constraints in mind.

Audio processing paths avoid:

- dynamic allocation
- mutexes
- filesystem access
- blocking operations

Parameter changes are transferred safely into the audio thread and smoothed where necessary.

Sample-rate-independent behaviour is tested at multiple common rates, including 44.1, 48 and 96 kHz.

## Neural amp modelling

Amplifier tones are provided by NAM / NeuralAmpModelerCore.

The longer-term design is not to treat a raw `.nam` capture as the complete user-facing amplifier.

Instead, a curated amp can combine:

- a selected neural amp capture
- input calibration
- optional pre-drive processing
- tone-shaping defaults
- output-level compensation

This allows some amplifier models to behave like pedal-platform captures while others can use already-saturated captures with the drive stage disabled.

I am also experimenting with the idea of multi-capture amplifier models where several captures of the same amplifier at different gain settings could provide a more continuous amplifier-gain experience.

## Current status

TechDeathMachine is an active research and development project.

Currently working:

- NAM amplifier processing
- cabinet IR processing
- input/output gain stages
- custom gate
- TightDrive
- ToneShape
- stereo delay
- algorithmic reverb
- native development UI
- offline rendering and DSP test infrastructure

Currently under research:

- low-latency polyphonic pitch shifting
- curated amplifier/capture system
- additional stereo ambience algorithms
- experimental performance effects

Planned later:

- plugin/standalone product layer
- preset/state system
- MIDI/expression control
- additional delay and reverb types
- experimental effects such as pitch dives, stutter and more aggressive performance processing

## Development environment

The project is currently developed primarily on macOS using:

- C++17/modern C++
- CoreAudio
- Make
- NeuralAmpModelerCore
- custom DSP implementations

The current native developer application is intentionally lightweight and exists primarily for DSP auditioning rather than as the final product UI.

## Why I built this

My professional background is primarily in frontend/software development, and this project is partly an exploration outside my usual stack.

It has given me a practical way to work with:

- modern C++
- real-time programming constraints
- audio signal processing
- numerical algorithms
- multithreaded application boundaries
- performance measurement
- test-driven DSP development
- iterative technical research

It is also a good example of how I like to approach unfamiliar technical domains: build small, measure behaviour, test assumptions, reject weak approaches early, and gradually turn experimental work into maintainable software.

---

**Status:** Active development / research project

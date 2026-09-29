# TechDeathMachine M0 build (plain make; no cmake required).
#
# Third-party DSP code (NeuralAmpModelerCore + Eigen + nlohmann/json) is NOT
# vendored. The Makefile uses it read-only from NAM_CORE_DIR, which defaults
# to the local reference checkout. Override on the command line:
#
#   make NAM_CORE_DIR=/path/to/NeuralAmpModelerCore
#
# Follow-up: replace these overrides with a pinned submodule + cmake once
# network access is available (see docs/milestone0-smoke.md risks).

CXX ?= c++
NAM_CORE_DIR ?= ../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore
EIGEN_DIR ?= $(NAM_CORE_DIR)/Dependencies/eigen
JSON_DIR ?= $(NAM_CORE_DIR)/Dependencies/nlohmann

BUILD := build
NAMOBJ := $(BUILD)/namcore

# CommandLineTools-only Macs keep libc++ headers inside the SDK, which bare
# c++ does not search: add them explicitly when present.
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
SYSINCLUDES := $(shell test -d "$(SDKROOT)/usr/include/c++/v1" && echo "-isystem $(SDKROOT)/usr/include/c++/v1")

STD := -std=c++20
OPT := -O2
DEFS := -DNAM_ENABLE_A2_FAST
INCLUDES := -I$(NAM_CORE_DIR) -I$(EIGEN_DIR) -I$(JSON_DIR) -I.
TDM_FLAGS := $(STD) $(OPT) -Wall -Wextra $(DEFS) $(INCLUDES) $(SYSINCLUDES) -MMD -MP
NAM_FLAGS := $(STD) $(OPT) -w $(DEFS) -I$(NAM_CORE_DIR) -I$(EIGEN_DIR) -I$(JSON_DIR) $(SYSINCLUDES)

NAM_SRCS := $(wildcard $(NAM_CORE_DIR)/NAM/*.cpp) $(wildcard $(NAM_CORE_DIR)/NAM/wavenet/*.cpp)
NAM_OBJS := $(patsubst $(NAM_CORE_DIR)/%.cpp,$(NAMOBJ)/%.o,$(NAM_SRCS))

TDM_SRCS := dsp/NamStage.cpp dsp/CabIrStage.cpp dsp/WavFile.cpp dsp/TechDeathRig.cpp dsp/Gate/TechDeathGate.cpp dsp/InputTrim.cpp dsp/TightDrive/TightDrive.cpp dsp/ToneShape/ToneShape.cpp dsp/Space/Delay.cpp dsp/Space/Reverb.cpp dsp/Space/SpaceProcessor.cpp dsp/OutputTrim.cpp
TDM_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TDM_SRCS))

# Live-audio host layer (CoreAudio duplex). Kept OUT of TDM_OBJS so the
# offline tests and tdm_render stay portable and framework-free.
ENGINE_SRCS := host/TdmEngine.cpp
ENGINE_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(ENGINE_SRCS))

# Developer Control App (native AppKit, Objective-C++ with ARC).
DEV_SRCS := host/dev/TdmDevApp.mm
DEV_OBJS := $(patsubst %.mm,$(BUILD)/%.o,$(DEV_SRCS))

TEST_SRCS := tests/TestMain.cpp tests/TestWav.cpp tests/TestCabIr.cpp tests/TestNam.cpp tests/TestRig.cpp tests/TestRigParams.cpp tests/TestGate.cpp tests/TestTrim.cpp tests/TestTightDrive.cpp tests/TestToneShape.cpp tests/TestSpace.cpp tests/TestOutputTrim.cpp tests/TestLabPitch.cpp tests/TestLabMulti.cpp tests/TestLabWsola.cpp tests/TestLabWsolaLatency.cpp tests/TestLabWsolaLive.cpp tests/TestLabPitchV2.cpp tests/TestLabWsolaV2.cpp tests/TestHostBuffer.cpp
TEST_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SRCS))

# Lab pitch prototype: standalone offline tool, deliberately NOT linked into
# the rig, tests of the rig, or the dev app. Shares only WavFile + LabFft.
LABPITCH_SRCS := dsp/lab/Pitch/LabFft.cpp dsp/lab/Pitch/LabPitchShift.cpp dsp/lab/Pitch/LabCrossover.cpp dsp/lab/Pitch/LabMultiPitch.cpp dsp/lab/Pitch/LabWsolaShift.cpp dsp/lab/Pitch/LabPitchV2.cpp dsp/lab/Pitch/LabWsolaV2.cpp dsp/lab/Pitch/LabPitchRender.cpp
LABPITCH_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(LABPITCH_SRCS))
LABPITCH_LIB := $(BUILD)/dsp/lab/Pitch/LabFft.o $(BUILD)/dsp/lab/Pitch/LabPitchShift.o $(BUILD)/dsp/lab/Pitch/LabCrossover.o $(BUILD)/dsp/lab/Pitch/LabMultiPitch.o $(BUILD)/dsp/lab/Pitch/LabWsolaShift.o $(BUILD)/dsp/lab/Pitch/LabPitchV2.o $(BUILD)/dsp/lab/Pitch/LabWsolaV2.o

# LAB AUDITION wrapper (temporary): linked into the live/dev hosts (which own
# the TdmEngine insert) and the tests. NOT linked into the rig or tdm_render.
# The hosts also link the W20 shifter object itself (tests already get it
# via LABPITCH_LIB, so it stays out of LABLIVE_OBJS to avoid duplicates).
LABLIVE_SRCS := dsp/lab/Pitch/LabWsolaLive.cpp
LABLIVE_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(LABLIVE_SRCS))
LABWSOLA_OBJ := $(BUILD)/dsp/lab/Pitch/LabWsolaShift.o

FRAMEWORKS := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
DEV_FRAMEWORKS := $(FRAMEWORKS) -framework Cocoa -framework UniformTypeIdentifiers
BENCH_FRAMEWORKS := -framework Accelerate

# External benchmark dependencies (RESEARCH/BENCHMARK ONLY - never product).
# Rubber Band 4.0.0 (GPL-2+), SoundTouch 2.4.1 (LGPL-2.1), Signalsmith
# Stretch 1.4.0 + linear 0.6.4 (MIT). Used read-only from OUTSIDE this repo
# (TDM_BENCH_DEPS, override on the command line). Nothing under
# TDM_BENCH_DEPS is copied into TechDeathMachine source; only our own
# shims in dsp/lab/bench/ live here. Bench binaries are lab-only and NOT
# part of `all` (linking GPL code stays an explicit local act).
TDM_BENCH_DEPS ?= /tmp/tdm-pitch-study
BENCH_INCS := -I$(TDM_BENCH_DEPS)/rubberband -I$(TDM_BENCH_DEPS)/soundtouch/include -I$(TDM_BENCH_DEPS)/signalsmith-stretch -I$(TDM_BENCH_DEPS)/linear/include
BENCH_SRCS := dsp/lab/bench/BenchRubberBand.cpp dsp/lab/bench/BenchSoundTouch.cpp dsp/lab/bench/BenchSignalsmith.cpp
BENCH_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(BENCH_SRCS))
# SoundTouch arm64-safe subset (x86-only mmx/sse + unused BPMDetect left out;
# cpu_detect is preprocessor-guarded to near-empty off x86).
ST_SRCS := SoundTouch.cpp TDStretch.cpp RateTransposer.cpp FIFOSampleBuffer.cpp AAFilter.cpp FIRFilter.cpp InterpolateCubic.cpp InterpolateLinear.cpp InterpolateShannon.cpp PeakFinder.cpp cpu_detect_x86.cpp
ST_OBJS := $(addprefix $(BUILD)/bench/st/,$(ST_SRCS:.cpp=.o))
RB_SINGLE_OBJ := $(BUILD)/bench/RubberBandSingle.o
BENCH_EXT_OBJS := $(RB_SINGLE_OBJ) $(ST_OBJS)

.PHONY: all test smoke clean check-deps check-bench-deps
all: check-deps $(BUILD)/tdm_tests $(BUILD)/tdm_render $(BUILD)/tdm_live $(BUILD)/tdm_dev

check-deps:
	@test -d "$(NAM_CORE_DIR)/NAM" || (echo "error: NAM_CORE_DIR not found: $(NAM_CORE_DIR)"; exit 1)
	@test -f "$(EIGEN_DIR)/Eigen/Dense" || (echo "error: Eigen/Dense not found under $(EIGEN_DIR)"; exit 1)
	@test -f "$(JSON_DIR)/json.hpp" || (echo "error: json.hpp not found under $(JSON_DIR)"; exit 1)

$(NAMOBJ)/%.o: $(NAM_CORE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(NAM_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TDM_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(TDM_FLAGS) -fobjc-arc -c $< -o $@

# Bench shims (our code, strict warnings) + external objects (silent -w).
$(BENCH_OBJS): $(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TDM_FLAGS) $(BENCH_INCS) -c $< -o $@

$(RB_SINGLE_OBJ): $(TDM_BENCH_DEPS)/rubberband/single/RubberBandSingle.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(OPT) -w -I$(TDM_BENCH_DEPS)/rubberband $(SYSINCLUDES) -c $< -o $@

$(ST_OBJS): $(BUILD)/bench/st/%.o: $(TDM_BENCH_DEPS)/soundtouch/source/SoundTouch/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(OPT) -w -I$(TDM_BENCH_DEPS)/soundtouch/include $(SYSINCLUDES) -c $< -o $@

check-bench-deps:
	@test -f "$(TDM_BENCH_DEPS)/rubberband/single/RubberBandSingle.cpp" || (echo "error: RubberBandSingle.cpp not found under $(TDM_BENCH_DEPS)" ; exit 1)
	@test -f "$(TDM_BENCH_DEPS)/soundtouch/include/SoundTouch.h" || (echo "error: SoundTouch.h not found under $(TDM_BENCH_DEPS)" ; exit 1)
	@test -f "$(TDM_BENCH_DEPS)/signalsmith-stretch/signalsmith-stretch.h" || (echo "error: signalsmith-stretch.h not found under $(TDM_BENCH_DEPS)" ; exit 1)
	@test -f "$(TDM_BENCH_DEPS)/linear/include/signalsmith-linear/stft.h" || (echo "error: linear stft.h not found under $(TDM_BENCH_DEPS)" ; exit 1)

-include $(TDM_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(ENGINE_OBJS:.o=.d) $(DEV_OBJS:.o=.d) $(LABPITCH_OBJS:.o=.d) $(LABLIVE_OBJS:.o=.d) $(BUILD)/app/TdmLive.d $(BUILD)/app/TdmRender.d

$(BUILD)/tdm_tests: $(TDM_OBJS) $(TEST_OBJS) $(NAM_OBJS) $(LABPITCH_LIB) $(LABLIVE_OBJS)
	$(CXX) $(STD) $^ -o $@

$(BUILD)/tdm_render: $(TDM_OBJS) $(NAM_OBJS) $(BUILD)/app/TdmRender.o
	$(CXX) $(STD) $^ -o $@

$(BUILD)/tdm_live: $(TDM_OBJS) $(ENGINE_OBJS) $(NAM_OBJS) $(LABLIVE_OBJS) $(LABWSOLA_OBJ) $(BUILD)/app/TdmLive.o
	$(CXX) $(STD) $^ $(FRAMEWORKS) -o $@

$(BUILD)/tdm_dev: $(TDM_OBJS) $(ENGINE_OBJS) $(NAM_OBJS) $(LABLIVE_OBJS) $(LABWSOLA_OBJ) $(DEV_OBJS)
	$(CXX) $(STD) $^ $(DEV_FRAMEWORKS) -o $@

$(BUILD)/tdm_labpitch: $(LABPITCH_OBJS) $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@

labpitch: $(BUILD)/tdm_labpitch

# WSOLA latency study: measurement harness (lab only, not in `all`).
$(BUILD)/tdm_wsola_study: $(BUILD)/dsp/lab/Pitch/LabWsolaStudy.o $(LABWSOLA_OBJ) $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@

wsola-study: $(BUILD)/tdm_wsola_study

# WSOLA wobble investigation: trajectory instrument (lab only, not in `all`).
$(BUILD)/tdm_wsola_traj: $(BUILD)/dsp/lab/Pitch/LabWsolaTraj.o $(LABWSOLA_OBJ) $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@

wsola-traj: $(BUILD)/tdm_wsola_traj

# PV-D v2 A/B/C/D comparison harness (lab only, not in `all`).
$(BUILD)/tdm_pv2_study: $(BUILD)/dsp/lab/Pitch/LabPV2Study.o $(BUILD)/dsp/lab/Pitch/LabPitchV2.o $(BUILD)/dsp/lab/Pitch/LabFft.o $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@

pv2-study: $(BUILD)/tdm_pv2_study

# E2 causal / small-skip WSOLA comparison harness (lab only, not in `all`).
$(BUILD)/tdm_e2_study: $(BUILD)/dsp/lab/Pitch/LabWsolaE2Study.o $(BUILD)/dsp/lab/Pitch/LabWsolaV2.o $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@

e2-study: $(BUILD)/tdm_e2_study

# External-vs-internal pitch benchmark (lab only, not in `all`).
# Links GPL/LGPL research deps: local benchmarking only, never product.
$(BUILD)/tdm_bench_study: $(BUILD)/dsp/lab/bench/BenchStudy.o $(BENCH_OBJS) $(BENCH_EXT_OBJS) $(BUILD)/dsp/lab/Pitch/LabWsolaShift.o $(BUILD)/dsp/lab/Pitch/LabPitchShift.o $(BUILD)/dsp/lab/Pitch/LabFft.o $(BUILD)/dsp/WavFile.o
	$(CXX) $(STD) $^ -o $@ $(BENCH_FRAMEWORKS)

bench-study: check-bench-deps $(BUILD)/tdm_bench_study

# Bench audition live binary (lab only, not in `all`): TdmEngine + TdmLive
# recompiled with -DTDM_BENCH_LIVE into build/bench-live/ (the tdm_live and
# tdm_dev objects/binaries are untouched), exposing ONLY the nominated
# audition config (--bench rb2:SHIFT). Links GPL/LGPL research deps: local
# audition only, never product.
BENCHLIVE_OBJS := $(BUILD)/bench-live/host/TdmEngine.o $(BUILD)/bench-live/app/TdmLive.o
$(BUILD)/bench-live/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TDM_FLAGS) $(BENCH_INCS) -DTDM_BENCH_LIVE -c $< -o $@

$(BUILD)/tdm_bench_live: $(TDM_OBJS) $(NAM_OBJS) $(LABLIVE_OBJS) $(LABWSOLA_OBJ) $(BENCHLIVE_OBJS) $(BENCH_OBJS) $(BENCH_EXT_OBJS)
	$(CXX) $(STD) $^ $(FRAMEWORKS) $(BENCH_FRAMEWORKS) -o $@

bench-live: check-bench-deps $(BUILD)/tdm_bench_live

test: $(BUILD)/tdm_tests
	./$(BUILD)/tdm_tests

# Offline smoke: synth DI + IR with stdlib python3, render through the real
# A2 example model, verify finite bounded output.
smoke: $(BUILD)/tdm_render
	python3 scripts/smoke_synth.py
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --nam "$(EXAMPLE_NAM)" --ir $(BUILD)/smoke_ir.wav --out $(BUILD)/smoke_out.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_out.wav
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --nam "$(EXAMPLE_NAM)" --ir $(BUILD)/smoke_ir.wav --gate-thresh -40 --gate-rel 50 --out $(BUILD)/smoke_gated.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_gated.wav
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --nam "$(EXAMPLE_NAM)" --ir $(BUILD)/smoke_ir.wav --gate-thresh -40 --gate-rel 50 --tight-drive --tight 0.55 --drive 0.35 --bite 0.6 --out $(BUILD)/smoke_drive.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_drive.wav
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --nam "$(EXAMPLE_NAM)" --ir $(BUILD)/smoke_ir.wav --tone-shape --weight 0.7 --contour 0.4 --presence 0.6 --out $(BUILD)/smoke_shape.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_shape.wav
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --nam "$(EXAMPLE_NAM)" --ir $(BUILD)/smoke_ir.wav --delay --delay-time 220 --delay-fb 0.4 --delay-mix 0.3 --reverb --reverb-decay 0.5 --reverb-mix 0.25 --out $(BUILD)/smoke_space.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_space.wav
	./$(BUILD)/tdm_render --in $(BUILD)/smoke_di.wav --output-trim 14.45 --out $(BUILD)/smoke_outtrim.wav
	python3 scripts/smoke_check.py $(BUILD)/smoke_outtrim.wav

EXAMPLE_NAM ?= ../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore/example_models/wavenet_a2_max.nam

clean:
	rm -rf $(BUILD)

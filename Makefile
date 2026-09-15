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
TDM_FLAGS := $(STD) $(OPT) -Wall -Wextra $(DEFS) $(INCLUDES) $(SYSINCLUDES)
NAM_FLAGS := $(STD) $(OPT) -w $(DEFS) -I$(NAM_CORE_DIR) -I$(EIGEN_DIR) -I$(JSON_DIR) $(SYSINCLUDES)

NAM_SRCS := $(wildcard $(NAM_CORE_DIR)/NAM/*.cpp) $(wildcard $(NAM_CORE_DIR)/NAM/wavenet/*.cpp)
NAM_OBJS := $(patsubst $(NAM_CORE_DIR)/%.cpp,$(NAMOBJ)/%.o,$(NAM_SRCS))

TDM_SRCS := dsp/NamStage.cpp dsp/CabIrStage.cpp dsp/WavFile.cpp dsp/TechDeathRig.cpp dsp/TechDeathGate.cpp dsp/InputTrim.cpp
TDM_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TDM_SRCS))

TEST_SRCS := tests/TestMain.cpp tests/TestWav.cpp tests/TestCabIr.cpp tests/TestNam.cpp tests/TestRig.cpp tests/TestGate.cpp tests/TestTrim.cpp
TEST_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SRCS))

FRAMEWORKS := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

.PHONY: all test smoke clean check-deps
all: check-deps $(BUILD)/tdm_tests $(BUILD)/tdm_render $(BUILD)/tdm_live

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

$(BUILD)/tdm_tests: $(TDM_OBJS) $(TEST_OBJS) $(NAM_OBJS)
	$(CXX) $(STD) $^ -o $@

$(BUILD)/tdm_render: $(TDM_OBJS) $(NAM_OBJS) $(BUILD)/app/TdmRender.o
	$(CXX) $(STD) $^ -o $@

$(BUILD)/tdm_live: $(TDM_OBJS) $(NAM_OBJS) $(BUILD)/app/TdmLive.o
	$(CXX) $(STD) $^ $(FRAMEWORKS) -o $@

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

EXAMPLE_NAM ?= ../nam_holdsworth/NeuralAmpModelerPlugin/NeuralAmpModelerCore/example_models/wavenet_a2_max.nam

clean:
	rm -rf $(BUILD)

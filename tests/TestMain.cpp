// M0 test runner: every suite reports failures into a shared list so a
// single crash-prone area cannot hide the rest. Nonzero exit on failure.

#include <cstdio>

#include "tests/Assert.h"

void runWavTests();
void runCabIrTests();
void runNamTests();
void runRigTests();
void runRigParamsTests();
void runGateTests();
void runTrimTests();
void runTightDriveTests();
void runToneShapeTests();
void runSpaceTests();
void runOutputTrimTests();
void runLabPitchTests();
void runLabMultiTests();
void runLabWsolaTests();

namespace
{
void runSuite(const char* name, void (*fn)())
{
  const size_t before = tdm_test::failures().size();
  try
  {
    fn();
  }
  catch (const std::exception& e)
  {
    tdm_test::failures().push_back(std::string(name) + " threw: " + e.what());
  }
  catch (...)
  {
    tdm_test::failures().push_back(std::string(name) + " threw unknown exception");
  }
  const size_t after = tdm_test::failures().size();
  std::printf("[%s] %s (%zu new failures)\n", after == before ? "PASS" : "FAIL", name, after - before);
}
} // namespace

int main()
{
  runSuite("wav", runWavTests);
  runSuite("cabir", runCabIrTests);
  runSuite("nam", runNamTests);
  runSuite("rig", runRigTests);
  runSuite("rigparams", runRigParamsTests);
  runSuite("gate", runGateTests);
  runSuite("trim", runTrimTests);
  runSuite("drive", runTightDriveTests);
  runSuite("toneshape", runToneShapeTests);
  runSuite("space", runSpaceTests);
  runSuite("outtrim", runOutputTrimTests);
  runSuite("labpitch", runLabPitchTests);
  runSuite("labmulti", runLabMultiTests);
  runSuite("labwsola", runLabWsolaTests);
  std::printf("checks=%d failures=%zu\n", tdm_test::checkCount(), tdm_test::failures().size());
  for (const auto& f : tdm_test::failures())
    std::printf("  FAIL %s\n", f.c_str());
  return tdm_test::failures().empty() ? 0 : 1;
}

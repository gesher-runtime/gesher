// Gesher: the main of ART's gtests (cmake/aosp/art_gtests.cmake), in place of
// art/test/common/gtest_main.cc. That one initializes ART's locks, logging and
// MemMap, then hands over to gtest_extras' IsolateMain, which forks a child per
// test. Windows cannot fork, so the tests run in this process after the same
// initialization.

#include "base/logging.h"
#include "base/mem_map.h"
#include "base/mutex.h"
#include "gtest/gtest.h"
#include "runtime.h"

// Allow other test code to run global initialization/configuration before gtest infra takes over.
extern "C" __attribute__((weak)) void ArtTestGlobalInit();

int main(int argc, char** argv) {
  art::Locks::Init();
  art::InitLogging(argv, art::Runtime::Abort);
  art::MemMap::Init();
  LOG(INFO) << "Running main() from art_gtest_main.cc...";
  if (ArtTestGlobalInit != nullptr) {
    ArtTestGlobalInit();
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

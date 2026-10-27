#include "arm64_varargs.h"

int main(int argc, char** argv) {
  auto result = arm64_varargs::Run(argc > 1 ? argv[1] : ".");
  for (const auto& failure : result.failures) std::printf("FAIL arm64 varargs: %s\n", failure.c_str());
  std::printf("arm64 varargs: %s; %s\n", result.failures.empty() ? "all passed" : "FAILED", result.Summary());
  return result.failures.empty() ? 0 : 1;
}

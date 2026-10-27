#include "net4_ifaddrs.h"

int main(int argc, char** argv) {
  net4::Result result = net4::RunIfaddrs(argc > 1 ? argv[1] : nullptr);
  for (const std::string& failure : result.failures) std::printf("FAIL net4 ifaddrs: %s\n", failure.c_str());
  std::printf("net4 ifaddrs: %s; %s\n", result.failures.empty() ? "all passed" : "FAILED", result.Summary().c_str());
  return result.failures.empty() ? 0 : 1;
}

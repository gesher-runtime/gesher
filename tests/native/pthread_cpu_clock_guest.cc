// Both real NDK ABIs execute the same contracts as the native2 ART/APK part.
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
struct Part {
  std::string name;
  std::vector<std::string> failures;
  std::vector<std::string> notes;
  explicit Part(std::string label) : name(std::move(label)) {}
  void Check(bool ok, const std::string& message) { if (!ok) failures.push_back(message); }
};
#include "../native2/pthread_cpu_clock.inc"
int main() {
  Part part = PthreadCpuClocks();
  for (const auto& failure : part.failures) std::printf("FAIL guest CPU clock: %s\n", failure.c_str());
  for (const auto& note : part.notes) std::printf("INFO guest CPU clock: %s\n", note.c_str());
  std::printf("%s guest CPU clock: retained identity, busy target, errors and replacement lifecycle\n",
              part.failures.empty() ? "PASS" : "FAIL");
  return part.failures.empty() ? 0 : 1;
}

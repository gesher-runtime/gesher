// Real Win32 clock brackets. Optional --after-suspend records a physical
// sleep/hibernate run; the default CI mode never suspends the machine.
#include <gesher/clocks.h>

#include <cinttypes>
#include <initializer_list>
#include <cstdio>
#include <cstring>

namespace {
int checks, failures;
void Check(const char* name, bool ok) {
  ++checks;
  if (!ok) ++failures;
  std::printf("clock-domains-windows: %s %s\n", ok ? "PASS" : "FAIL", name);
}
uint64_t NativeTicks(bool awake) {
  ULONGLONG value;
  if (awake) QueryUnbiasedInterruptTimePrecise(&value);
  else QueryInterruptTimePrecise(&value);
  return value;
}
int64_t QpcNanos() {
  LARGE_INTEGER frequency, value;
  QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&value);
  return static_cast<int64_t>(static_cast<__int128>(value.QuadPart) * 1000000000 / frequency.QuadPart);
}
}

int main(int argc, char** argv) {
  for (bool awake : {true, false}) {
    const uint64_t before = NativeTicks(awake);
    const int64_t actual = awake ? gesher::MonotonicNanos() : gesher::BoottimeNanos();
    const uint64_t after = NativeTicks(awake);
    Check(awake ? "MONOTONIC matches independent unbiased bracket" :
                  "BOOTTIME matches independent biased bracket",
          actual >= 0 && static_cast<uint64_t>(actual) >= before * 100 &&
          static_cast<uint64_t>(actual) <= after * 100);
  }
  timespec host{}, android{};
  const int64_t before = QpcNanos();
  const int status = clock_gettime(CLOCK_MONOTONIC, &host);
  const int64_t after = QpcNanos();
  const int64_t value = static_cast<int64_t>(host.tv_sec) * 1000000000 + host.tv_nsec;
  Check("host winpthreads keeps its QPC epoch", status == 0 && value >= before && value <= after);
  const int64_t awake_before = gesher::MonotonicNanos();
  const int android_status = gesher::AndroidClockGettime(CLOCK_MONOTONIC, &android);
  const int64_t awake_after = gesher::MonotonicNanos();
  const int64_t android_value = static_cast<int64_t>(android.tv_sec) * 1000000000 + android.tv_nsec;
  Check("explicit Android adapter keeps the awake epoch", android_status == 0 &&
        android_value >= awake_before && android_value <= awake_after);
  Check("BOOTTIME is not behind awake time", gesher::BoottimeNanos() >= awake_after);
  if (argc == 2 && std::strcmp(argv[1], "--after-suspend") == 0) {
    const int64_t start_awake = gesher::MonotonicNanos();
    const int64_t start_boot = gesher::BoottimeNanos();
    std::puts("Suspend or hibernate Windows for at least 5 seconds, resume, then press Enter.");
    std::fflush(stdout);
    if (std::getchar() == EOF) return 2;
    const int64_t end_boot = gesher::BoottimeNanos();
    const int64_t end_awake = gesher::MonotonicNanos();
    const int64_t excluded = (end_boot - start_boot) - (end_awake - start_awake);
    std::printf("clock-domains-windows: excluded suspend nanoseconds=%" PRId64 "\n", excluded);
    Check("physical suspend appears only in BOOTTIME", excluded > 1000000000);
  } else if (argc != 1) {
    return 2;
  }
  std::printf("clock-domains-windows: %d assertions, %d failures\n", checks, failures);
  return failures != 0;
}

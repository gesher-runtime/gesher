// The window-thread watch (compat/input/thread_watch.cc) measures waits in
// awake time, Android's uptimeMillis clock, so that a sleep or hibernate is
// never reported as an unanswered window. The default mode never suspends the
// machine; --after-suspend records a physical sleep/hibernate run.
#include <windows.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "thread_watch.h"

namespace {
int checks, failures;
void Check(const char* name, bool ok) {
  ++checks;
  if (!ok) ++failures;
  std::printf("thread-watch-clock: %s %s\n", ok ? "PASS" : "FAIL", name);
}
uint64_t UnbiasedMs() {
  ULONGLONG ticks;
  QueryUnbiasedInterruptTimePrecise(&ticks);
  return ticks / 10000;
}
uint64_t BiasedMs() {
  ULONGLONG ticks;
  QueryInterruptTimePrecise(&ticks);
  return ticks / 10000;
}
}  // namespace

int main(int argc, char** argv) {
  using gesher::input::ThreadWatchClockMs;
  const uint64_t before = UnbiasedMs();
  const uint64_t value = ThreadWatchClockMs();
  const uint64_t after = UnbiasedMs();
  Check("the watch clock matches an independent unbiased interrupt-time bracket",
        value >= before && value <= after);
  // When this boot has slept, the biased clocks (GetTickCount64, interrupt
  // time) are ahead of awake time by the time asleep; the watch must not
  // follow them. A machine that never slept cannot tell them apart.
  const uint64_t biased = BiasedMs();
  const uint64_t slept = biased > after ? biased - after : 0;
  if (slept > 1000) {
    const uint64_t watch = ThreadWatchClockMs();
    const uint64_t tick = GetTickCount64();
    Check("the watch clock leaves out the time this boot slept",
          tick > watch && tick - watch + 100 >= slept);
  } else {
    std::printf("thread-watch-clock: this boot has not slept (%" PRIu64 " ms); "
                "the sleep exclusion is checked by --after-suspend\n", slept);
  }
  const uint64_t start = ThreadWatchClockMs();
  Sleep(50);
  const uint64_t end = ThreadWatchClockMs();
  Check("the watch clock advances while awake", end - start >= 40 && end - start < 5000);
  if (argc == 2 && std::strcmp(argv[1], "--after-suspend") == 0) {
    const uint64_t start_watch = ThreadWatchClockMs();
    const uint64_t start_tick = GetTickCount64();
    std::puts("Suspend or hibernate Windows for at least 10 seconds, resume, then press Enter.");
    std::fflush(stdout);
    if (std::getchar() == EOF) return 2;
    const uint64_t watch = ThreadWatchClockMs() - start_watch;
    const uint64_t tick = GetTickCount64() - start_tick;
    std::printf("thread-watch-clock: watch %" PRIu64 " ms, GetTickCount64 %" PRIu64 " ms\n",
                watch, tick);
    Check("a physical suspend is not counted as a wait", tick > watch + 5000);
  } else if (argc != 1) {
    return 2;
  }
  std::printf("thread-watch-clock: %d assertions, %d failures\n", checks, failures);
  return failures != 0;
}

// Actual NDK pthreads; run in both ABI ART and APK contexts with native2.
// Keep the target alive while sampling it from an otherwise idle caller.
struct CpuClockWorker {
  std::atomic<int> phase{0};
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t completed = PTHREAD_COND_INITIALIZER;
  uint64_t checksum = 1;

  ~CpuClockWorker() {
    pthread_cond_destroy(&completed);
    pthread_mutex_destroy(&mutex);
  }
};

int64_t CpuClockNanos(clockid_t clock) {
  timespec ts{};
  if (clock_gettime(clock, &ts) != 0) return -1;
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void* CpuClockTarget(void* raw) {
  auto& state = *static_cast<CpuClockWorker*>(raw);
  state.phase.store(1);
  while (state.phase.load() == 1) usleep(1000);
  if (state.phase.load() == 4) return nullptr;
  int64_t before = CpuClockNanos(CLOCK_THREAD_CPUTIME_ID);
  int64_t deadline = CpuClockNanos(CLOCK_MONOTONIC) + 10000000000LL;
  while (state.phase.load() == 2 && CpuClockNanos(CLOCK_MONOTONIC) < deadline &&
         CpuClockNanos(CLOCK_THREAD_CPUTIME_ID) - before < 100000000) {
    for (unsigned i = 0; i < 10000; ++i) state.checksum = state.checksum * 6364136223846793005ULL + 1;
  }
  pthread_mutex_lock(&state.mutex);
  if (state.phase.load() == 2) state.phase.store(3);
  pthread_cond_signal(&state.completed);
  pthread_mutex_unlock(&state.mutex);
  while (state.phase.load() == 3) usleep(1000);
  return nullptr;
}

Part PthreadCpuClocks() {
  Part p{"pthread_cpu_clock"};
  CpuClockWorker state;
  pthread_t target{};
  int created = pthread_create(&target, nullptr, CpuClockTarget, &state);
  p.Check(created == 0, "create real target");
  if (created != 0) return p;
  while (state.phase.load() != 1) usleep(1000);
  clockid_t clock = 0, again = 0;
  p.Check(pthread_getcpuclockid(target, &clock) == 0, "acquire target clock");
  bool repeated = true;
  for (int i = 0; i < 1024; ++i) {
    repeated &= pthread_getcpuclockid(target, &again) == 0 && again == clock;
  }
  p.Check(repeated, "1024 acquisitions retain one target identity");
  timespec resolution{};
  p.Check(clock_getres(clock, &resolution) == 0 &&
              resolution.tv_sec == 0 && resolution.tv_nsec > 0, "target resolution");
  // The caller must block rather than repeatedly execute translated polling
  // code while the target burns CPU. Keep one monotonic deadline across any
  // spurious wakeups, and retain the strict independent-CPU assertion below.
  timespec deadline{};
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += 15;
  pthread_mutex_lock(&state.mutex);
  int64_t target_before = CpuClockNanos(clock);
  int64_t caller_before = CpuClockNanos(CLOCK_THREAD_CPUTIME_ID);
  state.phase.store(2);
  int waited = 0;
  while (state.phase.load() != 3 && waited == 0) {
    waited = pthread_cond_timedwait_monotonic_np(&state.completed, &state.mutex, &deadline);
  }
  bool completed = waited == 0 && state.phase.load() == 3;
  pthread_mutex_unlock(&state.mutex);
  int64_t target_delta = CpuClockNanos(clock) - target_before;
  int64_t caller_delta = CpuClockNanos(CLOCK_THREAD_CPUTIME_ID) - caller_before;
  p.Check(completed, "target completes within monotonic deadline");
  p.Check(target_before >= 0 && target_delta >= 50000000 && caller_delta >= 0 &&
              target_delta > caller_delta, "busy target advances independently of idle caller");
  clockid_t unchanged = 77;
  p.Check(pthread_getcpuclockid(pthread_t{}, &unchanged) == ESRCH && unchanged == 77,
          "invalid pthread rejected without output mutation");
  state.phase.store(4);
  p.Check(pthread_join(target, nullptr) == 0, "join target");
  p.Check(pthread_getcpuclockid(target, &unchanged) == ESRCH, "dead pthread rejected");
  timespec sample{};
  errno = 0;
  p.Check(clock_gettime(clock, &sample) == -1 && errno == EINVAL, "dead retained clock rejected");
  errno = 0;
  p.Check(clock_getres(clock, &sample) == -1 && errno == EINVAL,
          "dead clock resolution matches observed Linux EINVAL");
  // Replacement threads must never revive the retired clock even if kernel
  // or userspace allocators reuse their storage or thread identifiers.
  bool retired = true;
  for (int i = 0; i < 64; ++i) {
    CpuClockWorker replacement;
    pthread_t fresh{};
    if (pthread_create(&fresh, nullptr, CpuClockTarget, &replacement) != 0) {
      retired = false;
      break;
    }
    while (replacement.phase.load() != 1) usleep(1000);
    clockid_t fresh_clock = 0;
    retired &= pthread_getcpuclockid(fresh, &fresh_clock) == 0 && fresh_clock != clock;
    errno = 0;
    retired &= clock_gettime(clock, &sample) == -1 && errno == EINVAL;
    // Skip the CPU burn for replacement controls.
    replacement.phase.store(4);
    retired &= pthread_join(fresh, nullptr) == 0;
  }
  p.Check(retired, "64 replacements cannot revive retired identity");
  p.notes.push_back("target_ns=" + std::to_string(target_delta) +
                    " caller_ns=" + std::to_string(caller_delta) +
                    " checksum=" + std::to_string(state.checksum));
  return p;
}

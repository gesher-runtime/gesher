// Complete source-component measurement: actual key creation, actual host
// pthread TLS exit, per-thread value checks and retirement are inside the clock.
// This does not execute Windows thread registration, ART or guest ELF loading.
#include "pthread_key_measurement.h"

#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <time.h>

using namespace gesher::bionic;

namespace {
struct Workload {
  const char* name;
  int readers;
  bool distinct;
  size_t reads;
  size_t churn;
  int groups;
  size_t mutations;
};

constexpr Workload kWorkloads[] = {
    {"cold-single", 1, false, 1, 0, 1, 0},
    {"single", 1, false, 1000000, 0, 1, 0},
    {"shared8", 8, false, 125000, 0, 1, 0},
    {"distinct8", 8, true, 125000, 0, 1, 0},
    {"distinct32", 32, true, 31250, 0, 1, 0},
    {"churn8", 8, false, 125000, 4000, 1, 0},
    {"tiny-lifecycle", 8, true, 1, 0, 64, 0},
    {"no-read-lifecycle", 8, true, 0, 0, 64, 0},
    {"mutation-control", 0, false, 0, 0, 0, 40000},
};

void Require(bool condition) {
  if (!condition) {
    std::fputs("pthread key measurement ownership/value assertion failed\n", stderr);
    std::abort();
  }
}

uint64_t ProcessNs() {
  timespec time{};
  Require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) == 0);
  return uint64_t(time.tv_sec) * 1000000000 + uint64_t(time.tv_nsec);
}

uint64_t ThreadNs() {
  timespec time{};
  Require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0);
  return uint64_t(time.tv_sec) * 1000000000 + uint64_t(time.tv_nsec);
}

uint64_t WallNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct alignas(64) ReaderResult {
  size_t verified = 0;
  unsigned exits = 0;
  bool valid = true;
  uint64_t cpu_start = 0, cpu_to_destructor = 0;
};
static_assert(sizeof(ReaderResult) == 64);

void BIONIC_SYSV ReaderExit(void* value) {
  ReaderResult& result = *static_cast<ReaderResult*>(value);
  ++result.exits;
  result.cpu_to_destructor = ThreadNs() - result.cpu_start;
}

void Run(const Workload& work, int sample) {
  // Counted and uninstrumented executables are separate. Never use the counted
  // binary's time as the CPU evidence for this change.
  std::array<uint64_t, 513> worker_samples{};
  size_t worker_count = 0;
  const uint64_t locks_before = KeyMeasureRegistryLocks();
  const uint64_t cpu_start = ProcessNs(), wall_start = WallNs();
  const uint64_t main_cpu_start = ThreadNs();
  uint64_t verified = 0, exits = 0, worker_cpu_ns = 0;
  for (int group = 0; group < work.groups; ++group) {
    const int key_count = work.distinct ? work.readers : 1;
    std::array<bionic_pthread_key_t, 32> keys{};
    for (int k = 0; k < key_count; ++k)
      Require(gesher_bionic_pthread_key_create(&keys[k], ReaderExit) == 0);
    std::array<ReaderResult, 32> results{};
    std::array<std::thread, 32> threads;
    std::barrier start(work.readers + 1 + int(work.churn != 0));
    for (int i = 0; i < work.readers; ++i) {
      threads[i] = std::thread([&, i] {
        ReaderResult& result = results[i];
        result.cpu_start = ThreadNs();
        const auto key = keys[work.distinct ? i : 0];
        Require(gesher_bionic_pthread_setspecific(key, &result) == 0);
        start.arrive_and_wait();
        errno = 123;
        for (size_t n = 0; n < work.reads; ++n) {
          if (gesher_bionic_pthread_getspecific(key) != &result) result.valid = false;
          ++result.verified;
        }
        if (errno != 123) result.valid = false;
      });
    }
    std::thread writer;
    uint64_t writer_cpu_ns = 0;
    if (work.churn) writer = std::thread([&] {
      const uint64_t writer_start = ThreadNs();
      start.arrive_and_wait();
      for (size_t n = 0; n < work.churn; ++n) {
        bionic_pthread_key_t key = -1;
        Require(gesher_bionic_pthread_key_create(&key, nullptr) == 0);
        Require(gesher_bionic_pthread_key_delete(key) == 0);
      }
      writer_cpu_ns = ThreadNs() - writer_start;
    });
    start.arrive_and_wait();
    for (int i = 0; i < work.readers; ++i) {
      threads[i].join();
      Require(results[i].valid && results[i].verified == work.reads && results[i].exits == 1);
      verified += results[i].verified;
      exits += results[i].exits;
      Require(worker_count < worker_samples.size());
      worker_samples[worker_count++] = results[i].cpu_to_destructor;
      worker_cpu_ns += results[i].cpu_to_destructor;
    }
    if (work.churn) {
      writer.join();
      Require(worker_count < worker_samples.size());
      worker_samples[worker_count++] = writer_cpu_ns;
      worker_cpu_ns += writer_cpu_ns;
    }
    for (int k = 0; k < key_count; ++k)
      Require(gesher_bionic_pthread_key_delete(keys[k]) == 0);
  }
  for (size_t n = 0; n < work.mutations; ++n) {
    bionic_pthread_key_t key = -1;
    Require(gesher_bionic_pthread_key_create(&key, nullptr) == 0);
    Require(gesher_bionic_pthread_setspecific(key, &verified) == 0);
    Require(gesher_bionic_pthread_setspecific(key, nullptr) == 0);
    Require(gesher_bionic_pthread_key_delete(key) == 0);
  }
  const uint64_t main_cpu_ns = ThreadNs() - main_cpu_start;
  const uint64_t cpu_ns = ProcessNs() - cpu_start, wall_ns = WallNs() - wall_start;
  const uint64_t locks = KeyMeasureRegistryLocks() - locks_before;
  Require(verified == work.reads * work.readers * work.groups);
  Require(exits == uint64_t(work.readers * work.groups));
  // This environment's first unobserved short-thread campaign produced suspect
  // process-CPU receipts. Retain an independent per-thread lower bound and
  // refuse CPU qualification if the completed process interval undercounts it.
  const bool cpu_consistent = cpu_ns >= main_cpu_ns + worker_cpu_ns;
  std::printf("{\"case\":\"%s\",\"sample\":%d,\"verified\":%llu,\"exits\":%llu,"
              "\"cpu_ns\":%llu,\"wall_ns\":%llu,\"registry_locks\":%llu,"
              "\"global_array_bytes\":%zu,\"thread_array_bytes\":%zu,\"instrumented\":%s,"
              "\"main_cpu_ns\":%llu,\"worker_cpu_ns\":%llu,\"cpu_consistent\":%s,\"worker_cpu_samples_ns\":[",
              work.name, sample, static_cast<unsigned long long>(verified),
              static_cast<unsigned long long>(exits), static_cast<unsigned long long>(cpu_ns),
              static_cast<unsigned long long>(wall_ns), static_cast<unsigned long long>(locks),
              KeyMeasureGlobalArrays(), KeyMeasureLocalArrays(),
              KeyMeasureInstrumented() ? "true" : "false",
              static_cast<unsigned long long>(main_cpu_ns),
              static_cast<unsigned long long>(worker_cpu_ns), cpu_consistent ? "true" : "false");
  for (size_t i = 0; i < worker_count; ++i)
    std::printf("%s%llu", i == 0 ? "" : ",", static_cast<unsigned long long>(worker_samples[i]));
  std::printf("]}\n");
  std::fflush(stdout);
  Require(cpu_consistent);
}
}

int main(int argc, char** argv) {
  int samples = 10;
  if (argc == 2 || argc == 3) {
    char* end = nullptr;
    const long requested = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || requested < 1 || requested > 100) return 2;
    samples = static_cast<int>(requested);
  } else if (argc != 1) return 2;
  bool selected = false;
  for (size_t i = 0; i < std::size(kWorkloads); ++i) {
    if (argc == 3 && std::strcmp(argv[2], kWorkloads[i].name) != 0) continue;
    selected = true;
    // Cold-single is once per fresh process. Other cases retain samples0/1 as
    // warm-ups. The driver isolates each timed workload in its own process.
    for (int sample = 0; sample < (i == 0 ? 1 : samples); ++sample)
      Run(kWorkloads[i], sample);
  }
  if (!selected) return 2;
  return 0;
}

// Complete assignment components: key creation, real host threads, actual
// assignment/get/clear calls, host TLS-exit callbacks, joins and key retirement.
// Production is a separate TU with its unchanged host-exit arming; no LTO.
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
enum class Mode { Assign, Cycle, Clear, Read, None };
struct Workload {
  const char* name;
  int writers;
  bool distinct;
  Mode mode;
  size_t iterations;
  int groups;
  size_t churn;
  size_t mutations;
};
constexpr Workload kWorkloads[] = {
    {"cold-single", 1, false, Mode::Assign, 1, 1, 0, 0},
    {"single", 1, false, Mode::Cycle, 262144, 1, 0, 0},
    {"shared2", 2, false, Mode::Cycle, 131072, 1, 0, 0},
    {"shared8", 8, false, Mode::Cycle, 32768, 1, 0, 0},
    {"distinct8", 8, true, Mode::Cycle, 32768, 1, 0, 0},
    {"distinct32", 32, true, Mode::Cycle, 8192, 1, 0, 0},
    {"clear8", 8, false, Mode::Clear, 32768, 1, 0, 0},
    {"churn8", 8, false, Mode::Cycle, 32768, 1, 4000, 0},
    {"tiny-lifecycle", 8, true, Mode::Assign, 1, 32, 0, 0},
    {"no-set-lifecycle", 8, true, Mode::None, 0, 32, 0, 0},
    {"create-delete-control", 0, false, Mode::None, 0, 0, 0, 40000},
    {"read-only-control", 1, false, Mode::Read, 786432, 1, 0, 0},
};

void Require(bool value) {
  if (!value) {
    std::fputs("pthread assignment ownership/value/CPU assertion failed\n", stderr);
    std::abort();
  }
}
uint64_t CpuNs(clockid_t clock) {
  timespec value{};
  Require(clock_gettime(clock, &value) == 0);
  return uint64_t(value.tv_sec) * 1000000000 + uint64_t(value.tv_nsec);
}
uint64_t WallNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Result;
struct Value { Result* owner; unsigned tag; };
struct alignas(64) Result {
  Value a{this, 0}, b{this, 1};
  uint64_t start = 0, cpu = 0, sets = 0, reads = 0;
  unsigned exits = 0, last_tag = 9;
  bool valid = true;
};
void BIONIC_SYSV AtExit(void* pointer) {
  auto& value = *static_cast<Value*>(pointer);
  Result& result = *value.owner;
  ++result.exits;
  result.last_tag = value.tag;
  result.cpu = CpuNs(CLOCK_THREAD_CPUTIME_ID) - result.start;
}
void Run(const Workload& work, int sample) {
  std::array<uint64_t, 257> worker_samples{};
  size_t worker_count = 0;
  const uint64_t locks_start = KeyMeasureRegistryLocks();
  const uint64_t cpu_start = CpuNs(CLOCK_PROCESS_CPUTIME_ID), wall_start = WallNs();
  const uint64_t main_start = CpuNs(CLOCK_THREAD_CPUTIME_ID);
  uint64_t sets = 0, reads = 0, exits = 0, created = 0, worker_cpu = 0;
  for (int group = 0; group < work.groups; ++group) {
    const int key_count = work.distinct ? work.writers : 1;
    std::array<bionic_pthread_key_t, 32> keys{};
    for (int i = 0; i < key_count; ++i)
      Require(gesher_bionic_pthread_key_create(&keys[i], AtExit) == 0);
    created += key_count;
    std::array<Result, 32> result{};
    std::array<std::thread, 32> writers;
    std::barrier start(work.writers + 1 + int(work.churn != 0));
    for (int i = 0; i < work.writers; ++i) writers[i] = std::thread([&, i] {
      Result& r = result[i];
      r.start = CpuNs(CLOCK_THREAD_CPUTIME_ID);
      const auto key = keys[work.distinct ? i : 0];
      auto set = [&](const void* value) {
        r.valid &= gesher_bionic_pthread_setspecific(key, value) == 0;
        ++r.sets;
      };
      auto get = [&](const void* expected) {
        r.valid &= gesher_bionic_pthread_getspecific(key) == expected;
        ++r.reads;
      };
      start.arrive_and_wait();
      errno = EDOM;
      if (work.mode == Mode::Read || work.mode == Mode::Clear) set(&r.b);
      for (size_t n = 0; n < work.iterations; ++n) {
        switch (work.mode) {
          case Mode::Assign: {
            Value* value = n % 2 == 0 ? &r.a : &r.b;
            set(value); get(value); break;
          }
          case Mode::Cycle:
            set(&r.a); get(&r.a);
            set(nullptr); get(nullptr);
            set(&r.b); get(&r.b);
            break;
          case Mode::Clear: set(nullptr); get(nullptr); break;
          case Mode::Read: get(&r.b); break;
          case Mode::None: break;
        }
      }
      if (work.mode == Mode::Clear) set(&r.b);
      r.valid &= errno == EDOM;
      if (work.mode == Mode::None)
        r.cpu = CpuNs(CLOCK_THREAD_CPUTIME_ID) - r.start;
      // Assigned threads record their CPU in the actual production TLS-exit
      // callback. No-set threads have no such callback and record at return.
    });
    std::thread churn;
    uint64_t churn_cpu = 0;
    if (work.churn) churn = std::thread([&] {
      const uint64_t begin = CpuNs(CLOCK_THREAD_CPUTIME_ID);
      start.arrive_and_wait();
      for (size_t i = 0; i < work.churn; ++i) {
        bionic_pthread_key_t key = -1;
        Require(gesher_bionic_pthread_key_create(&key, nullptr) == 0);
        Require(gesher_bionic_pthread_key_delete(key) == 0);
      }
      churn_cpu = CpuNs(CLOCK_THREAD_CPUTIME_ID) - begin;
    });
    start.arrive_and_wait();
    for (int i = 0; i < work.writers; ++i) {
      writers[i].join();
      const auto& r = result[i];
      const bool assigned = work.mode != Mode::None;
      const unsigned expected_tag = work.mode == Mode::Assign ? unsigned((work.iterations - 1) % 2) : 1;
      Require(r.valid && r.exits == unsigned(assigned) && (!assigned || r.last_tag == expected_tag));
      sets += r.sets; reads += r.reads; exits += r.exits;
      Require(worker_count < worker_samples.size());
      worker_samples[worker_count++] = r.cpu;
      worker_cpu += r.cpu;
    }
    if (work.churn) {
      churn.join();
      created += work.churn;
      Require(worker_count < worker_samples.size());
      worker_samples[worker_count++] = churn_cpu;
      worker_cpu += churn_cpu;
    }
    for (int i = 0; i < key_count; ++i) Require(gesher_bionic_pthread_key_delete(keys[i]) == 0);
  }
  for (size_t i = 0; i < work.mutations; ++i) {
    bionic_pthread_key_t key = -1;
    Require(gesher_bionic_pthread_key_create(&key, nullptr) == 0);
    Require(gesher_bionic_pthread_key_delete(key) == 0);
  }
  created += work.mutations;
  const uint64_t main_cpu = CpuNs(CLOCK_THREAD_CPUTIME_ID) - main_start;
  const uint64_t cpu = CpuNs(CLOCK_PROCESS_CPUTIME_ID) - cpu_start, wall = WallNs() - wall_start;
  const uint64_t locks = KeyMeasureRegistryLocks() - locks_start;
  const uint64_t calls = uint64_t(work.writers) * work.groups;
  const uint64_t expected_sets_per_worker = work.mode == Mode::Cycle ? work.iterations * 3 :
      work.mode == Mode::Assign ? work.iterations : work.mode == Mode::Clear ? work.iterations + 2 :
      work.mode == Mode::Read ? 1 : 0;
  const uint64_t expected_reads_per_worker = work.mode == Mode::Cycle ? work.iterations * 3 : work.iterations;
  Require(sets == calls * expected_sets_per_worker && reads == calls * expected_reads_per_worker &&
          exits == (work.mode == Mode::None ? 0 : calls));
  const bool consistent = cpu >= main_cpu + worker_cpu;
  std::printf("{\"case\":\"%s\",\"sample\":%d,\"verified\":%llu,\"sets\":%llu,\"reads\":%llu,"
              "\"exits\":%llu,\"created\":%llu,\"cpu_ns\":%llu,\"wall_ns\":%llu,\"registry_locks\":%llu,"
              "\"global_array_bytes\":%zu,\"thread_array_bytes\":%zu,\"instrumented\":%s,"
              "\"main_cpu_ns\":%llu,\"worker_cpu_ns\":%llu,\"cpu_consistent\":%s,\"worker_cpu_samples_ns\":[",
              work.name, sample, (unsigned long long)reads, (unsigned long long)sets, (unsigned long long)reads,
              (unsigned long long)exits, (unsigned long long)created, (unsigned long long)cpu,
              (unsigned long long)wall, (unsigned long long)locks, KeyMeasureGlobalArrays(), KeyMeasureLocalArrays(),
              KeyMeasureInstrumented() ? "true" : "false", (unsigned long long)main_cpu,
              (unsigned long long)worker_cpu, consistent ? "true" : "false");
  for (size_t i = 0; i < worker_count; ++i)
    std::printf("%s%llu", i ? "," : "", (unsigned long long)worker_samples[i]);
  std::printf("]}\n");
  std::fflush(stdout);
  Require(consistent);
}
}
int main(int argc, char** argv) {
  int samples = 10;
  if (argc == 2 || argc == 3) {
    char* end = nullptr;
    const long count = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end || count < 1 || count > 100) return 2;
    samples = int(count);
  } else if (argc != 1) return 2;
  bool selected = false;
  for (size_t i = 0; i < std::size(kWorkloads); ++i) {
    if (argc == 3 && std::strcmp(argv[2], kWorkloads[i].name) != 0) continue;
    selected = true;
    for (int n = 0; n < (i == 0 ? 1 : samples); ++n) Run(kWorkloads[i], n);
  }
  return selected ? 0 : 2;
}

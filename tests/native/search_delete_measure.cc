// Separate ordinary caller for complete public operations. No production extraction.
#include "search_bionic.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace tree = gesher::bionic::search;
struct Key { int value; char text[96]; };
static unsigned long long checks;
static void require(bool ok) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "SEARCH_DELETE_MEASURE_FAILURE %llu\n", checks); std::exit(1); }
}
#if defined(TREE_DELETE_COUNTED)
static unsigned long long comparisons, allocations, allocated_bytes, frees;
static bool count_work;
extern "C" void* __real_malloc(size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_malloc(size_t size) {
  if (count_work) { ++allocations; allocated_bytes += size; }
  return __real_malloc(size);
}
extern "C" void __wrap_free(void* value) {
  if (count_work && value) ++frees;
  __real_free(value);
}
#endif
__attribute__((noinline)) static int BIONIC_SYSV ints(const void* a, const void* b) {
#if defined(TREE_DELETE_COUNTED)
  if (count_work) ++comparisons;
#endif
  int x = static_cast<const Key*>(a)->value, y = static_cast<const Key*>(b)->value;
  return (x > y) - (x < y);
}
__attribute__((noinline)) static int BIONIC_SYSV strings(const void* a, const void* b) {
#if defined(TREE_DELETE_COUNTED)
  if (count_work) ++comparisons;
#endif
  return std::strcmp(static_cast<const Key*>(a)->text, static_cast<const Key*>(b)->text);
}
static void BIONIC_SYSV keep(void*) {}
static Key make_key(int value) {
  Key key{};
  key.value = value;
  std::memset(key.text, 'p', 72);
  std::snprintf(key.text + 72, 24, "%010d", value);
  return key;
}
static long long cpu() {
  timespec now{};
  require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0);
  return now.tv_sec * 1000000000LL + now.tv_nsec;
}
static long long wall() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum class Mode { Drain, Churn, Missing, Find, Lifecycle, Duplicate, Mixed25, Mixed75, Empty };
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string name = argv[1];
  Mode mode;
  if (name == "drain") mode = Mode::Drain;
  else if (name == "churn") mode = Mode::Churn;
  else if (name == "missing") mode = Mode::Missing;
  else if (name == "find") mode = Mode::Find;
  else if (name == "lifecycle") mode = Mode::Lifecycle;
  else if (name == "duplicate") mode = Mode::Duplicate;
  else if (name == "mixed25") mode = Mode::Mixed25;
  else if (name == "mixed75") mode = Mode::Mixed75;
  else if (name == "empty") mode = Mode::Empty;
  else return 2;
  int size = std::atoi(argv[2]);
  if (size < 1 || size > 65536) return 2;
  auto compare = !std::strcmp(argv[3], "string") ? strings : ints;
  if (std::strcmp(argv[3], "string") && std::strcmp(argv[3], "int")) return 2;
  std::vector<Key> keys, missing;
  keys.reserve(size); missing.reserve(size);
  for (int i = 0; i < size; ++i) {
    keys.push_back(make_key(2 * i)); missing.push_back(make_key(2 * i + 1));
  }
#if defined(TREE_DELETE_COUNTED)
  const int rounds = 1;
#else
  const int rounds = std::max(4, 524288 / size);
#endif
  long long total_cpu = 0, total_wall = 0, first_cpu = 0, first_wall = 0;
  unsigned long long operations = 0, checksum = 0;
  for (int round = -1; round < rounds; ++round) {
    void* root = nullptr;
    auto build = [&] {
      for (int i = 0; i < size; ++i) require(tree::Tsearch(&keys[i], &root, compare) != nullptr);
    };
    if (mode != Mode::Lifecycle && mode != Mode::Empty) build();
#if defined(TREE_DELETE_COUNTED)
    count_work = true;
#endif
    auto c0 = cpu(), w0 = wall();
    if (mode == Mode::Lifecycle) build();
    for (int j = 0; j < size; ++j) {
      int i = (j * 197) % size;
      switch (mode) {
        case Mode::Drain:
        case Mode::Lifecycle:
          require(tree::Tdelete(&keys[i], &root, compare) != nullptr);
          break;
        case Mode::Churn: {
          require(tree::Tdelete(&keys[i], &root, compare) != nullptr);
          auto* found = static_cast<const Key* const*>(tree::Tsearch(&keys[i], &root, compare));
          require(found && *found == &keys[i]);
          break;
        }
        case Mode::Missing:
        case Mode::Empty:
          require(tree::Tdelete(&missing[i], &root, compare) == nullptr);
          break;
        case Mode::Find: {
          auto* found = static_cast<const Key* const*>(tree::Tfind(&keys[i], &root, compare));
          require(found && *found == &keys[i]);
          break;
        }
        case Mode::Duplicate: {
          auto* found = static_cast<const Key* const*>(tree::Tsearch(&keys[i], &root, compare));
          require(found && *found == &keys[i]);
          break;
        }
        case Mode::Mixed25:
        case Mode::Mixed75: {
          bool present = mode == Mode::Mixed25 ? (j % 4 == 0) : (j % 4 != 0);
          if (present) {
            require(tree::Tdelete(&keys[i], &root, compare) != nullptr);
            auto* found = static_cast<const Key* const*>(tree::Tsearch(&keys[i], &root, compare));
            require(found && *found == &keys[i]);
          } else {
            require(tree::Tdelete(&missing[i], &root, compare) == nullptr);
          }
          break;
        }
      }
      checksum += static_cast<unsigned>(keys[i].value) + 1;
    }
    auto w1 = wall(), c1 = cpu();
#if defined(TREE_DELETE_COUNTED)
    count_work = false;
#endif
    if (round < 0) { first_cpu = c1 - c0; first_wall = w1 - w0; }
    else { total_cpu += c1 - c0; total_wall += w1 - w0; operations += size; }
    if (mode == Mode::Drain || mode == Mode::Lifecycle || mode == Mode::Empty) {
      require(root == nullptr);
    } else {
      for (int i = 0; i < size; ++i) {
        auto* found = static_cast<const Key* const*>(tree::Tfind(&keys[i], &root, compare));
        require(found && *found == &keys[i]);
      }
      tree::Tdestroy(root, keep);
    }
  }
  auto expected = static_cast<unsigned long long>(rounds + 1) * size * size;
  require(checksum == expected);
  std::printf("{\"mode\":\"%s\",\"size\":%d,\"kind\":\"%s\",\"operations\":%llu,"
              "\"first_operations\":%d,\"cpu_ns\":%lld,\"wall_ns\":%lld,"
              "\"first_cpu_ns\":%lld,\"first_wall_ns\":%lld,\"checksum\":%llu,\"expected\":%llu",
              name.c_str(), size, argv[3], operations, size, total_cpu, total_wall,
              first_cpu, first_wall, checksum, expected);
#if defined(TREE_DELETE_COUNTED)
  std::printf(",\"counted_only\":true,\"comparisons\":%llu,\"allocations\":%llu,"
              "\"allocated_bytes\":%llu,\"frees\":%llu", comparisons, allocations, allocated_bytes, frees);
#endif
  std::printf("}\n");
}

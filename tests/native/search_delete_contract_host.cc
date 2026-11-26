// Compile beside the complete production search_bionic.cc, not an extracted helper.
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(TREE_DELETE_ANDROID_REFERENCE)
#include <search.h>
#define TREE_CALLBACK_ABI
#define TREE_DELETE_CHECK_BALANCE 0
#else
#include "search_bionic.h"
using VISIT = int;
enum { preorder, postorder, endorder, leaf };
#define TREE_CALLBACK_ABI BIONIC_SYSV
#define tsearch gesher::bionic::search::Tsearch
#define tfind gesher::bionic::search::Tfind
#define tdelete gesher::bionic::search::Tdelete
#define twalk gesher::bionic::search::Twalk
#define tdestroy gesher::bionic::search::Tdestroy
#endif

static std::atomic<unsigned long long> host_checks{0};
static void check(const char* what, int ok) {
  ++host_checks;
  if (!ok) {
    std::fprintf(stderr, "SEARCH_DELETE_CONTRACT_FAILURE: %s\n", what);
    std::exit(1);
  }
}
static int TREE_CALLBACK_ABI compare_ints(const void* a, const void* b) {
  int x = *static_cast<const int*>(a), y = *static_cast<const int*>(b);
  return (x > y) - (x < y);
}

#define TREE_DELETE_EXTENDED 1
#include "search_test.inc"

#if defined(TREE_DELETE_ANDROID_REFERENCE)
#define TREE_HOST_ABI
#else
#define TREE_HOST_ABI BIONIC_SYSV
#endif

static void TREE_HOST_ABI keep_key(void*) {}
struct Reentrant {
  void** outer;
  void** inner;
  const int* inner_key;
  bool inside = false;
  bool retired = false;
  unsigned lookups = 0;
};
static thread_local Reentrant* reentrant;
static int TREE_HOST_ABI nested_compare(const void* a, const void* b) {
  if (reentrant && !reentrant->inside) {
    reentrant->inside = true;
    check("read-only comparator lookup of current tree", tfind(b, reentrant->outer,
                                                             compare_ints) != nullptr);
    ++reentrant->lookups;
    if (!reentrant->retired) {
      check("comparator deletes from independent tree", tdelete(reentrant->inner_key,
            reentrant->inner, compare_ints) != nullptr);
      reentrant->retired = true;
    }
    reentrant->inside = false;
  }
  return compare_ints(a, b);
}

static void check_reentrance() {
  std::array<int, 127> keys{};
  void* outer = nullptr;
  void* inner = nullptr;
  for (int i = 0; i < 127; ++i) {
    keys[i] = i;
    check("reentrant outer setup", tsearch(&keys[i], &outer, compare_ints) != nullptr);
    check("reentrant inner setup", tsearch(&keys[i], &inner, compare_ints) != nullptr);
  }
  for (int i = 0; i < 127; ++i) {
    Reentrant state{&outer, &inner, &keys[i]};
    reentrant = &state;
    check("outer deletion across nested lookup/deletion", tdelete(&keys[i], &outer,
                                                                 nested_compare) != nullptr);
    reentrant = nullptr;
    check("nested callback actually ran", state.retired && state.lookups != 0);
    check("both keys removed", tfind(&keys[i], &outer, compare_ints) == nullptr &&
          tfind(&keys[i], &inner, compare_ints) == nullptr);
  }
  check("both nested trees drain", outer == nullptr && inner == nullptr);
}

static void check_independent_threads() {
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([worker] {
      std::array<int, 127> keys{};
      void* root = nullptr;
      for (int i = 0; i < 127; ++i) {
        keys[i] = i;
        check("independent thread tree setup", tsearch(&keys[i], &root, compare_ints) != nullptr);
      }
      for (int round = 0; round < 4096; ++round) {
        int key = (round * 37 + worker) % 127;
        check("independent thread delete", tdelete(&keys[key], &root, compare_ints) != nullptr);
        check("independent thread absent key", tfind(&keys[key], &root, compare_ints) == nullptr);
        auto* found = static_cast<const int* const*>(tsearch(&keys[key], &root, compare_ints));
        check("independent thread key ownership", found && *found == &keys[key]);
      }
      tdestroy(root, keep_key);
    });
  }
  for (auto& worker : workers) worker.join();
}

#if defined(TREE_DELETE_WRAP_ALLOCATOR)
extern "C" void* __real_malloc(size_t);
extern "C" void __real_free(void*);
static thread_local bool allocation_probe;
static thread_local unsigned allocation_calls, release_calls;
extern "C" void* __wrap_malloc(size_t size) {
  if (allocation_probe) { ++allocation_calls; errno = ENOMEM; return nullptr; }
  return __real_malloc(size);
}
extern "C" void __wrap_free(void* value) {
  if (allocation_probe && value) ++release_calls;
  __real_free(value);
}
static void check_no_allocation() {
  std::array<int, 257> keys{};
  void* root = nullptr;
  for (int i = 0; i < 257; ++i) {
    keys[i] = i;
    check("allocation probe setup", tsearch(&keys[i], &root, compare_ints) != nullptr);
  }
  for (int i = 0; i < 257; ++i) {
    allocation_calls = release_calls = 0;
    allocation_probe = true;
    void* result = tdelete(&keys[i], &root, compare_ints);
    allocation_probe = false;
    check("delete succeeds under denied allocations", result != nullptr && allocation_calls == 0 &&
          release_calls == 1);
    allocation_calls = release_calls = 0;
    allocation_probe = true;
    result = tdelete(&keys[i], &root, compare_ints);
    allocation_probe = false;
    check("miss allocates and frees nothing", result == nullptr && allocation_calls == 0 && release_calls == 0);
  }
  check("allocation probe drains", root == nullptr);
}
#endif

int main() {
  test_search();
  check_reentrance();
  check_independent_threads();
#if defined(TREE_DELETE_WRAP_ALLOCATOR)
  check_no_allocation();
#endif
  std::printf("SEARCH_DELETE_CONTRACT_PASS host_checks=%llu shared_checks=%llu trace=%016llx\n",
              host_checks.load(), tree_delete_checks, tree_delete_digest);
}

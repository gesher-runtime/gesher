// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Execute real, original ELF dependency graphs through the public loader API.
// Usage: elf_load_transaction_test <fixture-root> [--arm64]
// The Linux host runner substitutes Win32 primitives explicitly. Windows CI
// uses the production mapping/rewrite/TLS implementations and optional ARM64
// translator. No application-specific behavior or production failure switch.

#include <gesher/elf_loader.h>
#ifdef _WIN32
#include <gesher/arm64_runtime.h>
#endif
#include <elf.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#define TX_SYSV __attribute__((sysv_abi))
#else
#define TX_SYSV
#endif

namespace elf = gesher::elf;
using namespace std::chrono_literals;

namespace {
int checks, failures;
std::string complete_dir, incomplete_dir;
std::array<std::atomic<int>, 32> initialized{};
std::mutex events_lock;
std::vector<int> events;
std::atomic<bool> supply_missing{false}, inject_protection{false}, fail_protection_once{false};
std::atomic<int> protection_failures{0};
bool reentry_parent_visible, reentry_child_worked;
std::mutex barrier_lock;
std::condition_variable barrier_cv;
bool barrier_enabled, barrier_entered, barrier_release;

void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL ELF transaction: %s\n", label);
#ifdef GESHER_ELF_TRANSACTION_FAIL_FAST
    // The old-source negative control must stop at its first observed contract
    // failure, before later tests could execute an invalid partial handle.
    std::fflush(nullptr);
    std::_Exit(1);
#endif
  }
}

struct Watchdog {
  std::mutex lock;
  std::condition_variable changed;
  bool finished = false;
  std::thread worker;
  Watchdog() : worker([this] {
    std::unique_lock<std::mutex> guard(lock);
    if (!changed.wait_for(guard, 90s, [this] { return finished; })) {
      std::fputs("FAIL ELF transaction: watchdog timeout (possible loader deadlock)\n", stderr);
      std::fflush(nullptr);
      std::_Exit(124);
    }
  }) {}
  ~Watchdog() {
    { std::lock_guard<std::mutex> guard(lock); finished = true; }
    changed.notify_all();
    worker.join();
  }
};

// Only this isolated fixture imports host callbacks outside an ELF provider graph.
// SFINAE keeps historical-source behavioral controls buildable with the old API.
template<class T> auto DiagnosticResolver(T& options, int)
    -> decltype(options.allow_unscoped_resolver = true, options.flags = 0x102, void()) {
  options.allow_unscoped_resolver = true;
  options.flags = 0x102;  // Make the existing global-publication assertions meaningful.
}
template<class T> void DiagnosticResolver(T&, long) {}

elf::LoadOptions Options(const std::string& directory) {
  elf::LoadOptions options;
  options.search_paths.push_back(directory);
  options.allow_shimmed_needed = false;
  options.stub_unresolved_functions = false;
  DiagnosticResolver(options, 0);
  return options;
}

elf::Library* Load(const std::string& directory, const char* name,
                   const elf::LoadOptions& options, std::string* error) {
  const std::string path = directory + "/" + name;
  return elf::Load(path.c_str(), error, options);
}

int Call(elf::Library* library, const char* symbol) {
  if (!library) return -999;
  void* entry = elf::Lookup(library, symbol);
  if (!entry) return -998;
#ifdef _WIN32
  if (elf::IsAarch64(library)) entry = gesher::arm64::HostEntry(entry);
#endif
  if (!entry) return -997;
  return reinterpret_cast<int(TX_SYSV*)()>(entry)();
}

size_t Count() {
  size_t count = 0;
  elf::ForEachLoaded([&](const elf::PhdrInfo&) { ++count; return 0; });
  return count;
}

void Unpublished(std::initializer_list<const char*> names) {
  for (const char* name : names) {
    Check(elf::Find(name) == nullptr, "failed graph has no public Find handle");
    std::string symbol(name);
    symbol.resize(symbol.size() - 3);  // original fixture names end in .so
    Check(elf::LookupGlobal(symbol.c_str()) == nullptr, "failed graph has no global export");
    bool seen = false;
    elf::ForEachLoaded([&](const elf::PhdrInfo& info) {
      if ((std::string(info.name).substr(std::string(info.name).find_last_of("/\\") + 1) == name)) seen = true;
      return 0;
    });
    Check(!seen, "failed graph is absent from public program-header iteration");
  }
}

void TX_SYSV Event(int role) {
  if (role < 1 || role >= static_cast<int>(initialized.size())) std::abort();
  ++initialized[role];
  std::lock_guard<std::mutex> guard(events_lock);
  events.push_back(role);
}

int TX_SYSV Missing() { return 41; }
int TX_SYSV BarrierValue() { return 26; }
int TX_SYSV ProtectedValue() { return 18; }
struct TlsIndex { size_t module, offset; };
void* TX_SYSV GetTls(TlsIndex* index) { return elf::TlsGetAddr(index->module, index->offset); }

void TX_SYSV Reenter() {
  reentry_parent_visible = elf::Find("tx_reentrant_root.so") != nullptr;
  std::string error;
  auto* child = Load(complete_dir, "tx_reentrant_child.so", Options(complete_dir), &error);
  reentry_child_worked = child != nullptr && Call(child, "tx_reentrant_child") == 13;
}

void Barrier() {
  std::unique_lock<std::mutex> guard(barrier_lock);
  if (!barrier_enabled) return;
  barrier_entered = true;
  barrier_cv.notify_all();
  barrier_cv.wait(guard, [] { return barrier_release; });
}

void* Resolve(const char* name) {
  if (std::strcmp(name, "__tls_get_addr") == 0) return reinterpret_cast<void*>(&GetTls);
  if (std::strcmp(name, "tx_event") == 0) return reinterpret_cast<void*>(&Event);
  if (std::strcmp(name, "tx_missing") == 0)
    return supply_missing ? reinterpret_cast<void*>(&Missing) : nullptr;
  if (std::strcmp(name, "tx_reenter") == 0) return reinterpret_cast<void*>(&Reenter);
  if (std::strcmp(name, "tx_barrier") == 0) {
    Barrier(); return reinterpret_cast<void*>(&BarrierValue);
  }
  if (std::strcmp(name, "tx_barrier_failure") == 0) { Barrier(); return nullptr; }
  if (std::strcmp(name, "tx_arm_protection_failure") == 0) {
    if (inject_protection) fail_protection_once = true;
    return reinterpret_cast<void*>(&ProtectedValue);
  }
  return nullptr;
}

void MissingDependencyAndRetry() {
  const int before = failures;
  const size_t initial_count = Count();
  std::string error;
  const auto incomplete = Options(incomplete_dir);
  auto* direct = Load(incomplete_dir, "tx_middle.so", incomplete, &error);
  Check(direct == nullptr && !error.empty(), "missing direct dependency fails with a diagnostic");
  Unpublished({"tx_middle.so"});
  Check(initialized[3] == 0, "failed direct dependency does not run a constructor");
  if (failures != before) return;  // Do not execute a baseline's partial handle.

  auto* root = Load(incomplete_dir, "tx_root.so", incomplete, &error);
  Check(root == nullptr, "missing transitive dependency fails the entire root graph");
  Unpublished({"tx_root.so", "tx_middle.so", "tx_newchild.so"});
  Check(initialized[1] == 0 && initialized[3] == 0 && initialized[4] == 0,
        "new successful child has no constructor side effect before whole-graph success");
  Check(Count() == initial_count, "failed graph does not grow the public registry");
  if (failures != before) return;

  std::ifstream input(incomplete_dir + "/tx_root.so", std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  Check(!bytes.empty(), "LoadFromMemory uses bytes from the original real ELF fixture");
  if (bytes.empty()) return;
  for (int attempt = 0; attempt < 32; ++attempt) {
    root = elf::LoadFromMemory("tx_root.so", bytes.data(), bytes.size(), &error, incomplete);
    Check(root == nullptr && !error.empty(), "repeated in-memory failure never returns cached partial success");
    Check(elf::Find("tx_root.so") == nullptr && Count() == initial_count,
          "repeated failure retires each private graph");
  }
  Check(initialized[1] == 0 && initialized[3] == 0 && initialized[4] == 0,
        "repeated failures do not run any failed graph constructor");
  if (failures != before) return;

  auto* borrowed = Load(incomplete_dir, "tx_newchild.so", incomplete, &error);
  Check(borrowed != nullptr && Call(borrowed, "tx_newchild") == 10, "preload a real committed dependency");
  Check(initialized[1] == 1, "preloaded dependency initializes exactly once");
  root = Load(incomplete_dir, "tx_root.so", incomplete, &error);
  Check(root == nullptr, "a borrowed dependency does not hide a later missing dependency");
  Unpublished({"tx_root.so", "tx_middle.so"});
  Check(elf::Find("tx_newchild.so") == borrowed && Call(borrowed, "tx_newchild") == 10,
        "rollback preserves the borrowed dependency's mapping and executable code");
  Check(initialized[1] == 1 && Count() == initial_count + 1,
        "rollback neither finalizes nor reinitializes borrowed dependencies");
  if (failures != before) return;

  auto repaired = incomplete;
  repaired.search_paths.push_back(complete_dir);
  root = elf::LoadFromMemory("tx_root.so", bytes.data(), bytes.size(), &error, repaired);
  Check(root != nullptr && Call(root, "tx_root") == 32, "repairing the dependency permits a complete in-memory retry");
  Check(initialized[1] == 1 && initialized[2] == 1 && initialized[3] == 1 && initialized[4] == 1,
        "repaired graph and borrowed dependency each initialize once");
  Check(Count() == initial_count + 4, "only the four successful graph nodes are published");
  std::set<size_t> tls_ids;
  int with_tls = 0;
  const bool guest_arm64 = elf::IsAarch64(root);
  elf::ForEachLoaded([&](const elf::PhdrInfo& info) {
    for (const char* name : {"tx_newchild.so", "tx_late.so", "tx_middle.so", "tx_root.so"}) {
      if ((std::string(info.name).substr(std::string(info.name).find_last_of("/\\") + 1) == name)) {
        const auto* headers = static_cast<const Elf64_Phdr*>(info.phdr);
        bool has_tls = false;
        for (size_t i = 0; headers && i < info.phnum; ++i) {
          if (headers[i].p_type == PT_TLS && headers[i].p_filesz == sizeof(int)
              && headers[i].p_memsz >= sizeof(int)) has_tls = true;
        }
        if (has_tls) ++with_tls;
        if (info.tls_modid != 0) tls_ids.insert(info.tls_modid);
      }
    }
    return 0;
  });
  Check(with_tls == 4, "all four committed nodes retain their real PT_TLS program headers");
  // The ARM backend uses static TP offsets, not the x86 module-ID registry.
  if (!guest_arm64) Check(tls_ids.size() == 4, "committed x86 TLS graph has distinct real module identities");
}

void RelocationFailureAndRetry() {
  const int before = failures;
  const size_t initial_count = Count();
  std::string error;
  auto* root = Load(complete_dir, "tx_reloc_root.so", Options(complete_dir), &error);
  Check(root == nullptr && !error.empty(), "unresolved required relocation fails the graph");
  Unpublished({"tx_reloc_root.so", "tx_reloc_child.so"});
  Check(initialized[5] == 0 && initialized[6] == 0 && Count() == initial_count,
        "relocation failure runs no child constructor and publishes no nodes");
  if (failures != before) return;
  supply_missing = true;
  root = Load(complete_dir, "tx_reloc_root.so", Options(complete_dir), &error);
  Check(root != nullptr && Call(root, "tx_reloc_root") == 46, "supplying the real callback repairs a failed relocation retry");
  Check(initialized[5] == 1 && initialized[6] == 1 && Count() == initial_count + 2,
        "repaired relocation initializes and publishes both nodes once");
}

void DiamondAndCycle() {
  const size_t initial_count = Count();
  std::string error;
  auto* diamond = Load(complete_dir, "tx_diamond.so", Options(complete_dir), &error);
  Check(diamond != nullptr && Call(diamond, "tx_diamond") == 17, "diamond graph resolves both shared-dependency paths");
  Check(initialized[7] == 1 && initialized[8] == 1 && initialized[9] == 1 && initialized[10] == 1,
        "diamond shared node and every constructor are unique");
  Check(Count() == initial_count + 4, "diamond publishes four distinct nodes");
  auto position = [](int role) {
    std::lock_guard<std::mutex> guard(events_lock);
    return std::find(events.begin(), events.end(), role) - events.begin();
  };
  Check(position(7) < position(8) && position(7) < position(9)
            && position(8) < position(10) && position(9) < position(10),
        "constructors run after their dependency constructors");
  auto* a = Load(complete_dir, "tx_cycle_a.so", Options(complete_dir), &error);
  Check(a != nullptr && Call(a, "tx_cycle_answer") == 23, "cycle resolves imports in both directions");
  auto* b = Load(complete_dir, "tx_cycle_b.so", Options(complete_dir), &error);
  Check(b != nullptr && b == elf::Find("tx_cycle_b.so") && Call(b, "tx_cycle_back") == 11,
        "cycle reuses the already-published partner");
  Check(initialized[11] == 1 && initialized[12] == 1 && Count() == initial_count + 6,
        "cycle constructors and mappings occur once");
}

void ConstructorReentry() {
  std::string error;
  auto* root = Load(complete_dir, "tx_reentrant_root.so", Options(complete_dir), &error);
  Check(root != nullptr && Call(root, "tx_reentrant_root") == 14, "outer constructor can reenter the public loader");
  Check(reentry_parent_visible && reentry_child_worked,
        "constructor reentry sees its committed parent and a usable newly loaded child");
  Check(initialized[13] == 1 && initialized[14] == 1, "nested constructor load initializes both libraries once");
}

void IfuncFileAndMemory() {
  const size_t initial_count = Count();
  std::string error;
  auto* file = Load(complete_dir, "tx_ifunc_file.so", Options(complete_dir), &error);
  Check(file != nullptr && Call(file, "tx_ifunc_result") == 121,
        "real IFUNC executes during file relocation and selects executable code");
  Check(initialized[21] == 1, "file IFUNC object constructs only after relocation");
  std::ifstream input(complete_dir + "/tx_ifunc_memory.so", std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  Check(!bytes.empty(), "memory IFUNC uses original fixture bytes");
  if (bytes.empty()) return;
  auto* memory = elf::LoadFromMemory("tx_ifunc_memory.so", bytes.data(), bytes.size(),
                                   &error, Options(complete_dir));
  Check(memory != nullptr && Call(memory, "tx_ifunc_result") == 122,
        "copied code is executable for a real IFUNC before final protection");
  Check(initialized[22] == 1 && Count() == initial_count + 2,
        "both IFUNC objects publish and construct exactly once");
}

void IfuncDependencyTls() {
  std::string error;
  auto* root = Load(complete_dir, "tx_ifunc_tls_root.so", Options(complete_dir), &error);
  Check(root != nullptr && Call(root, "tx_ifunc_tls_observed") == 37,
        "parent IFUNC observes the relocated child's private TLS initial image");
  auto* child = elf::Find("tx_ifunc_tls_child.so");
  Check(child != nullptr && Call(child, "tx_child_tls_read") == 93,
        "graph commit preserves the owner's IFUNC TLS mutation");
  int later = -1;
  std::thread thread([&] { later = Call(child, "tx_child_tls_read"); });
  thread.join();
  Check(later == 37, "a later thread receives the original child TLS image");
  Check(initialized[23] == 1 && initialized[24] == 1,
        "TLS-dependent IFUNC graph constructs once after complete success");
}

void Concurrent(bool failure) {
  const size_t initial_count = Count();
  const char* name = failure ? "tx_concurrent_fail_root.so" : "tx_concurrent_root.so";
  { std::lock_guard<std::mutex> guard(barrier_lock);
    barrier_enabled = true; barrier_entered = false; barrier_release = false; }
  elf::Library *first = nullptr, *second = nullptr, *observed = nullptr;
  size_t observed_count = 0;
  std::atomic<bool> second_started{false}, observer_started{false};
  std::atomic<bool> second_finished{false}, observer_finished{false};
  std::string first_error, second_error;
  std::thread loader([&] { first = Load(complete_dir, name, Options(complete_dir), &first_error); });
  bool entered;
  { std::unique_lock<std::mutex> guard(barrier_lock);
    entered = barrier_cv.wait_for(guard, 10s, [] { return barrier_entered; }); }
  Check(entered, "actual relocation resolver reaches the deterministic concurrency barrier");
  std::thread duplicate([&] {
    second_started = true;
    second = Load(complete_dir, name, Options(complete_dir), &second_error);
    second_finished = true;
  });
  std::thread observer([&] {
    observer_started = true;
    observed = elf::Find(name);
    observed_count = Count();
    observer_finished = true;
  });
  while (!second_started || !observer_started) std::this_thread::yield();
  // The loader is held at a known relocation point. Give the independently
  // started API callers a scheduling window, then release it unconditionally.
  std::this_thread::sleep_for(150ms);
  Check(!second_finished && !observer_finished, "foreign load and observer cannot return an incomplete graph");
  { std::lock_guard<std::mutex> guard(barrier_lock); barrier_release = true; }
  barrier_cv.notify_all();
  loader.join(); duplicate.join(); observer.join();
  { std::lock_guard<std::mutex> guard(barrier_lock); barrier_enabled = false; }
  if (failure) {
    Check(first == nullptr && second == nullptr && observed == nullptr,
          "both concurrent failures and the observer report no library");
    Check(!first_error.empty() && !second_error.empty(), "both concurrent failures retain diagnostics");
    Check(initialized[19] == 0 && initialized[20] == 0 && Count() == initial_count,
          "concurrent failed graphs run no constructors or leave mappings published");
    Unpublished({"tx_concurrent_fail_root.so", "tx_concurrent_fail_child.so"});
  } else {
    Check(first != nullptr && second == first && observed == first,
          "concurrent successful requests and the observer share one complete handle");
    Check(first != nullptr && Call(first, "tx_concurrent_root") == 41,
          "concurrently loaded graph executes its real linked imports");
    Check(initialized[15] == 1 && initialized[16] == 1 && observed_count == initial_count + 2,
          "concurrent success publishes and initializes each graph node once");
  }
}

#ifdef GESHER_ELF_TRANSACTION_FAILURE
void ProtectionFailureAndRetry() {
  const int before = failures;
  const size_t initial_count = Count();
  std::string error;
  inject_protection = true;
  auto* root = Load(complete_dir, "tx_protect_root.so", Options(complete_dir), &error);
  inject_protection = false;
  Check(root == nullptr && !error.empty() && protection_failures == 1,
        "real final protection path propagates its injected primitive failure");
  Unpublished({"tx_protect_root.so", "tx_protect_child.so"});
  Check(initialized[17] == 0 && initialized[18] == 0 && Count() == initial_count,
        "final protection failure rolls back before any graph constructor");
  if (failures != before) return;
  root = Load(complete_dir, "tx_protect_root.so", Options(complete_dir), &error);
  Check(root != nullptr && Call(root, "tx_protect_root") == 35,
        "protection retry uses actual mappings and successfully executable pages");
  Check(initialized[17] == 1 && initialized[18] == 1 && Count() == initial_count + 2,
        "protection retry publishes and initializes both nodes once");
}
#endif
}  // namespace

// Only the separate test-loader target redirects calls here. The normal
// gesher_elf target and all production executables use real mprotect directly.
extern "C" int gesher_elf_test_mprotect(void* address, size_t length, int protection) {
  if (fail_protection_once.exchange(false)) {
    ++protection_failures;
    errno = EACCES;
    return -1;
  }
  return ::mprotect(address, length, protection);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--arm64") != 0)) {
    std::fprintf(stderr, "usage: %s <fixture-root> [--arm64]\n", argv[0]);
    return 2;
  }
  Watchdog watchdog;
  complete_dir = std::string(argv[1]) + "/complete";
  incomplete_dir = std::string(argv[1]) + "/incomplete";
  if (argc == 3) {
#ifdef _WIN32
    Check(gesher::arm64::EnsureLoaded(), "production ARM64 translator is available");
    if (failures) return 1;
#else
    std::fputs("ARM64 translator execution requires the Windows fixture.\n", stderr);
    return 2;
#endif
  }
  elf::SetBionicResolver(Resolve);
  MissingDependencyAndRetry();
  RelocationFailureAndRetry();
  DiamondAndCycle();
  ConstructorReentry();
  IfuncFileAndMemory();
  IfuncDependencyTls();
  Concurrent(false);
  Concurrent(true);
#ifdef GESHER_ELF_TRANSACTION_FAILURE
  ProtectionFailureAndRetry();
#endif
  std::printf("ELF transaction: %d checks, %d failures\n", checks, failures);
  if (failures == 0) std::puts("elf_load_transaction_test all passed");
  return failures == 0 ? 0 : 1;
}

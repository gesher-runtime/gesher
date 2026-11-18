// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Public-loader checks using original, valid C DSOs and linker version scripts.
// Run each case in a fresh process: the loader deliberately retains SONAMEs.
// Usage: elf_versions_test <fixture-root> <case> [--arm64]
#include <gesher/elf_loader.h>
#include <elf.h>
#ifdef _WIN32
#include <gesher/arm64_runtime.h>
#endif

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(__x86_64__)
#define GV_SYSV __attribute__((sysv_abi))
#else
#define GV_SYSV
#endif

namespace elf = gesher::elf;
namespace {
int checks, failures;
std::array<int, 12> constructors{};
bool arm64, virtual_symbols;
int virtual_data = 555;
std::string root;

void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL ELF versions: %s\n", label);
  }
}

struct Watchdog {
  std::mutex lock;
  std::condition_variable changed;
  bool finished = false;
  std::thread worker;
  Watchdog() : worker([this] {
    std::unique_lock<std::mutex> guard(lock);
    if (!changed.wait_for(guard, std::chrono::seconds(90), [this] { return finished; })) {
      std::fputs("FAIL ELF versions: watchdog timeout\n", stderr);
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

void GV_SYSV Event(int role) {
  if (role < 1 || role >= static_cast<int>(constructors.size())) std::abort();
  ++constructors[role];
}
int GV_SYSV VirtualValue() { return 55; }
struct TlsIndex { size_t module, offset; };
void* GV_SYSV GetTls(TlsIndex* index) { return elf::TlsGetAddr(index->module, index->offset); }
void* Resolve(const char* name) {
  if (std::strcmp(name, "gv_event") == 0) return reinterpret_cast<void*>(&Event);
  if (std::strcmp(name, "__tls_get_addr") == 0) return reinterpret_cast<void*>(&GetTls);
  if (virtual_symbols && std::strcmp(name, "gv_value") == 0)
    return reinterpret_cast<void*>(&VirtualValue);
  if (virtual_symbols && std::strcmp(name, "gv_data") == 0) return &virtual_data;
  return nullptr;
}
void RegisterCallbacks() {
  elf::ProviderOptions provider;
  provider.soname = "libgv_test_host.so";
  provider.lookup = [](const char* name) -> void* {
    if (std::strcmp(name, "gv_event") == 0) return reinterpret_cast<void*>(&Event);
    if (std::strcmp(name, "__tls_get_addr") == 0) return reinterpret_cast<void*>(&GetTls);
    return nullptr;
  };
  std::string error;
  Check(elf::RegisterProvider(provider, &error) != nullptr, "register actual isolated callback provider");
}

elf::LoadOptions Options(const std::string& directory) {
  elf::LoadOptions options;
  options.search_paths.push_back(root + "/" + directory);
  options.allow_shimmed_needed = true;
  options.flags = 0x102;  // This fixture explicitly verifies DEFAULT/global version lookup.
  options.stub_unresolved_functions = false;
  return options;
}
elf::Library* Load(const std::string& directory, const char* filename,
                   const elf::LoadOptions& options, std::string* error) {
  return elf::Load((root + "/" + directory + "/" + filename).c_str(), error, options);
}
elf::Library* Load(const std::string& directory, const char* filename) {
  std::string error;
  auto* lib = Load(directory, filename, Options(directory), &error);
  if (!lib) std::printf("load diagnostic: %s\n", error.c_str());
  Check(lib != nullptr, "ordinary valid fixture loads successfully");
  if (lib) Check(elf::IsAarch64(lib) == arm64, "fixture architecture matches the requested runtime");
  return lib;
}
int Call(void* entry) {
  if (!entry) return -9999;
#ifdef _WIN32
  if (arm64) entry = gesher::arm64::HostEntry(entry);
#endif
  if (!entry) return -9998;
  return reinterpret_cast<int(GV_SYSV*)()>(entry)();
}
int Data(void* address) { return address ? *static_cast<int*>(address) : -9999; }
int64_t CallOffset(void* entry) {
  if (!entry) return INT64_MIN;
#ifdef _WIN32
  if (arm64) entry = gesher::arm64::HostEntry(entry);
#endif
  if (!entry) return INT64_MIN;
  return reinterpret_cast<int64_t(GV_SYSV*)()>(entry)();
}
size_t Count() {
  size_t count = 0;
  elf::ForEachLoaded([&](const elf::PhdrInfo&) { ++count; return 0; });
  return count;
}

void Lookup(const char* directory) {
  auto* lib = Load(directory, "libgv_provider.so");
  if (!lib) return;
  Check(Call(elf::Lookup(lib, "gv_value")) == 22, "unversioned lookup selects the default definition");
  Check(Call(elf::LookupVersion(lib, "gv_value", nullptr)) == 22, "null version preserves ordinary lookup");
  Check(Call(elf::LookupVersion(lib, "gv_value", "GV_1")) == 11, "explicit old version reaches a hidden alias");
  Check(Call(elf::LookupVersion(lib, "gv_value", "GV_2")) == 22, "explicit new version reaches the default alias");
  Check(elf::LookupVersion(lib, "gv_value", "GV_missing") == nullptr, "missing requested version does not use a different definition");
  Check(elf::LookupVersion(lib, "gv_value", "") == nullptr, "empty requested version is not an unversioned request");
  Check(elf::Lookup(lib, "gv_hidden") == nullptr, "a hidden-only definition is absent from unversioned lookup");
  Check(Call(elf::LookupVersion(lib, "gv_hidden", "GV_1")) == 31, "explicit lookup can reach the hidden-only definition");
  Check(Data(elf::Lookup(lib, "gv_data")) == 222, "ordinary data lookup uses the default version");
  Check(Data(elf::LookupVersion(lib, "gv_data", "GV_1")) == 111, "explicit version selects the corresponding data object");
  Check(Call(elf::LookupVersion(lib, "gv_weak", "GV_1")) == 51, "weak definition participates in explicit version lookup");
  Check(Call(elf::Lookup(lib, "gv_weak")) == 52, "weak default definition participates in ordinary lookup");
  Check(Call(elf::LookupVersion(lib, "gv_ifunc", "GV_1")) == 71, "old IFUNC version executes its real resolver");
  Check(Call(elf::Lookup(lib, "gv_ifunc")) == 72, "default IFUNC version executes its real resolver");
  void* resolutions = elf::Lookup(lib, "gv_null_resolutions");
  Check(resolutions != nullptr, "real null IFUNC exposes an independent invocation counter");
  for (const char* name : {"gv_null", "getpid"}) {
    elf::LookupStatus status = elf::LookupStatus::kNotFound;
    int before = Call(resolutions);
    Check(elf::LookupVersion(lib, name, "GV_2", &status) == nullptr && status == elf::LookupStatus::kResolved,
          "handle selection distinguishes a null IFUNC result from absence");
    Check(Call(resolutions) == before + 1, "handle lookup invokes a null IFUNC exactly once");
    status = elf::LookupStatus::kNotFound;
    before = Call(resolutions);
    Check(elf::LookupGlobalVersion(name, "GV_2", &status) == nullptr && status == elf::LookupStatus::kResolved,
          "global selection distinguishes a null IFUNC result from absence");
    Check(Call(resolutions) == before + 1, "global lookup invokes a null IFUNC exactly once");
    Check(elf::LookupVersion(lib, name, "GV_missing", &status) == nullptr && status == elf::LookupStatus::kNotFound,
          "handle missing version reports absence independently");
    status = elf::LookupStatus::kResolved;
    Check(elf::LookupGlobalVersion(name, "GV_missing", &status) == nullptr && status == elf::LookupStatus::kNotFound,
          "global missing version reports absence independently");
  }
  Check(Call(elf::LookupVersion(lib, "gv_bA", "GV_2")) == 301, "first colliding symbol name remains distinct");
  Check(Call(elf::LookupVersion(lib, "gv_ab", "GV_2")) == 302, "GNU hash collision still compares the full symbol name");
  Check(Call(elf::LookupVersion(lib, "gv_aQ", "GV_2")) == 303, "SysV hash collision still compares the full symbol name");
  Check(elf::LookupVersion(lib, "gv_hash_value", "GV_1") == nullptr,
        "another defined version cannot select an unrelated alias");
  for (int repeat = 0; repeat < 32; ++repeat) {
    Check(Call(elf::LookupVersion(lib, "gv_hash_value", "GV_bA")) == 61,
          "colliding version hashes still compare the old full version name");
    Check(Call(elf::LookupVersion(lib, "gv_hash_value", "GV_aQ")) == 62,
          "colliding version hashes still compare the new full version name");
    Check(Call(elf::LookupGlobalVersion("gv_value", "GV_1")) == 11,
          "global old-version lookup remains independent of earlier requests");
    Check(Call(elf::LookupGlobalVersion("gv_value", "GV_2")) == 22,
          "global new-version lookup remains independent of earlier requests");
    Check(Call(elf::LookupGlobal("gv_value")) == 22,
          "ordinary global lookup is unaffected by preceding explicit versions");
  }
  Check(constructors[1] == 1 && Count() == 1, "lookups neither reload nor reinitialize the provider");
}

void Required() {
  for (bool stubs : {false, true}) {
    auto options = Options("required/bad");
    options.stub_unresolved_functions = stubs;
    std::string error;
    auto* lib = Load("required/bad", "libgv_client.so", options, &error);
    Check(lib == nullptr && !error.empty(), "missing required version fails even when ordinary stubs are enabled");
    if (lib) return;
    Check(elf::Find("libgv_client.so") == nullptr && elf::Find("libgv_provider.so") == nullptr && Count() == 0,
          "failed valid alternative provider graph leaves no public handles");
    Check(constructors[2] == 0 && constructors[3] == 0, "version failure runs no provider or client constructor");
  }
  auto* lib = Load("required/good", "libgv_client.so");
  if (!lib) return;
  Check(Call(elf::Lookup(lib, "gv_client_pair")) == 1122,
        "one client binds two versions of the same function by distinct relocation identities");
  Check(Call(elf::Lookup(lib, "gv_client_data")) == 333,
        "versioned data relocations bind both original objects");
  Check(Call(elf::LookupVersion(lib, "gv_value", "GV_1")) == 11,
        "explicit handle lookup searches dependency versions");
  Check(constructors[1] == 1 && constructors[3] == 1 && Count() == 2,
        "repairing the provider permits a clean retry with exactly-once constructors");
}

void Weak(bool present) {
  // Pinned Android16r4 linker_relocate.cpp lookup_symbol accepts a missing
  // weak symbol after version-aware lookup. glibc's upfront VERNEED checks
  // differ, so a glibc rejection is not the expected Android result here.
  auto* lib = Load(present ? "weak/good" : "weak/bad", "libgv_client.so");
  if (!lib) return;
  Check(Call(elf::Lookup(lib, "gv_client_weak")) == (present ? 82 : -7),
        "weak versioned import binds the requested version or zero without choosing the old version");
  Check(constructors[present ? 1 : 2] == 1 && constructors[4] == 1,
        "valid weak-symbol graph constructs normally");
}

void Unversioned(bool global) {
  // Pinned Android16r4 linker_soinfo.cpp check_symbol_version accepts a
  // provider without VERSYM, or index1 when the requested VERDEF is absent.
  auto* lib = Load(global ? "global" : "unversioned", "libgv_client.so");
  if (!lib) return;
  Check(Call(elf::Lookup(lib, "gv_client_pair")) == (global ? 4444 : 3333),
        "Bionic's unversioned-definition allowance applies to required relocations");
  Check(Call(elf::Lookup(lib, "gv_client_data")) == (global ? 888 : 666),
        "unversioned data definitions use the same version allowance");
  auto* provider = elf::Find("libgv_provider.so");
  Check(Call(elf::LookupVersion(provider, "gv_value", "GV_absent")) == (global ? 44 : 33),
        "explicit version accepts only a truly unversioned or global-index definition");
  if (global) {
    Check(Call(elf::LookupVersion(provider, "gv_tagged", "GV_OTHER")) == 66,
          "versioned provider still matches its tagged definition exactly");
    Check(elf::LookupVersion(provider, "gv_tagged", "GV_absent") == nullptr,
          "the global-index allowance does not erase a tagged definition's version");
    Check(elf::LookupVersion(provider, "gv_value", "GV_OTHER") == nullptr,
          "a known requested definition does not use the global-index fallback");
  }
}

void Virtual() {
  virtual_symbols = true;
  auto options = Options("virtual");
  options.allow_shimmed_needed = true;
  std::string error;
  auto* lib = Load("virtual", "libgv_client.so", options, &error);
  Check(lib == nullptr && !error.empty() && Count() == 0,
        "a symbol callback alone does not invent a required provider identity");
  if (lib) return;
  elf::ProviderOptions provider;
  provider.soname = "libgv_provider.so";
  provider.lookup = [](const char* name) -> void* {
    if (std::strcmp(name, "gv_value") == 0) return reinterpret_cast<void*>(&VirtualValue);
    if (std::strcmp(name, "gv_data") == 0) return &virtual_data;
    return nullptr;
  };
  Check(elf::RegisterProvider(provider, &error) != nullptr, "register actual owned virtual exports");
  lib = Load("virtual", "libgv_client.so", options, &error);
  Check(lib != nullptr, "an explicitly recognized unversioned virtual provider can satisfy its imports");
  if (!lib) return;
  Check(Call(elf::Lookup(lib, "gv_client_pair")) == 5555 &&
        Call(elf::Lookup(lib, "gv_client_data")) == 1110,
        "recognized virtual provider returns actual function and data definitions");
  Check(constructors[3] == 1 && Count() == 1, "virtual provider retry publishes only the real client object");
}

void* CallPointer(void* entry) {
  if (!entry) return nullptr;
#ifdef _WIN32
  if (arm64) entry = gesher::arm64::HostEntry(entry);
#endif
  return entry ? reinterpret_cast<void*(GV_SYSV*)()>(entry)() : nullptr;
}

size_t TlsSize(elf::Library* library, const char* entry_name) {
  const uintptr_t entry = reinterpret_cast<uintptr_t>(elf::Lookup(library, entry_name));
  size_t result = 0;
  elf::ForEachLoaded([&](const elf::PhdrInfo& info) {
    const auto* headers = static_cast<const Elf64_Phdr*>(info.phdr);
    bool belongs = false;
    for (size_t i = 0; i < info.phnum; ++i) {
      const auto& ph = headers[i];
      if (ph.p_type == PT_LOAD && entry >= info.addr + ph.p_vaddr &&
          entry - (info.addr + ph.p_vaddr) < ph.p_memsz) belongs = true;
    }
    if (belongs) for (size_t i = 0; i < info.phnum; ++i)
      if (headers[i].p_type == PT_TLS) result = headers[i].p_memsz;
    return 0;
  });
  return result;
}

void TlsScope() {
  struct Scenario { const char* requester; bool global, preempt, exported, versioned; };
  const Scenario cases[] = {
    {"libgv_scope_requester.so", false, false, true, false},
    {"libgv_scope_requester.so", true, true, true, false},
    {"libgv_scope_protected.so", true, false, true, false},
    {"libgv_scope_hidden.so", true, false, false, false},
    {"libgv_scope_static.so", true, false, false, false},
    {"libgv_scope_symbolic.so", true, false, true, false},
    {"libgv_scope_requester_versioned.so", true, true, true, true},
  };
  for (size_t index = 0; index < std::size(cases); ++index) {
    const Scenario& test = cases[index];
    elf::NamespaceOptions config;
    config.name = "version-tls-scope-" + std::to_string(index);
    config.default_library_paths = {root + "/tls-scope"};
    std::string error;
    auto* ns = elf::CreateNamespace(config, &error);
    Check(ns && elf::LinkNamespaces(ns, elf::DefaultNamespace(), {"libgv_test_host.so"}, &error),
          "TLS scope has a distinct namespace and actual callback provider");
    if (!ns) return;
    auto options = Options("tls-scope"); options.context.name_space = ns;
    const char* name = test.versioned ? "libgv_scope_global_versioned.so" :
        test.global ? "libgv_scope_global.so" : "libgv_scope_local.so";
    auto* provider = Load("tls-scope", name, options, &error);
    auto* requester = Load("tls-scope", test.requester, options, &error);
    std::string ie_name = test.requester;
    ie_name.resize(ie_name.size() - 3);
    ie_name += "_ie.so";
    auto* ie_requester = Load("tls-scope", ie_name.c_str(), options, &error);
    if (!provider || !requester) std::fprintf(stderr, "TLS scope: %s\n", error.c_str());
    Check(provider && requester && ie_requester, "ordinary TLS provider and GD/IE requesters load");
    if (!provider || !requester || !ie_requester) return;
    auto* provider_tls = static_cast<int*>(elf::Lookup(provider, "gv_scope_tls"));
    auto* own_tls = static_cast<int*>(elf::Lookup(requester, "gv_scope_tls"));
    void* getter = elf::Lookup(requester, "gv_scope_tls_address");
    void* reader = elf::Lookup(requester, "gv_scope_tls_read");
    void* advance = elf::Lookup(requester, "gv_scope_tls_advance");
    void* ie = elf::Lookup(ie_requester, "gv_scope_requester_ie_offset");
    auto* selected = static_cast<int*>(CallPointer(getter));
    const int expected = test.preempt ? 41 : 17;
    Check(provider_tls && *provider_tls == 41 && selected && Call(reader) == expected,
          "defined GD/TLSDESC reads selected TLS value, not another symbol's padding");
    if (!provider_tls || !selected) return;
    auto* padding = static_cast<int*>(elf::Lookup(provider, "gv_scope_padding"));
    Check(padding && *padding == 901 && reinterpret_cast<uintptr_t>(provider_tls) ==
          reinterpret_cast<uintptr_t>(padding) + 7 * sizeof(int) &&
          TlsSize(provider, "gv_scope_provider_ie_offset") == 8 * sizeof(int) &&
          TlsSize(requester, "gv_scope_tls_read") == sizeof(int),
          "actual PT_TLS layouts require provider offset28 and requester offset0");
    Check((selected == provider_tls) == test.preempt,
          "selected TLS module and offset identify the same actual provider");
    Check(test.exported ? own_tls && *own_tls == 17 : own_tls == nullptr,
          "explicit handle retains own TLS definition or correctly hides a local symbol");
    Check((CallOffset(ie) == CallOffset(elf::Lookup(provider, "gv_scope_provider_ie_offset"))) == test.preempt,
          "initial-exec relocation agrees with the GD/TLSDESC provider choice");
    Check(Call(advance) == expected + 3 && *selected == expected + 3 &&
          *provider_tls == (test.preempt ? 44 : 41), "real write updates only the selected TLS object");
    if (test.exported) Check(*own_tls == (test.preempt ? 17 : 20),
                            "interposition does not overwrite the requester's separate TLS image");
    if (test.versioned) Check(elf::LookupVersion(provider, "gv_scope_tls", "GV_SCOPE") == provider_tls,
                              "defined versioned TLS resolves through its actual selected version");
    int* worker_address = nullptr;
    bool worker_ok = false;
    std::thread worker([&] {
      auto* worker_provider = static_cast<int*>(elf::Lookup(provider, "gv_scope_tls"));
      worker_address = static_cast<int*>(CallPointer(getter));
      worker_ok = worker_provider && worker_address && worker_address != selected &&
          (worker_address == worker_provider) == test.preempt && Call(reader) == expected &&
          Call(advance) == expected + 3 && *worker_provider == (test.preempt ? 44 : 41);
    });
    worker.join();
    Check(worker_ok && *selected == expected + 3 && *provider_tls == (test.preempt ? 44 : 41),
          "fresh thread keeps selected definitions, initial images and mutations independent");
  }
}

void Tls() {
  auto* lib = Load("tls", "libgv_tls_client.so");
  if (!lib) return;
  auto* first = static_cast<int*>(elf::LookupVersion(lib, "gv_tls", "GV_1"));
  auto* second = static_cast<int*>(elf::LookupVersion(lib, "gv_tls", "GV_2"));
  Check(first && second && first != second && *first == 17 && *second == 29,
        "versioned TLS lookup returns the caller's distinct initialized objects");
  if (!first || !second) return;
  Check(elf::Lookup(lib, "gv_tls") == second && elf::LookupGlobal("gv_tls") == second,
        "ordinary handle and global TLS lookup select this thread's default object");
  Check(elf::LookupGlobalVersion("gv_tls", "GV_1") == first &&
        elf::LookupGlobalVersion("gv_tls", "GV_2") == second,
        "explicit global TLS lookup preserves both version and thread identity");
  Check(elf::LookupVersion(lib, "gv_tls", "GV_missing") == nullptr,
        "a missing TLS version cannot select another thread-local definition");
  void* pair = elf::Lookup(lib, "gv_tls_pair");
  void* advance = elf::Lookup(lib, "gv_tls_advance");
  Check(Call(pair) == 1729, "distinct versioned TLS imports retain their own initialized offsets");
  Check(Call(advance) == 2034, "writes through each TLS version reach independent objects");
  Check(Call(pair) == 2034, "versioned TLS writes persist on the calling thread");
  Check(*first == 20 && *second == 34, "TLS lookup pointers see writes from real ELF relocations");
  *first = 23;
  *second = 39;
  Check(Call(pair) == 2339, "real ELF imports see writes through public versioned TLS pointers");
  *first = 20;
  *second = 34;
  std::array<int, 5> other{};
  int* other_first = nullptr;
  int* other_second = nullptr;
  std::thread worker([&] {
    // Lookup itself must attach/initialize this thread, before any guest call.
    other_first = static_cast<int*>(elf::LookupGlobalVersion("gv_tls", "GV_1"));
    other_second = static_cast<int*>(elf::LookupVersion(lib, "gv_tls", "GV_2"));
    other[4] = other_first && other_second ? 100 * *other_first + *other_second : -1;
    other[0] = Call(pair);
    other[1] = Call(advance);
    other[2] = Call(advance);
    other[3] = Call(pair);
  });
  worker.join();
  Check(other_first && other_second && other_first != first && other_second != second &&
        other_first != other_second && other[4] == 1729,
        "first lookup on another thread creates independent initialized TLS addresses");
  Check(other[0] == 1729, "another thread receives both original versioned TLS images");
  Check(other[1] == 2034 && other[2] == 2339 && other[3] == 2339,
        "another thread can update each selected TLS offset independently");
  Check(Call(pair) == 2034, "other-thread versioned TLS writes preserve the caller's objects");
  Check(elf::LookupVersion(lib, "gv_tls", "GV_1") == first && *first == 20 && *second == 34,
        "repeated TLS lookup retains addresses and never recopies initial values");
  Check(constructors[7] == 1 && constructors[8] == 1 && Count() == 2,
        "versioned TLS access does not reload or reinitialize either DSO");
}
void TlsWeak(bool present) {
  const char* directory = present ? "tls-weak/good" : "tls-weak/bad";
  auto* lib = Load(directory, "libgv_tls_weak_client.so");
  if (!lib) return;
  void* value = elf::Lookup(lib, "gv_tls_weak_value");
  void* advance = elf::Lookup(lib, "gv_tls_weak_advance");
  void* anchor = elf::Lookup(lib, "gv_tls_anchor_value");
  Check(Call(value) == (present ? 29 : -7),
        "weak TLS version binds the matching object or an actual null general-dynamic address");
  Check(Call(advance) == (present ? 34 : -7) && Call(anchor) == 67,
        "weak TLS writes never alias the requester's own TLS module");
  std::array<int, 3> other{};
  std::thread worker([&] {
    other[0] = Call(value);
    other[1] = Call(advance);
    other[2] = Call(anchor);
  });
  worker.join();
  Check(other[0] == (present ? 29 : -7) && other[1] == (present ? 34 : -7) && other[2] == 67,
        "weak versioned TLS has the same binding and independent state on another thread");
  Check(Call(value) == (present ? 34 : -7), "other-thread weak TLS preserves caller state");
  auto* initial_exec = Load(directory, "libgv_tls_ie_client.so");
  if (!initial_exec) return;
  const int64_t optional = CallOffset(elf::Lookup(initial_exec, "gv_tls_ie_optional_offset"));
  const int64_t required = CallOffset(elf::Lookup(initial_exec, "gv_tls_ie_required_offset"));
  Check(required != 0 && required != INT64_MIN, "present initial-exec TLS has a real module offset");
  if (present) {
    void* first = elf::LookupVersion(lib, "gv_tls", "GV_1");
    void* second = elf::LookupVersion(lib, "gv_tls", "GV_2");
    const int64_t distance = static_cast<int64_t>(reinterpret_cast<uintptr_t>(second) -
                                                 reinterpret_cast<uintptr_t>(first));
    Check(first && second && optional != INT64_MIN && optional - required == distance,
          "initial-exec weak relocation selects the requested version's exact offset");
  } else {
    Check(optional == 0, "absent weak initial-exec relocation is zero relative to TP");
  }
  Check(constructors[present ? 7 : 9] == 1 && constructors[10] == 1 && constructors[11] == 1 && Count() == 3,
        "weak TLS imports construct once and preserve the shared provider");
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3 || argc > 4 || (argc == 4 && std::strcmp(argv[3], "--arm64") != 0)) {
    std::fprintf(stderr, "usage: %s <fixture-root> <case> [--arm64]\n", argv[0]);
    return 2;
  }
  Watchdog watchdog;
  root = argv[1];
  arm64 = argc == 4;
  if (arm64) {
#ifdef _WIN32
    Check(gesher::arm64::EnsureLoaded(), "production ARM64 translator is available");
    if (failures) return 1;
#else
    std::fputs("ARM64 execution requires the real Windows translator fixture.\n", stderr);
    return 2;
#endif
  }
  elf::SetBionicResolver(Resolve);
  RegisterCallbacks();
  const std::string test = argv[2];
  if (test == "lookup-gnu" || test == "lookup-sysv") Lookup(argv[2]);
  else if (test == "required") Required();
  else if (test == "weak") Weak(false);
  else if (test == "weak-present") Weak(true);
  else if (test == "unversioned") Unversioned(false);
  else if (test == "global") Unversioned(true);
  else if (test == "virtual") Virtual();
  else if (test == "tls") Tls();
  else if (test == "tls-weak") TlsWeak(false);
  else if (test == "tls-weak-present") TlsWeak(true);
  else if (test == "tls-scope") TlsScope();
  else { std::fprintf(stderr, "unknown ELF version case: %s\n", argv[2]); return 2; }
  std::printf("ELF versions: %s: %d checks, %d failures\n", argv[2], checks, failures);
  if (!failures) std::puts("elf_versions_test all passed");
  return failures ? 1 : 0;
}

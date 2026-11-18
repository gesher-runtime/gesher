// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Production-linked guest libdl/caller identity gate.
// Usage: guest_libdl_namespaces_test <fixture-directory> [--arm64]
// The same driver runs with the documented Linux host boundary.
#include <gesher/elf_loader.h>
#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#if defined(__x86_64__)
#define BIONIC_SYSV __attribute__((sysv_abi))
#else
#define BIONIC_SYSV
#endif

namespace {
std::atomic<int> checks{0};
int BIONIC_SYSV InternalOnly() { return 701; }
int BIONIC_SYSV CoreExport() { return 501; }
int BIONIC_SYSV CoreReplacement() { return 502; }
int BIONIC_SYSV LogExport() { return 601; }
void Check(bool condition, const char* text) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL actual guest libdl: %s (check %d)\n", text, checks.load());
    std::exit(1);
  }
}
// Public Android LP64 android_dlextinfo layout; only USE_NAMESPACE is used.
struct BionicDlextinfo {
  uint64_t flags;
  void* reserved_addr;
  size_t reserved_size;
  int relro_fd;
  int library_fd;
  int64_t library_fd_offset;
  gesher::elf::Namespace* library_namespace;
};
static_assert(sizeof(BionicDlextinfo) == 48);
using Error = const char*(BIONIC_SYSV*)();
using Sym = void*(BIONIC_SYSV*)(void*, const char*);
using VSym = void*(BIONIC_SYSV*)(void*, const char*, const char*);
using ExtOpen = void*(BIONIC_SYSV*)(const char*, int, const BionicDlextinfo*);
Error dlerror_entry;
Sym dlsym_entry;
VSym dlvsym_entry;
ExtOpen extopen_entry;
}  // namespace

namespace {
using namespace gesher::elf;
bool arm64_guest = false;

void* GuestDefault(Library* caller, const char* name, bool next = false) {
  void* entry = Lookup(caller, next ? "ns_next" : "ns_default");
  gesher::bionic::EnterLoadedCode();
  Check(entry != nullptr, "actual default/next call site exists");
  if (arm64_guest) {
    uint64_t args[] = {reinterpret_cast<uint64_t>(name)};
    return reinterpret_cast<void*>(gesher::arm64::Call(entry, args, 1));
  }
  return reinterpret_cast<void*(BIONIC_SYSV*)(const char*)>(entry)(name);
}

void* GuestHandle(Library* caller, void* handle, const char* name) {
  void* entry = Lookup(caller, "ns_handle");
  gesher::bionic::EnterLoadedCode();
  Check(entry != nullptr, "actual handle call site exists");
  if (arm64_guest) {
    uint64_t args[] = {reinterpret_cast<uint64_t>(handle), reinterpret_cast<uint64_t>(name)};
    return reinterpret_cast<void*>(gesher::arm64::Call(entry, args, 2));
  }
  return reinterpret_cast<void*(BIONIC_SYSV*)(void*, const char*)>(entry)(handle, name);
}

void* GuestOpen(Library* caller, const char* name, int flags) {
  void* entry = Lookup(caller, "ns_open");
  gesher::bionic::EnterLoadedCode();
  Check(entry != nullptr, "actual open call site exists");
  if (arm64_guest) {
    uint64_t args[] = {reinterpret_cast<uint64_t>(name), static_cast<uint64_t>(flags)};
    return reinterpret_cast<void*>(gesher::arm64::Call(entry, args, 2));
  }
  return reinterpret_cast<void*(BIONIC_SYSV*)(const char*, int)>(entry)(name, flags);
}

int Value(void* symbol) {
  gesher::bionic::EnterLoadedCode();
  Check(symbol != nullptr, "selected symbol exists");
  if (arm64_guest && gesher::arm64::IsGuestCode(symbol))
    return static_cast<int>(gesher::arm64::Call(symbol, nullptr, 0));
  return reinterpret_cast<int(BIONIC_SYSV*)()>(symbol)();
}

Namespace* MakeNamespace(const std::string& root, const char* side) {
  NamespaceOptions options;
  options.name = side;
  options.isolated = true;
  options.ld_library_paths = {root + "/caller/" + side};
  options.permitted_paths = options.ld_library_paths;
  std::string error;
  Namespace* result = CreateNamespace(options, &error);
  Check(result != nullptr, error.c_str());
  const bool linked = LinkNamespaces(result, DefaultNamespace(), {"libdl.so", "libc.so", "liblog.so"}, &error);
  Check(linked, error.c_str());
  return result;
}

void InstallProviders() {
  using namespace gesher::bionic;
  InstallBionicShim();
  AddBionicSymbol("ns_internal_only", reinterpret_cast<void*>(&InternalOnly));
  AddBionicSymbol("libc.so", "ns_core_export", reinterpret_cast<void*>(&CoreExport));
  AddBionicSymbol("liblog.so", "ns_log_export", reinterpret_cast<void*>(&LogExport));
  AddBionicSymbol("libc.so", "ns_shared_export", reinterpret_cast<void*>(&CoreExport));
  AddBionicSymbol("liblog.so", "ns_shared_export", reinterpret_cast<void*>(&LogExport));
  dlerror_entry = reinterpret_cast<Error>(LookupBionicSymbol("dlerror"));
  dlsym_entry = reinterpret_cast<Sym>(LookupBionicSymbol("dlsym"));
  dlvsym_entry = reinterpret_cast<VSym>(LookupBionicSymbol("dlvsym"));
  extopen_entry = reinterpret_cast<ExtOpen>(LookupBionicSymbol("android_dlopen_ext"));
  Check(dlerror_entry && dlsym_entry && dlvsym_entry && extopen_entry,
        "real public libdl exports are registered");
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--arm64") != 0)) {
    std::fprintf(stderr, "usage: %s <fixture-directory> [--arm64]\n", argv[0]);
    return 2;
  }
  const std::string root = argv[1];
  arm64_guest = argc == 3;
  InstallProviders();
  if (arm64_guest) Check(gesher::arm64::EnsureLoaded(), "production ARM64 translator is available");
  Namespace* a_ns = MakeNamespace(root, "a");
  Namespace* b_ns = MakeNamespace(root, "b");
  LoadOptions options;
  options.context.name_space = a_ns;
  std::string error;
  Library* a = Load((root + "/caller/a/libns_caller_a.so").c_str(), &error, options);
  Check(a != nullptr, error.c_str());
  options.context.name_space = b_ns;
  Library* b = Load((root + "/caller/b/libns_caller_b.so").c_str(), &error, options);
  Check(b != nullptr, error.c_str());
  Check(IsAarch64(a) == arm64_guest && IsAarch64(b) == arm64_guest,
        "loaded caller ELFs have the requested architecture");
  Check(Value(GuestDefault(a, "ns_collision")) == 301, "DEFAULT uses A's actual guest caller");
  Check(Value(GuestDefault(b, "ns_collision")) == 302, "DEFAULT uses B's actual guest caller");
  Check(Value(GuestDefault(a, "ns_collision", true)) == 401, "NEXT starts after A's actual caller");
  Check(Value(GuestDefault(b, "ns_collision", true)) == 402, "NEXT starts after B's actual caller");
  if (arm64_guest) {
    auto via_entry = reinterpret_cast<void*(BIONIC_SYSV*)(const char*)>(
        gesher::arm64::HostEntry(Lookup(a, "ns_default")));
    Check(via_entry != nullptr && Value(via_entry("ns_collision")) == 301,
          "published host entry preserves actual guest caller transport through libdl");
  }
  Check(Value(GuestHandle(a, b, "ns_collision")) == 302, "explicit handle retains B's lookup graph");
  Check(GuestOpen(a, "libns_next_a.so", 2) != nullptr, "guest dlopen inherits A's search paths");
  Check(GuestOpen(b, "libns_next_b.so", 2) != nullptr, "guest dlopen inherits B's search paths");
  Check(GuestOpen(a, "libns_next_b.so", 2) == nullptr, "guest A cannot search B's private directory");
  Check(dlerror_entry() != nullptr && dlerror_entry() == nullptr,
        "namespace load error is consumed once");
  Check(GuestOpen(a, (root + "/caller/b/libns_next_b.so").c_str(), 2) == nullptr,
        "explicit outside path cannot bypass A's isolation");
  dlerror_entry();
  void* dl = GuestOpen(a, "libdl.so", 2);
  Check(dl != nullptr, "actual declared virtual provider opens");
  Check(GuestHandle(a, dl, "ns_internal_only") == nullptr,
        "provider handle does not fall back to the internal aggregate");
  dlerror_entry();
  Check(GuestHandle(a, a, "ns_internal_only") == nullptr,
        "ELF handle does not fall back to the internal aggregate");
  dlerror_entry();
  Check(GuestDefault(a, "ns_internal_only") == nullptr,
        "DEFAULT does not fall back to the internal aggregate");
  dlerror_entry();
  void* log = GuestOpen(a, "liblog.so", 2);
  Check(log != nullptr && Value(GuestHandle(a, log, "ns_core_export")) == 501,
        "provider handle traverses a legitimate transitive dependency");
  Check(GuestHandle(a, dl, "ns_log_export") == nullptr, "libdl cannot reach an unrelated provider");
  dlerror_entry();
  void* core = GuestOpen(a, "libc.so", 2);
#if !defined(GESHER_TEST_HOST_LIBDL_BOUNDARY)
  // Full production bootstrap only. The Linux caller-route harness uses
  // explicit fixture providers; its separate ownership gate executes the
  // actual Registry with source-derived keys and inert function addresses.
  // These names are 32-bit-only in Bionic's actual generated version scripts
  // and must stay internal on both x86_64 and translated AArch64 guest routes.
  for (const char* name : {"atexit", "bcopy", "bsd_signal", "bzero", "gmtime64_r",
                           "localtime64_r", "mktime64", "putw", "sysv_signal", "timegm64", "tkill"}) {
    Check(gesher::bionic::LookupBionicSymbol(name) != nullptr,
          "32-bit-only compatibility entry remains internally available");
    Check(GuestHandle(a, core, name) == nullptr,
          "32-bit-only map entry has no LP64 libc provider ownership");
    Check(dlerror_entry() != nullptr, "missing LP64 provider export owns an error");
  }
  for (const char* name : {"__cxa_atexit", "memcpy", "gmtime_r", "timegm", "tgkill"}) {
    Check(GuestHandle(a, core, name) != nullptr, "valid LP64 libc export remains available");
    Check(dlerror_entry() == nullptr, "valid LP64 export lookup has no error");
  }
#endif
  Check(core != nullptr && Value(GuestHandle(a, core, "ns_shared_export")) == 501 &&
        Value(GuestHandle(a, log, "ns_shared_export")) == 601,
        "a multi-owner export retains each provider's real definition");
  gesher::bionic::AddBionicSymbol("libc.so", "ns_shared_export", reinterpret_cast<void*>(&CoreReplacement));
  Check(Value(GuestHandle(a, core, "ns_shared_export")) == 502 &&
        Value(GuestHandle(a, log, "ns_shared_export")) == 601,
        "late replacement changes the selected owner without overwriting another owner");
  Check(dlsym_entry(reinterpret_cast<void*>(-1), "ns_collision") == nullptr,
        "NEXT without an identified guest caller fails");
  Check(dlerror_entry() != nullptr, "unknown caller owns a useful diagnostic");
  Check(dlvsym_entry(dl, "dlopen", "LIBC") == gesher::bionic::LookupBionicSymbol("dlopen"),
        "N05 unversioned-provider matching follows scoped selection");
  Check(GuestOpen(a, "libdl.so", 0x40000000) == nullptr, "guest flags are validated instead of discarded");
  dlerror_entry();
  BionicDlextinfo ext{};
  ext.flags = 1;
  Check(extopen_entry("libdl.so", 2, &ext) == nullptr,
        "unimplemented extended flags fail instead of being silently ignored");
  dlerror_entry();
  ext.flags = 0x200;
  ext.library_namespace = b_ns;
  Check(extopen_entry("libns_next_b.so", 2, &ext) != nullptr,
        "explicit namespace subset uses the authoritative namespace");
  auto repeated = [](Library* caller, int own, int next) {
    for (int repeat = 0; repeat < 8; ++repeat) {
      Check(Value(GuestDefault(caller, "ns_collision")) == own,
            "concurrent guest DEFAULT keeps this thread's caller namespace");
      Check(Value(GuestDefault(caller, "ns_collision", true)) == next,
            "concurrent guest NEXT keeps this thread's caller position");
    }
  };
  std::thread one(repeated, a, 301, 401);
  std::thread two(repeated, b, 302, 402);
  one.join();
  two.join();
  std::printf("Actual guest libdl namespace contracts: %d checks; architecture=%s\n", checks.load(),
               arm64_guest ? "AArch64 through real Dynarmic" : "x86-64 native ELF");
}

// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real shim exports and real versioned ELF providers, on Windows x86/ARM64.
// Usage: elf_dlvsym_test <fixture-root> [--arm64]
#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif

namespace {
int checks;
std::atomic<int> constructor_calls{0};
void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL elf_dlvsym_test: %s (check %d)\n", description, checks);
    std::exit(1);
  }
}
void SYSV Event(int role) {
  if (role != 1 && role != 7) std::abort();
  ++constructor_calls;
}
struct TlsIndex { size_t module, offset; };
void* SYSV GetTls(TlsIndex* index) {
  return gesher::elf::TlsGetAddr(index->module, index->offset);
}
int Call(void* function, bool arm64) {
  if (!function) return -999;
  gesher::bionic::EnterLoadedCode();
  if (arm64 && gesher::arm64::IsGuestCode(function))
    function = gesher::arm64::HostEntry(function);
  if (!function) return -998;
  return reinterpret_cast<int(SYSV*)()>(function)();
}
bool Contains(const char* value, const char* expected) {
  return value && std::strstr(value, expected) != nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--arm64") != 0)) {
    std::fprintf(stderr, "usage: %s <fixture-root> [--arm64]\n", argv[0]);
    return 2;
  }
  const bool arm64 = argc == 3;
  gesher::bionic::InstallBionicShim();
  gesher::elf::ProviderOptions callbacks;
  callbacks.soname = "libgv_test_host.so";
  callbacks.lookup = [](const char* name) -> void* {
    if (std::strcmp(name, "gv_event") == 0) return reinterpret_cast<void*>(&Event);
    if (std::strcmp(name, "__tls_get_addr") == 0) return reinterpret_cast<void*>(&GetTls);
    return nullptr;
  };
  std::string registration_error;
  Check(gesher::elf::RegisterProvider(callbacks, &registration_error) != nullptr,
        "isolated callback provider has actual owned exports");
  if (arm64) Check(gesher::arm64::EnsureLoaded(), "production ARM64 translator is available");
  using Open = void*(SYSV*)(const char*, int);
  using Sym = void*(SYSV*)(void*, const char*);
  using VSym = void*(SYSV*)(void*, const char*, const char*);
  using Error = const char*(SYSV*)();
  auto open = reinterpret_cast<Open>(gesher::bionic::LookupBionicSymbol("dlopen"));
  auto sym = reinterpret_cast<Sym>(gesher::bionic::LookupBionicSymbol("dlsym"));
  auto vsym = reinterpret_cast<VSym>(gesher::bionic::LookupBionicSymbol("dlvsym"));
  auto error = reinterpret_cast<Error>(gesher::bionic::LookupBionicSymbol("dlerror"));
  Check(open && sym && vsym && error, "actual libdl exports are registered");
  Check(error() == nullptr, "actual error storage starts empty");

  // The later unrelated import needs a true Bionic relocation-global provider.
  const std::string path = std::string(argv[1]) + "/null-global/libgv_provider.so";
  void* handle = open(path.c_str(), 0x102);  // NOW | GLOBAL: this fixture tests DEFAULT.
  if (!handle) {
    const char* detail = error();
    std::fprintf(stderr, "provider load failed: %s\n", detail ? detail : "no diagnostic");
  }
  Check(handle != nullptr, "real version-script provider loads through shim dlopen");
  Check(gesher::elf::IsAarch64(static_cast<gesher::elf::Library*>(handle)) == arm64,
        "loaded provider has the requested architecture");
  Check(constructor_calls == 1, "real provider constructor runs exactly once");
  Check(Call(sym(handle, "gv_value"), arm64) == 22, "ordinary dlsym selects the default version");
  Check(Call(vsym(handle, "gv_value", "GV_1"), arm64) == 11,
        "actual three-argument dlvsym selects the non-default version");
  Check(Call(vsym(handle, "gv_value", "GV_2"), arm64) == 22,
        "actual dlvsym selects the default version explicitly");
  Check(Call(vsym(handle, "gv_value", nullptr), arm64) == 22,
        "null version uses ordinary lookup");
  for (int i = 0; i < 16; ++i) {
    Check(Call(vsym(handle, "gv_value", "GV_1"), arm64) == 11, "repeated GV_1 lookup stays distinct");
    Check(Call(vsym(handle, "gv_value", "GV_2"), arm64) == 22, "repeated GV_2 lookup stays distinct");
  }
  Check(Call(vsym(nullptr, "gv_value", "GV_1"), arm64) == 11,
        "RTLD_DEFAULT preserves the explicit version request");
  auto* data1 = static_cast<int*>(vsym(handle, "gv_data", "GV_1"));
  auto* data2 = static_cast<int*>(vsym(handle, "gv_data", "GV_2"));
  Check(data1 && data2 && data1 != data2 && *data1 == 111 && *data2 == 222,
        "versioned data addresses identify distinct real objects");
  Check(Call(vsym(handle, "gv_ifunc", "GV_1"), arm64) == 71 &&
        Call(vsym(handle, "gv_ifunc", "GV_2"), arm64) == 72,
        "versioned IFUNC exports execute their real resolvers");
  void* resolutions = sym(handle, "gv_null_resolutions");
  Check(resolutions != nullptr, "null IFUNC exposes a real resolver counter");
  for (void* scope : {handle, static_cast<void*>(nullptr)}) {
    for (const char* name : {"gv_null", "getpid"}) {
      const int before = Call(resolutions, arm64);
      if (scope == nullptr && std::strcmp(name, "getpid") == 0) {
        Check(vsym(scope, name, "GV_2") == gesher::bionic::LookupBionicSymbol("getpid") &&
              error() == nullptr, "DEFAULT retains the earlier actual unversioned libc export");
        Check(Call(resolutions, arm64) == before,
              "an unselected later ELF IFUNC is not executed");
        continue;
      }
      Check(vsym(scope, name, "GV_2") == nullptr && error() == nullptr,
            "successful null IFUNC neither reports an error nor becomes a builtin");
      Check(Call(resolutions, arm64) == before + 1,
            "actual versioned null resolution executes the resolver once");
      Check(sym(scope, name) == nullptr && error() == nullptr,
            "ordinary lookup also preserves a selected null result");
      Check(vsym(scope, "gv_value", "GV_pending_null") == nullptr,
            "create a pending error before null IFUNC success");
      Check(vsym(scope, name, "GV_2") == nullptr && Contains(error(), "GV_pending_null"),
            "successful null IFUNC preserves an earlier unconsumed error");
      Check(error() == nullptr, "preserved error is consumed once");
    }
  }
  const int before_import = Call(resolutions, arm64);
  const std::string null_import_path = std::string(argv[1]) + "/libgv_null_import.so";
  void* null_import = open(null_import_path.c_str(), 2);
  Check(null_import != nullptr, "global null IFUNC relocation is a successful binding");
  void* import_entry = sym(null_import, "gv_null_import");
  Check(import_entry != nullptr, "global null import fixture exposes its actual result");
  gesher::bionic::EnterLoadedCode();
  if (arm64) import_entry = gesher::arm64::HostEntry(import_entry);
  Check(import_entry && reinterpret_cast<void*(SYSV*)()>(import_entry)() == nullptr,
        "global relocation retains null instead of the same-name builtin");
  Check(Call(resolutions, arm64) == before_import + 1,
        "global null relocation invokes its IFUNC once");
  Check(sym(handle, "gv_hidden") == nullptr, "ordinary lookup excludes a hidden-only definition");
  Check(Contains(error(), "gv_hidden") && error() == nullptr, "hidden-only failure is consumed once");
  Check(Call(vsym(handle, "gv_hidden", "GV_1"), arm64) == 31,
        "explicit lookup reaches the hidden-only definition");

  char requested_version[] = "GV_missing";
  Check(vsym(handle, "gv_value", requested_version) == nullptr,
        "an existing name does not silently satisfy an unavailable version");
  requested_version[0] = 'X';
  const char* retained = error();
  Check(Contains(retained, "gv_value") && Contains(retained, "GV_missing"),
        "error storage owns the requested version after caller mutation");
  const std::string retained_copy = retained;
  Check(error() == nullptr && retained_copy == retained, "dlerror is consumed once without invalidating its storage");
  Check(Call(sym(handle, "gv_value"), arm64) == 22 && retained_copy == retained && error() == nullptr,
        "successful ordinary lookup preserves prior error storage");
  Check(vsym(handle, "gv_value", "GV_pending") == nullptr, "create an unconsumed version error");
  Check(Call(vsym(handle, "gv_value", "GV_1"), arm64) == 11 && Contains(error(), "GV_pending"),
        "success does not erase an unconsumed error");
  Check(vsym(handle, nullptr, "GV_1") == nullptr && Contains(error(), "null symbol"),
        "null symbol fails through the real shim");

  void* libc = open("libc.so", 2);
  Check(libc != nullptr, "existing virtual libc provider is available");
  void* malloc_entry = sym(libc, "malloc");
  Check(malloc_entry && vsym(libc, "malloc", "GV_unversioned_request") == malloc_entry,
        "actual unversioned virtual export follows Bionic's no-version-table allowance");
  Check(vsym(libc, "gv_absent_virtual_export", "GV_unversioned_request") == nullptr &&
        Contains(error(), "GV_unversioned_request"), "virtual provider cannot fabricate a missing export");

  Check(vsym(handle, "gv_value", "GV_main_missing") == nullptr, "retain main-thread version error");
  bool worker_empty = false, worker_once = false;
  int worker_value = -1;
  std::string worker_error;
  std::thread worker([&] {
    worker_empty = error() == nullptr;
    vsym(handle, "gv_value", "GV_worker_missing");
    if (const char* value = error()) worker_error = value;
    worker_once = error() == nullptr;
    worker_value = Call(vsym(handle, "gv_value", "GV_1"), arm64);
  });
  worker.join();
  Check(worker_empty && worker_once && worker_value == 11 &&
        worker_error.find("GV_worker_missing") != std::string::npos,
        "worker uses actual exports with independent error storage");
  Check(Contains(error(), "GV_main_missing") && error() == nullptr,
        "worker errors do not replace the main-thread error");
  Check(constructor_calls == 1, "lookups do not rerun provider constructors");
  const std::string tls_path = std::string(argv[1]) + "/tls/libgv_tls_provider.so";
  void* tls_handle = open(tls_path.c_str(), 0x102);
  Check(tls_handle != nullptr, "real versioned TLS provider loads through shim dlopen");
  auto* tls1 = static_cast<int*>(vsym(tls_handle, "gv_tls", "GV_1"));
  auto* tls2 = static_cast<int*>(vsym(tls_handle, "gv_tls", "GV_2"));
  Check(tls1 && tls2 && tls1 != tls2 && *tls1 == 17 && *tls2 == 29,
        "actual dlvsym returns distinct current-thread TLS objects");
  Check(sym(tls_handle, "gv_tls") == tls2 && vsym(nullptr, "gv_tls", "GV_1") == tls1,
        "default and global shim lookups preserve TLS identity");
  *tls1 = 101;
  *tls2 = 202;
  int* other_tls1 = nullptr;
  int* other_tls2 = nullptr;
  bool other_tls_initial = false;
  std::thread tls_worker([&] {
    other_tls1 = static_cast<int*>(vsym(tls_handle, "gv_tls", "GV_1"));
    other_tls2 = static_cast<int*>(vsym(tls_handle, "gv_tls", "GV_2"));
    other_tls_initial = other_tls1 && other_tls2 && *other_tls1 == 17 && *other_tls2 == 29;
    if (other_tls_initial) { *other_tls1 = 303; *other_tls2 = 404; }
  });
  tls_worker.join();
  Check(other_tls_initial && other_tls1 != tls1 && other_tls2 != tls2 && other_tls1 != other_tls2,
        "actual dlvsym allocates independent TLS on first lookup in another thread");
  Check(vsym(tls_handle, "gv_tls", "GV_1") == tls1 && *tls1 == 101 && *tls2 == 202,
        "worker lookup and mutation preserve the caller's TLS values");
  Check(vsym(tls_handle, "gv_tls", "GV_missing") == nullptr && Contains(error(), "GV_missing"),
        "missing TLS version reports the requested version through actual dlerror");
  Check(constructor_calls == 2, "TLS lookups do not reload either real provider");
  if (arm64) {
    const std::string guest_path = std::string(argv[1]) + "/libgv_guest_dlvsym.so";
    void* guest = open(guest_path.c_str(), 2);
    Check(guest != nullptr, "real guest libdl caller loads through shim dlopen");
    void* entry = sym(guest, "gv_guest_dlvsym");
    Check(entry != nullptr, "real guest libdl caller exports its test entry");
    gesher::bionic::EnterLoadedCode();
    entry = gesher::arm64::HostEntry(entry);
    Check(entry != nullptr, "real translator provides a host entry for the guest caller");
    auto run_guest = reinterpret_cast<int(SYSV*)(void*, void*)>(entry);
    const int guest_result = run_guest(handle, guest);
    if (guest_result != 0) std::fprintf(stderr, "guest dlvsym contract failed at case %d\n", guest_result);
    Check(guest_result == 0,
          "guest dlvsym preserves real versions, ARM overrides, ELF definitions and errors");
  }
  std::printf("elf_dlvsym_test: %d checks passed\n", checks);
  std::puts("elf_dlvsym_test all passed");
}

// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Full arm64_runtime.cc test with an explicit DLL discovery/API boundary.
#include <gesher/arm64_runtime.h>
#include <gesher/arm64_translator.h>
#include <gesher/elf_loader.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::string mode;
int checks = 0, calls = 0, initializations = 0;
unsigned char storage[16]{};
void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL ARM TLS API: %s: %s\n", mode.c_str(), label);
    std::abort();
  }
}
void* Address(int64_t offset, size_t symbol) {
  ++calls;
  if (offset == 96 && symbol < sizeof(storage)) return storage + symbol;
  return nullptr;
}
void* WeakDescriptor() { ++calls; return storage; }
void* Bind(void* pointer) { return pointer; }
gesher_arm64_api api{};
// A genuinely short older table: sanitizer redzones detect access to the new
// fields before the production front end has rejected the version prefix.
struct LegacyPrefix { uint32_t version; } legacy{4};
const gesher_arm64_api* Init(const gesher_arm64_host* host) {
  ++initializations;
  Check(host && host->version == 5 && host->resolve && host->describe,
        "host advertises API5 before requesting current-thread lookup");
  if (mode == "rejected") return nullptr;
  if (mode == "old-prefix") return reinterpret_cast<const gesher_arm64_api*>(&legacy);
  api.version = GESHER_ARM64_API_VERSION;
  api.tls_address = mode == "missing-address" ? nullptr : Address;
  api.tlsdesc_unresolved_weak = mode == "missing-address" ? nullptr : WeakDescriptor;
  return &api;
}
}  // namespace

HMODULE LoadLibraryA(const char* name) {
  Check(std::strcmp(name, GESHER_ARM64_DLL) == 0, "actual requested DLL name");
  return mode == "missing-dll" ? nullptr : storage;
}
FARPROC GetProcAddress(HMODULE module, const char* name) {
  Check(module == storage, "entry point queried on discovered DLL");
  if (std::strcmp(name, GESHER_ARM64_INIT_SYMBOL) == 0)
    return mode == "missing-init" ? nullptr : reinterpret_cast<FARPROC>(&Init);
  if (std::strcmp(name, "gesher_arm64_bind") == 0) return reinterpret_cast<FARPROC>(&Bind);
  std::abort();
}
namespace gesher::arm64 {
bool InitDynarmicBridge(const gesher_arm64_host*, const gesher_arm64_api**, void* (**)(void*)) {
  return false;  // This fixture selects the production DLL front-end path.
}
}
namespace gesher::elf {
void* ResolveBionic(const char*) { return nullptr; }
void* LookupGlobal(const char*) { return nullptr; }
bool FindAddress(const void*, AddressInfo*) { return false; }
}

int main(int argc, char** argv) {
  Check(argc == 2, "one isolated initialization case");
  mode = argv[1];
  Check(gesher::arm64::TlsAddress(96, 4) == nullptr && calls == 0,
        "uninitialized runtime exposes no TLS address");
  Check(gesher::arm64::TlsDescUnresolvedWeak() == nullptr && calls == 0,
        "uninitialized runtime exposes no weak descriptor");
  const bool initialized = mode == "current" || mode == "missing-address";
  Check(gesher::arm64::EnsureLoaded() == initialized, "initialization preserves rejection");
  Check(gesher::arm64::Available() == initialized, "availability matches initialization");
  const int before = initializations;
  Check(gesher::arm64::EnsureLoaded() == initialized && initializations == before,
        "initialization remains one-time");
  if (mode == "current") {
    auto* pointer = static_cast<unsigned char*>(gesher::arm64::TlsAddress(96, 4));
    Check(pointer == storage + 4 && calls == 1, "module and symbol offsets forwarded once");
    *pointer = 93;
    Check(gesher::arm64::TlsAddress(96, 4) == pointer && *pointer == 93 && calls == 2,
          "returned current-thread pointer is preserved without a copy");
    Check(gesher::arm64::TlsAddress(96, SIZE_MAX) == nullptr && calls == 3,
          "backend's unavailable offset propagates unchanged");
    Check(gesher::arm64::TlsAddress(-1, 0) == nullptr && calls == 4,
          "backend's unknown module propagates unchanged");
    Check(gesher::arm64::TlsDescUnresolvedWeak() == storage && calls == 5,
          "actual weak descriptor address forwarded once");
  } else {
    Check(gesher::arm64::TlsAddress(96, 4) == nullptr && calls == 0,
          "incompatible or absent address field is never called");
    Check(gesher::arm64::TlsDescUnresolvedWeak() == nullptr && calls == 0,
          "incompatible or absent descriptor field is never called");
  }
  std::printf("ARM TLS API: %s: %d checks passed\n", mode.c_str(), checks);
}

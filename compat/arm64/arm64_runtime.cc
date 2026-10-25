// gesher: the loader-side front end to gesher_arm64.dll (arm64_runtime.h,
// docs/arm64.md, ADR 0047). Loads the translator DLL and forwards to its
// gesher_arm64_api; provides the host callbacks it needs (symbol lookup and
// address description) over the ELF loader.

#include <gesher/arm64_runtime.h>

#include <gesher/arm64_translator.h>
#include <gesher/elf_loader.h>

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace gesher::arm64 {

namespace {

const gesher_arm64_api* g_api = nullptr;
void* (*g_bind)(void*) = nullptr;
std::once_flag g_once;
bool g_ok = false;

// Host callback: the host (bionic shim) definition of a libc symbol, which the
// translator wraps or calls (guest_libc.cc); else any loaded object's.
void* HostResolve(const char* name) {
  if (void* a = gesher::elf::ResolveBionic(name)) return a;
  return gesher::elf::LookupGlobal(name);
}

// Host callback: describe an address for a guest crash report.
int HostDescribe(const void* addr, const char** object, const void** object_base,
                 const char** symbol, const void** symbol_addr) {
  gesher::elf::AddressInfo info;
  if (!gesher::elf::FindAddress(addr, &info)) return 0;
  if (object) *object = info.soname;
  if (object_base) *object_base = info.base;
  if (symbol) *symbol = info.symbol;
  if (symbol_addr) *symbol_addr = info.symbol_addr;
  return 1;
}

void Load() {
  HMODULE dll = LoadLibraryA(GESHER_ARM64_DLL);
  if (dll == nullptr) {
    std::fprintf(stderr, "gesher arm64: %s not found; arm64 libraries cannot be loaded\n",
                 GESHER_ARM64_DLL);
    return;
  }
  auto init = reinterpret_cast<gesher_arm64_init_fn>(
      reinterpret_cast<void*>(GetProcAddress(dll, GESHER_ARM64_INIT_SYMBOL)));
  g_bind = reinterpret_cast<void* (*)(void*)>(
      reinterpret_cast<void*>(GetProcAddress(dll, "gesher_arm64_bind")));
  if (init == nullptr || g_bind == nullptr) {
    std::fprintf(stderr, "gesher arm64: %s lacks its entry points\n", GESHER_ARM64_DLL);
    return;
  }
  static gesher_arm64_host host = {GESHER_ARM64_API_VERSION, HostResolve, HostDescribe};
  g_api = init(&host);
  if (g_api == nullptr || g_api->version != GESHER_ARM64_API_VERSION) {
    std::fprintf(stderr, "gesher arm64: %s version mismatch\n", GESHER_ARM64_DLL);
    g_api = nullptr;
    return;
  }
  g_ok = true;
}

}  // namespace

bool EnsureLoaded() {
  std::call_once(g_once, Load);
  return g_ok;
}

bool Available() { return g_ok; }

void AddCode(const void* start, size_t size, const char* name) {
  if (g_api) g_api->add_code(start, size, name);
}

void* BindHostFunction(void* host_fn) { return g_bind ? g_bind(host_fn) : nullptr; }

void* HostEntry(const void* fn) { return g_api ? g_api->host_entry(fn) : nullptr; }

uint64_t Call(const void* fn, const uint64_t* args, size_t nargs) {
  return g_api ? g_api->call(fn, args, nargs) : 0;
}

int64_t TlsRegister(size_t total_size, size_t align) {
  return g_api ? g_api->tls_register(total_size, align) : -1;
}

void TlsPublish(int64_t offset, const void* image, size_t image_size, size_t total_size) {
  if (g_api) g_api->tls_publish(offset, image, image_size, total_size);
}

void AddVaListFunction(const void* fn, int index) {
  if (g_api) g_api->add_va_list_function(fn, index);
}

bool IsGuestCode(const void* addr) { return g_api && g_api->is_guest_code(addr); }

void* GuestSymbol(const char* name) { return g_api ? g_api->guest_symbol(name) : nullptr; }

void* TlsDescStatic() { return g_api ? g_api->tlsdesc_static() : nullptr; }

uint64_t CallIfuncResolver(const void* resolver) {
  // bionic's arm64 IFUNC resolvers take (hwcap | _IFUNC_ARG_HWCAP, &arg), with
  // the capabilities the translator implements (getauxval's AT_HWCAP).
  struct IfuncArg {
    uint64_t size;
    uint64_t hwcap;
    uint64_t hwcap2;
  };
  static IfuncArg arg = {sizeof(IfuncArg), kGuestHwcap, 0};
  uint64_t args[2] = {kGuestHwcap | (1ull << 62), reinterpret_cast<uint64_t>(&arg)};
  return Call(resolver, args, 2);
}

}  // namespace gesher::arm64

// gesher: the loader-side front end to the arm64 translator DLL
// (gesher_arm64.dll, docs/arm64.md, ADR 0047). The ELF loader (compat/elf)
// calls these when it meets an EM_AARCH64 object; they LoadLibrary the DLL on
// first use and forward to its gesher_arm64_api. If the DLL is missing, Available
// is false and loading an arm64 object fails with a clear message.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gesher::arm64 {

// Loads gesher_arm64.dll (once) and exchanges the interface tables. Returns
// false if the DLL or its entry point is missing, or the versions disagree.
bool EnsureLoaded();

// True once EnsureLoaded() has succeeded.
bool Available();

// Registers a mapped guest executable segment [start, start+size).
void AddCode(const void* start, size_t size, const char* name);

// Binds host function `host_fn` (System V) to a guest trampoline: the address
// returned is guest code the guest can call, which marshals into `host_fn`.
// Used for a guest import that resolves to a host (shim) symbol.
void* BindHostFunction(void* host_fn);

// A System V x86-64 entry point for guest function `fn` (host -> guest): a stub
// host code calls as an ordinary System V function. For init/fini arrays, IFUNC
// resolvers, the JNI natives and callbacks.
void* HostEntry(const void* fn);

// Calls guest function `fn` with up to 8 integer arguments on this thread;
// returns X0. For init functions and IFUNC resolvers during loading.
uint64_t Call(const void* fn, const uint64_t* args, size_t nargs);

// Guest TLS (variant I). Registers a PT_TLS module of `total_size`/`align` and
// returns its offset from the thread pointer, or -1 if the static area is full.
int64_t TlsRegister(size_t total_size, size_t align);
// Publishes a module's initialisation image into every thread's block.
void TlsPublish(int64_t offset, const void* image, size_t image_size, size_t total_size);

// Marks host function `fn` as taking a va_list at argument `index`, so the
// guest's AAPCS64 va_list is converted when it calls it (the shim's vsnprintf,
// the JNI bridge's Call*MethodV).
void AddVaListFunction(const void* fn, int index);

// Is `addr` inside a registered guest code region?
bool IsGuestCode(const void* addr);

}  // namespace gesher::arm64

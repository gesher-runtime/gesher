// gesher: the interface between the ELF loader (compat/elf, in dalvikvm and
// the test programs) and the arm64 translator, gesher_arm64.dll
// (compat/arm64, docs/arm64.md, docs/decisions/0047-arm64-translation.md).
//
// The loader maps an EM_AARCH64 object itself, as it maps x86-64 ones, but
// its executable segments are mapped readable and NOT executable: the host
// never runs guest code. The translator (dynarmic's A64 frontend and x86-64
// backend) runs it:
//
//  - Host -> guest. Host code calls guest function pointers as System V
//    functions (init arrays, JNI natives through the JNI bridge, pthread start
//    routines, qsort comparators, NativeActivity callbacks...). Such a call
//    faults with DEP on the non-executable guest page; the translator's
//    vectored exception handler sees an execute fault at an address in a
//    registered guest code region and redirects the thread into the guest:
//    the System V argument registers and stack become AAPCS64 ones, the
//    guest runs on its own stack until it returns to a marker address, and
//    its X0/X1/V0/V1 become RAX/RDX/XMM0/XMM1 for the caller. `host_entry`
//    gives an explicit x86-64 entry stub for a guest function, which avoids
//    the fault.
//  - Guest -> host. Guest imports are bound straight to the host's System V
//    functions (the bionic shim, libandroid, EGL/GLES, the JNI proxies'
//    tables). When guest code branches to an address outside every guest
//    code region, the translator leaves the guest and calls it with the
//    guest's X0-X7/V0-V7 and stack arguments mapped to System V, then
//    returns to the guest's X30. A few functions get guest-specific versions
//    (`guest_symbol`): setjmp/longjmp as guest code, getauxval with arm64
//    capabilities, syscall with arm64 numbers, the TLS descriptors.
//
// Everything here uses the default (Microsoft x64) calling convention.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GESHER_ARM64_API_VERSION 1

// What the host gives the translator.
struct gesher_arm64_host {
  uint32_t version;  // GESHER_ARM64_API_VERSION
  // Resolves a symbol the way the loader does for a guest import that no
  // guest object defines (the bionic shim): nullptr if unknown.
  void* (*resolve)(const char* name);
  // For crash reports: names the loaded object (and the closest exported
  // symbol) holding `addr`. Returns 0 if `addr` is in no loaded object.
  int (*describe)(const void* addr, const char** object, const void** object_base,
                  const char** symbol, const void** symbol_addr);
};

// What the translator offers the host.
struct gesher_arm64_api {
  uint32_t version;  // GESHER_ARM64_API_VERSION

  // A mapped executable segment of a guest object: [start, start + size).
  // Branches into it run as guest code; `name` is kept for diagnostics.
  void (*add_code)(const void* start, size_t size, const char* name);

  // Calls guest function `fn` with up to 8 integer arguments (X0..X7) on
  // the calling thread; returns X0. For init functions and IFUNC resolvers.
  uint64_t (*call)(const void* fn, const uint64_t* args, size_t nargs);

  // A System V x86-64 entry point for guest function `fn` (an explicit form
  // of the DEP redirection; one stub per function, never freed).
  void* (*host_entry)(const void* fn);

  // The guest-specific definition of import `name`, or nullptr if the
  // guest uses the host's (the bionic shim's) definition.
  void* (*guest_symbol)(const char* name);

  // A host function whose argument `index` (0-based) is a va_list: the
  // guest's AAPCS64 va_list is converted to a System V one when the guest
  // calls it. The translator knows the shim's (vsnprintf...) by name; the
  // JNI bridge registers its Call*MethodV forms.
  void (*add_va_list_function)(const void* fn, int index);

  // Guest TLS (variant I, TPIDR_EL0). Registers a PT_TLS module and gives
  // its offset from the thread pointer, the same in every thread; -1 if the
  // static TLS area is full. The image is copied into every thread's block
  // when it is published (after the module's relocations).
  int64_t (*tls_register)(size_t total_size, size_t align);
  void (*tls_publish)(int64_t offset, const void* image, size_t image_size, size_t total_size);
  // The guest address of the TLSDESC resolver for static TLS: the
  // descriptor's second word is the offset from the thread pointer.
  void* (*tlsdesc_static)(void);

  // Is `addr` inside a registered guest code region?
  int (*is_guest_code)(const void* addr);
};

// The DLL's one export.
typedef const struct gesher_arm64_api* (*gesher_arm64_init_fn)(const struct gesher_arm64_host*);
#define GESHER_ARM64_INIT_SYMBOL "gesher_arm64_init"
#define GESHER_ARM64_DLL "gesher_arm64.dll"

#ifdef __cplusplus
}
#endif

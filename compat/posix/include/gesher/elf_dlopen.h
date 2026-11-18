// gesher-base: dlopen of ELF shared objects (an Android app's .so files).
//
// Windows' loader takes PE files only. When dlopen is given a file that starts
// with the ELF magic, it hands it to the loader registered here (Gesher's ELF
// loader, compat/elf, with the JNI bridge in compat/bionic/jni_bridge.cc) and
// keeps the handle it returns, so dlsym and dlclose on that handle go to the
// same loader. This is how ART's host libnativeloader path
// (OpenNativeLibrary's plain dlopen, then dlsym for JNI_OnLoad and Java_*)
// reaches an app's .so (docs/native-libs.md).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gesher_elf_dlopen_ops {
  // Loads the ELF object at `path`; returns a handle, or NULL with a message
  // written to error[0..error_size).
  void* (*open)(const char* path, int flags, char* error, size_t error_size);
  // dlsym on a handle `open` returned; NULL if the symbol is not there.
  void* (*sym)(void* handle, const char* symbol);
  // dlclose on such a handle; 0 on success.
  int (*close)(void* handle);
};

// Registers the loader dlopen hands ELF files to. `ops` must stay valid; call
// it before the first ELF dlopen (a static initializer will do).
void gesher_set_elf_dlopen(const struct gesher_elf_dlopen_ops* ops);

// Contextual Android loading is separate from the host PE/builtin route. A
// classloader namespace is owned by the common loader and remains stable for
// the lifetime of its retained libraries. No caller pointer is synthesized.
struct gesher_elf_load_context {
  unsigned version;  // 1
  unsigned size;     // sizeof(struct gesher_elf_load_context)
  void* name_space;
  const void* caller_address;
  unsigned architecture;  // 0 auto, 1 x86_64, 2 AArch64
};

struct gesher_elf_namespace_ops {
  unsigned version;  // 1
  unsigned size;     // sizeof(struct gesher_elf_namespace_ops)
  void* (*open)(const char* path, int flags, const struct gesher_elf_load_context* context,
                char* error, size_t error_size);
};

// Returns 0 for a supported table, -1 with dlerror() on an invalid version or
// size. The registered table must remain valid. Does not replace old sym/close
// hooks, which continue to adapt JNI symbols for handles opened by either path.
int gesher_set_elf_namespace_ops(const struct gesher_elf_namespace_ops* ops);

// Loads only through the contextual ELF/provider callback; never falls back
// to PE or to a host builtin matched by basename. dlerror owns failure text.
void* gesher_dlopen_in_namespace(const char* path, int flags,
                                const struct gesher_elf_load_context* context);

// Explicit host route for ART boot/framework builtins and Windows PE modules.
// Ordinary application native loads use gesher_dlopen_in_namespace instead.
void* gesher_dlopen_host(const char* path, int flags);

#ifdef __cplusplus
}
#endif

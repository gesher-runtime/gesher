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

#ifdef __cplusplus
}
#endif

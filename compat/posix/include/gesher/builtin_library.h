// gesher-base: shared libraries linked into the executable.
//
// Android loads libraries such as libjavacore.so with dlopen and finds their
// JNI_OnLoad with dlsym. Gesher links them in instead and registers each
// under its file name: host dlopen of that name or a registered runtime alias
// returns a handle for it, and dlsym on the handle looks only in its symbol
// table. Several libraries can each have a JNI_OnLoad this way, under their
// own C names. Natives found by name (Java_*) must be in the table too:
// JNIEXPORT exports nothing in Gesher (libnativehelper patch 0001).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gesher_builtin_symbol {
  const char* name;
  void* address;
};

// Registers `name` (e.g. "libjavacore.so"). `symbols` must stay valid; call
// before the library is first opened, e.g. from a static initializer.
void gesher_register_builtin_library(const char* name, const struct gesher_builtin_symbol* symbols,
                                     size_t count);

// Adds an exact trusted runtime path for an already registered builtin.
// Bare names, /system/lib64/name and ANDROID_ROOT/lib64/name are recognized
// by the host route. Arbitrary directories never acquire a builtin identity.
// Returns 0 on success or -1 with dlerror() if the library is not registered.
int gesher_register_builtin_library_alias(const char* name, const char* path);

#ifdef __cplusplus
}
#endif

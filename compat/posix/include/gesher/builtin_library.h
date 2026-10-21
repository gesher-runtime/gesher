// gesher-base: shared libraries linked into the executable.
//
// Android loads libraries such as libjavacore.so with dlopen and finds their
// JNI_OnLoad with dlsym. Gesher links them in instead and registers each
// under its file name: dlopen of that name, with or without a directory,
// returns a handle for it, and dlsym on the handle looks in its symbol table,
// then, for native method names (Java_*), in the executable's exported
// symbols (JNIEXPORT functions). Several libraries can each have a
// JNI_OnLoad this way, under their own C names.
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

#ifdef __cplusplus
}
#endif

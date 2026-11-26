#pragma once

#include "shim_internal.h"

// The production key implementation is built in a separate translation unit,
// without LTO. These declarations retain the actual guest calling convention.
namespace gesher::bionic {
extern "C" {
int BIONIC_SYSV gesher_bionic_pthread_key_create(
    bionic_pthread_key_t*, void (BIONIC_SYSV*)(void*));
int BIONIC_SYSV gesher_bionic_pthread_key_delete(bionic_pthread_key_t);
void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t);
int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t, const void*);
}
// Fixture observations only; never linked into the runtime.
uint64_t KeyMeasureRegistryLocks();
size_t KeyMeasureGlobalArrays();
size_t KeyMeasureLocalArrays();
bool KeyMeasureInstrumented();
}

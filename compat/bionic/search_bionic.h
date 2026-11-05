// The bionic search-tree API. Callbacks, as well as the entry points, use
// the loaded library's System V ABI (AAPCS64 callers use the translator).
#pragma once

#include "abi_types.h"

namespace gesher::bionic::search {

using Compare = int (BIONIC_SYSV *)(const void*, const void*);
using Visit = void (BIONIC_SYSV *)(const void*, int, int);
using Destroy = void (BIONIC_SYSV *)(void*);

void* BIONIC_SYSV Tsearch(const void* key, void** root, Compare compare);
void* BIONIC_SYSV Tfind(const void* key, void* const* root, Compare compare);
void* BIONIC_SYSV Tdelete(const void* key, void** root, Compare compare);
void BIONIC_SYSV Twalk(const void* root, Visit action);
void BIONIC_SYSV Tdestroy(void* root, Destroy free_key);

}  // namespace gesher::bionic::search

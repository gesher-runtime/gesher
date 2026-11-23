// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Bionic builds gdtoa with MULTIPLE_THREADS and maps its two dtoa locks to
// pthread mutexes. gdtoaimp.h expands ACQUIRE_DTOA_LOCK(n) and
// FREE_DTOA_LOCK(n) to _MUTEX_LOCK/_MUTEX_UNLOCK(&__dtoa_locks[n]), only in
// misc.c where that array is defined; pass the lock number to the hooks in
// binary128_parse.cc. Lock 0 guards the Bigint free list and private arena;
// lock 1 guards growth of the shared powers-of-five cache.
void gesher_gdtoa_lock(int lock);
void gesher_gdtoa_unlock(int lock);
#define _MUTEX_LOCK(l) gesher_gdtoa_lock((int)((l) - __dtoa_locks))
#define _MUTEX_UNLOCK(l) gesher_gdtoa_unlock((int)((l) - __dtoa_locks))

/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* JNI identity/allocation/write-size boundaries. Real manager, services and
 * framework Java; deterministic clocks/executor. No host Binder transport. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
/* Sync host checks do not use notification file-descriptor reads. Keep this
 * unrelated JNI boundary explicitly unsupported on the Windows host JVM. */
static ssize_t sync_unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread sync_unsupported_pread
#endif
#include "../scheduler/framework_host.c"

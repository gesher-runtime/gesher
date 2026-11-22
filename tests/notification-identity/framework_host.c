/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing log/identity/array/clock native boundaries. No native Parcel transport. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
static ssize_t identity_unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread identity_unsupported_pread
#endif
#include "../notification/framework_host.c"

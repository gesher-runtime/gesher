/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing host identity/log/clock boundaries only; no UI or UIA emulation. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
static ssize_t uia_unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread uia_unsupported_pread
#endif
#include "../notification/framework_host.c"

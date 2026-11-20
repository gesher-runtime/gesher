/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
/* The unrelated notification FD read boundary is not exercised by these tests. */
static ssize_t unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS; return -1;
}
#define pread unsupported_pread
#endif
#include "../sync/framework_host.c"

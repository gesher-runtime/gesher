/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* The cookie fixture supplies real Handler/Looper queue waits and host clocks.
 * Unrelated notification descriptor reads remain unsupported on Windows. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
static ssize_t geolocation_unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread geolocation_unsupported_pread
#endif
#include "../cookie-callbacks/framework_host.c"

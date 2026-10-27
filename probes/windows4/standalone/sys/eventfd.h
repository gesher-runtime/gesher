// Only the standalone CI window/COM harness includes this file. It exercises
// the production HWND, COM source and condition-variable event queue without
// building AOSP-dependent gesher-base. Full ci.yml uses real base/eventfd.
#pragma once
#include <stdint.h>
#include <errno.h>
typedef uint64_t eventfd_t;
#define EFD_NONBLOCK 04000
#define EFD_CLOEXEC 02000000
inline int eventfd(unsigned int, int) { errno = ENOSYS; return -1; }
inline int eventfd_write(int, eventfd_t) { errno = ENOSYS; return -1; }
inline int eventfd_read(int, eventfd_t*) { errno = ENOSYS; return -1; }

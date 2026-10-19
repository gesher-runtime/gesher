// gesher-base: POSIX additions to <limits.h>.
#pragma once

#include_next <limits.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

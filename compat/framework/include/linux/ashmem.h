#pragma once

// Return codes used by the framework's ashmem clients and libcutils' host
// backend. No ashmem device ioctls exist on Windows.
#define ASHMEM_NOT_PURGED 0
#define ASHMEM_WAS_PURGED 1
#define ASHMEM_IS_UNPINNED 0
#define ASHMEM_IS_PINNED 1

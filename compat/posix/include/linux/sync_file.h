// gesher-base: <linux/sync_file.h>, the sync_file ioctl interface (Linux
// UAPI) that libsync's sync_file_info uses on Android native fence fds.
// gesher-base implements SYNC_IOC_FILE_INFO for its native fences
// (native_fence.cc); SYNC_IOC_MERGE is not implemented and fails with ENOTTY
// (libsync's sync_merge then fails).
#pragma once

#include <linux/ioctl.h>
#include <stdint.h>

struct sync_merge_data {
  char name[32];
  int32_t fd2;
  int32_t fence;
  uint32_t flags;
  uint32_t pad;
};

struct sync_fence_info {
  char obj_name[32];
  char driver_name[32];
  int32_t status;  // 1 signaled, 0 active, negative: an error
  uint32_t flags;
  uint64_t timestamp_ns;  // CLOCK_MONOTONIC, when signaled (0 while active)
};

struct sync_file_info {
  char name[32];
  int32_t status;
  uint32_t flags;
  uint32_t num_fences;
  uint32_t pad;
  uint64_t sync_fence_info;  // a pointer to num_fences struct sync_fence_info
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)

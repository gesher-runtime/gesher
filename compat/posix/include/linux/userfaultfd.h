// gesher-base: <linux/userfaultfd.h>, declarations only.
//
// userfaultfd has no Windows equivalent. ART's CMC garbage collector probes
// for it at startup and falls back to another collector when the syscall
// fails, which it always does on Windows. These definitions exist only so
// that code compiles; the values are never passed to a real kernel.
#pragma once

#include <stdint.h>

#define UFFD_API ((uint64_t)0xAA)
#define UFFD_USER_MODE_ONLY 1

#define UFFD_FEATURE_SIGBUS         (1 << 7)
#define UFFD_FEATURE_MISSING_SHMEM  (1 << 5)
#define UFFD_FEATURE_MINOR_SHMEM    (1 << 10)
#define UFFD_FEATURE_MOVE           (1 << 14)

#define UFFDIO_REGISTER_MODE_MISSING ((uint64_t)1 << 0)
#define UFFDIO_COPY_MODE_MMAP_TRYLOCK ((uint64_t)1 << 63)
#define UFFDIO_ZEROPAGE_MODE_MMAP_TRYLOCK ((uint64_t)1 << 63)
#define UFFDIO_MOVE_MODE_ALLOW_SRC_HOLES ((uint64_t)1 << 1)

#define UFFDIO_API        0xc018aa3f
#define UFFDIO_REGISTER   0xc020aa00
#define UFFDIO_UNREGISTER 0x8010aa01
#define UFFDIO_COPY       0xc028aa03
#define UFFDIO_ZEROPAGE   0xc020aa04
#define UFFDIO_MOVE       0xc028aa05

struct uffdio_api {
  uint64_t api;
  uint64_t features;
  uint64_t ioctls;
};

struct uffdio_range {
  uint64_t start;
  uint64_t len;
};

struct uffdio_register {
  struct uffdio_range range;
  uint64_t mode;
  uint64_t ioctls;
};

struct uffdio_copy {
  uint64_t dst;
  uint64_t src;
  uint64_t len;
  uint64_t mode;
  int64_t copy;
};

struct uffdio_zeropage {
  struct uffdio_range range;
  uint64_t mode;
  int64_t zeropage;
};

struct uffdio_move {
  uint64_t dst;
  uint64_t src;
  uint64_t len;
  uint64_t mode;
  int64_t move;
};

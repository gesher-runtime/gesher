// gesher-base: <sys/mman.h> for Windows, implemented in compat/posix/src/mman.cc.
// Constants use Linux values, since AOSP code and Android binaries assume them.
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_ANON            MAP_ANONYMOUS
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED ((void*)-1)

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
#define MADV_REMOVE     9
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2
#define MREMAP_DONTUNMAP 4

#define MCL_CURRENT 1
#define MCL_FUTURE  2

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
void* mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);
int madvise(void* addr, size_t length, int advice);
int msync(void* addr, size_t length, int flags);
int mincore(void* addr, size_t length, unsigned char* vec);
int mlock(const void* addr, size_t length);
int munlock(const void* addr, size_t length);
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB       0x0004U
#define MFD_NOEXEC_SEAL   0x0008U
#define MFD_EXEC          0x0010U

// An anonymous file, backed by a delete-on-close temporary file.
int memfd_create(const char* name, unsigned int flags);

void* mremap(void* old_address, size_t old_size, size_t new_size, int flags, ...);

#ifdef __cplusplus
}
#endif

#pragma once
#include <sys/types.h>
#include <stddef.h>

namespace gesher {
// Android Bionic's public O_PATH fallback. The sys/xattr.h fd entry points
// deliberately keep the legacy syscall behavior (O_PATH returns EBADF).
ssize_t BionicFgetxattr(int fd, const char* name, void* value, size_t size);
int BionicFsetxattr(int fd, const char* name, const void* value, size_t size, int flags);
ssize_t BionicFlistxattr(int fd, char* list, size_t size);
}  // namespace gesher

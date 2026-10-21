// gesher-base: <sys/xattr.h>. Extended attributes are not supported: the
// calls fail with ENOTSUP.
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XATTR_CREATE  1
#define XATTR_REPLACE 2

ssize_t getxattr(const char* path, const char* name, void* value, size_t size);
ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size);
ssize_t fgetxattr(int fd, const char* name, void* value, size_t size);
int setxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags);
ssize_t listxattr(const char* path, char* list, size_t size);
ssize_t llistxattr(const char* path, char* list, size_t size);
ssize_t flistxattr(int fd, char* list, size_t size);
int removexattr(const char* path, const char* name);
int lremovexattr(const char* path, const char* name);
int fremovexattr(int fd, const char* name);

#ifdef __cplusplus
}
#endif

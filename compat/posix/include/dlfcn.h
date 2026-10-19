// gesher-base: <dlfcn.h> on Windows DLLs (LoadLibrary/GetProcAddress).
// Gesher's ELF loader will extend this to Android .so files.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_NOLOAD   0x00004
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0
#define RTLD_NODELETE 0x01000

#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT    ((void*)-1)

typedef struct {
  const char* dli_fname;
  void* dli_fbase;
  const char* dli_sname;
  void* dli_saddr;
} Dl_info;

void* dlopen(const char* filename, int flags);
void* dlsym(void* handle, const char* symbol);
int dlclose(void* handle);
char* dlerror(void);
int dladdr(const void* addr, Dl_info* info);

#ifdef __cplusplus
}
#endif

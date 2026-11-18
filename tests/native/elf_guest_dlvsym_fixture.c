// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Original guest calls exercise the translator's real libdl ABI overrides.
extern void* dlsym(void*, const char*);
extern void* dlopen(const char*, int);
extern void* dlvsym(void*, const char*, const char*);
extern const char* dlerror(void);

int gv_user_open(const char* path, int flags, ...) {
  (void)path;
  (void)flags;
  return 12345;
}
const unsigned int gv_user_env[2] = {7, 9};
__asm__(".symver gv_user_open,open@@GV_GUEST");
__asm__(".symver gv_user_env,__fe_dfl_env@@GV_GUEST");

int gv_guest_dlvsym(void* provider, void* self) {
  typedef int (*Value)(void);
  typedef int (*Open)(const char*, int, ...);
  Value old = (Value)dlvsym(provider, "gv_value", "GV_1");
  Value current = (Value)dlvsym(provider, "gv_value", "GV_2");
  if (!old || !current || old() != 11 || current() != 22) return 1;
  if (dlvsym(provider, "gv_value", (const char*)0) != (void*)current) return 2;

  void* libc = dlopen("libc.so", 2);
  void* libm = dlopen("libm.so", 2);
  if (!libc || !libm) return 3;
  void* open_default = dlsym(libc, "open");
  void* open_versioned = dlvsym(libc, "open", "LIBC");
  if (!open_default || open_default != open_versioned) return 3;
  const unsigned int* env = (const unsigned int*)dlvsym(libm, "__fe_dfl_env", "LIBC");
  if (!env || env != dlsym(libm, "__fe_dfl_env")) return 4;
  if (env[0] != 0 || env[1] != 0) return 5;  // ARM FPCR/FPSR default, eight bytes.

  Open own_open = (Open)dlvsym(self, "open", "GV_GUEST");
  const unsigned int* own_env = (const unsigned int*)dlvsym(self, "__fe_dfl_env", "GV_GUEST");
  if (!own_open || own_open((const char*)0, 0) != 12345) return 6;
  if (!own_env || own_env[0] != 7 || own_env[1] != 9 || own_env == env) return 7;

  (void)dlerror();
  if (dlvsym(provider, "gv_value", "GV_missing")) return 8;
  if (!dlerror() || dlerror()) return 9;
  if (dlvsym(provider, (const char*)0, "GV_1")) return 10;
  if (!dlerror() || dlerror()) return 11;
  if (dlvsym(provider, "gv_null", "GV_2") || dlerror()) return 12;
  if (dlvsym(provider, "getpid", "GV_2") || dlerror()) return 13;
  if (dlvsym((void*)0, "gv_null", "GV_2") || dlerror()) return 14;
  if (dlvsym(provider, "gv_value", "GV_missing")) return 15;
  if (dlvsym(provider, "gv_null", "GV_2")) return 16;
  if (!dlerror() || dlerror()) return 17;
  return 0;
}

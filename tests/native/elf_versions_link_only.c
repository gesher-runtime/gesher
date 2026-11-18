// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Ordinary static-link inputs only. Never install these files in a runtime
// search path: runtime tests register actual owned host/library definitions.
#if GV_LINK_ROLE == 1
void gv_event(int role) { (void)role; }
void* __tls_get_addr(void* index) { (void)index; return (void*)0; }
#elif GV_LINK_ROLE == 2
void* dlopen(const char* path, int flags) { (void)path; (void)flags; return (void*)0; }
void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return (void*)0; }
void* dlvsym(void* handle, const char* name, const char* version) {
  (void)handle; (void)name; (void)version; return (void*)0;
}
char* dlerror(void) { return (void*)0; }
#elif GV_LINK_ROLE == 3
int open(const char* path, int flags, ...) { (void)path; (void)flags; return -1; }
#elif GV_LINK_ROLE == 4
int __fe_dfl_env;
#else
#error GV_LINK_ROLE is required
#endif

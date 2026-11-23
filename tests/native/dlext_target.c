// A small library for dlext_test.c to load with android_dlopen_ext. Built
// under several sonames (libdlext_a.so, libdlext_b.so, ...), one per case
// that loads it, since a library already loaded is returned where it is;
// some with link options that change its layout (a non-zero first p_vaddr,
// a large gap between its segments).
//
// With -DGESHER_DLEXT_TLS_FAIL it is libdlext_tlsfail.so instead: it uses a
// thread-local that nothing defines, so its load fails at relocation, after
// its segments were mapped (into the caller's reservation).
// With -DGESHER_DLEXT_CLOSE it also exports a symbol no other library has, so
// dlsym(RTLD_DEFAULT) shows whether it is still loaded.

#if defined(GESHER_DLEXT_TLS_FAIL)
extern __thread int gesher_dlext_missing_tls;
int gesher_dlext_tls(void) { return gesher_dlext_missing_tls; }
#else
int gesher_dlext_value = 41;  // a writable data segment
int gesher_dlext_answer(void) { return gesher_dlext_value + 1; }
void gesher_dlext_set(int value) { gesher_dlext_value = value; }
#endif

#if defined(GESHER_DLEXT_CLOSE)
int gesher_dlext_close_marker(void) { return 7; }
#endif

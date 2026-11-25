// A small library for dlext_test.c to load with android_dlopen_ext. Built
// under several sonames (libdlext_a.so, libdlext_b.so, ...), one per case
// that loads it, since a library already loaded is returned where it is;
// some with link options that change its layout (a non-zero first p_vaddr,
// a large gap between its segments) and with these variants
// (tests/native/build-dlext.sh):
//   GESHER_DLEXT_TLS_FAIL   libdlext_tlsfail.so: uses a thread-local nothing
//                           defines, so its load fails at relocation, after
//                           its segments were mapped (into the reservation).
//   GESHER_DLEXT_MARKER=f   also exports f(), returning 7, a symbol no other
//                           library has: dlsym(RTLD_DEFAULT) shows whether it
//                           is loaded, and libdlext_user.so binds to it.
//   GESHER_DLEXT_BSS        a 64 KiB .bss: a segment of many pages.
//   GESHER_DLEXT_THREAD_DTOR registers a C++-style thread_local destructor
//                           (__cxa_thread_atexit_impl with its __dso_handle).
//   GESHER_DLEXT_TLS        initialized thread-locals (PT_TLS with .tdata).
//   GESHER_DLEXT_USER       libdlext_user.so: calls gesher_dlext_bind_marker,
//                           which it does not define and has no DT_NEEDED for:
//                           bound through the global scope to whatever
//                           loaded library defines it.

#if defined(GESHER_DLEXT_TLS_FAIL)
extern __thread int gesher_dlext_missing_tls;
int gesher_dlext_tls(void) { return gesher_dlext_missing_tls; }
#else
int gesher_dlext_value = 41;  // a writable data segment
int gesher_dlext_answer(void) { return gesher_dlext_value + 1; }
void gesher_dlext_set(int value) { gesher_dlext_value = value; }
#endif

#if defined(GESHER_DLEXT_MARKER)
int GESHER_DLEXT_MARKER(void) { return 7; }
#endif

#if defined(GESHER_DLEXT_BSS)
char gesher_dlext_bss[64 * 1024];
#endif

#if defined(GESHER_DLEXT_THREAD_DTOR)
extern void* __dso_handle;
int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* dso);
static void ThreadDtor(void* flag) { *(volatile int*)flag = 1; }
// Registers a destructor for the calling thread that sets *flag when it ends.
int gesher_dlext_thread_dtor(int* flag) {
  return __cxa_thread_atexit_impl(ThreadDtor, flag, &__dso_handle);
}
#endif

#if defined(GESHER_DLEXT_TLS)
__thread int gesher_dlext_tls_value = 5;
__thread char gesher_dlext_tls_bytes[64] = {1, 2, 3};
int gesher_dlext_tls_get(void) { return gesher_dlext_tls_value + gesher_dlext_tls_bytes[63]; }
#endif

#if defined(GESHER_DLEXT_USER)
int gesher_dlext_bind_marker(void);
int gesher_dlext_call_bound(void) { return gesher_dlext_bind_marker(); }
#endif

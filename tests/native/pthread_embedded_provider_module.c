/* Statically linked winpthreads inside a real PE DLL, not the supplier DLL. */
#include <pthread.h>
#include <time.h>
__declspec(dllexport) pthread_t __cdecl embedded_self(void) { return pthread_self(); }
__declspec(dllexport) int __cdecl embedded_set(pthread_key_t k,void *v) { return pthread_setspecific(k,v); }
__declspec(dllexport) int __cdecl embedded_clock(pthread_t t,clockid_t *id) { return pthread_getcpuclockid(t,id); }
__declspec(dllexport) void (** __cdecl embedded_key_data(void))(void *) { return _pthread_key_dest; }
__declspec(dllexport) int __cdecl embedded_mutex_init(pthread_mutex_t *m) { return pthread_mutex_init(m,NULL); }
__declspec(dllexport) int __cdecl embedded_key_create(pthread_key_t *k,void (*d)(void *)) { return pthread_key_create(k,d); }
__declspec(dllexport) int __cdecl embedded_key_delete(pthread_key_t k) { return pthread_key_delete(k); }

/* A host with an incomplete authority must reject the request without leaking. */
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <windows.h>
#include <stdio.h>
typedef struct { unsigned size, version; void *register_clock, *read_clock; } invalid_authority;
__declspec(dllexport) const invalid_authority * __cdecl gesher_clock_anchor_v1(void) {
  static const invalid_authority invalid = { sizeof(invalid_authority), 1, NULL, NULL };
  return &invalid;
}
int main(void) {
  pthread_t self=pthread_self(); DWORD before, after; clockid_t out=123;
  if (!self || !GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  int rc=pthread_getcpuclockid(self,&out);
  if (!GetProcessHandleCount(GetCurrentProcess(),&after)) return 2;
  if (rc!=EAGAIN || out!=123 || after!=before) return 3;
  puts("PASS malformed process clock authority rejected with unchanged output and handles");
  return 0;
}

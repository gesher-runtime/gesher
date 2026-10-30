/* Capture a target lease, let real native TLS teardown finish, then resume. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
extern int __cdecl gesher_resource_lease_probe_v1(pthread_t,void *,void *,DWORD);
extern int __cdecl gesher_resource_fault_probe_v1(pthread_t,int);
struct state { HANDLE ready,exit_now,captured,resume; pthread_t target; };
static DWORD WINAPI target(void *arg) {
  struct state *s=arg; int old=0;
  s->target=pthread_self();
  if (!s->target || pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old) ||
      !SetEvent(s->ready) || WaitForSingleObject(s->exit_now,5000)!=WAIT_OBJECT_0) return 1;
  ExitThread(61); return 0;
}
static DWORD WINAPI consumer(void *arg) {
  struct state *s=arg;
  return (DWORD)gesher_resource_lease_probe_v1(s->target,s->captured,s->resume,61);
}
int main(void) {
  DWORD before=0,after=0,code=0; struct state s={0};
  if (!GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  s.ready=CreateEventW(NULL,TRUE,FALSE,NULL); s.exit_now=CreateEventW(NULL,TRUE,FALSE,NULL);
  s.captured=CreateEventW(NULL,TRUE,FALSE,NULL); s.resume=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!s.ready || !s.exit_now || !s.captured || !s.resume) return 2;
  HANDLE t=CreateThread(NULL,0,target,&s,0,NULL);
  if (!t || WaitForSingleObject(s.ready,5000)!=WAIT_OBJECT_0) return 3;
  if (gesher_resource_fault_probe_v1(s.target,1) || gesher_resource_fault_probe_v1(s.target,2)) return 3;
  puts("PASS target resource lease duplicate thread/event failures atomic with exact handle rollback");
  HANDLE c=CreateThread(NULL,0,consumer,&s,0,NULL);
  if (!c || WaitForSingleObject(s.captured,5000)!=WAIT_OBJECT_0 || !SetEvent(s.exit_now) ||
      WaitForSingleObject(t,5000)!=WAIT_OBJECT_0 || !GetExitCodeThread(t,&code) || code!=61 ||
      !SetEvent(s.resume) || WaitForSingleObject(c,5000)!=WAIT_OBJECT_0 ||
      !GetExitCodeThread(c,&code) || code) return 4;
  if (!CloseHandle(c) || !CloseHandle(t) || !CloseHandle(s.ready) || !CloseHandle(s.exit_now) ||
      !CloseHandle(s.captured) || !CloseHandle(s.resume) ||
      !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 5;
  printf("PASS target resource lease survives actual native TLS retirement handles=%lu/%lu\n",before,after);
  return 0;
}

/* CI-only actual SRW key-registry contention; all handles bounded. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
extern void __cdecl gesher_key_probe_arm_v1(void *);
extern int __cdecl gesher_key_probe_hold_v1(void *,void *);
struct gate { HANDLE entered,release,contended,done; int hold_result,create_result; pthread_key_t key; };
static DWORD WINAPI holder(void *p) { struct gate *g=p; g->hold_result=gesher_key_probe_hold_v1(g->entered,g->release); return 0; }
static DWORD WINAPI creator(void *p) { struct gate *g=p; g->create_result=pthread_key_create(&g->key,NULL); SetEvent(g->done); return 0; }
int main(void) {
  struct gate g={0}; HANDLE hold=NULL,create=NULL; int result=1;
  g.entered=CreateEventW(NULL,TRUE,FALSE,NULL); g.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  g.contended=CreateEventW(NULL,TRUE,FALSE,NULL); g.done=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!g.entered || !g.release || !g.contended || !g.done) goto done;
  gesher_key_probe_arm_v1(g.contended);
  hold=CreateThread(NULL,0,holder,&g,0,NULL);
  if (!hold || WaitForSingleObject(g.entered,5000)!=WAIT_OBJECT_0) goto done;
  create=CreateThread(NULL,0,creator,&g,0,NULL);
  if (!create || WaitForSingleObject(g.contended,5000)!=WAIT_OBJECT_0 ||
      WaitForSingleObject(g.done,0)!=WAIT_TIMEOUT || !SetEvent(g.release) ||
      WaitForSingleObject(hold,5000)!=WAIT_OBJECT_0 || WaitForSingleObject(create,5000)!=WAIT_OBJECT_0 ||
      g.hold_result || g.create_result || pthread_key_delete(g.key)) goto done;
  puts("PASS probe key registry actual lock contention and create/delete"); result=0;
done:
  if (g.release) SetEvent(g.release);
  if (hold && WaitForSingleObject(hold,5000)!=WAIT_OBJECT_0) return 3;
  if (create && WaitForSingleObject(create,5000)!=WAIT_OBJECT_0) return 3;
  gesher_key_probe_arm_v1(NULL);
  if (hold) CloseHandle(hold); if (create) CloseHandle(create);
  if (g.entered) CloseHandle(g.entered); if (g.release) CloseHandle(g.release);
  if (g.contended) CloseHandle(g.contended); if (g.done) CloseHandle(g.done);
  return result;
}

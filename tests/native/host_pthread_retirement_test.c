/* CI-only client of separately patched supplier; no POSIX overlay. */
#include <pthread.h>
#include <errno.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct target { HANDLE ready, start, thread, cap, de, dr; pthread_t id; unsigned long long gen; };
static pthread_key_t key;
static volatile LONG destructors;
static int owned, tls_failure;
static void check(int ok, const char *s) { if (!ok) { fprintf(stderr,"FAIL retirement %s\n",s); abort(); } }
static HANDLE event(void) { HANDLE h=CreateEvent(NULL,TRUE,FALSE,NULL); check(h!=NULL,"event"); ++owned; return h; }
static void close_owned(HANDLE h) { check(CloseHandle(h),"close"); check(--owned>=0,"owned count"); }
static void await(HANDLE h) { check(WaitForSingleObject(h,5000)==WAIT_OBJECT_0,"event/thread deadline"); }
static void destructor(void *p) {
  struct target *t=p; check(t!=NULL,"destructor value"); InterlockedIncrement(&destructors);
  if (tls_failure) { puts("CONTROL TLS-clear-failure destructor exactly once before abort"); fflush(stdout); }
  if (t->de) { check(SetEvent(t->de),"destructor entered"); await(t->dr); }
}
static DWORD WINAPI target(void *p) {
  struct target *t=p; t->id=pthread_self();
  check(pthread_setspecific(key,t)==0,"setspecific"); check(SetEvent(t->ready),"ready");
  await(t->start); pthread_exit(NULL); return 0;
}
static void launch(struct target *t) {
  t->ready=event(); t->start=event(); t->thread=CreateThread(NULL,0,target,t,0,NULL);
  check(t->thread!=NULL,"thread"); ++owned; await(t->ready);
  check(gesher_winpthread_dup_cpu_handle_v1(t->id,(void **)&t->cap,&t->gen)==0,"live capability"); ++owned;
  check(GetThreadId(t->cap)==GetThreadId(t->thread),"same original kernel thread");
}
static void retired(struct target *t) {
  void *unchanged=(void *)1; unsigned long long gen=2;
  check(pthread_detach(t->id)==ESRCH,"retired identity explicit ESRCH");
  check(gesher_winpthread_dup_cpu_handle_v1(t->id,&unchanged,&gen)==ESRCH && unchanged==(void *)1 && gen==2,"retired acquire unchanged");
  check(WaitForSingleObject(t->cap,0)==WAIT_OBJECT_0 && GetThreadId(t->cap)==GetThreadId(t->thread),"retained original handle");
}
static void finish(struct target *t) { close_owned(t->thread); close_owned(t->cap); close_owned(t->ready); close_owned(t->start); }
static DWORD WINAPI detacher(void *p) { struct target *t=p; check(pthread_detach(t->id)==0,"detach after publication"); return 0; }
static void contention(void) {
  for (int i=0;i<64;++i) {
    struct target t={0}; HANDLE entered=event(), release=event(), contended=event(); LONG before=destructors;
    launch(&t); check(gesher_winpthread_exit_test_v1(t.id,entered,release,contended)==0,"arm");
    check(SetEvent(t.start),"start"); await(entered); check(destructors==before+1,"destructor before publication");
    HANDLE th=CreateThread(NULL,0,detacher,&t,0,NULL); check(th!=NULL,"detacher"); ++owned;
    /* The provider signals only after its actual mutex trylock fails. */
    await(contended); check(SetEvent(release),"release registry"); await(t.thread); await(th); retired(&t);
    check(gesher_winpthread_exit_test_v1(0,NULL,NULL,NULL)==0,"disarm");
    close_owned(th); close_owned(entered); close_owned(release); close_owned(contended); finish(&t);
  }
  puts("PASS retirement 64 actual registry mutex contentions destructor-before-publication retired-ESRCH");
}
static void unlocked_destructor(void) {
  struct target t={0}; t.de=event(); t.dr=event(); launch(&t);
  check(SetEvent(t.start),"destructor case start"); await(t.de);
  /* Another OS thread acquires the registry while the callback is held. */
  check(pthread_detach(t.id)==0,"registry available during destructor");
  check(SetEvent(t.dr),"release destructor"); await(t.thread); retired(&t);
  close_owned(t.de); close_owned(t.dr); finish(&t);
  puts("PASS retirement user-destructor outside registry lock");
}
static DWORD WINAPI joiner(void *p) { struct target *t=p; check(pthread_join(t->id,NULL)==0,"live join result"); return 0; }
static void live_join(void) {
  struct target t={0}; HANDLE entered=event(); launch(&t);
  check(gesher_winpthread_exit_test_v1(t.id,NULL,NULL,entered)==0,"arm live join");
  HANDLE th=CreateThread(NULL,0,joiner,&t,0,NULL); check(th!=NULL,"joiner"); ++owned;
  await(entered); /* Provider verified target kernel handle is still nonsignaled. */
  check(SetEvent(t.start),"release joined target"); await(th); await(t.thread);
  check(gesher_winpthread_exit_test_v1(0,NULL,NULL,NULL)==0,"disarm join");
  retired(&t); finish(&t); close_owned(th); close_owned(entered);
  puts("PASS retirement valid join reached live controller-held target");
}
static void live_and_reuse(void) {
  unsigned long long last=0;
  for (int batch=0;batch<16;++batch) {
    struct target t[16]={{0}}; LONG before=destructors;
    for (int i=0;i<16;++i) {
      launch(&t[i]); check(t[i].gen>last,"fresh generation across reuse"); last=t[i].gen;
      for (int j=0;j<i;++j) check(t[i].id!=t[j].id && GetThreadId(t[i].cap)!=GetThreadId(t[j].cap),"distinct live identities");
      if (i%3==0) check(pthread_detach(t[i].id)==0,"valid live detach");
    }
    for (int i=0;i<16;++i) check(SetEvent(t[i].start),"batch start");
    for (int i=0;i<16;++i) {
      /* No simultaneous join and detach of the same target. */
      if (i%3==1) check(pthread_join(t[i].id,NULL)==0,"valid join");
      await(t[i].thread);
      if (i%3==2) check(pthread_detach(t[i].id)==0,"valid ended detach");
      retired(&t[i]);
    }
    check(destructors==before+16,"batch exact destructors");
    /* Keep old handles across actual record reuse, then verify them again. */
    struct target next={0}; launch(&next);
    for (int i=0;i<16;++i) retired(&t[i]);
    check(pthread_detach(next.id)==0,"replacement detach"); check(SetEvent(next.start),"replacement start");
    await(next.thread); retired(&next); finish(&next);
    for (int i=0;i<16;++i) finish(&t[i]);
  }
  puts("PASS retirement 256 concurrent-live detach join ended-detach reuse original-capability");
}
int main(int argc,char **argv) {
  DWORD before,after; check(argc==1 || (argc==2 && strcmp(argv[1],"--tls-clear-failure")==0),"arguments");
  tls_failure=argc==2;
  if (tls_failure) { SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT); }
  check(GetProcessHandleCount(GetCurrentProcess(),&before),"original baseline"); check(pthread_key_create(&key,destructor)==0,"key");
  if (tls_failure) {
    struct target t={0}; launch(&t); check(gesher_winpthread_cpu_test_control_v1(3,1)==0,"inject TLS clear failure");
    check(SetEvent(t.start),"failure start"); await(t.thread); puts("FAIL TLS-clear-failure survived"); return 9;
  }
  contention(); unlocked_destructor(); live_join(); live_and_reuse();
  check(destructors==338,"all exact destructors"); check(pthread_key_delete(key)==0,"delete key"); check(owned==0,"all fixture handles closed");
  check(GetProcessHandleCount(GetCurrentProcess(),&after) && after<=before+8,"unchanged original handle bound");
  printf("PASS retirement all 338 destructors owned=0 handles=%lu/%lu\n",before,after); return 0;
}

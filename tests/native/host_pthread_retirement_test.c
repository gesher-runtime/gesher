/* CI-only client of separately patched supplier; no POSIX overlay. */
#include <pthread.h>
#include <errno.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
/* ProcessHandleInformation: handle values and object type indices, not inferred ownership. */
typedef struct { HANDLE value; ULONG_PTR handle_count, pointer_count; ULONG access, type, attributes, reserved; } handle_entry;
typedef struct { ULONG_PTR count, reserved; handle_entry entries[1]; } handle_snapshot;
typedef LONG (WINAPI *query_process_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (WINAPI *query_object_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef struct { USHORT length, maximum; WCHAR *buffer; } object_name;
static void check(int ok, const char *s);
static handle_snapshot *previous;
typedef struct { DWORD tid; FILETIME created; } thread_identity;
static thread_identity targets[400];
static size_t target_count;
static int same_time(FILETIME a, FILETIME b) { return a.dwLowDateTime==b.dwLowDateTime && a.dwHighDateTime==b.dwHighDateTime; }
static void inventory(const char *phase) {
  HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
  query_process_fn query=(query_process_fn)GetProcAddress(ntdll,"NtQueryInformationProcess");
  query_object_fn object=(query_object_fn)GetProcAddress(ntdll,"NtQueryObject");
  check(query!=NULL && object!=NULL,"handle inventory query");
  ULONG bytes=65536, required=0; handle_snapshot *snapshot=NULL; LONG status;
  do {
    free(snapshot); snapshot=malloc(bytes); check(snapshot!=NULL,"handle inventory allocation");
    status=query(GetCurrentProcess(),51,snapshot,bytes,&required);
    if (status<0) { check(required>bytes && required<16*1024*1024,"handle inventory size"); bytes=required+4096; }
  } while (status<0);
  check(snapshot->count <= (bytes - 2*sizeof(ULONG_PTR))/sizeof(handle_entry),"handle inventory bounds");
  for (ULONG_PTR i=0;i<snapshot->count;++i) {
    handle_entry *e=&snapshot->entries[i]; int found=0;
    if (previous) for (ULONG_PTR j=0;j<previous->count;++j)
      if (previous->entries[j].value==e->value && previous->entries[j].type==e->type) { found=1; break; }
    if (!found) {
      union { ULONG_PTR alignment; unsigned char bytes[4096]; } storage={0}; ULONG returned=0;
      LONG result=object(e->value,2,storage.bytes,sizeof(storage.bytes),&returned);
      object_name *name=(object_name *)storage.bytes;
      uintptr_t start=(uintptr_t)storage.bytes, end=start+sizeof(storage.bytes), ptr=(uintptr_t)name->buffer;
      if (result>=0 && ptr>=start && ptr<=end && name->length<=end-ptr && name->length<=name->maximum)
        printf("HANDLE_INVENTORY phase=%s value=%p type_index=%lu access=%08lx type=%.*ls\n",phase,e->value,e->type,e->access,(int)(name->length/sizeof(WCHAR)),name->buffer);
      else printf("HANDLE_INVENTORY phase=%s value=%p type_index=%lu access=%08lx query_status=%08lx\n",phase,e->value,e->type,e->access,(unsigned long)result);
      if (result>=0 && ptr>=start && ptr<=end && name->length<=end-ptr && name->length==12 && wcsncmp(name->buffer,L"Thread",6)==0) {
        FILETIME created,exited,kernel,user; DWORD tid=GetThreadId(e->value); int match=0;
        if (tid && GetThreadTimes(e->value,&created,&exited,&kernel,&user)) {
          for (size_t j=0;j<target_count;++j) if (tid==targets[j].tid && same_time(created,targets[j].created)) ++match;
          printf("HANDLE_THREAD phase=%s value=%p tid=%lu current_tid=%lu creation=%08lx%08lx signaled=%lu target_matches=%d\n",
                 phase,e->value,tid,GetCurrentThreadId(),created.dwHighDateTime,created.dwLowDateTime,WaitForSingleObject(e->value,0),match);
        } else printf("HANDLE_THREAD phase=%s value=%p query_failed=%lu\n",phase,e->value,GetLastError());
      }
    }
  }
  free(previous); previous=snapshot;
}
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
  check(pthread_join(t->id,NULL)==EDEADLK,"self join");
  check(_pthread_tryjoin(t->id,NULL)==EDEADLK,"self tryjoin");
  check(pthread_setspecific(key,t)==0,"setspecific"); check(SetEvent(t->ready),"ready");
  await(t->start); pthread_exit(NULL); return 0;
}
static void launch(struct target *t) {
  t->ready=event(); t->start=event(); t->thread=CreateThread(NULL,0,target,t,0,NULL);
  check(t->thread!=NULL,"thread"); ++owned; await(t->ready);
  FILETIME c={0},e={0},k={0},u={0}; check(target_count<400 && GetThreadTimes(t->thread,&c,&e,&k,&u),"target identity");
  DWORD tid=GetThreadId(t->thread); check(tid!=0,"target thread ID");
  targets[target_count++]=(thread_identity){tid,c};
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
static void counts(const char *phase) { DWORD count=0; check(GetProcessHandleCount(GetCurrentProcess(),&count),"phase handle count"); printf("RETIREMENT_COUNTS phase=%s handles=%lu owned=%d destructors=%ld\n",phase,count,owned,destructors); inventory(phase); fflush(stdout); }
#ifdef GESHER_WINPTHREAD_CPU_TEST_API
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
#else
static void contention(void) {
  for (int i=0;i<64;++i) {
    struct target t={0}; LONG before=destructors; launch(&t);
    check(pthread_detach(t.id)==0,"production live detach");
    check(SetEvent(t.start),"production start"); await(t.thread); retired(&t);
    check(destructors==before+1,"production exact destructor"); finish(&t);
  }
  puts("PASS retirement production 64 live detach and exact destructors");
}
#endif
static void unlocked_destructor(void) {
  struct target t={0}; t.de=event(); t.dr=event(); launch(&t);
  check(SetEvent(t.start),"destructor case start"); await(t.de);
  /* Another OS thread acquires the registry while the callback is held. */
  check(pthread_detach(t.id)==0,"registry available during destructor");
  check(SetEvent(t.dr),"release destructor"); await(t.thread); retired(&t);
  close_owned(t.de); close_owned(t.dr); finish(&t);
  puts("PASS retirement user-destructor outside registry lock");
}
#ifdef GESHER_WINPTHREAD_CPU_TEST_API
static DWORD WINAPI joiner(void *p) { struct target *t=p; check(pthread_join(t->id,NULL)==0,"live join result"); return 0; }
static void live_join(void) {
  struct target t={0}; HANDLE entered=event(); launch(&t);
  check(_pthread_tryjoin(t.id,NULL)==EBUSY,"foreign live tryjoin");
  check(gesher_winpthread_exit_test_v1(t.id,NULL,NULL,entered)==0,"arm live join");
  HANDLE th=CreateThread(NULL,0,joiner,&t,0,NULL); check(th!=NULL,"joiner"); ++owned;
  await(entered); /* Provider verified target kernel handle is still nonsignaled. */
  check(SetEvent(t.start),"release joined target"); await(th); await(t.thread);
  check(gesher_winpthread_exit_test_v1(0,NULL,NULL,NULL)==0,"disarm join");
  retired(&t); finish(&t); close_owned(th); close_owned(entered);
  puts("PASS retirement valid join reached live controller-held target");
}
#else
static HANDLE production_join_started;
static DWORD WINAPI joiner(void *p) { struct target *t=p; check(SetEvent(production_join_started),"joiner started"); check(pthread_join(t->id,NULL)==0,"join result"); return 0; }
static void live_join(void) {
  struct target t={0}; production_join_started=event(); launch(&t);
  check(_pthread_tryjoin(t.id,NULL)==EBUSY,"foreign live tryjoin");
  HANDLE th=CreateThread(NULL,0,joiner,&t,0,NULL); check(th!=NULL,"joiner"); ++owned;
  await(production_join_started); check(SetEvent(t.start),"release joined target"); await(th); await(t.thread);
  retired(&t); finish(&t); close_owned(th); close_owned(production_join_started);
  puts("PASS retirement production joiner-start-before-release and foreign tryjoin");
}
#endif
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
  setvbuf(stdout,NULL,_IONBF,0);
  DWORD before,after; check(argc==1 || (argc==2 && strcmp(argv[1],"--tls-clear-failure")==0),"arguments");
  tls_failure=argc==2;
  if (tls_failure) { SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT); }
  check(GetProcessHandleCount(GetCurrentProcess(),&before),"original baseline"); if (!tls_failure) inventory("original"); check(pthread_key_create(&key,destructor)==0,"key");
#ifdef GESHER_WINPTHREAD_CPU_TEST_API
  if (tls_failure) {
    struct target t={0}; launch(&t); check(gesher_winpthread_cpu_test_control_v1(3,1)==0,"inject TLS clear failure");
    check(SetEvent(t.start),"failure start"); await(t.thread); puts("FAIL TLS-clear-failure survived"); return 9;
  }
  counts("baseline"); contention(); counts("contention"); unlocked_destructor(); counts("destructor"); live_join(); counts("join"); live_and_reuse(); counts("reuse");
  check(destructors==338,"all exact destructors");
#else
  check(!tls_failure,"failure injection requires fault supplier");
  counts("baseline"); contention(); counts("contention"); unlocked_destructor(); counts("destructor"); live_join(); counts("join"); live_and_reuse(); counts("reuse");
  check(destructors==338,"all production destructors");
#endif
  check(pthread_key_delete(key)==0,"delete key"); check(owned==0,"all fixture handles closed");
  check(GetProcessHandleCount(GetCurrentProcess(),&after),"final handle count");
  inventory("final");
  printf("RETIREMENT_COUNTS phase=final before=%lu after=%lu owned=%d destructors=%ld\n",before,after,owned,destructors);
  check(after<=before+8,"unchanged original handle bound");
  printf("PASS retirement all %ld destructors owned=0 handles=%lu/%lu\n",destructors,before,after); return 0;
}

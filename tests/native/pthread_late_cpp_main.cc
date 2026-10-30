// Genuine default-compiler cross-TU thread_local destructors; no order hook.
#include "pthread_late_cpp_shared.h"
#include <cstdio>
#include <cerrno>
#include <signal.h>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif
static void Require(bool ok,const char *message) {
  if (!ok) { std::fprintf(stderr,"FAIL CPP_TLS fixture %s\n",message); std::exit(1); }
}
static std::uint64_t NativeId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  const long id=syscall(SYS_gettid); Require(id>0,"actual native Linux thread identity");
  return static_cast<std::uint64_t>(id);
#endif
}
static pthread_key_t late_key;
#ifdef _WIN32
static pthread_key_t early_key;
#endif
static void LateKeyDestructor(void *raw) {
  auto *state=static_cast<LateCppCase *>(raw);
  const pthread_t actual=pthread_self();
  const bool same=pthread_equal(actual,state->expected)!=0 && NativeId()==state->native;
  ++state->late_key_dtors;
  if (!same) ++state->mismatch;
  std::printf("CPP_TLS_LATE_KEY_DTOR mode=%s equal=%d observation_only=1\n",state->mode,same?1:0);
}
#ifdef _WIN32
static void EarlyKeyDestructor(void *raw) {
  auto *state=static_cast<LateCppCase *>(raw);
  ++state->early_key_dtors;
  // Deliberately reach the published platform budget once. Main deletes this
  // residual value while the separate genuine C++ callback remains held.
  Require(!pthread_setspecific(early_key,state),"reschedule early key within its original pass budget");
}
#endif
void LateCppRecord(LateCppCase *state,const char *tu) {
  const pthread_t actual=pthread_self();
  const bool same=pthread_equal(actual,state->expected)!=0;
  const auto native=NativeId();
  if (tu[0]=='A') ++state->a; else ++state->b;
  if (!same || native!=state->native || !state->body_left.load()) ++state->mismatch;
  std::printf("CPP_TLS_DTOR mode=%s tu=%s sequence=%d native=%llu expected_native=%llu expected=%llu actual=%llu equal=%d body_left=%d\n",
      state->mode,tu,++state->sequence,static_cast<unsigned long long>(native),
      static_cast<unsigned long long>(state->native),static_cast<unsigned long long>(state->expected),
      static_cast<unsigned long long>(actual),same?1:0,state->body_left.load());
  Require(!pthread_setspecific(late_key,state) && pthread_getspecific(late_key)==state,
      "late genuine C++ callback key get/set on original TLS record");
  ++state->late_sets;
#ifdef _WIN32
  if (state->held) {
    if (state->hold_claimed.exchange(1)==0) {
      Require(state->early_key_dtors==PTHREAD_DESTRUCTOR_ITERATIONS,"unchanged early owned TSD pass budget");
      std::printf("CPP_TLS_HELD_ENTER mode=%s tu=%s early_budget=%d actual_early=%d\n",
          state->mode,tu,PTHREAD_DESTRUCTOR_ITERATIONS,state->early_key_dtors.load());
      Require(SetEvent(reinterpret_cast<HANDLE>(state->hold_entered)) &&
          WaitForSingleObject(reinterpret_cast<HANDLE>(state->hold_release),10000)==WAIT_OBJECT_0,
          "real C++ destructor held until actual concurrent retirement checks");
    }
    // This bounded case qualifies live late key access, not a new promise to
    // execute late-created TSD callbacks under the loader. Clear it explicitly.
    Require(!pthread_setspecific(late_key,nullptr),"explicit held-case late value clear");
  }
#endif
}
static void *Body(void *arg) {
  auto *state=static_cast<LateCppCase *>(arg);
  state->expected=pthread_self(); state->native=NativeId();
  Require(state->expected!=pthread_t{} && state->native!=0,"initial worker identities");
#ifdef _WIN32
  HANDLE completion=nullptr;
  Require(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),
      &completion,SYNCHRONIZE|THREAD_QUERY_LIMITED_INFORMATION,FALSE,0)!=0,
      "owned actual native completion handle before TLS construction");
  state->native_handle=reinterpret_cast<std::uintptr_t>(completion);
#endif
  LateCppTouchA(state); LateCppTouchB(state);
#ifdef _WIN32
  if (state->held) Require(!pthread_setspecific(early_key,state),"early TSD value before owned return");
#endif
  std::printf("CPP_TLS_BODY mode=%s native=%llu pthread=%llu constructed=%d\n",state->mode,
      static_cast<unsigned long long>(state->native),static_cast<unsigned long long>(state->expected),state->constructed.load());
  state->body_left=1;
  if (!std::strcmp(state->mode,"pthread_exit")) pthread_exit(reinterpret_cast<void *>(0x1234));
#ifdef _WIN32
  if (!std::strcmp(state->mode,"native_ExitThread")) ExitThread(61);
#endif
  return reinterpret_cast<void *>(0x1234);
}
#ifdef _WIN32
static DWORD WINAPI NativeBody(void *arg) { Body(arg); return 62; }
#endif
#ifdef _WIN32
struct RetirerFunctions {
  int (__cdecl *start)();
  int (__cdecl *watch)(pthread_t);
  int (__cdecl *counts)(LONG *,unsigned);
  int (__cdecl *snapshot)(pthread_t,unsigned long long *,unsigned);
};
static RetirerFunctions Retirer() {
  HMODULE owner=GetModuleHandleW(nullptr);
  RetirerFunctions result{
      reinterpret_cast<int (__cdecl *)()>(GetProcAddress(owner,"gesher_tls_reaper_start_v1")),
      reinterpret_cast<int (__cdecl *)(pthread_t)>(GetProcAddress(owner,"gesher_tls_retirement_watch_v1")),
      reinterpret_cast<int (__cdecl *)(LONG *,unsigned)>(GetProcAddress(owner,"gesher_tls_retirement_counts_v1")),
      reinterpret_cast<int (__cdecl *)(pthread_t,unsigned long long *,unsigned)>(GetProcAddress(owner,"gesher_tls_retirement_snapshot_v1"))};
  Require((result.start!=nullptr)==(result.watch!=nullptr) &&
      (result.start!=nullptr)==(result.counts!=nullptr) &&
      (result.start!=nullptr)==(result.snapshot!=nullptr),"complete distinct diagnostic reaper exports");
  return result;
}
static void DrainRetirer(const RetirerFunctions& reaper) {
  if (!reaper.start) return;
  const auto deadline=GetTickCount64()+10000;
  for (;;) {
    LONG values[8]; Require(!reaper.counts(values,8),"retirement queue observation");
    if (!values[0]) return;
    Require(GetTickCount64()<deadline,"actual kernel retirement drain deadline"); Sleep(1);
  }
}
struct HeldJoin {
  pthread_t target;
  LateCppCase *state;
  int result=-1;
  void *value=nullptr;
  std::atomic<int> complete_at_join{0};
};
static void *JoinHeldTarget(void *raw) {
  auto& join=*static_cast<HeldJoin *>(raw);
  join.result=pthread_join(join.target,&join.value);
  join.complete_at_join=join.state->a_done==1 && join.state->b_done==1;
  return nullptr;
}
static bool RunHeldCase(const char *mode,bool detach,const RetirerFunctions& reaper) {
  DrainRetirer(reaper);
  LateCppCase state{mode}; state.held=true;
  HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  state.hold_entered=reinterpret_cast<std::uintptr_t>(entered);
  state.hold_release=reinterpret_cast<std::uintptr_t>(release);
  Require(entered && release && !pthread_key_create(&early_key,EarlyKeyDestructor),"held case events and distinct early key");
  pthread_t target{};
  Require(!pthread_create(&target,nullptr,Body,&state) &&
      WaitForSingleObject(entered,10000)==WAIT_OBJECT_0,"actual genuine C++ callback entered");
  Require(state.expected==target && state.early_key_dtors==PTHREAD_DESTRUCTOR_ITERATIONS,
      "original identity and original owned TSD budget precede held C++ callback");
  Require(!reaper.watch(target),"watch exact retained target generation");
  Require(!pthread_kill(target,0),"live validated lookup before detach ownership transition");
  HeldJoin joining{target,&state}; pthread_t joiner{};
  if (detach) Require(!pthread_detach(target),"actual detach while genuine callback held");
  else Require(!pthread_create(&joiner,nullptr,JoinHeldTarget,&joining),"actual concurrent join caller");
  unsigned long long snapshot[12]={}; LONG values[8]={};
  const auto deadline=GetTickCount64()+10000;
  for (;;) {
    Require(!reaper.snapshot(target,snapshot,12) && !reaper.counts(values,8),"validated actual retained resources");
    if (values[1]>0 && (detach || snapshot[6]!=0)) break;
    Require(GetTickCount64()<deadline,"actual reaper scan and join claim deadline"); Sleep(1);
  }
  Require(snapshot[0]==static_cast<unsigned long long>(target) && snapshot[2]==1 &&
      snapshot[3]==0 && snapshot[4]==3 && snapshot[5]==0 && snapshot[7]!=0 &&
      snapshot[8]==WAIT_TIMEOUT && snapshot[9]==1 && snapshot[10]==0 && snapshot[11]==1 &&
      values[2]==0 && values[3]==0 && values[4]==0 && values[5]==0 && values[6]==0 && values[7]==0,
      "callback-held record original identity clock/event/native handle and zero early retirement");
  // Existing kill contract rejects detached bookkeeping h==NULL. The snapshot
  // independently validates that the original record/resources remain owned.
  Require(pthread_kill(target,0)==(detach?ESRCH:0),"unchanged detached versus joinable lookup contract while held");
  void *untouched=reinterpret_cast<void *>(0x789a);
  if (detach) Require(_pthread_tryjoin(target,&untouched)==ESRCH,"detached handle cannot be joined");
  else {
    Require(_pthread_tryjoin(target,&untouched)==EINVAL && pthread_detach(target)==EINVAL,
        "existing join claim rejects concurrent tryjoin and detach");
  }
  Require(untouched==reinterpret_cast<void *>(0x789a),"failed retirement attempts preserve result output");
  Require(!pthread_key_delete(early_key),"external deletion clears actual residual early key on held record");
  Require(!reaper.snapshot(target,snapshot,12) && snapshot[0]==static_cast<unsigned long long>(target) &&
      snapshot[3]==0 && snapshot[8]==WAIT_TIMEOUT && snapshot[9]==1 && snapshot[10]==0 && snapshot[11]==1,
      "external key deletion preserves retained record and resources");
  std::printf("CPP_TLS_HELD_BEFORE mode=%s original_identity=1 tls_owner=1 lifecycle=3 ended=0 kernel=258 clock_live=1 event_live=1 spin_live=1 scans=%ld finalized=0 released=0 pool=0 early_budget=%d external_delete=1\n",
      mode,values[1],PTHREAD_DESTRUCTOR_ITERATIONS);
  Require(SetEvent(release)!=0,"release actual C++ callback");
  if (!detach) {
    void *value=nullptr;
    Require(!pthread_join(joiner,&value) && joining.result==0 &&
        joining.value==reinterpret_cast<void *>(0x1234) && joining.complete_at_join==1,
        "actual target join observes completed genuine callbacks");
  }
  HANDLE completion=reinterpret_cast<HANDLE>(state.native_handle.load()); DWORD exit=0;
  Require(completion && WaitForSingleObject(completion,10000)==WAIT_OBJECT_0 &&
      GetExitCodeThread(completion,&exit) && exit==0x1234 && CloseHandle(completion),
      "held target actual kernel completion and owned storage handle");
  DrainRetirer(reaper);
  Require(!reaper.counts(values,8) && values[0]==0 && values[1]>0 && values[2]==1 &&
      values[3]==1 && values[4]==1 && values[5]==1 && values[6]==1 && values[7]==1,
      "actual target finalization event/clock closure queue release and pool retirement exactly once");
  Require(pthread_kill(target,0)==ESRCH && state.early_key_dtors==PTHREAD_DESTRUCTOR_ITERATIONS &&
      state.late_key_dtors==0 && state.late_sets==2 && state.a==1 && state.b==1 &&
      state.a_done==1 && state.b_done==1 && state.mismatch==0,
      "retired identity and unchanged early budget with explicitly cleared late key");
  Require(CloseHandle(entered) && CloseHandle(release),"held callback fixture events closed");
  std::printf("CPP_TLS_HELD_AFTER mode=%s kernel_signaled=1 finalized=1 released=1 clock_destroy=1 event_close=1 spin_destroy=1 pool_retire=1 original_identity=1 a_done=1 b_done=1 late_key_explicit_clear=1\n",mode);
  return true;
}
#endif

int main() {
  setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
  DWORD origin_before=0,before=0,after=0;
  Require(GetProcessHandleCount(GetCurrentProcess(),&origin_before)!=0,"original initial handles");
  const RetirerFunctions reaper=Retirer();
  if (reaper.start) Require(!reaper.start(),"explicit normal-main diagnostic process retirement owner");
  Require(GetProcessHandleCount(GetCurrentProcess(),&before)!=0 &&
      before==origin_before+(reaper.start?2u:0u),"exact process-lived owner event/thread handle accounting");
  std::printf("CPP_TLS_OWNER_BOOTSTRAP present=%d original=%lu baseline=%lu persistent_handles=%lu\n",
      reaper.start?1:0,origin_before,before,before-origin_before);
#endif
  Require(!pthread_key_create(&late_key,LateKeyDestructor),"common late-key observation registration");
  const char *modes[]={"pthread_return","pthread_exit","std_thread_return"
#ifdef _WIN32
    ,"native_ExitThread"
#endif
  };
  int failures=0,cases=0;
  for (const char *mode:modes) {
    LateCppCase state{mode};
    bool join_conformant=true;
    if (!std::strcmp(mode,"std_thread_return")) {
      std::thread thread([&state] { Body(&state); }); thread.join();
    }
#ifdef _WIN32
    else if (!std::strcmp(mode,"native_ExitThread")) {
      HANDLE thread=CreateThread(nullptr,0,NativeBody,&state,0,nullptr); DWORD code=0;
      Require(thread && WaitForSingleObject(thread,10000)==WAIT_OBJECT_0 &&
          GetExitCodeThread(thread,&code) && code==61 && CloseHandle(thread),"native kernel termination61 and owned handle");
    }
#endif
    else {
      pthread_t thread{}; void *value=nullptr;
      Require(!pthread_create(&thread,nullptr,Body,&state) && !pthread_join(thread,&value) &&
          value==reinterpret_cast<void *>(0x1234),"pthread create/join result");
    }
    if (std::strcmp(mode,"native_ExitThread")) {
      const int at_join_a_done=state.a_done.load(),at_join_b_done=state.b_done.load();
      const int at_join_a=state.a.load(),at_join_b=state.b.load(),at_join_mismatch=state.mismatch.load();
      const int at_join_sequence=state.sequence.load();
#ifdef _WIN32
      const DWORD at_join_kernel=WaitForSingleObject(reinterpret_cast<HANDLE>(state.native_handle.load()),0);
      Require(at_join_kernel==WAIT_OBJECT_0 || at_join_kernel==WAIT_TIMEOUT,"owned join-return kernel sample");
#endif
      join_conformant=at_join_a==1 && at_join_b==1 && at_join_mismatch==0 && at_join_sequence==2 &&
          at_join_a_done==1 && at_join_b_done==1;
      std::printf("CPP_TLS_JOIN mode=%s phase=join-return a=%d b=%d mismatch=%d sequence=%d a_done=%d b_done=%d conformant=%d\n",
          mode,at_join_a,at_join_b,at_join_mismatch,at_join_sequence,at_join_a_done,at_join_b_done,join_conformant?1:0);
#ifdef _WIN32
      std::printf("CPP_TLS_JOIN_KERNEL mode=%s phase=join-return wait=%lu observation_only=1\n",mode,at_join_kernel);
#endif
    }
#ifdef _WIN32
    // The supplier's join ordering is under test. Keep state alive until actual
    // kernel termination even if a supplier publishes pthread completion early.
    HANDLE completion=reinterpret_cast<HANDLE>(state.native_handle.load());
    DWORD native_exit=0;
    Require(completion && WaitForSingleObject(completion,10000)==WAIT_OBJECT_0 &&
        GetExitCodeThread(completion,&native_exit) && CloseHandle(completion),
        "every callback state retained through actual kernel termination");
    std::printf("CPP_TLS_KERNEL mode=%s signaled=1 exit=%lu owned_handle_closed=1\n",mode,native_exit);
#endif
    Require(state.constructed==2 && state.body_left==1,"both genuine TLS objects constructed before exit");
    const bool conformant=state.a==1 && state.b==1 && state.mismatch==0 &&
        state.a_done==1 && state.b_done==1 && join_conformant;
    std::printf("CPP_TLS_CASE mode=%s constructed=%d a=%d b=%d mismatch=%d a_done=%d b_done=%d conformant=%d\n",
        mode,state.constructed.load(),state.a.load(),state.b.load(),state.mismatch.load(),
        state.a_done.load(),state.b_done.load(),conformant?1:0);
    std::printf("CPP_TLS_LATE_KEY mode=%s successful_set_get=%d destructor_calls=%d observation_only=1\n",
        mode,state.late_sets.load(),state.late_key_dtors.load());
    ++cases; if (!conformant) ++failures;
  }
#ifdef _WIN32
  if (reaper.start) {
    if (!RunHeldCase("owned_held_join",false,reaper)) ++failures;
    if (!RunHeldCase("owned_held_detach",true,reaper)) ++failures;
    cases+=2;
  }
#endif
  Require(!pthread_key_delete(late_key),"late observation key deletion");
#ifdef _WIN32
  DrainRetirer(reaper);
  Require(GetProcessHandleCount(GetCurrentProcess(),&after)!=0,"final handles");
  const bool bounded=after<=before+8 && after<=origin_before+8;
  std::printf("CPP_TLS_OWNER_HANDLES original=%lu baseline=%lu after=%lu persistent_handles=%lu original_total_bound=8 bounded=%d\n",
      origin_before,before,after,before-origin_before,bounded?1:0);
  if (!bounded) ++failures;
  std::printf("CPP_TLS_HANDLES before=%lu after=%lu original_bound=8 bounded=%d\n",before,after,bounded?1:0);
#endif
  std::printf("CPP_TLS_RESULT cases=%d failures=%d conformant=%d\n",cases,failures,failures?0:1);
  return failures?2:0;
}

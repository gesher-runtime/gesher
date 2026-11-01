// Distinct minimum-candidate diagnostics. Compile/run ONLY in reviewed CI.
// No TLS-order flags/hooks, private supplier activation, or original census edits.
#include <windows.h>
#include <pthread.h>
extern "C" {
#include "thread.h"  // Exact pristine private ABI, with the C getter linkage.
}
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>

static_assert(sizeof(_pthread_v)==480 && alignof(_pthread_v)==16);
static_assert(offsetof(_pthread_v,valid)==0 && offsetof(_pthread_v,ret_arg)==8);
static_assert(offsetof(_pthread_v,h)==40 && offsetof(_pthread_v,tid)==112);
static_assert(offsetof(_pthread_v,ended)==188 && offsetof(_pthread_v,next)==464 && offsetof(_pthread_v,x)==472);

namespace {
constexpr DWORD kBound=15000;
int failures=0;
void* const kSentinel=reinterpret_cast<void*>(std::uintptr_t{0x789a});
void* const kReturn=reinterpret_cast<void*>(std::uintptr_t{0x1234});
[[noreturn]] void Fatal(const char* where) {
  std::fprintf(stderr,"MINIMUM_SETUP_FAIL where=%s winerror=%lu state_not_returned_while_native_live=1\n",where,GetLastError());
  std::fflush(nullptr);TerminateProcess(GetCurrentProcess(),97);std::abort();
}
void Need(bool okay,const char* where){if(!okay)Fatal(where);}
void Check(bool okay,const char* label){if(!okay){++failures;std::printf("MINIMUM_ASSERT_FAIL label=%s\n",label);}}
HANDLE Event(){HANDLE h=CreateEventW(nullptr,TRUE,FALSE,nullptr);Need(h!=nullptr,"event_create");return h;}
void Wait(HANDLE h,const char* where){Need(h&&WaitForSingleObject(h,kBound)==WAIT_OBJECT_0,where);}
void Close(HANDLE h){Need(h&&CloseHandle(h)!=0,"fixture_handle_close");}
HANDLE DuplicateCurrent(){HANDLE h=nullptr;Need(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&h,SYNCHRONIZE|THREAD_QUERY_LIMITED_INFORMATION,FALSE,0)!=0,"own_actual_native_handle");return h;}
DWORD Count(){DWORD n=0;Need(GetProcessHandleCount(GetCurrentProcess(),&n)!=0,"handle_count");return n;}
void Provider(){
  HMODULE module=GetModuleHandleW(L"libwinpthread-1.dll");
  if(!module){std::puts("MINIMUM_PROVIDER dll=0");return;}
  wchar_t exe[32768],dll[32768];DWORD en=GetModuleFileNameW(nullptr,exe,32768),dn=GetModuleFileNameW(module,dll,32768);
  Need(en&&en<32768&&dn&&dn<32768,"provider_paths");
  wchar_t* slash=std::wcsrchr(exe,L'\\');Need(slash!=nullptr,"exe_directory");slash[1]=L'\0';
  constexpr wchar_t name[]=L"libwinpthread-1.dll";
  Need(std::wcslen(exe)+std::wcslen(name)<32768,"provider_path_bound");std::wcscat(exe,name);
  const bool same=_wcsicmp(exe,dll)==0;
  std::printf("MINIMUM_PROVIDER dll=1 exact_executable_directory=%d\n",same?1:0);Check(same,"adjacent_actual_imported_provider");
}
struct BodyState {
  HANDLE ready=Event(),release=Event(),native=nullptr;
  DWORD tid=0,borrowed_tid=0,borrowed_wait=WAIT_FAILED;
  pthread_t id=0;int join_result=-1,try_result=-1;
  void* join_value=kSentinel;void* try_value=kSentinel;
};
void Publish(BodyState& state){
  state.native=DuplicateCurrent();state.tid=GetCurrentThreadId();Need(SetEvent(state.ready)!=0,"body_publish");
}
void* Basic(void* raw){auto& state=*static_cast<BodyState*>(raw);Publish(state);return kReturn;}
void* SelfBody(void* raw){
  auto& state=*static_cast<BodyState*>(raw);state.id=pthread_self();
  HANDLE borrowed=static_cast<HANDLE>(pthread_gethandle(state.id));
  state.borrowed_tid=borrowed?GetThreadId(borrowed):0;state.borrowed_wait=borrowed?WaitForSingleObject(borrowed,0):WAIT_FAILED;
  Publish(state);
  std::printf("MINIMUM_SELF_BEFORE id=%llu native_tid=%lu borrowed_tid=%lu kernel=%lu\n",static_cast<unsigned long long>(state.id),state.tid,state.borrowed_tid,state.borrowed_wait);std::fflush(stdout);
  state.join_result=pthread_join(state.id,&state.join_value);
  state.try_result=_pthread_tryjoin(state.id,&state.try_value);
  std::printf("MINIMUM_SELF_RETURN id=%llu join_rc=%d try_rc=%d expected=%d join_output_unchanged=%d try_output_unchanged=%d\n",static_cast<unsigned long long>(state.id),state.join_result,state.try_result,EDEADLK,state.join_value==kSentinel,state.try_value==kSentinel);
  return kReturn;
}
DWORD WINAPI ForeignSelf(void* raw){SelfBody(raw);ExitThread(61);return 61;}
void FinishBody(BodyState& state){Wait(state.native,"body_actual_kernel_completion");Close(state.native);Close(state.ready);Close(state.release);}
void ErrorCalls(const char* label,pthread_t id,int expected){
  void* a=kSentinel;void* b=kSentinel;int join=pthread_join(id,&a),attempt=_pthread_tryjoin(id,&b);
  std::printf("MINIMUM_ERROR label=%s id=%llu join_rc=%d try_rc=%d expected=%d join_output_unchanged=%d try_output_unchanged=%d\n",label,static_cast<unsigned long long>(id),join,attempt,expected,a==kSentinel,b==kSentinel);
  Check(join==expected&&attempt==expected&&a==kSentinel&&b==kSentinel,label);
}
int Attachment(int expected){
  const DWORD cold=Count();pthread_key_t key{};Need(pthread_key_create(&key,nullptr)==0,"attachment_fresh_key");Need(pthread_key_delete(key)==0,"attachment_delete_key");
  const DWORD keys=Count();BodyState state;pthread_t worker{};Need(pthread_create(&worker,nullptr,Basic,&state)==0,"attachment_basic_create");Wait(state.ready,"attachment_body_ready");
  void* value=kSentinel;const int joined=pthread_join(worker,&value);Wait(state.native,"attachment_kernel");
  Need(joined==0&&value==kReturn,"attachment_basic_join");FinishBody(state);
  const DWORD before=Count();const pthread_t first=pthread_self();const DWORD after=Count();const pthread_t second=pthread_self();const DWORD repeated=Count();
  std::printf("MINIMUM_ATTACHMENT expected_delta=%d cold=%lu after_key_setup=%lu before_first_self=%lu after_first_self=%lu after_second_self=%lu delta=%lld repeated_delta=%lld first=%llu second=%llu native_tid=%lu observation_only=1 original_census_unchanged=1\n",expected,cold,keys,before,after,repeated,static_cast<long long>(after)-before,static_cast<long long>(repeated)-after,static_cast<unsigned long long>(first),static_cast<unsigned long long>(second),GetCurrentThreadId());
  Check(first!=0&&first==second&&static_cast<long long>(after)-before==expected&&repeated==after,"first_explicit_main_attachment");
  return failures?1:0;
}
struct Terminated {HANDLE native;DWORD old_tid;};
DWORD WINAPI ObserveTerminated(void* raw){
  auto& item=*static_cast<Terminated*>(raw);const DWORD tid=GetCurrentThreadId(),stored=GetThreadId(item.native),wait=WaitForSingleObject(item.native,0);
  const bool reused=tid==stored,would_be_self=reused&&wait==WAIT_TIMEOUT;
  std::printf("MINIMUM_TERMINATED_OBJECT old_tid=%lu actual_object_tid=%lu actual_current_tid=%lu kernel=%lu os_tid_reuse_observed=%d would_be_self=%d optional_reuse_only=1\n",item.old_tid,stored,tid,wait,reused?1:0,would_be_self?1:0);
  return stored==item.old_tid&&wait==WAIT_OBJECT_0&&!would_be_self?0:1;
}
void OwnedSelf(){
  BodyState state;pthread_t target{};Need(pthread_create(&target,nullptr,SelfBody,&state)==0,"owned_self_create");Wait(state.ready,"owned_self_ready");Wait(state.native,"owned_self_actual_kernel");
  DWORD code=0;Need(GetExitCodeThread(state.native,&code)!=0,"owned_self_exit_code");
  Check(state.id==target&&state.borrowed_tid==state.tid&&state.borrowed_wait==WAIT_TIMEOUT&&state.join_result==EDEADLK&&state.try_result==EDEADLK&&state.join_value==kSentinel&&state.try_value==kSentinel,"owned_actual_self");
  void* value=kSentinel;const int joined=pthread_join(target,&value);const DWORD final_wait=WaitForSingleObject(state.native,0);
  std::printf("MINIMUM_OWNED_SELF id=%llu native_tid=%lu borrowed_kernel=%lu join_self=%d try_self=%d later_join=%d actual_kernel=%lu native_exit=%lu return_match=%d\n",static_cast<unsigned long long>(target),state.tid,state.borrowed_wait,state.join_result,state.try_result,joined,final_wait,code,value==kReturn);
  Check(joined==0&&value==kReturn&&code==0x1234&&final_wait==WAIT_OBJECT_0,"owned_normal_join_after_kernel");
  ErrorCalls("stale_owned",target,ESRCH);
  Terminated retained{state.native,state.tid};HANDLE observer=CreateThread(nullptr,0,ObserveTerminated,&retained,0,nullptr);Need(observer!=nullptr,"terminated_observer_create");Wait(observer,"terminated_observer_kernel");DWORD observed=1;Need(GetExitCodeThread(observer,&observed)!=0,"terminated_observer_status");Check(observed==0,"retained_actual_terminated_object");Close(observer);FinishBody(state);
}
void Foreign(){
  BodyState state;HANDLE thread=CreateThread(nullptr,0,ForeignSelf,&state,0,nullptr);Need(thread!=nullptr,"foreign_native_create");Wait(state.ready,"foreign_self_ready");Wait(thread,"foreign_actual_kernel");Wait(state.native,"foreign_owned_actual_kernel");DWORD code=0;Need(GetExitCodeThread(thread,&code)!=0,"foreign_exit_code");
  std::printf("MINIMUM_FOREIGN_SELF id=%llu native_tid=%lu borrowed_tid=%lu borrowed_kernel=%lu join_self=%d try_self=%d native_exit=%lu join_output_unchanged=%d try_output_unchanged=%d\n",static_cast<unsigned long long>(state.id),state.tid,state.borrowed_tid,state.borrowed_wait,state.join_result,state.try_result,code,state.join_value==kSentinel,state.try_value==kSentinel);
  Check(state.id!=0&&state.borrowed_tid==state.tid&&state.borrowed_wait==WAIT_TIMEOUT&&state.join_result==EDEADLK&&state.try_result==EDEADLK&&state.join_value==kSentinel&&state.try_value==kSentinel&&code==61,"foreign_actual_self");
  ErrorCalls("stale_foreign_after_kernel",state.id,ESRCH);Close(thread);FinishBody(state);
}
void* Blocked(void* raw){auto& state=*static_cast<BodyState*>(raw);Publish(state);Wait(state.release,"blocked_body_release");return kReturn;}
void Detached(bool at_creation){
  BodyState state;pthread_attr_t attributes;Need(pthread_attr_init(&attributes)==0,"detach_attribute_init");Need(pthread_attr_setdetachstate(&attributes,at_creation?PTHREAD_CREATE_DETACHED:PTHREAD_CREATE_JOINABLE)==0,"detach_attribute_mode");
  pthread_t target{};Need(pthread_create(&target,&attributes,Blocked,&state)==0,"detach_actual_create");Need(pthread_attr_destroy(&attributes)==0,"detach_attribute_destroy");Wait(state.ready,"detach_actual_ready");
  if(!at_creation)Need(pthread_detach(target)==0,"detach_live_public_call");
  const HANDLE borrowed=static_cast<HANDLE>(pthread_gethandle(target));const DWORD wait=WaitForSingleObject(state.native,0);const int again=pthread_detach(target);
  const char* label=at_creation?"attr_detached":"detach_live";
  std::printf("MINIMUM_DETACHED label=%s id=%llu public_handle_null=%d native_tid=%lu independent_kernel=%lu repeat_detach=%d expected=%d\n",label,static_cast<unsigned long long>(target),borrowed==nullptr,state.tid,wait,again,ESRCH);
  Check(borrowed==nullptr&&wait==WAIT_TIMEOUT&&again==ESRCH,label);ErrorCalls(label,target,ESRCH);
  Need(SetEvent(state.release)!=0,"detach_release");Wait(state.native,"detach_actual_kernel");FinishBody(state);
}
struct HeldCase {
  HANDLE entered=Event(),release=Event(),native=nullptr;
  DWORD tid=0;volatile LONG constructed=0,returned=0;
};
struct GenuineGuard {
  HeldCase* state=nullptr;
  ~GenuineGuard(){if(state){Need(SetEvent(state->entered)!=0,"genuine_tls_enter");Wait(state->release,"genuine_tls_release");InterlockedIncrement(&state->returned);}}
};
GenuineGuard& Guard(){thread_local GenuineGuard guard;return guard;}
void* HeldBody(void* raw){auto& state=*static_cast<HeldCase*>(raw);state.native=DuplicateCurrent();state.tid=GetCurrentThreadId();Guard().state=&state;InterlockedIncrement(&state.constructed);return kReturn;}
struct ReleaseHelper {HeldCase* target;HANDLE ready=Event(),go=Event();DWORD actual_wait=WAIT_FAILED;};
DWORD WINAPI ReleaseHeld(void* raw){auto& helper=*static_cast<ReleaseHelper*>(raw);Need(SetEvent(helper.ready)!=0,"prestarted_helper_ready");Wait(helper.go,"prestarted_helper_go");Need(SetEvent(helper.target->release)!=0,"native_helper_release");helper.actual_wait=WaitForSingleObject(helper.target->native,kBound);return helper.actual_wait==WAIT_OBJECT_0?0:1;}
void Held(){
  HeldCase state;ReleaseHelper helper{&state};HANDLE releaser=CreateThread(nullptr,0,ReleaseHeld,&helper,0,nullptr);Need(releaser!=nullptr,"prestarted_native_helper_create");Wait(helper.ready,"prestarted_native_helper_entered");
  std::puts("MINIMUM_HELD_HELPER_READY before_target=1 native_only=1");
  pthread_t target{};Need(pthread_create(&target,nullptr,HeldBody,&state)==0,"genuine_held_target_create");Wait(state.entered,"genuine_default_tls_entered");
  // Read-only diagnostic while the real gate owns target lifetime. No read of
  // this record is permitted after tryjoin might consume the old bad target.
  _pthread_v* record=__pth_gpointer_locked(target);Need(record!=nullptr,"held_private_record");
  const int ended=record->ended;const pthread_t x=record->x;const DWORD stored_tid=record->tid;
  const HANDLE borrowed=record->h;const DWORD borrowed_tid=borrowed?GetThreadId(borrowed):0;
  const DWORD before=WaitForSingleObject(state.native,0);const LONG before_done=InterlockedCompareExchange(&state.returned,0,0);
  std::printf("MINIMUM_HELD_BEFORE id=%llu record_id=%llu ended=%d stored_tid=%lu actual_tid=%lu borrowed_tid=%lu kernel=%lu cpp_constructed=%ld cpp_returned=%ld private_size=%zu\n",static_cast<unsigned long long>(target),static_cast<unsigned long long>(x),ended,stored_tid,state.tid,borrowed_tid,before,state.constructed,before_done,sizeof(_pthread_v));std::fflush(stdout);
  void* result=kSentinel;const int attempt=_pthread_tryjoin(target,&result);const DWORD after=WaitForSingleObject(state.native,0);
  const bool consumed=attempt==0;
  std::printf("MINIMUM_HELD_TRYJOIN rc=%d expected=%d result_unchanged=%d result_return=%d kernel_after=%lu consumed=%d\n",attempt,EBUSY,result==kSentinel,result==kReturn,after,consumed?1:0);std::fflush(stdout);
  // From this point until true kernel completion: no record access, no new
  // pthread/TLS-dependent fixture work, and no rejoin of a consumed old ID.
  Need(SetEvent(helper.go)!=0,"release_prestarted_helper");Wait(state.native,"held_actual_kernel_completion");Wait(releaser,"prestarted_helper_kernel_completion");DWORD helper_code=1,target_code=0;Need(GetExitCodeThread(releaser,&helper_code)&&GetExitCodeThread(state.native,&target_code),"held_native_status");
  int joined=-1;void* value=kSentinel;
  if(!consumed)joined=pthread_join(target,&value);
  const LONG completed=InterlockedCompareExchange(&state.returned,0,0);
  std::printf("MINIMUM_HELD_AFTER actual_kernel=%lu cpp_returned=%ld helper_kernel=%lu helper_exit=%lu target_exit=%lu normal_join=%d normal_return=%d consumed_id_rejoined=0\n",WaitForSingleObject(state.native,0),completed,helper.actual_wait,helper_code,target_code,joined,value==kReturn);
  Check(x==target&&stored_tid==state.tid&&borrowed_tid==state.tid&&ended==1&&before==WAIT_TIMEOUT&&before_done==0&&state.constructed==1,"genuine_ended_before_kernel_crossing");
  Check(attempt==EBUSY&&result==kSentinel&&after==WAIT_TIMEOUT&&!consumed&&joined==0&&value==kReturn&&completed==1&&helper.actual_wait==WAIT_OBJECT_0&&helper_code==0&&target_code==0x1234,"tryjoin_requires_actual_kernel_completion");
  Close(releaser);Close(helper.ready);Close(helper.go);Close(state.native);Close(state.entered);Close(state.release);
}
int Contracts(){
  ErrorCalls("invalid_zero",pthread_t{0},ESRCH);ErrorCalls("invalid_max",std::numeric_limits<pthread_t>::max(),ESRCH);
  pthread_attr_t attributes;Need(pthread_attr_init(&attributes)==0,"invalid_attribute_init");const int bad=pthread_attr_setdetachstate(&attributes,12345);std::printf("MINIMUM_INVALID_ATTR rc=%d expected=%d detached_valid_handle_not_fabricated=1\n",bad,EINVAL);Check(bad==EINVAL,"invalid_attribute_einval");Need(pthread_attr_destroy(&attributes)==0,"invalid_attribute_destroy");
  OwnedSelf();Foreign();Detached(true);Detached(false);Held();
  std::printf("MINIMUM_CONTRACTS_COMPLETE failures=%d original_census_unchanged=1 lifecycle_adoption_not_claimed=1\n",failures);return failures?1:0;
}
} // namespace
int main(int argc,char** argv){
  const char* ci=std::getenv("GITHUB_ACTIONS");if(!ci||std::strcmp(ci,"true")){std::fputs("MINIMUM_CI_ONLY\n",stderr);return 2;}
  Provider();
  if(argc==3&&!std::strcmp(argv[1],"attachment")){
    if(std::strcmp(argv[2],"0")&&std::strcmp(argv[2],"2"))return 2;
    return Attachment(argv[2][0]-'0');
  }
  if(argc==2&&!std::strcmp(argv[1],"contracts"))return Contracts();
  std::fputs("usage: controls attachment <0|2> | controls contracts\n",stderr);return 2;
}

// Host POSIX calls and the actual font owner; execute only in qualified CI.
#include <gesher/font_environment.h>
#include <gesher/font_paths.h>
#include <windows.h>
#include <wct.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
using gesher::FontEnvironmentKey;
using gesher::FontEnvironmentValue;
using State = FontEnvironmentValue::State;
constexpr const char* kNames[] = {"ANDROID_ROOT", "GESHER_WINDOWS_FONTS"};
constexpr const wchar_t* kWideNames[] = {L"ANDROID_ROOT", L"GESHER_WINDOWS_FONTS"};
constexpr const char* kInitial = "initial-\xd7\x90\xd7\x91 space";
constexpr const char* kSupplementary = "initial-\xf0\x9f\x98\x80 space";
constexpr const char* kA = "owned-A-\xd7\x90\xd7\x91 space";
constexpr const char* kB = "owned-B-\xd7\x92\xd7\x93 space";
constexpr const char* kLater = "wide-later-\xd7\x92 space";
void (*g_failure_cleanup)() = nullptr;
bool HeldCallback() noexcept;
[[noreturn]] void Die(const char* message) {
  DWORD last = GetLastError(); int saved_errno = errno;
  if (g_failure_cleanup) g_failure_cleanup();
  std::fprintf(stderr,"FONT_ENV_HOST_FAIL %s errno=%d winerror=%lu\n",message,saved_errno,last);
  ExitProcess(1); // Never return stack-backed state while a worker may live.
}
void Require(bool value,const char* message){if(!value)Die(message);}
FontEnvironmentKey Key(int key){return static_cast<FontEnvironmentKey>(key);}
void InitialWide(int key,std::string_view mode){
  wchar_t wide[512]{};SetLastError(ERROR_SUCCESS);
  DWORD size=GetEnvironmentVariableW(kWideNames[key],wide,512),error=GetLastError();
  Require(size<512,"bounded initial wide receipt");
  std::printf("FONT_ENV_HOST_INITIAL_WIDE key=%s present=%d units=%lu error=%lu utf16=",kNames[key],!(size==0&&error==ERROR_ENVVAR_NOT_FOUND),size,error);
  for(DWORD i=0;i<size;++i)std::printf("%s%04x",i?",":"",static_cast<unsigned>(wide[i]));
  std::puts("");
  if(mode=="missing"){Require(size==0&&error==ERROR_ENVVAR_NOT_FOUND,"actual missing OS value");return;}
  if(mode=="empty"){Require(size==0&&error==ERROR_SUCCESS,"actual present empty OS value");return;}
  const wchar_t invalid[]={static_cast<wchar_t>(0xd800),0};
  std::wstring_view expected=mode=="wide"?std::wstring_view(L"initial-\u05d0\u05d1 space"):
    mode=="supplementary"?std::wstring_view(L"initial-\U0001f600 space"):std::wstring_view(invalid,1);
  Require(size==expected.size(),"actual initial UTF16 length");
  for(DWORD i=0;i<size;++i)Require(wide[i]==expected[i],"actual initial UTF16 unit");
}
FontEnvironmentValue Check(int key,State state,std::string_view wanted){
  auto actual=gesher::ReadFontEnvironment(Key(key));
  Require(actual.state==state&&actual.value()==wanted,"snapshot state/bytes");return actual;
}
std::atomic<int> g_invalid{0};
std::atomic<bool> g_reentered{false};
void __cdecl Invalid(const wchar_t*,const wchar_t*,const wchar_t*,unsigned int,uintptr_t){
  g_invalid.fetch_add(1);if(HeldCallback())return;
  auto value=gesher::ReadFontEnvironment(Key(0));
  g_reentered.store(value.state==State::kValue&&value.value()==kA);
}
// Public WCT accepts a caller buffer of up to16 nodes; MinGW omits the SDK macro.
constexpr DWORD kWaitChainCapacity = 16;
// Diagnostic module identity only: the address may resolve to an EXE thunk.
// Discover before entering any held invalid handler; never load a module here.
void ModuleProfile(const char* label, uintptr_t address, HMODULE named=nullptr){
  HMODULE module=named;SetLastError(ERROR_SUCCESS);
  BOOL located=module?TRUE:GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&module);
  DWORD locate_error=GetLastError();wchar_t path[32768]{};
  SetLastError(ERROR_SUCCESS);DWORD length=located?GetModuleFileNameW(module,path,32768):0;
  DWORD path_error=GetLastError();bool valid=located&&length>0&&length<32768;
  std::printf("FONT_ENV_MODULE label=%s address=0x%llx base=0x%llx offset=0x%llx located=%d locate_error=%lu path_valid=%d path_error=%lu units=%lu utf16=",label,static_cast<unsigned long long>(address),reinterpret_cast<unsigned long long>(module),located&&address?static_cast<unsigned long long>(address-reinterpret_cast<uintptr_t>(module)):0,located,locate_error,valid,path_error,valid?length:0);
  if(valid)for(DWORD i=0;i<length;++i)std::printf("%s%04x",i?",":"",static_cast<unsigned>(path[i]));
  std::puts("");
}
// WCT is used only by this CI fixture and only for its own process threads.
// All setup/lazy WCT work and the writer body precede the held CRT callback.
struct Cycle {
  HANDLE token=nullptr,writer_ready=nullptr,writer_go=nullptr,entered=nullptr,proceed=nullptr;
  HANDLE writer=nullptr,handler=nullptr;
  HWCT session=nullptr;
  TOKEN_PRIVILEGES previous_privilege{};
  bool privilege_saved=false,handler_installed=false,cleaned=false,cleanup_ok=true;
  _invalid_parameter_handler previous_handler=nullptr;
  DWORD writer_tid=0,handler_tid=0;
  std::string invalid_value=std::string(32767,'x');
  std::atomic<bool> read_ok{false},handler_wait_ok{false};
  std::atomic<int> writer_result{-99},handler_result{-99},handler_errno{0};
  std::atomic<DWORD> writer_body_tid{0},handler_body_tid{0},callback_tid{0};
  std::atomic<int> writer_stage{0},handler_stage{0},callback_stage{0},writer_errno{0};
  void DiagnosticState(const char* phase) noexcept;
  bool Cleanup() noexcept;
};
std::atomic<Cycle*> g_cycle{nullptr};
void FailureCycleCleanup(){if(auto* cycle=g_cycle.load())cycle->Cleanup();}
void Cycle::DiagnosticState(const char* phase) noexcept {
  DWORD handler_wait=handler?WaitForSingleObject(handler,0):WAIT_FAILED;
  DWORD writer_wait=writer?WaitForSingleObject(writer,0):WAIT_FAILED;
  std::printf("FONT_ENV_WCT_STATE phase=%s pid=%lu observer_tid=%lu writer_tid=%lu handler_tid=%lu writer_body_tid=%lu handler_body_tid=%lu callback_tid=%lu writer_stage=%d handler_stage=%d callback_stage=%d writer_result=%d writer_errno=%d handler_result=%d handler_errno=%d handler_wait_ok=%d read_ok=%d handler_kernel_wait=%lu writer_kernel_wait=%lu\n",phase,GetCurrentProcessId(),GetCurrentThreadId(),writer_tid,handler_tid,writer_body_tid.load(),handler_body_tid.load(),callback_tid.load(),writer_stage.load(),handler_stage.load(),callback_stage.load(),writer_result.load(),writer_errno.load(),handler_result.load(),handler_errno.load(),handler_wait_ok.load(),read_ok.load(),handler_wait,writer_wait);
}
bool Cycle::Cleanup() noexcept {
  if(cleaned)return cleanup_ok;
  cleaned=true;
  DiagnosticState("before_cleanup");
  if(writer_go)SetEvent(writer_go);
  if(proceed)SetEvent(proceed);
  for(HANDLE thread:{handler,writer})if(thread&&WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0)cleanup_ok=false;
  DiagnosticState("after_release_completion");
  if(handler_installed){_set_invalid_parameter_handler(previous_handler);handler_installed=false;}
  if(session){CloseThreadWaitChainSession(session);session=nullptr;}
  if(privilege_saved){
    SetLastError(ERROR_SUCCESS);
    BOOL restored=AdjustTokenPrivileges(token,FALSE,&previous_privilege,0,nullptr,nullptr);
    DWORD error=GetLastError();
    std::printf("FONT_ENV_WCT_PRIVILEGE_RESTORE success=%d error=%lu\n",restored&&error==ERROR_SUCCESS,error);
    if(!restored||error!=ERROR_SUCCESS)cleanup_ok=false;
    privilege_saved=false;
  }
  for(HANDLE handle:{handler,writer,writer_ready,writer_go,entered,proceed,token})
    if(handle&&!CloseHandle(handle))cleanup_ok=false;
  g_cycle.store(nullptr);g_failure_cleanup=nullptr;
  return cleanup_ok;
}
bool HeldCallback() noexcept {
  auto* cycle=g_cycle.load();if(!cycle)return false;
  cycle->callback_tid.store(GetCurrentThreadId());cycle->callback_stage.store(1);
  if(!SetEvent(cycle->entered)||WaitForSingleObject(cycle->proceed,15000)!=WAIT_OBJECT_0)return true;
  cycle->callback_stage.store(2);
  cycle->handler_wait_ok.store(true);
  auto value=gesher::ReadFontEnvironment(Key(0));
  cycle->read_ok.store(value.state==State::kValue&&value.value()==kA);
  cycle->callback_stage.store(3);
  return true;
}
DWORD WINAPI HeldWriter(void* argument){
  auto& cycle=*static_cast<Cycle*>(argument);
  cycle.writer_body_tid.store(GetCurrentThreadId());cycle.writer_stage.store(1);
  if(!SetEvent(cycle.writer_ready)||WaitForSingleObject(cycle.writer_go,15000)!=WAIT_OBJECT_0)return 1;
  cycle.writer_stage.store(2);
  // Existing owned key + unset has zero owner-buffer preparatory allocation.
  errno=0;cycle.writer_stage.store(3);
  int result=unsetenv("ANDROID_ROOT");int error=errno;
  cycle.writer_result.store(result);cycle.writer_errno.store(error);cycle.writer_stage.store(4);return 0;
}
DWORD WINAPI HeldInvalid(void* argument){
  auto& cycle=*static_cast<Cycle*>(argument);errno=0;
  cycle.handler_body_tid.store(GetCurrentThreadId());cycle.handler_stage.store(1);
  cycle.handler_result.store(setenv("ANDROID_ROOT",cycle.invalid_value.c_str(),1));
  cycle.handler_errno.store(errno);cycle.handler_stage.store(2);return 0;
}
void ConcurrentReadCallback(){
  Require(setenv("ANDROID_ROOT",kA,1)==0,"prepare held CRT owner");
  auto retained=Check(0,State::kValue,kA);
  ModuleProfile("putenv_s",reinterpret_cast<uintptr_t>(&_putenv_s));
  ModuleProfile("getenv",reinterpret_cast<uintptr_t>(&std::getenv));
  HMODULE ucrt=GetModuleHandleW(L"ucrtbase.dll");
  if(ucrt)ModuleProfile("already_loaded_ucrtbase",0,ucrt);
  else std::puts("FONT_ENV_MODULE label=already_loaded_ucrtbase path_valid=0 diagnostic_error=NOT_ALREADY_LOADED");
  Cycle cycle;g_cycle.store(&cycle);g_failure_cleanup=FailureCycleCleanup;
  Require(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&cycle.token),"WCT own-token prerequisite");
  TOKEN_PRIVILEGES requested{};requested.PrivilegeCount=1;
  Require(LookupPrivilegeValueW(nullptr,L"SeDebugPrivilege",&requested.Privileges[0].Luid),"WCT privilege lookup");
  requested.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;DWORD previous_size=sizeof(cycle.previous_privilege);
  SetLastError(ERROR_SUCCESS);
  BOOL adjusted=AdjustTokenPrivileges(cycle.token,FALSE,&requested,sizeof(cycle.previous_privilege),&cycle.previous_privilege,&previous_size);
  DWORD privilege_error=GetLastError();cycle.privilege_saved=adjusted!=FALSE;
  std::printf("FONT_ENV_WCT_PRIVILEGE prerequisite_adjusted=%d error=%lu previous_count=%lu scope=OWN_PROCESS_ONLY\n",adjusted,privilege_error,cycle.previous_privilege.PrivilegeCount);
  Require(adjusted&&privilege_error==ERROR_SUCCESS,"WCT missing privilege prerequisite");
  cycle.session=OpenThreadWaitChainSession(0,nullptr);Require(cycle.session!=nullptr,"synchronous WCT prerequisite");
  WAITCHAIN_NODE_INFO nodes[kWaitChainCapacity]{};DWORD count=kWaitChainCapacity;BOOL is_cycle=FALSE;
  SetLastError(ERROR_SUCCESS);BOOL prewarm=GetThreadWaitChain(cycle.session,0,0,GetCurrentThreadId(),&count,nodes,&is_cycle);DWORD prewarm_error=GetLastError();
  std::printf("FONT_ENV_WCT_QUERY phase=prewarm sample=0 success=%d count=%lu cycle=%d error=%lu error_meaningful=%d\n",prewarm,count,is_cycle,prewarm_error,!prewarm);
  Require(prewarm,"WCT prewarm own observer");
  cycle.writer_ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);cycle.writer_go=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  cycle.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);cycle.proceed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  Require(cycle.writer_ready&&cycle.writer_go&&cycle.entered&&cycle.proceed,"held choreography events");
  cycle.writer=CreateThread(nullptr,0,HeldWriter,&cycle,0,&cycle.writer_tid);
  Require(cycle.writer&&WaitForSingleObject(cycle.writer_ready,10000)==WAIT_OBJECT_0,"prestarted native writer body");
  std::printf("FONT_ENV_WCT_PRESTART observer_tid=%lu writer_tid=%lu session_warmed=1 writer_body_ready=1\n",GetCurrentThreadId(),cycle.writer_tid);
  int invalid_before=g_invalid.load();
  cycle.previous_handler=_set_invalid_parameter_handler(Invalid);cycle.handler_installed=true;
  cycle.handler=CreateThread(nullptr,0,HeldInvalid,&cycle,0,&cycle.handler_tid);
  Require(cycle.handler&&WaitForSingleObject(cycle.entered,10000)==WAIT_OBJECT_0,"actual held CRT handler");
  cycle.DiagnosticState("handler_entered_before_writer_release");
  Require(SetEvent(cycle.writer_go),"release prestarted tracked unset");
  bool observed=false;ULONGLONG deadline=GetTickCount64()+10000;
  unsigned sample=0,dumps=0;DWORD previous_count=0;BOOL previous_cycle=FALSE;
  WAITCHAIN_NODE_INFO previous_nodes[kWaitChainCapacity]{};
  do{
    count=kWaitChainCapacity;is_cycle=FALSE;
    SetLastError(ERROR_SUCCESS);BOOL query=GetThreadWaitChain(cycle.session,0,0,cycle.writer_tid,&count,nodes,&is_cycle);DWORD query_error=GetLastError();
    ++sample;if(sample<=1024)std::printf("FONT_ENV_WCT_QUERY phase=writer sample=%u success=%d count=%lu cycle=%d error=%lu error_meaningful=%d\n",sample,query,count,is_cycle,query_error,!query);
    bool changed=count!=previous_count||is_cycle!=previous_cycle;
    for(DWORD i=0;!changed&&i<count&&i<kWaitChainCapacity;++i){
      const auto& a=nodes[i];const auto& b=previous_nodes[i];
      changed=a.ObjectType!=b.ObjectType||a.ObjectStatus!=b.ObjectStatus||(a.ObjectType==WctThreadType&&(a.ThreadObject.ProcessId!=b.ThreadObject.ProcessId||a.ThreadObject.ThreadId!=b.ThreadObject.ThreadId));
    }
    if(dumps<32&&(sample<=4||changed||!query)){
      for(DWORD i=0;i<count&&i<kWaitChainCapacity;++i)std::printf("FONT_ENV_WCT_SAMPLE_NODE sample=%u index=%lu type=%u status=%u pid=%lu tid=%lu\n",sample,i,static_cast<unsigned>(nodes[i].ObjectType),static_cast<unsigned>(nodes[i].ObjectStatus),nodes[i].ObjectType==WctThreadType?nodes[i].ThreadObject.ProcessId:0,nodes[i].ObjectType==WctThreadType?nodes[i].ThreadObject.ThreadId:0);
      cycle.DiagnosticState("sample");++dumps;
    }
    previous_count=count;previous_cycle=is_cycle;std::memcpy(previous_nodes,nodes,sizeof(nodes));
    Require(query,"own writer WCT retrieval prerequisite");
    observed=count>=3&&!is_cycle&&nodes[0].ObjectType==WctThreadType&&nodes[0].ObjectStatus==WctStatusBlocked&&
      nodes[0].ThreadObject.ProcessId==GetCurrentProcessId()&&nodes[0].ThreadObject.ThreadId==cycle.writer_tid&&
      nodes[1].ObjectType==WctCriticalSectionType&&nodes[1].ObjectStatus==WctStatusOwned&&
      nodes[2].ObjectType==WctThreadType&&nodes[2].ThreadObject.ProcessId==GetCurrentProcessId()&&nodes[2].ThreadObject.ThreadId==cycle.handler_tid;
    if(!observed)Sleep(10);
  }while(!observed&&GetTickCount64()<deadline);
  std::printf("FONT_ENV_WCT_SAMPLE_TOTAL queries=%u query_record_limit=1024 full_dumps=%u full_dump_limit=32\n",sample,dumps);
  cycle.DiagnosticState("final_before_ownership_gate");
  for(DWORD i=0;i<count&&i<kWaitChainCapacity;++i)
    std::printf("FONT_ENV_WCT_NODE index=%lu type=%u status=%u pid=%lu tid=%lu\n",i,static_cast<unsigned>(nodes[i].ObjectType),static_cast<unsigned>(nodes[i].ObjectStatus),nodes[i].ObjectType==WctThreadType?nodes[i].ThreadObject.ProcessId:0,nodes[i].ObjectType==WctThreadType?nodes[i].ThreadObject.ThreadId:0);
  Require(observed&&WaitForSingleObject(cycle.writer,0)==WAIT_TIMEOUT&&!cycle.handler_wait_ok.load(),"positive writer to CRT critical-section to held handler ownership");
  Require(SetEvent(cycle.proceed),"allow held handler snapshot read");
  HANDLE threads[]={cycle.handler,cycle.writer};
  Require(WaitForMultipleObjects(2,threads,TRUE,15000)==WAIT_OBJECT_0,"true held reader/writer completion");
  Require(cycle.handler_wait_ok.load()&&cycle.read_ok.load()&&cycle.writer_result.load()==0&&cycle.handler_result.load()==-1&&cycle.handler_errno.load()==EINVAL&&g_invalid.load()==invalid_before+1,"held read callback outcome");
  Require(retained.value()==kA,"retained owner after held unset");Check(0,State::kAbsent,"");
  Require(cycle.Cleanup(),"restore exact privilege/handler and close WCT/token/native handles");
  std::puts("FONT_ENV_WCT_PASS own_process=1 specific_writer_wait=CRITICAL_SECTION held_owner=INVALID_HANDLER concurrent_read_callback=1 unset_writer=1 true_kernel_completion=1 privilege_restored=1 session_token_handles_closed=1 arbitrary_writer_reentry_qualified=0");
}

struct Work{HANDLE start;bool writer;std::atomic<int>* failures;};
DWORD WINAPI Worker(void* argument){
  auto& work=*static_cast<Work*>(argument);
  if(WaitForSingleObject(work.start,10000)!=WAIT_OBJECT_0)ExitProcess(1);
  for(int i=0;i<1000;++i){
    if(work.writer){if(setenv("ANDROID_ROOT",(i&1)?kA:kB,1)!=0)work.failures->fetch_add(1);}
    else{
      auto value=gesher::ReadFontEnvironment(Key(0));
      if(value.state!=State::kValue||(value.value()!=kA&&value.value()!=kB))work.failures->fetch_add(1);
      auto copy=value;auto moved=std::move(value);
      if(copy.value()!=moved.value())work.failures->fetch_add(1);
    }
  }return 0;
}
int main(int argc,char** argv){
  Require(std::getenv("GITHUB_ACTIONS")&&std::strcmp(std::getenv("GITHUB_ACTIONS"),"true")==0,"CI execution only");
  Require(argc==2,"one explicit initial-state mode");std::string_view mode(argv[1]);
  Require(mode=="wide"||mode=="missing"||mode=="empty"||mode=="invalid-wide"||mode=="supplementary","initial mode");
  State state=mode=="missing"?State::kAbsent:mode=="invalid-wide"?State::kError:State::kValue;
  std::string_view initial=mode=="wide"?kInitial:mode=="supplementary"?kSupplementary:"";
  InitialWide(0,mode);InitialWide(1,mode);
  auto retained=Check(0,state,initial);Check(1,state,initial);
  if(mode=="invalid-wide"){
    auto files=gesher::SystemFontFiles();
    Require(files.xml.empty()&&files.directory.empty(),"invalid root used installed defaults");
  }
  for(int key=0;key<2;++key){
    bool existed=std::getenv(kNames[key])!=nullptr;
    Require(setenv(kNames[key],kA,0)==0,"overwrite0");
    if(existed){
      Check(key,state,initial);
      Require(SetEnvironmentVariableW(kWideNames[key],L"wide-later-\u05d2 space"),"unowned OS write");
      Check(key,State::kValue,kLater); // Skipped POSIX call did not take ownership.
    }else Check(key,State::kValue,kA);
    Require(setenv(kNames[key],kA,1)==0,"native UTF8 setenv");auto old=Check(key,State::kValue,kA);
    Require(SetEnvironmentVariableW(kWideNames[key],L"external-untracked"),"owned OS write");
    Check(key,State::kValue,kA);
    Require(setenv(kNames[key],kB,1)==0,"native replacement");Check(key,State::kValue,kB);
    Require(old.value()==kA,"retained snapshot changed/freed");
    auto copy=old;auto moved=std::move(old);
    Require(copy.value()==kA&&moved.value()==kA,"snapshot copy/move");
    Require(unsetenv(kNames[key])==0,"native unsetenv");Check(key,State::kAbsent,"");
    Require(SetEnvironmentVariableW(kWideNames[key],L"external-after-unset"),"OS write after unset");
    Check(key,State::kAbsent,""); // Explicit tombstone, not Unrecorded again.
    Require(setenv(kNames[key],kA,0)==0,"overwrite0 after removal");Check(key,State::kValue,kA);
    Require(setenv(kNames[key],"",1)==0,"legacy empty-value removal");Check(key,State::kAbsent,"");
    std::printf("FONT_ENV_HOST_KEY key=%s initial_crt_present=%d overwrite0=1 retained_snapshot=1 external_owned_policy=1 unset_tombstone=1 legacy_empty_removal=1\n",kNames[key],existed);
  }
  Require(retained.state==state&&retained.value()==initial,"initial snapshot lifetime");
  Require(setenv("android_root",kA,1)==0,"ASCII key alias");Check(0,State::kValue,kA);
  Require(setenv("GESHER_FONT_ENV_UNRELATED","canary",1)==0,"unrelated setenv");
  Require(std::getenv("GESHER_FONT_ENV_UNRELATED")&&std::strcmp(std::getenv("GESHER_FONT_ENV_UNRELATED"),"canary")==0,"unrelated getter");
  Check(0,State::kValue,kA);
  Require(unsetenv("GESHER_FONT_ENV_UNRELATED")==0&&!std::getenv("GESHER_FONT_ENV_UNRELATED"),"unrelated unsetenv");
  // Documented PRE-mutation invalid-parameter failure; callback re-enters the
  // reader to prove this preserved invalid-input path holds no owner lock.
  std::string long_value(32767,'x');auto previous=_set_invalid_parameter_handler(Invalid);
  errno=0;int failed=setenv("ANDROID_ROOT",long_value.c_str(),1);int error=errno;
  _set_invalid_parameter_handler(previous);
  Require(failed==-1&&error==EINVAL&&g_invalid.load()==1&&g_reentered.load(),"pre-mutation failure/callback");
  Check(0,State::kValue,kA);
  Require(std::getenv("ANDROID_ROOT")&&std::strcmp(std::getenv("ANDROID_ROOT"),kA)==0,"pre-mutation CRT change");
  Require(setenv("ANDROID_ROOT","\xff",1)==0,"owned raw-byte control");Check(0,State::kError,"");
  auto files=gesher::SystemFontFiles();Require(files.xml.empty()&&files.directory.empty(),"invalid bytes used defaults");
  ConcurrentReadCallback();
  Require(setenv("ANDROID_ROOT",kA,1)==0,"restore before concurrency");
  HANDLE start=CreateEventW(nullptr,TRUE,FALSE,nullptr);Require(start!=nullptr,"start event");
  std::atomic<int> failures{0};Work work[]={{start,true,&failures},{start,false,&failures}};
  HANDLE threads[]={CreateThread(nullptr,0,Worker,&work[0],0,nullptr),CreateThread(nullptr,0,Worker,&work[1],0,nullptr)};
  Require(threads[0]&&threads[1]&&SetEvent(start),"workers/start");
  Require(WaitForMultipleObjects(2,threads,TRUE,15000)==WAIT_OBJECT_0,"true worker completion");
  DWORD codes[2]{};
  Require(GetExitCodeThread(threads[0],&codes[0])&&GetExitCodeThread(threads[1],&codes[1])&&codes[0]==0&&codes[1]==0&&failures.load()==0,"concurrent snapshots");
  Require(CloseHandle(threads[0])&&CloseHandle(threads[1])&&CloseHandle(start),"worker handle cleanup");
  std::printf("FONT_ENV_HOST_PASS mode=%s two_keys=1 initial_wide=1 copy_move_lifetime=1 native_POSIX=1 overwrite0=1 callback_failure=PRE_MUTATION_READ_ONLY concurrent_read_callback_WCT=1 writer_callback_reentrancy_qualification=0 preparation_oom_qualification=0 raw_partial_rollback_claim=0 invalid_owned_fail_closed=1 concurrency=1000 handles_closed=1 global_getenv_repair=0 font_fallback_qualified=0\n",argv[1]);
  return 0;
}

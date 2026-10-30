// Host POSIX calls and the actual font owner; execute only in qualified CI.
#include <gesher/font_environment.h>
#include <gesher/font_paths.h>
#include <windows.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
[[noreturn]] void Die(const char* message) {
  std::fprintf(stderr,"FONT_ENV_HOST_FAIL %s errno=%d winerror=%lu\n",message,errno,GetLastError());
  ExitProcess(1); // Never return stack-backed state while a worker may live.
}
void Require(bool value,const char* message){if(!value)Die(message);}
FontEnvironmentKey Key(int key){return static_cast<FontEnvironmentKey>(key);}
FontEnvironmentValue Check(int key,State state,std::string_view wanted){
  auto actual=gesher::ReadFontEnvironment(Key(key));
  Require(actual.state==state&&actual.value()==wanted,"snapshot state/bytes");return actual;
}
std::atomic<int> g_invalid{0};
std::atomic<bool> g_reentered{false};
void __cdecl Invalid(const wchar_t*,const wchar_t*,const wchar_t*,unsigned int,uintptr_t){
  g_invalid.fetch_add(1);auto value=gesher::ReadFontEnvironment(Key(0));
  g_reentered.store(value.state==State::kValue&&value.value()==kA);
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
  Require(argc==2,"one explicit initial-state mode");std::string_view mode(argv[1]);
  Require(mode=="wide"||mode=="missing"||mode=="empty"||mode=="invalid-wide"||mode=="supplementary","initial mode");
  State state=mode=="missing"?State::kAbsent:mode=="invalid-wide"?State::kError:State::kValue;
  std::string_view initial=mode=="wide"?kInitial:mode=="supplementary"?kSupplementary:"";
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
  Require(setenv("ANDROID_ROOT",kA,1)==0,"restore before concurrency");
  HANDLE start=CreateEventW(nullptr,TRUE,FALSE,nullptr);Require(start!=nullptr,"start event");
  std::atomic<int> failures{0};Work work[]={{start,true,&failures},{start,false,&failures}};
  HANDLE threads[]={CreateThread(nullptr,0,Worker,&work[0],0,nullptr),CreateThread(nullptr,0,Worker,&work[1],0,nullptr)};
  Require(threads[0]&&threads[1]&&SetEvent(start),"workers/start");
  Require(WaitForMultipleObjects(2,threads,TRUE,15000)==WAIT_OBJECT_0,"true worker completion");
  DWORD codes[2]{};
  Require(GetExitCodeThread(threads[0],&codes[0])&&GetExitCodeThread(threads[1],&codes[1])&&codes[0]==0&&codes[1]==0&&failures.load()==0,"concurrent snapshots");
  Require(CloseHandle(threads[0])&&CloseHandle(threads[1])&&CloseHandle(start),"worker handle cleanup");
  std::printf("FONT_ENV_HOST_PASS mode=%s two_keys=1 initial_wide=1 copy_move_lifetime=1 native_POSIX=1 overwrite0=1 callback_failure=PRE_MUTATION raw_partial_rollback_claim=0 invalid_owned_fail_closed=1 concurrency=1000 handles_closed=1 global_getenv_repair=0 font_fallback_qualified=0\n",argv[1]);
  return 0;
}

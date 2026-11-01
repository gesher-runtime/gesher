// CI-only NEW diagnostic. No WCT replacement or resource-owner assertion.
#include <gesher/font_environment.h>
#include <gesher/font_paths.h>
#include <windows.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <initializer_list>

namespace {
constexpr char kOwned[] = "resource-owned-ascii";
struct State {
  HANDLE done=nullptr, start=nullptr, ready=nullptr, entered=nullptr, release=nullptr, writer_go=nullptr;
  HANDLE handler=nullptr, writer=nullptr, watchdog=nullptr;
  DWORD handler_tid=0, writer_tid=0;
  char invalid[32768]{};
  std::atomic<int> writer_stage{0}, handler_stage{0}, writer_result{-99}, handler_result{-99};
  std::atomic<int> writer_error{0}, handler_error{0};
  std::atomic<bool> callback_read{false}, callback_wait{false};
  _invalid_parameter_handler previous=nullptr;
};
std::atomic<State*> g_state{nullptr};
[[noreturn]] void Fail() {
  // Process termination is the quarantine boundary. Never unwind/free live state.
  if(auto* state=g_state.load()) { SetEvent(state->release); SetEvent(state->writer_go); SetEvent(state->start); }
  ExitProcess(1);
}
void Check(bool ok){if(!ok)Fail();}
void Record(const char* label, State* state=nullptr) {
  // Fixed buffer, decimal formatter and Win32 pipe only: no CRT formatting while held.
  char buffer[512];size_t used=0;
  auto text=[&](const char* value){while(*value){Check(used<sizeof(buffer));buffer[used++]=*value++;}};
  auto number=[&](DWORD value){char digits[10];unsigned n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){Check(used<sizeof(buffer));buffer[used++]=digits[--n];}};
  text(label);text(" pid=");number(GetCurrentProcessId());
  if(state){text(" writer=");number(state->writer_tid);text(" handler=");number(state->handler_tid);}
  text("\n");DWORD written=0;Check(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),buffer,static_cast<DWORD>(used),&written,nullptr)&&written==used);
}
bool Command(const char* expected) {
  char value[16]{};DWORD count=0;
  for(unsigned i=0;i<sizeof(value)-1;++i){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),value+i,1,&got,nullptr)||got!=1)return false;if(value[i]=='\n'){count=i;break;}if(i==sizeof(value)-2)return false;}
  if(count&&value[count-1]=='\r')--count;
  return count==std::strlen(expected)&&std::memcmp(value,expected,count)==0;
}
void __cdecl Invalid(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t) {
  auto* state=g_state.load();if(!state)return;
  state->handler_stage.store(2);
  if(!SetEvent(state->entered))return;
  if(WaitForSingleObject(state->release,180000)!=WAIT_OBJECT_0)return;
  state->callback_wait.store(true);
  const auto snapshot=gesher::ReadFontEnvironment(gesher::FontEnvironmentKey::kAndroidRoot);
  state->callback_read.store(snapshot.state==gesher::FontEnvironmentValue::State::kValue&&snapshot.value()==kOwned);
  state->handler_stage.store(3);
}
DWORD WINAPI Writer(void* value) {
  auto* state=static_cast<State*>(value);state->writer_stage.store(1);
  if(!SetEvent(state->ready)||WaitForSingleObject(state->writer_go,300000)!=WAIT_OBJECT_0)return 1;
  errno=0;state->writer_stage.store(3);
  state->writer_result.store(unsetenv("ANDROID_ROOT"));state->writer_error.store(errno);
  state->writer_stage.store(4);return 0;
}
DWORD WINAPI Handler(void* value) {
  auto* state=static_cast<State*>(value);
  if(WaitForSingleObject(state->start,300000)!=WAIT_OBJECT_0)return 1;
  state->handler_stage.store(1);errno=0;
  state->handler_result.store(setenv("ANDROID_ROOT",state->invalid,1));state->handler_error.store(errno);return 0;
}
DWORD WINAPI Watchdog(void* value) {
  auto* state=static_cast<State*>(value);
  if(WaitForSingleObject(state->done,330000)!=WAIT_OBJECT_0)Fail();return 0;
}
}
int main() {
  Check(std::getenv("GITHUB_ACTIONS")&&std::strcmp(std::getenv("GITHUB_ACTIONS"),"true")==0);
  auto* state=new(std::nothrow) State;Check(state!=nullptr);g_state.store(state);
  std::memset(state->invalid,'x',32767);
  Check(setenv("ANDROID_ROOT",kOwned,1)==0);
  // Force the actual font-path reader into this same owner-linked image too.
  const auto files=gesher::SystemFontFiles();Check(!files.xml.empty());
  state->done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  state->start=CreateEventW(nullptr,TRUE,FALSE,nullptr);state->ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  state->entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);state->release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  state->writer_go=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  Check(state->done&&state->start&&state->ready&&state->entered&&state->release&&state->writer_go);
  state->previous=_set_invalid_parameter_handler(Invalid);
  state->writer=CreateThread(nullptr,0,Writer,state,0,&state->writer_tid);
  state->handler=CreateThread(nullptr,0,Handler,state,0,&state->handler_tid);
  state->watchdog=CreateThread(nullptr,0,Watchdog,state,0,nullptr);
  Check(state->writer&&state->handler&&state->watchdog&&WaitForSingleObject(state->ready,10000)==WAIT_OBJECT_0);
  Record("FONT_RESOURCE_PREPARED",state);
  Check(Command("START"));Check(SetEvent(state->start));
  Check(WaitForSingleObject(state->entered,10000)==WAIT_OBJECT_0);Check(SetEvent(state->writer_go));
  const auto deadline=GetTickCount64()+10000;
  while(state->writer_stage.load()!=3){Check(GetTickCount64()<deadline);Sleep(1);}
  Check(state->handler_stage.load()==2&&WaitForSingleObject(state->handler,0)==WAIT_TIMEOUT&&WaitForSingleObject(state->writer,0)==WAIT_TIMEOUT);
  Record("FONT_RESOURCE_HOLD_READY",state); // Preconditions ONLY, not owner proof.
  Check(Command("RELEASE"));Check(SetEvent(state->release));
  Check(WaitForSingleObject(state->handler,10000)==WAIT_OBJECT_0&&WaitForSingleObject(state->writer,10000)==WAIT_OBJECT_0);
  DWORD writer_exit=99,handler_exit=99;
  Check(GetExitCodeThread(state->writer,&writer_exit)&&GetExitCodeThread(state->handler,&handler_exit)&&writer_exit==0&&handler_exit==0);
  Check(state->callback_wait.load()&&state->callback_read.load()&&state->writer_result.load()==0&&state->writer_error.load()==0&&state->handler_result.load()==-1&&state->handler_error.load()==EINVAL);
  _set_invalid_parameter_handler(state->previous);
  Check(SetEvent(state->done)&&WaitForSingleObject(state->watchdog,10000)==WAIT_OBJECT_0);
  for(HANDLE handle:{state->writer,state->handler,state->done,state->start,state->ready,state->entered,state->release,state->writer_go,state->watchdog})Check(CloseHandle(handle));
  Record("FONT_RESOURCE_COMPLETE",state);g_state.store(nullptr);delete state;return 0;
}

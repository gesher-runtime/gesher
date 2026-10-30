// Genuine default-compiler cross-TU thread_local destructors; no order hook.
#include "pthread_late_cpp_shared.h"
#include <cstdio>
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
}
static void *Body(void *arg) {
  auto *state=static_cast<LateCppCase *>(arg);
  state->expected=pthread_self(); state->native=NativeId();
  Require(state->expected!=pthread_t{} && state->native!=0,"initial worker identities");
  LateCppTouchA(state); LateCppTouchB(state);
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
int main() {
  setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
  DWORD before=0,after=0; Require(GetProcessHandleCount(GetCurrentProcess(),&before)!=0,"initial handles");
#endif
  const char *modes[]={"pthread_return","pthread_exit","std_thread_return"
#ifdef _WIN32
    ,"native_ExitThread"
#endif
  };
  int failures=0,cases=0;
  for (const char *mode:modes) {
    LateCppCase state{mode};
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
    Require(state.constructed==2 && state.body_left==1,"both genuine TLS objects constructed before exit");
    const bool conformant=state.a==1 && state.b==1 && state.mismatch==0;
    std::printf("CPP_TLS_CASE mode=%s constructed=%d a=%d b=%d mismatch=%d conformant=%d\n",
        mode,state.constructed.load(),state.a.load(),state.b.load(),state.mismatch.load(),conformant?1:0);
    ++cases; if (!conformant) ++failures;
  }
#ifdef _WIN32
  Require(GetProcessHandleCount(GetCurrentProcess(),&after)!=0,"final handles");
  const bool bounded=after<=before+8;
  if (!bounded) ++failures;
  std::printf("CPP_TLS_HANDLES before=%lu after=%lu original_bound=8 bounded=%d\n",before,after,bounded?1:0);
#endif
  std::printf("CPP_TLS_RESULT cases=%d failures=%d conformant=%d\n",cases,failures,failures?0:1);
  return failures?2:0;
}

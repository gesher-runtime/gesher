// CI-only observations of the public stream lock. No FILE layout, substitute
// lock, production callback, or closer-side readiness barrier participates.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace native3_public_lock_probe {
struct Sample {
  FILE* file = nullptr;
  FILE* sibling = nullptr;
  DWORD thread = 0;
  uint64_t birth = 0;
  int target_result = -1;
  int sibling_result = -1;
};

inline uint64_t Birth(const FILETIME& value) {
  return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

inline DWORD WINAPI TryBoth(void* raw) {
  auto& value = *static_cast<Sample*>(raw);
  value.thread = GetCurrentThreadId();
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 1;
  value.birth = Birth(created);
  value.target_result = ftrylockfile(value.file);
  if (value.target_result == 0) funlockfile(value.file);
  value.sibling_result = ftrylockfile(value.sibling);
  if (value.sibling_result == 0) funlockfile(value.sibling);
  return 0;
}

// The real probe thread must exit before its stack-owned arguments are retired.
// A failed/expired wait terminates this owned test process, never abandons a
// callback or extends the command's original absolute five-second deadline.
inline bool Observe(FILE* file, FILE* sibling, ULONGLONG deadline, Sample* output) {
  if (!file || !sibling || file == sibling || GetTickCount64() >= deadline) return false;
  Sample value{file, sibling};
  DWORD tid = 0;
  HANDLE thread = CreateThread(nullptr, 0, TryBoth, &value, 0, &tid);
  if (!thread) return false;
  ULONGLONG now = GetTickCount64();
  DWORD wait = now < deadline && deadline - now <= MAXDWORD
      ? WaitForSingleObject(thread, static_cast<DWORD>(deadline - now)) : WAIT_TIMEOUT;
  if (wait != WAIT_OBJECT_0) {
    static const char note[] = "STDIO_DIAGNOSTIC phase=fatal94-public-probe-wait\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), note, sizeof(note) - 1, &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 94);
    std::_Exit(94);
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  DWORD code = 1;
  bool valid = GetProcessIdOfThread(thread) == GetCurrentProcessId() &&
      GetThreadTimes(thread, &created, &exited, &kernel, &user) &&
      GetExitCodeThread(thread, &code) && code == 0 && value.thread == tid &&
      value.thread != GetCurrentThreadId() && value.birth != 0 &&
      value.birth == Birth(created) && GetTickCount64() < deadline;
  bool closed = CloseHandle(thread);
  if (!valid || !closed) return false;
  *output = value;
  return true;
}

// This balances only the extra recursive acquisition, retaining the caller's
// original flockfile ownership throughout the contention experiment.
inline bool RecursiveOwner(FILE* file) {
  if (ftrylockfile(file) != 0) return false;
  funlockfile(file);
  return true;
}

inline void Report(const char* phase, const Sample& sample, DWORD owner,
                   uint64_t owner_birth, bool recursive_owner) {
  std::fprintf(stderr, "ACTUAL_PUBLIC_FILE_LOCK phase=%s FILE=%p sibling=%p owner=%lu owner-birth=%llu probe=%lu probe-birth=%llu target-result=%d sibling-result=%d recursive-owner=%d signaled-exit=true handle-closed=true\n",
      phase, static_cast<void*>(sample.file), static_cast<void*>(sample.sibling), owner,
      static_cast<unsigned long long>(owner_birth), sample.thread,
      static_cast<unsigned long long>(sample.birth), sample.target_result,
      sample.sibling_result, recursive_owner);
}
}  // namespace native3_public_lock_probe

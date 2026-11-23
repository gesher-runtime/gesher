// Real Windows children and exact production spawning/waiting; no process API adapters.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "../../compat/framework/services_zygote.h"
#include "../../compat/framework/services_dexopt_watch.h"

namespace {
std::atomic<int> checks{0};
void Check(bool ok, const char* name) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (Win32 %lu)\n", name, GetLastError());
    ExitProcess(1);
  }
}
std::u16string Utf16(const std::wstring& value) {
  return std::u16string(reinterpret_cast<const char16_t*>(value.data()), value.size());
}
std::u16string log_path;
int Spawn(const char16_t* mode, bool budget = false) {
  return gesher::zygote::Spawn(u"\"" + gesher::zygote::ExePath() + u"\" " + mode,
      {}, {}, log_path, false, budget ? 512 : 0, budget ? 2000 : 0);
}
int Wait(int pid, int timeout = 10000, intptr_t cancel = 0) {
  Check(pid > 0, "child spawned");
  int64_t identity = gesher::zygote::ChildStartTime(pid);
  Check(identity > 0, "retained child creation identity");
  return gesher::zygote::WaitChild(pid, identity, timeout, cancel, false);
}
struct ReadContext {
  uint64_t token;
  std::vector<std::u16string> names;
  uint32_t error = 0;
  bool ok = false;
  explicit ReadContext(uint64_t value) : token(value) {}
};
DWORD WINAPI ReadDirectoryThread(void* value) {
  auto* context = static_cast<ReadContext*>(value);
  context->ok = gesher::dexopt::ReadProfileWatch(context->token, &context->names, &context->error);
  return 0;
}
HANDLE StartRead(ReadContext* context) {
  HANDLE thread = CreateThread(nullptr, 0, ReadDirectoryThread, context, 0, nullptr);
  Check(thread != nullptr, "start directory waiter");
  return thread;
}
void EndRead(HANDLE thread, uint64_t token) {
  DWORD result = WaitForSingleObject(thread, 5000);
  if (result != WAIT_OBJECT_0) {
    gesher::dexopt::WakeProfileWatch(token);
    WaitForSingleObject(thread, 5000);
  }
  CloseHandle(thread);
  Check(result == WAIT_OBJECT_0, "directory wait completes on its real event");
}
uint64_t ThreadCpu(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  Check(GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE, "directory waiter CPU counters");
  return (static_cast<uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
      (static_cast<uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}
void WatchContracts(const std::wstring& path) {
  Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "profile directory root");
  uint32_t error = 0;
  uint64_t token = gesher::dexopt::OpenProfileWatch(Utf16(path), &error);
  Check(token != 0, "recursive profile notification registration");
  ReadContext idle{token};
  HANDLE waiting = StartRead(&idle);
  uint64_t before_cpu = ThreadCpu(waiting);
  Sleep(1000);
  uint64_t idle_cpu = ThreadCpu(waiting) - before_cpu;
  Check(WaitForSingleObject(waiting, 0) == WAIT_TIMEOUT, "clean profile watch remains blocked");
  Check(idle_cpu <= 250000, "clean native watcher consumes at most 25ms CPU in 1s");
  gesher::dexopt::WakeProfileWatch(token); EndRead(waiting, token);
  Check(idle.ok && idle.names.empty(), "explicit registry wake requests reconciliation");
  std::wstring package = path + L"\\example.package", profile = package + L"\\primary.prof";
  Check(CreateDirectoryW(package.c_str(), nullptr) != FALSE, "new package directory");
  HANDLE file = CreateFileW(profile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
      FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "new current profile");
  DWORD written = 0;
  Check(WriteFile(file, "profile", 7, &written, nullptr) != FALSE && written == 7, "write current profile");
  CloseHandle(file);
  bool found = false;
  for (int batch = 0; batch < 8 && !found; ++batch) {
    ReadContext changed{token}; HANDLE thread = StartRead(&changed); EndRead(thread, token);
    Check(changed.ok, "read recursive profile changes");
    for (const auto& name : changed.names) if (name == u"example.package\\primary.prof") found = true;
  }
  Check(found, "recursive read reports the actual package profile");
  gesher::dexopt::CloseProfileWatch(token);
  token = gesher::dexopt::OpenProfileWatch(Utf16(path), &error);
  Check(token != 0, "watch reopened");
  ReadContext closed{token}; waiting = StartRead(&closed);
  gesher::dexopt::CloseProfileWatch(token); EndRead(waiting, token);
  Check(!closed.ok && (closed.error == ERROR_OPERATION_ABORTED || closed.error == ERROR_INVALID_HANDLE),
      "close interrupts a pending read without use-after-close");
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "watch handle baseline");
  for (int i = 0; i < 20; ++i) {
    token = gesher::dexopt::OpenProfileWatch(Utf16(path), &error);
    Check(token != 0, "repeated watch registration");
    gesher::dexopt::CloseProfileWatch(token);
  }
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && before == after,
      "overlapped watch close drains I/O and releases all handles");
  DeleteFileW(profile.c_str()); RemoveDirectoryW(package.c_str()); RemoveDirectoryW(path.c_str());
  std::printf("Profile watcher idle CPU: %.3f ms/1000ms; handles %lu -> %lu\n",
      static_cast<double>(idle_cpu) / 10000, before, after);
}
}

int main(int argc, char** argv) {
  if (argc > 1) {
    if (std::strcmp(argv[1], "--hold") == 0) { Sleep(60000); return 99; }
    if (std::strcmp(argv[1], "--259") == 0) return 259;
    if (std::strcmp(argv[1], "--budget") == 0) {
      Check(GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "background CPU priority");
      MEMORY_PRIORITY_INFORMATION memory_priority = {};
      Check(GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority,
          &memory_priority, sizeof(memory_priority)) != FALSE && memory_priority.MemoryPriority == MEMORY_PRIORITY_LOW,
          "background page memory priority");
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
      Check(QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
          &limits, sizeof(limits), nullptr) != FALSE, "query compiler job budget");
      Check(limits.ProcessMemoryLimit == static_cast<SIZE_T>(512) * 1024 * 1024,
          "compiler process memory budget");
      Check(limits.BasicLimitInformation.ActiveProcessLimit == 1, "compiler job process bound");
      JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu = {};
      Check(QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation,
          &cpu, sizeof(cpu), nullptr) != FALSE && cpu.CpuRate == 2000 &&
          (cpu.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) != 0, "compiler CPU hard cap");
      void* beyond = VirtualAlloc(nullptr, static_cast<SIZE_T>(1024) * 1024 * 1024,
          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (beyond != nullptr) VirtualFree(beyond, 0, MEM_RELEASE);
      Check(beyond == nullptr, "actual compiler memory limit rejects excess commit");
      return 0;
    }
    return 42;
  }
  wchar_t temp[MAX_PATH], file[MAX_PATH];
  Check(GetTempPathW(MAX_PATH, temp) != 0 && GetTempFileNameW(temp, L"gdx", 0, file) != 0, "tool log");
  log_path = Utf16(file);
  Check(Wait(Spawn(u"--exit")) == 42, "blocking wait returns real exit");
  int special = Spawn(u"--259");
  Check(Wait(special) == 259 && gesher::zygote::ExitCode(special) == -1,
      "exited STILL_ACTIVE value is reaped by handle state");
  int hold = Spawn(u"--hold");
  int64_t identity = gesher::zygote::ChildStartTime(hold);
  Check(gesher::zygote::WaitChild(hold, identity + 1, 0, 0, false) == -1 &&
      gesher::zygote::ChildStartTime(hold) == identity, "wrong identity cannot consume or kill child");
  Check(Wait(hold, 50) == -2 && gesher::zygote::ChildStartTime(hold) == 0, "timeout terminates and reaps");
  std::u16string event_name = u"Local\\DexoptWaitFixture-" + Utf16(std::to_wstring(GetCurrentProcessId()));
  intptr_t cancel = gesher::zygote::MakeEvent(event_name);
  Check(cancel != 0, "cancellation event");
  hold = Spawn(u"--hold", true);
  Check(gesher::zygote::SignalEvent(event_name), "cancel before wait registration");
  Check(Wait(hold, 10000, cancel) == -3 && gesher::zygote::ChildStartTime(hold) == 0,
      "queued cancellation terminates and reaps budgeted child");
  hold = Spawn(u"--hold", true);
  int result = 0;
  std::thread waiter([&] { result = Wait(hold, 10000, cancel); });
  Check(gesher::zygote::SignalEvent(event_name), "cancel blocking waiter");
  waiter.join();
  Check(result == -3, "blocking waiter sees cancellation");
  Check(CloseHandle(reinterpret_cast<HANDLE>(cancel)) != FALSE, "close cancellation event");
  Check(Wait(Spawn(u"--budget", true)) == 0, "actual process CPU/RAM budgets before child runs");
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "handle baseline");
  for (int i = 0; i < 20; ++i) Check(Wait(Spawn(u"--exit", true)) == 42, "repeated budgeted wait");
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && before == after,
      "completed compiler jobs/process handles do not accumulate");
  Check(gesher::zygote::AvailableMemory() > 0, "real memory admission input");
  WatchContracts(std::wstring(file) + L"-profiles");
  DeleteFileW(file);
  std::printf("Dexopt Windows child wait/budget contracts passed: %d; handles %lu -> %lu\n", checks.load(), before, after);
  return 0;
}

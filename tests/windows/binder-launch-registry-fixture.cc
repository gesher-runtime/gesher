// Source-only real-process test of the registry primitive; NOT an end-to-end
// private bootstrap or Binder transport fixture. Build/run only in future CI.
#include "../../compat/binder/launch_registry.h"
#include <stdexcept>
#include <cstdio>
#include <string>
#include <vector>

using namespace gesher::binder_launch;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
HANDLE failed_wait_handle = nullptr;
DWORD WINAPI ControlledWait(HANDLE handle, DWORD timeout) {
  return handle == failed_wait_handle ? WAIT_FAILED : WaitForSingleObject(handle, timeout);
}
struct Child {
  HANDLE process, input, output;
  DWORD pid;
  uint64_t birth;
  ~Child() {
    TerminateProcess(process, 137);
    WaitForSingleObject(process, 5000);
    CloseHandle(process); CloseHandle(input); CloseHandle(output);
  }
};
void Transfer(HANDLE h, void* data, DWORD size, bool write) {
  DWORD count = 0;
  Check((write ? WriteFile(h, data, size, &count, nullptr)
               : ReadFile(h, data, size, &count, nullptr)) && count == size, "private transfer");
}
int main(int argc, char**) {
  if (argc == 2) {
    // Capability locator arrives through the owned pipe, never argv/env/log.
    uint64_t value;
    Transfer(GetStdHandle(STD_INPUT_HANDLE), &value, sizeof(value), false);
    HANDLE event = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    uint32_t ack = SetEvent(event) ? 1 : 0;
    CloseHandle(event);
    Transfer(GetStdHandle(STD_OUTPUT_HANDLE), &ack, sizeof(ack), true);
    Sleep(INFINITE);
    return 1;
  }
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE child_input, child_output, input, output;
  Check(CreatePipe(&child_input, &input, &inherit, 0) &&
        CreatePipe(&output, &child_output, &inherit, 0), "owned pipes");
  SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  Check(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "attribute list");
  HANDLE allow[]{child_input, child_output};
  Check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        allow, sizeof(allow), nullptr, nullptr), "restricted inheritance");
  wchar_t exe[32768];
  Check(GetModuleFileNameW(nullptr, exe, 32768), "fixture executable");
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_input;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = child_output;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION pi{};
  Check(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &pi), "actual suspended child");
  DeleteProcThreadAttributeList(attributes);
  CloseHandle(child_input); CloseHandle(child_output);
  FILETIME created{}, a{}, b{}, c{};
  Check(GetProcessTimes(pi.hProcess, &created, &a, &b, &c), "actual birth");
  Child child{pi.hProcess, input, output, pi.dwProcessId,
      (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime};
  Registry registry(ControlledWait);
  Install install{"fixture.a", {1}, 10000};
  HANDLE retained = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(),
        &retained, 0, FALSE, DUPLICATE_SAME_ACCESS), "retained launch instance");
  Check(registry.Publish(retained, install), "authorized publication before resume");
  auto lease = registry.Find(child.pid, child.birth, install.uid);
  Check(bool(lease), "honest registered control");
  failed_wait_handle = lease->process;
  HANDLE replacement = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(),
        &replacement, 0, FALSE, DUPLICATE_SAME_ACCESS), "replacement duplicate");
  Check(!registry.Publish(replacement, install), "unknown wait refuses replacement");
  registry.PruneDead();
  Check(!registry.Find(child.pid, child.birth, install.uid), "unknown wait denies lookup");
  failed_wait_handle = nullptr;
  Check(registry.Find(child.pid, child.birth, install.uid) == lease,
        "unknown wait retained original record through publish and prune");
  Check(!registry.Find(child.pid, child.birth, 1000), "forged own PID/system UID");
  Check(!registry.Find(child.pid, child.birth, 10001), "forged other package UID");
  Check(!registry.Find(child.pid, child.birth + 1, 10000), "wrong birth");
  Check(!registry.Find(GetCurrentProcessId(), child.birth, 10000), "unregistered real process");
  Check(ResumeThread(pi.hThread) != DWORD(-1), "resume after publication");
  CloseHandle(pi.hThread);
  Check(registry.Challenge(lease, [&](uint64_t value, uint64_t) {
      Transfer(child.input, &value, sizeof(value), true); return true;
    }, [&](uint64_t) {
      uint32_t ack; Transfer(child.output, &ack, sizeof(ack), false); return ack == 1;
    }), "live private capability control");
  Install reinstall{"fixture.a", {2}, 10000};
  registry.Revoke(reinstall);
  Check(registry.Live(lease), "wrong install incarnation cannot revoke current record");
  registry.Revoke(install);
  Check(!registry.Live(lease) && !registry.Find(child.pid, child.birth, 10000),
        "uninstall revokes retained lease and PID lookup");
  HANDLE second = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(),
        &second, 0, FALSE, DUPLICATE_SAME_ACCESS), "new retained duplicate");
  Check(!registry.Publish(second, reinstall), "reinstall cannot relabel/revive old child");
  registry.Revoke(install);
  auto fresh = registry.Find(child.pid, child.birth, 10000);
  Check(!fresh, "revoked old process remains refused after reinstall attempt");
  Registry death_registry;
  HANDLE death_handle = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(),
        &death_handle, 0, FALSE, DUPLICATE_SAME_ACCESS), "death test retained instance");
  Check(death_registry.Publish(death_handle, install), "death test honest publication");
  auto death_lease = death_registry.Find(child.pid, child.birth, 10000);
  Check(bool(death_lease), "termination negative starts with a live registered control");
  Registry authority_registry;
  HANDLE authority_handle = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(),
        &authority_handle, 0, FALSE, DUPLICATE_SAME_ACCESS), "authority test retained instance");
  Check(authority_registry.Publish(authority_handle, install), "authority honest publication");
  auto authority_lease = authority_registry.Find(child.pid, child.birth, 10000);
  Check(bool(authority_lease), "authority negative starts live");
  authority_registry.AuthorityDied();
  Check(!authority_registry.Live(authority_lease), "authority death revokes live instance");
  Check(TerminateProcess(child.process, 0) &&
        WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0, "terminated real child");
  Check(!death_registry.Live(death_lease) &&
        !death_registry.Find(child.pid, child.birth, 10000),
        "terminated instance rejected");
  registry.AuthorityDied();
  Check(!registry.Live(fresh), "authority death revokes all leases");
  puts("PASS registry real-child, failed-wait retention, revoke and authority death");
  return 0;
}

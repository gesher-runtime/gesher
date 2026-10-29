// Trusted host test owner only. Ordinary children never inherit the private
// publication endpoint. This does not isolate arbitrary same-user guest code.
#pragma once
#include "../../compat/binder/launch_control.h"
#include "../../compat/posix/src/launch_context.h"
#include <vector>
#include <cstdlib>

namespace binder_fixture {
namespace c = gesher::launch_control;
class Bootstrap {
 public:
  ~Bootstrap() {
    for (HANDLE child : children_) {
      if (WaitForSingleObject(child, 0) == WAIT_TIMEOUT) TerminateProcess(child, 211);
      if (WaitForSingleObject(child, 5000) != WAIT_OBJECT_0) std::abort();
      CloseHandle(child);
    }
    if (control_) CloseHandle(control_);
    if (broker_) CloseHandle(broker_);
  }
  HANDLE Begin(const std::wstring& exe, const std::wstring& arguments) {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    if (broker_ || !c::Channel(&server, &client)) return nullptr;
    gesher::launch::Context context;
    context.kind = 1; context.control = c::Value(server);
    HANDLE broker = Create(exe, arguments, context, server);
    CloseHandle(server);
    if (!broker) { CloseHandle(client); return nullptr; }
    control_ = client;
    if (!DuplicateHandle(GetCurrentProcess(), broker, GetCurrentProcess(), &broker_,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) { CloseHandle(broker); return nullptr; }
    // Real negative control: same retained process, wrong creation time must
    // be refused without consuming the root slot or authorizing a PID claim.
    if (Publish(GetCurrentProcess(), "@launcher", 2000, true) ||
        !Publish(GetCurrentProcess(), "@launcher", 2000)) { CloseHandle(broker); return nullptr; }
    // Honest legacy host selector only, after exact-process admission. Changing
    // this value cannot publish a record or authorize another process.
    _putenv("GESHER_BINDER_UID=2000");
    return broker;
  }
  HANDLE Spawn(const std::wstring& exe, const std::wstring& arguments, const char* role) {
    gesher::launch::Context context;
    context.uid = 1000;
    context.server_pid = GetProcessId(broker_);
    HANDLE child = Create(exe, arguments, context, nullptr, true);
    if (!child) return nullptr;
    // Create() retained the exact suspended instance and thread. Publication
    // consumes a broker-relative duplicate, never an OpenProcess(PID) result.
    bool ok = Publish(child, role, 1000) && ResumeThread(thread_) != DWORD(-1);
    CloseHandle(thread_); thread_ = nullptr;
    if (!ok) { TerminateProcess(child, 211); WaitForSingleObject(child, 5000); CloseHandle(child); return nullptr; }
    return child;
  }
 private:
  HANDLE control_ = nullptr, broker_ = nullptr, thread_ = nullptr;
  std::vector<HANDLE> children_;
  bool Publish(HANDLE process, const char* package, uint32_t uid, bool wrong_birth = false) {
    if (!control_) return false;
    c::Request request;
    request.op = c::kPublish; request.uid = uid;
    std::strncpy(request.package, package, sizeof(request.package) - 1);
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), process, broker_, &remote,
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, 0)) return false;
    request.process = c::Value(remote);
    request.birth = c::Birth(process) + (wrong_birth ? 1 : 0);
    int32_t reply = -1;
    uint64_t deadline = GetTickCount64() + 5000;
    if (!c::Transfer(control_, &request, sizeof(request), true, deadline) ||
        !c::Transfer(control_, &reply, sizeof(reply), false, deadline)) {
      CloseHandle(control_); control_ = nullptr;
      return false;
    }
    return reply == 0;
  }
  HANDLE Create(const std::wstring& exe, const std::wstring& args,
                gesher::launch::Context& context, HANDLE inherited, bool suspended = false) {
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.cbReserved2 = sizeof(context);
    startup.StartupInfo.lpReserved2 = reinterpret_cast<BYTE*>(&context);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized = false;
    if (inherited) {
      initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != FALSE;
      if (!initialized || !SetHandleInformation(inherited, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
          !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    &inherited, sizeof(inherited), nullptr, nullptr)) {
        if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
        return nullptr;
      }
    }
    std::wstring command = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_SUSPENDED | (inherited ? EXTENDED_STARTUPINFO_PRESENT : 0);
    if (!inherited) startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, inherited != nullptr,
                            flags, nullptr, nullptr, &startup.StartupInfo, &pi) != FALSE;
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (inherited) SetHandleInformation(inherited, HANDLE_FLAG_INHERIT, 0);
    if (!ok) return nullptr;
    HANDLE retained = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &retained,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
      TerminateProcess(pi.hProcess, 211); WaitForSingleObject(pi.hProcess, 5000);
      CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return nullptr;
    }
    children_.push_back(retained);
    if (suspended) thread_ = pi.hThread;
    else {
      ok = ResumeThread(pi.hThread) != DWORD(-1);
      CloseHandle(pi.hThread);
      if (!ok) { CloseHandle(pi.hProcess); return nullptr; }
    }
    return pi.hProcess;
  }
};
}

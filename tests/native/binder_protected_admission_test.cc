// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Exercise the same handlers as binderd's authenticated entry point. This
// standalone fixture gives the registry native child process handles from the
// test launcher and never starts or installs the privileged Windows service.
#include <windows.h>
#include <objbase.h>
#include <sddl.h>

#include <span>

#define main protected_broker_main
#include "../../compat/binder/binderd.cc"
#undef main

namespace {
using gesher::binder::ProtectedPeers;
using gesher::binder::PeerRole;

constexpr DWORD kWait = 6000;
constexpr uint32_t kUid = 1000;

struct CloseFixtureHandle {
  void operator()(void* h) const {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};
using FixtureHandle = std::unique_ptr<void, CloseFixtureHandle>;

void Require(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "protected Binder admission: %s (Windows error %lu)\n",
      message, GetLastError());
  std::exit(1);
}

struct Ticket {
  uint64_t key = 0;
  uint32_t id = 0;
};

FixtureHandle Connect(const wchar_t* name) {
  const ULONGLONG deadline = GetTickCount64() + kWait;
  do {
    HANDLE pipe = CreateFileW(name, wire::kPipeClientAccess, 0, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return FixtureHandle(pipe);
    if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
    Sleep(10);
  } while (GetTickCount64() < deadline);
  Require(false, "connect to isolated protected listener");
  return {};
}

bool ReadStatus(HANDLE pipe, int32_t* status) {
  std::vector<uint8_t> reply;
  if (!ReadMessage(pipe, &reply) || reply.size() < sizeof(*status)) return false;
  std::memcpy(status, reply.data(), sizeof(*status));
  return true;
}

void ProcHello(HANDLE pipe, uint32_t pid, uint32_t uid) {
  WireReply hello;
  hello.Put<uint32_t>(wire::kHelloProc);
  hello.Put<uint32_t>(wire::kMagic);
  hello.Put<uint32_t>(wire::kVersion);
  hello.Put<uint32_t>(pid);
  hello.Put<uint32_t>(uid);
  Require(WriteMessage(pipe, hello), "send process hello");
}

void ThreadHello(HANDLE pipe, const Ticket& ticket) {
  WireReply hello;
  hello.Put<uint32_t>(wire::kHelloThread);
  hello.Put<uint32_t>(wire::kMagic);
  hello.Put<uint32_t>(wire::kVersion);
  hello.Put<uint32_t>(ticket.id);
  hello.Put<uint64_t>(ticket.key);
  hello.Put<uint32_t>(GetCurrentThreadId());
  Require(WriteMessage(pipe, hello), "send thread hello");
}

Ticket OpenProcessTicket(HANDLE pipe) {
  ProcHello(pipe, GetCurrentProcessId(), kUid);
  std::vector<uint8_t> reply;
  Require(ReadMessage(pipe, &reply), "open Binder on registered process");
  In reader{reply.data(), reply.data() + reply.size()};
  int32_t result = 0;
  Ticket ticket;
  Require(reader.Get(&result) && result == 0 && reader.Get(&ticket.id) &&
      reader.Get(&ticket.key) && reader.p == reader.end, "receive exact registered process reply");
  return ticket;
}

int32_t Ioctl(HANDLE pipe, uint32_t request) {
  WireReply message;
  message.Put<uint32_t>(wire::kIoctl);
  message.Put<uint32_t>(request);
  if (request == BINDER_SET_MAX_THREADS) message.Put<uint32_t>(4);
  if (request == BINDER_SET_CONTEXT_MGR_EXT) message.Put(flat_binder_object{});
  Require(WriteMessage(pipe, message), "send thread ioctl");
  int32_t result = 0;
  Require(ReadStatus(pipe, &result), "receive thread ioctl result");
  return result;
}

void RegisteredClient(const wchar_t* name, HANDLE ticket_pipe, HANDLE resume) {
  int32_t result = 0;
  const DWORD pid = GetCurrentProcessId();
  {
    const auto pipe = Connect(name);
    ProcHello(pipe.get(), pid, kUid + 1);
    Require(!ReadStatus(pipe.get(), &result), "reject forged Android UID");
  }
  {
    const auto pipe = Connect(name);
    ProcHello(pipe.get(), pid ^ 0x80000000u, kUid);
    Require(!ReadStatus(pipe.get(), &result), "reject forged PID on genuine connection");
  }
  {
    const auto pipe = Connect(name);
    const uint32_t oversize = 29;
    Require(WriteExact(pipe.get(), &oversize, sizeof(oversize)), "send oversized hello header");
    Require(!ReadStatus(pipe.get(), &result), "close before reading oversized hello body");
  }
  {
    const auto pipe = Connect(name);
    WireReply message;
    message.Put<uint32_t>(wire::kHelloRights);
    message.Put<uint32_t>(wire::kMagic);
    message.Put<uint32_t>(wire::kRightsVersion);
    Require(WriteMessage(pipe.get(), message), "request ordinary rights from protected pipe");
    Require(!ReadStatus(pipe.get(), &result), "protected pipe refuses SCM_RIGHTS");
  }
  {
    const auto pipe = Connect(name);
    const FixtureHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Require(event != nullptr, "create publication impersonation fixture");
    WireReply message;
    message.Put<uint32_t>(wire::kPublishApplicationSharedMemory);
    message.Put<uint32_t>(wire::kMagic);
    message.Put<uint32_t>(wire::kVersion);
    message.Put<uint64_t>(reinterpret_cast<uintptr_t>(event.get()));
    message.Put<uint64_t>(4096);
    Require(WriteMessage(pipe.get(), message), "request producer-only memory publication");
    Require(ReadStatus(pipe.get(), &result) && result == ERROR_ACCESS_DENIED,
            "system UID without producer role cannot publish a resource");
  }

  const auto process = Connect(name);
  const Ticket ticket = OpenProcessTicket(process.get());
  const auto thread = Connect(name);
  ThreadHello(thread.get(), ticket);
  Require(ReadStatus(thread.get(), &result) && result == 0, "join thread of exact process");
  Require(Ioctl(thread.get(), BINDER_SET_MAX_THREADS) == 0,
          "ordinary Binder ioctl works for selected peer");
  Require(Ioctl(thread.get(), BINDER_SET_CONTEXT_MGR) == -EPERM &&
      Ioctl(thread.get(), BINDER_SET_CONTEXT_MGR_EXT) == -EPERM,
      "trusted UID without an explicit authority role cannot become context manager");
  WireReply polling;
  polling.Put<uint32_t>(wire::kWriteRead);
  polling.Put<uint64_t>(4096);
  polling.Put<uint32_t>(0);  // first read
  polling.Put<uint32_t>(0);  // blocking
  Require(WriteMessage(thread.get(), polling), "start a blocking Binder read");
  Require(WriteExact(ticket_pipe, &ticket, sizeof(ticket)), "show ticket to fixture parent");
  CloseHandle(ticket_pipe);
  Require(WaitForSingleObject(resume, kWait) == WAIT_OBJECT_0, "wait for authority revocation");
  CloseHandle(resume);
  Require(ReadStatus(thread.get(), &result) && result == -EBADF,
          "revocation wakes an existing blocked Binder read");
  Require(Ioctl(thread.get(), BINDER_SET_MAX_THREADS) == -EBADF,
          "revoked peer cannot use existing thread");
  const auto reopened = Connect(name);
  Require(!ReadStatus(reopened.get(), &result), "reject revoked live process before reading hello");
}

void ContextManager(const wchar_t* name) {
  const auto process = Connect(name);
  const Ticket ticket = OpenProcessTicket(process.get());
  const auto thread = Connect(name);
  ThreadHello(thread.get(), ticket);
  int32_t status = 0;
  Require(ReadStatus(thread.get(), &status) && status == 0, "join context manager thread");
  Require(Ioctl(thread.get(), BINDER_SET_CONTEXT_MGR_EXT) == 0,
          "explicit authority role can become context manager at the same Android UID");
  Require(Ioctl(thread.get(), BINDER_SET_CONTEXT_MGR) == -EBUSY,
          "only one context manager can be active");
}

void SameUidSibling(const wchar_t* name, const Ticket& found) {
  const auto pipe = Connect(name);
  ThreadHello(pipe.get(), found);
  int32_t status = 0;
  Require(!ReadStatus(pipe.get(), &status) || status == -ESRCH,
          "different retained process cannot reuse another proc's correct id and key");
}

struct UserPolicy {
  alignas(TOKEN_USER) BYTE data[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, FALSE};
  UserPolicy() {
    HANDLE raw = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "open fixture user");
    const FixtureHandle owner(raw);
    DWORD bytes = 0;
    Require(GetTokenInformation(raw, TokenUser, data, sizeof(data), &bytes), "read fixture owner SID");
    wchar_t* name = nullptr;
    Require(ConvertSidToStringSidW(Sid(), &name), "format owner SID");
    std::wstring policy = std::wstring(L"O:") + name + L"D:P(A;;0x001fffff;;;" + name + L")";
    LocalFree(name);
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(), SDDL_REVISION_1,
        &security.lpSecurityDescriptor, nullptr), "build fixture owner-only descriptor");
  }
  ~UserPolicy() { LocalFree(security.lpSecurityDescriptor); }
  PSID Sid() const { return reinterpret_cast<const TOKEN_USER*>(data)->User.Sid; }
};

bool ObserveSealedRegistration(HANDLE job, UserPolicy& owner, PSID fixture_sid,
                               bool* opened_by_pid, bool* cleaned) {
  struct SuspendedProcess {
    PROCESS_INFORMATION child{};
    bool Close() {
      if (!child.hProcess) return true;
      if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
        TerminateProcess(child.hProcess, 0);
      const bool exited = WaitForSingleObject(child.hProcess, kWait) == WAIT_OBJECT_0;
      const bool thread_closed = CloseHandle(child.hThread) != FALSE;
      const bool process_closed = CloseHandle(child.hProcess) != FALSE;
      child = {};
      return exited && thread_closed && process_closed;
    }
    ~SuspendedProcess() { Close(); }
  } caller, target;
  const auto observe = [&]() -> bool {
  wchar_t windows[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(windows, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  const std::wstring image = std::wstring(windows) + L"\\cmd.exe";
  SECURITY_CAPABILITIES capabilities{fixture_sid, nullptr, 0, 0};
  const auto create = [&](SECURITY_ATTRIBUTES* security, bool lowbox, PROCESS_INFORMATION* child) {
    const DWORD attributes = lowbox ? 2 : 1;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, attributes, 0, &size);
    std::vector<std::byte> storage(size);
    STARTUPINFOEXW setup{};
    setup.StartupInfo.cb = sizeof(setup);
    setup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(setup.lpAttributeList, attributes, 0, &size)) return false;
    // Both never-resumed children belong to the existing kill-on-close test
    // job at creation, including early-return and abnormal parent-exit paths.
    const bool created = UpdateProcThreadAttribute(setup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), nullptr, nullptr) &&
        (!lowbox || UpdateProcThreadAttribute(setup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof(capabilities), nullptr, nullptr)) &&
        CreateProcessW(image.c_str(), nullptr, security, security, FALSE,
          EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
          nullptr, nullptr, &setup.StartupInfo, child);
    DeleteProcThreadAttributeList(setup.lpAttributeList);
    BOOL bound = FALSE;
    return created && IsProcessInJob(child->hProcess, job, &bound) && bound;
  };
  if (!create(&owner.security, false, &target.child) || !create(nullptr, true, &caller.child)) return false;
  HANDLE raw = nullptr;
  if (!OpenProcessToken(caller.child.hProcess, TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) return false;
  const FixtureHandle lowbox(raw);
  DWORD is_container = 0, written = 0;
  if (!GetTokenInformation(lowbox.get(), TokenIsAppContainer, &is_container,
          sizeof(is_container), &written) || !is_container ||
      !DuplicateTokenEx(lowbox.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
          SecurityImpersonation, TokenImpersonation, &raw)) return false;
  const FixtureHandle impersonation(raw);
  constexpr DWORD access = PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  const HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, target.child.hProcess, self, &raw, access | READ_CONTROL, FALSE, 0)) return false;
  const FixtureHandle offered(raw);
  if (!DuplicateHandle(self, target.child.hProcess, self, &raw, access, FALSE, 0)) return false;
  const FixtureHandle no_acl_read(raw);
  DWORD session = 0;
  FILETIME birth{}, exit{}, kernel{}, user{};
  if (!ProcessIdToSessionId(target.child.dwProcessId, &session) ||
      !GetProcessTimes(target.child.hProcess, &birth, &exit, &kernel, &user)) return false;

  ProtectedPeers peers;
  ProtectedIdentity accepted, denied;
  if (!SetThreadToken(nullptr, impersonation.get())) return false;
  const FixtureHandle opened(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, target.child.dwProcessId));
  const DWORD open_error = GetLastError();
  *opened_by_pid = opened != nullptr;
  const DWORD denied_status = peers.RegisterForFixture(no_acl_read.get(), kUid,
      PeerRole::kContextManager, owner.Sid(), &denied);
  const DWORD status = peers.RegisterForFixture(offered.get(), kUid,
      PeerRole::kContextManager, owner.Sid(), &accepted);
  if (!RevertToSelf()) std::abort();
  std::printf("Protected Binder sealed peer: native PID open=%lu ACL read absent=%lu register=%lu\n",
              opened ? ERROR_SUCCESS : open_error, denied_status, status);
  return !opened && open_error == ERROR_ACCESS_DENIED && denied_status == ERROR_ACCESS_DENIED && !denied &&
      status == ERROR_SUCCESS && accepted && accepted->pid() == target.child.dwProcessId &&
      accepted->session() == session && accepted->creation_time() ==
          ((uint64_t{birth.dwHighDateTime} << 32) | birth.dwLowDateTime);
  };
  const bool observed = observe();
  const bool target_closed = target.Close();
  const bool caller_closed = caller.Close();
  *cleaned = target_closed && caller_closed;
  std::printf("Protected Binder sealed cleanup: target=%d caller=%d\n", target_closed, caller_closed);
  return observed && *cleaned;
}

void SealedSessionRegistration(HANDLE job, UserPolicy& owner) {
  GUID random{};
  wchar_t text[39]{};
  Require(SUCCEEDED(CoCreateGuid(&random)) && StringFromGUID2(random, text, 39) == 39,
          "create a unique lowbox fixture name");
  const std::wstring name = L"Gesher.BinderFixture." + std::wstring(text + 1, 36);
  gesher::appcontainer::Sid sid;
  Require(gesher::appcontainer::EnsureProfile(name, name, &sid) == ERROR_SUCCESS,
          "create disposable lowbox profile");
  bool opened_by_pid = false, cleaned = false;
  const bool observed = ObserveSealedRegistration(job, owner, sid.value, &opened_by_pid, &cleaned);
  Require(cleaned, "both owned sealed fixture children exited and handles closed");
  Require(gesher::appcontainer::DeleteContainerProfile(name) == ERROR_SUCCESS,
          "remove disposable lowbox profile");
  Require(!opened_by_pid && observed, "bound session from sealed peer's exact limited handle");
}

FixtureHandle StartChild(HANDLE job, const std::wstring& args, UserPolicy& owner,
                         ProtectedPeers* peers = nullptr, ProtectedIdentity* peer = nullptr,
                         std::span<HANDLE> inherited = {}, PeerRole role = PeerRole::kClient) {
  wchar_t image[MAX_PATH]{};
  const DWORD size = GetModuleFileNameW(nullptr, image, MAX_PATH);
  Require(size && size < MAX_PATH, "get test executable");
  std::wstring command = std::wstring(L"\"") + image + L"\" " + args;

  STARTUPINFOEXW setup{};
  setup.StartupInfo.cb = inherited.empty() ? sizeof(STARTUPINFOW) : sizeof(setup);
  DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
  std::vector<std::byte> storage;
  if (!inherited.empty()) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    storage.resize(bytes);
    setup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    Require(InitializeProcThreadAttributeList(setup.lpAttributeList, 1, 0, &bytes),
            "initialize explicit fixture handle inheritance");
    Require(UpdateProcThreadAttribute(setup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), inherited.size_bytes(), nullptr, nullptr), "allow fixture handles only");
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  PROCESS_INFORMATION child{};
  const BOOL created = CreateProcessW(image, command.data(), &owner.security, &owner.security,
      !inherited.empty(), flags, nullptr, nullptr, &setup.StartupInfo, &child);
  if (setup.lpAttributeList) DeleteProcThreadAttributeList(setup.lpAttributeList);
  Require(created, "start actual separate client process");
  FixtureHandle process(child.hProcess), thread(child.hThread);
  Require(AssignProcessToJobObject(job, process.get()), "own fixture child lifetime");
  if (peers) {
    Require(peers->RegisterForFixture(process.get(), kUid, role, owner.Sid(), peer) == ERROR_SUCCESS,
            "register exact child object before resume");
  }
  Require(ResumeThread(thread.get()) == 1, "resume fixture child");
  return process;
}

void Finished(HANDLE child, const char* operation) {
  DWORD result = 0;
  Require(WaitForSingleObject(child, kWait) == WAIT_OBJECT_0 &&
      GetExitCodeProcess(child, &result) && result == 0, operation);
}

bool IsWaiting(const Ticket& ticket) {
  std::lock_guard lock(g_lock);
  const auto proc = g_procs.find(ticket.id);
  if (proc == g_procs.end() || proc->second->key != ticket.key) return false;
  for (const Thread* thread : proc->second->threads) {
    if (thread->looper & kLooperWaiting) return true;
  }
  return false;
}

void Test() {
  char executable[] = "binderd";
  char flag[] = "--gesher-protected-binder";
  char bogus[] = "invalid-locator";
  char* refused[] = {executable, flag, bogus, bogus, bogus};
  Require(protected_broker_main(5, refused) == 1,
          "invalid authenticated startup does not fall back to ordinary pipe discovery");
  const FixtureHandle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  Require(job && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
      &limits, sizeof(limits)), "bound test child lifetime");
  UserPolicy owner;
  SealedSessionRegistration(job.get(), owner);
  ProtectedPeers peers;
  ProtectedIdentity selected, sibling, manager;
  const std::wstring name = L"\\\\.\\pipe\\gesher-protected-admission-fixture-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  // Seven initial opens from the selected process, plus outsider, sibling,
  // manager process/thread, and the selected process after revocation.
  unsigned accepted = 0, rejected = 0;
  std::thread server([&] {
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 12; ++i) {
      HANDLE listener = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
      Require(listener != INVALID_HANDLE_VALUE, "create isolated native listener");
      Require(ConnectNamedPipe(listener, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED,
          "accept real native client");
      auto identity = AdmitProtected(listener, peers);
      if (identity) {
        ++accepted;
        workers.emplace_back(ServeUsbProtectedConnection, listener, std::move(identity));
      } else {
        ++rejected;
      }
    }
    for (auto& worker : workers) worker.join();
  });
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE incoming = nullptr, outgoing = nullptr;
  Require(CreatePipe(&incoming, &outgoing, &inherit, 0), "create child test ticket stream");
  FixtureHandle reader(incoming), writer(outgoing);
  const FixtureHandle proceed(CreateEventW(&inherit, TRUE, FALSE, nullptr));
  Require(proceed && SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0),
          "prepare private fixture signals");
  HANDLE inherited[] = {writer.get(), proceed.get()};
  auto child = StartChild(job.get(), L"--selected " + name + L" " +
      std::to_wstring(reinterpret_cast<uintptr_t>(writer.get())) + L" " +
      std::to_wstring(reinterpret_cast<uintptr_t>(proceed.get())),
      owner, &peers, &selected, inherited);
  writer.reset();
  Ticket found;
  Require(ReadExact(reader.get(), &found, sizeof(found)), "selected child exercised allowed operations");
  auto outsider = StartChild(job.get(), L"--outsider " + name, owner);
  Finished(outsider.get(), "unregistered process rejected before a guest hello");
  auto second = StartChild(job.get(), L"--sibling " + name + L" " +
      std::to_wstring(found.id) + L" " + std::to_wstring(found.key), owner, &peers, &sibling);
  Finished(second.get(), "same-UID sibling could not join another process");
  auto context = StartChild(job.get(), L"--manager " + name, owner, &peers, &manager,
                            {}, PeerRole::kContextManager);
  Finished(context.get(), "authority-tagged system peer became context manager");
  const ULONGLONG wait_deadline = GetTickCount64() + kWait;
  while (!IsWaiting(found) && GetTickCount64() < wait_deadline) Sleep(10);
  Require(IsWaiting(found), "observe the native broker's blocked read before revocation");
  peers.Revoke(selected);
  Require(SetEvent(proceed.get()), "let selected child verify revocation");
  Finished(child.get(), "selected connection accepted then revoked");
  server.join();
  Require(accepted == 10 && rejected == 2, "only the registered live identities reached decoding");
  std::puts("Protected Binder admission: exact peer, UID, role, rights and revoke checks passed");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) {
    Test();
  } else if (argc >= 3) {
    std::wstring path(argv[2], argv[2] + std::strlen(argv[2]));
    const std::string_view mode(argv[1]);
    if (mode == "--selected" && argc == 5) {
      RegisteredClient(path.c_str(),
          reinterpret_cast<HANDLE>(std::strtoull(argv[3], nullptr, 10)),
          reinterpret_cast<HANDLE>(std::strtoull(argv[4], nullptr, 10)));
    } else if (mode == "--sibling" && argc == 5) {
      Ticket found{std::strtoull(argv[4], nullptr, 10),
                   static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10))};
      SameUidSibling(path.c_str(), found);
    } else if (mode == "--outsider" && argc == 3) {
      const auto pipe = Connect(path.c_str());
      int32_t result = 0;
      Require(!ReadStatus(pipe.get(), &result), "client rejected without sending any bytes");
    } else if (mode == "--manager" && argc == 3) {
      ContextManager(path.c_str());
    } else {
      return 2;
    }
  } else {
    return 2;
  }
  return 0;
}

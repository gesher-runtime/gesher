// Source-only controlled-process production bootstrap/admission fixture.
// Link with compat/framework/services_zygote.cc. Arguments: rebuilt binderd,
// rebuilt servicemanager, existing EMPTY owned fixture directory. Outer CI timeout
// required. No role asserted in argv/env or wire can grant writer authority.
#include "../../compat/framework/services_zygote.h"
#include "../../compat/posix/src/launch_context.h"
#include "../../compat/posix/src/binder_wire.h"
#include "../../compat/binder/launch_control.h"
#include "../../compat/binder/launch_registry.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace z = gesher::zygote;
namespace c = gesher::launch_control;
namespace w = gesher::binder_wire;
unsigned proof_events_opened = 0, proof_events_closed = 0, process_transfers = 0;
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
std::u16string U(const std::wstring& s) { return {s.begin(), s.end()}; }
std::wstring Wide(const std::u16string& s) { return {s.begin(), s.end()}; }
std::u16string Quote(const std::u16string& s) {
  // Fixture paths may contain spaces, but never quotes or trailing separators.
  Check(s.find(u'"') == s.npos && (s.empty() || s.back() != u'\\'), "fixture path quoting");
  return u"\"" + s + u"\"";
}
std::u16string Self() {
  wchar_t value[32768]; DWORD n = GetModuleFileNameW(nullptr, value, 32768);
  Check(n && n < 32768, "fixture executable"); return U(std::wstring(value, n));
}
void Marker(const std::u16string& path) {
  HANDLE file = CreateFileW(Wide(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "marker"); CloseHandle(file);
}
bool Exists(const std::u16string& path) { return GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }
void Await(const std::u16string& path) {
  uint64_t deadline = GetTickCount64() + 15000;
  while (!Exists(path)) { Check(GetTickCount64() < deadline, "marker timeout"); Sleep(10); }
}
void WaitChild(int pid, int64_t expected) {
  uint64_t deadline = GetTickCount64() + 20000;
  int64_t code;
  while ((code = z::ExitStatus(pid)) == z::kExitRunning) { Check(GetTickCount64() < deadline, "child timeout"); Sleep(10); }
  Check(code == expected, "child exit");
}
struct Pipe {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Pipe() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Pipe() = default; Pipe(const Pipe&) = delete;
};
bool Frame(HANDLE pipe, void* bytes, DWORD size, bool write) {
  uint64_t deadline = GetTickCount64() + 5000;
  uint32_t length = size;
  return c::Transfer(pipe, &length, 4, write, deadline) && length == size &&
      c::Transfer(pipe, bytes, size, write, deadline);
}
bool Reply(HANDLE pipe, std::vector<uint8_t>* bytes) {
  uint32_t length = 0; uint64_t deadline = GetTickCount64() + 5000;
  if (!c::Transfer(pipe, &length, 4, false, deadline) || length > 4096) return false;
  bytes->resize(length);
  return c::Transfer(pipe, bytes->data(), length, false, deadline);
}
bool ProofReply(HANDLE pipe, std::vector<uint8_t>* reply) {
  if (!Reply(pipe, reply)) return false;
  uint32_t tag = 0;
  if (reply->size() == 12) memcpy(&tag, reply->data(), 4);
  if (tag != w::kPeerChallenge) return true;
  uint64_t locator = 0; memcpy(&locator, reply->data()+4, 8);
  HANDLE event = c::Handle(locator);
  ++proof_events_opened;
  bool ok = SetEvent(event) != FALSE;
  Check(CloseHandle(event) != FALSE, "close proof event"); ++proof_events_closed;
  uint32_t ack = w::kPeerAck;
  return ok && Frame(pipe, &ack, 4, true) && Reply(pipe, reply);
}
void Connect(Pipe* pipe) {
  wchar_t device[256]; DWORD n = GetEnvironmentVariableW(L"GESHER_BINDER", device, 256);
  Check(n && n < 256, "device locator");
  std::wstring name = std::wstring(w::kPipePrefix) + device;
  uint64_t deadline = GetTickCount64() + 5000;
  do {
    pipe->value = CreateFileW(name.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe->value != INVALID_HANDLE_VALUE) break;
    Sleep(5);
  } while (GetTickCount64() < deadline);
  Check(pipe->value != INVALID_HANDLE_VALUE && c::Mode(pipe->value, true), "public connection");
}
struct ProcHello {
  uint32_t op = w::kHelloProc, magic = w::kMagic, version = w::kVersion, pid, uid;
  // Serialize explicitly: the wire has no native struct padding.
};
bool Hello(uint32_t uid, uint64_t birth, uint32_t pid = GetCurrentProcessId(),
           Pipe* held_proc = nullptr, Pipe* held_thread = nullptr) {
  Pipe pipe; Connect(&pipe);
  uint8_t bytes[28]{};
  uint32_t words[]{w::kHelloProc, w::kMagic, w::kVersion, pid, uid};
  memcpy(bytes, words, 20); memcpy(bytes+20, &birth, 8);
  std::vector<uint8_t> reply;
  Check(Frame(pipe.value, bytes, sizeof(bytes), true) && ProofReply(pipe.value, &reply), "proc handshake");
  int32_t status = -1; if (reply.size() >= 4) memcpy(&status, reply.data(), 4);
  if (status) return false;
  Check(reply.size() == 16, "proc response");
  uint32_t id = 0; uint64_t key = 0;
  memcpy(&id, reply.data()+4, 4); memcpy(&key, reply.data()+8, 8);
  Pipe thread; Connect(&thread);
  uint32_t threadwords[]{w::kHelloThread, w::kMagic, w::kVersion, id};
  uint8_t request[28]{};
  memcpy(request, threadwords, 16); memcpy(request+16, &key, 8);
  DWORD tid = GetCurrentThreadId(); memcpy(request+24, &tid, 4);
  Check(Frame(thread.value, request, sizeof(request), true) && ProofReply(thread.value, &reply), "thread handshake");
  Check(reply.size() == 4, "thread response"); memcpy(&status, reply.data(), 4);
  Check(status == 0, "registered thread control");
  if (held_proc) { held_proc->value = pipe.value; pipe.value = INVALID_HANDLE_VALUE; }
  if (held_thread) { held_thread->value = thread.value; thread.value = INVALID_HANDLE_VALUE; }
  return true;
}
void PublicNegatives(uint32_t uid) {
  uint64_t birth = c::Birth(GetCurrentProcess());
  Check(Hello(uid, birth), "honest registered proc/thread");
  Check(!Hello(uid + 1, birth), "forged other UID");
  Check(!Hello(1000, birth), "forged system UID");
  Check(!Hello(uid, birth+1), "wrong birth");
  Check(!Hello(uid, birth, GetCurrentProcessId()+4), "forged PID");
  Check(!z::IsAuthority(), "app has no writer handles");
}
int Command(c::Request request, HANDLE process = nullptr, bool wrong_birth = false) {
  const auto& startup = gesher::launch::kStartup;
  Check(z::IsAuthority(), "private writer required");
  if (process) {
    HANDLE remote = nullptr;
    Check(DuplicateHandle(GetCurrentProcess(), process, c::Handle(startup.broker), &remote,
          PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_DUP_HANDLE|SYNCHRONIZE, FALSE, 0), "actual retained transfer");
    ++process_transfers;
    request.process = c::Value(remote); request.birth = c::Birth(process) + (wrong_birth ? 1 : 0);
  }
  int32_t status = -2; uint64_t deadline = GetTickCount64()+5000;
  Check(c::Transfer(c::Handle(startup.control), &request, sizeof(request), true, deadline) &&
        c::Transfer(c::Handle(startup.control), &status, 4, false, deadline), "private control response");
  return status;
}
struct Child {
  PROCESS_INFORMATION pi{};
  ~Child() { if (pi.hProcess) { TerminateProcess(pi.hProcess, 137); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); } if (pi.hThread) CloseHandle(pi.hThread); }
  Child(const std::u16string& command, uint32_t uid) {
    // Explicit zero inherited handles. Self UID snapshot grants no registration.
    gesher::launch::Context context; context.uid = uid;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.cbReserved2 = sizeof(context); startup.lpReserved2 = reinterpret_cast<BYTE*>(&context);
    std::wstring cmd = Wide(command);
    Check(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED|CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &pi), "real suspended controlled child");
  }
  void Resume() { Check(ResumeThread(pi.hThread) != DWORD(-1), "resume after broker ACK"); }
  void Exit() { Check(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "controlled child exits"); DWORD code; Check(GetExitCodeProcess(pi.hProcess, &code) && code == 0, "controlled child success"); }
};

int wmain(int argc, wchar_t** argv) {
  try {
    const auto& startup = gesher::launch::kStartup;
    if (startup.present && startup.kind == 0) {
      Check(argc >= 3, "controlled child arguments");
      std::u16string mode = U(argv[1]), marker = U(argv[2]);
      if (mode == u"exit259" || mode == u"exitmax" || mode == u"exit137") {
        Check(!z::IsAuthority(), "exit child has no writer");
        Marker(marker+u".ready"); Await(marker+u".exit");
        ExitProcess(mode == u"exit259" ? 259 : 0xffffffffUL);
      } else if (mode == u"no-binder") {
        Check(!z::IsAuthority(), "no-Binder child has no writer");
      } else if (mode == u"unregistered") {
        Check(!Hello(startup.uid, c::Birth(GetCurrentProcess())), "unregistered actual self rejected");
        Check(!z::IsAuthority(), "no private writer inheritance");
      } else {
        PublicNegatives(startup.uid);
        if (mode == u"retained") {
          Marker(marker+u".ready"); Await(marker+u".wrong-revoke");
          Check(Hello(startup.uid, c::Birth(GetCurrentProcess())), "wrong install cannot revoke live record");
          Pipe proc, thread;
          Check(Hello(startup.uid, c::Birth(GetCurrentProcess()), GetCurrentProcessId(), &proc, &thread), "retained connection");
          uint8_t read[20]{}; uint32_t op = w::kWriteRead; uint64_t size = 4096;
          memcpy(read, &op, 4); memcpy(read+4, &size, 8);
          Check(Frame(thread.value, read, sizeof(read), true), "pending blocking read submitted");
          Marker(marker+u".still-live"); Await(marker+u".revoke");
          std::vector<uint8_t> reply;
          Check(Reply(thread.value, &reply) && reply.size() >= 4, "revocation wakes accepted connection");
          int32_t status = 0; memcpy(&status, reply.data(), 4);
          Check(status == -9, "accepted connection cannot dispatch after revoke");
          Check(!Hello(startup.uid, c::Birth(GetCurrentProcess())), "revoked live instance rejected");
          Marker(marker+u".revoked"); Await(marker+u".exit");
        }
      }
      Check(proof_events_opened == proof_events_closed, "child proof event balance");
      printf("CHILD_PROOF_EVENTS opened=%u closed=%u\n", proof_events_opened, proof_events_closed);
      return 0;
    }
    if (startup.present && startup.kind == 2) {
      Check(argc >= 2 && z::IsAuthority(), "owned writer bootstrap");
      std::u16string dir = U(argv[1])+u"\\";
      Check(Hello(1000, c::Birth(GetCurrentProcess())), "honest designated system instance");
      for (int i = 0; i < 2; ++i) {
        std::u16string package = i ? u"fixture.b" : u"fixture.a";
        int pid = z::Spawn(Quote(Self())+u" probe "+Quote(dir+u"probe"), {}, {}, dir+package+u".log", false,
                           10000+i, package, u"11111111-1111-1111-1111-111111111111");
        Check(pid > 0, "production package spawn and private publication"); WaitChild(pid, 0);
      }
      // Actual owned terminal259 and all-ones status, with a normal pending
      // control before release. No mocked GetExitCodeProcess or PID reopen.
      for (const auto& mode : {u"exit259", u"exitmax", u"exit137"}) {
        std::u16string marker = dir+mode;
        const std::u16string install = u"22222222-2222-2222-2222-222222222222";
        int pid = z::Spawn(Quote(Self())+u" "+mode+u" "+Quote(marker), {}, {},
                           dir+u"exit.log", false, 10003, u"fixture.exit", install);
        Check(pid > 0, "owned exit counterfactual spawn"); Await(marker+u".ready");
        Check(z::ExitStatus(pid) == z::kExitRunning && z::StartTime(pid) != 0 &&
              z::OwnedInstall(pid, u"fixture.exit", install), "pending owned instance control");
        if (std::u16string(mode) == u"exit137") Check(z::Kill(pid), "owned termination control");
        else Marker(marker+u".exit");
        uint64_t deadline = GetTickCount64()+15000;
        while (z::StartTime(pid) != 0) { Check(GetTickCount64() < deadline, "real exit signal"); Sleep(10); }
        Check(!z::OwnedInstall(pid, u"fixture.exit", install), "dead owned install rejected");
        WaitChild(pid, std::u16string(mode) == u"exit259" ? 259 :
                       std::u16string(mode) == u"exit137" ? 137 : 0xffffffffLL);
        Check(z::ExitStatus(pid) == z::kExitUnknown, "completion consumed retained HANDLE");
      }
      // Measure the real broker with its inherited actual HANDLE. Aggregate
      // kernel counts include process/event/control/public pipe capabilities;
      // never print locators. Poll permits detached pipe worker teardown.
      auto handles = [](HANDLE process) {
        DWORD count = 0; Check(GetProcessHandleCount(process, &count), "kernel handle count"); return count;
      };
      auto barrier = [&] {
        c::Request request; request.op = c::kRevoke;
        memcpy(request.package, "fixture.absent", sizeof("fixture.absent"));
        Check(Command(request) == 0, "maintenance barrier");
      };
      barrier(); Sleep(200);
      DWORD broker_base = handles(c::Handle(startup.broker));
      DWORD writer_base = handles(GetCurrentProcess());
      for (int cycle = 0; cycle < 48; ++cycle) {
        std::u16string mode = cycle % 2 ? u"probe" : u"no-binder";
        int pid = z::Spawn(Quote(Self())+u" "+mode+u" "+Quote(dir+u"churn"), {}, {},
            dir+u"churn.log", false, 10004, u"fixture.churn", u"33333333-3333-3333-3333-333333333333");
        Check(pid > 0, "unchanged install production churn"); WaitChild(pid, 0);
        barrier();
      }
      uint64_t settled = GetTickCount64()+15000;
      DWORD broker_after;
      do {
        barrier(); broker_after = handles(c::Handle(startup.broker));
        if (broker_after <= broker_base) break;
        Sleep(20);
      } while (GetTickCount64() < settled);
      Check(broker_after <= broker_base, "broker process/event/pipe handles stable after churn");
      Check(handles(GetCurrentProcess()) <= writer_base, "writer owned handles released after churn");
      printf("HANDLE_CHURN cycles=48 broker_before=%lu broker_after=%lu writer_before=%lu writer_after=%lu\n",
             broker_base, broker_after, writer_base, handles(GetCurrentProcess()));
      int unknown = z::Spawn(Quote(Self())+u" unregistered "+Quote(dir+u"unknown"), {}, {}, dir+u"unknown.log", false, 10000);
      Check(unknown > 0, "generic child created without publication"); WaitChild(unknown, 0);
      // Explicit cancel is distinct from ordinary death and generation revoke.
      {
        Child cancelled(Quote(Self())+u" no-binder "+Quote(dir+u"cancel"), 10005);
        c::Request publish; publish.op = c::kPublish; publish.uid = 10005;
        memcpy(publish.package, "fixture.cancel", sizeof("fixture.cancel")); publish.generation[0] = 4;
        Check(Command(publish, cancelled.pi.hProcess) == 0, "cancel control publication");
        auto cancel = publish; cancel.op = c::kCancel;
        Check(Command(cancel, cancelled.pi.hProcess) == 0, "cancel exact live birth");
        Check(Command(publish, cancelled.pi.hProcess) != 0, "cancel tombstone prevents live republish");
        cancelled.Resume(); cancelled.Exit();
      }
      // A real signaled process prunes even without any Binder connection. An
      // outstanding shared lease keeps its exact HANDLE valid until released.
      {
        Child dead(Quote(Self())+u" no-binder "+Quote(dir+u"lease"), 10006);
        DWORD before = handles(GetCurrentProcess());
        gesher::binder_launch::Registry registry;
        gesher::binder_launch::Install install{"fixture.lease", {}, 10006};
        install.generation[0] = 5;
        HANDLE duplicate = nullptr;
        Check(DuplicateHandle(GetCurrentProcess(), dead.pi.hProcess, GetCurrentProcess(),
            &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "registry actual duplicate");
        Check(registry.Publish(duplicate, install), "registry live publication");
        auto lease = registry.Find(dead.pi.dwProcessId, c::Birth(dead.pi.hProcess), 10006);
        Check(lease && registry.Live(lease), "registry pending control");
        dead.Resume(); dead.Exit(); registry.PruneDead();
        Check(!registry.Live(lease) && !registry.Find(dead.pi.dwProcessId, lease->birth, 10006),
              "pruned dead lease cannot admit");
        Check(WaitForSingleObject(lease->process, 0) == WAIT_OBJECT_0 &&
              handles(GetCurrentProcess()) == before+1, "outstanding shared lease retains exact signaled HANDLE");
        lease.reset();
        Check(handles(GetCurrentProcess()) == before, "last lease closes retained HANDLE");
        printf("LEASE_HANDLES opened=1 closed=1 before=%lu after=%lu\n", before, handles(GetCurrentProcess()));
      }
      std::u16string marker = dir+u"instance";
      Child child(Quote(Self())+u" retained "+Quote(marker), 10002);
      c::Request publish; publish.op = c::kPublish; publish.uid = 10002;
      memcpy(publish.package, "fixture.retained", sizeof("fixture.retained")); publish.generation[0] = 1;
      Check(Command(publish, child.pi.hProcess, true) != 0, "private wrong birth refused");
      auto forged = publish; forged.uid = 1000;
      Check(Command(forged, child.pi.hProcess) != 0, "writer cannot publish app as system UID");
      Check(Command(publish, child.pi.hProcess) == 0, "actual suspended child registered");
      child.Resume(); Await(marker+u".ready");
      c::Request revoke = publish; revoke.op = c::kRevoke; revoke.generation[0] = 2;
      Check(Command(revoke) == 0, "wrong install revoke processed"); Marker(marker+u".wrong-revoke");
      Await(marker+u".still-live"); barrier(); Sleep(200);
      DWORD before_revoke = handles(c::Handle(startup.broker));
      revoke.generation[0] = 1;
      Check(Command(revoke) == 0, "exact generation revoked"); Marker(marker+u".revoke");
      Await(marker+u".revoked");
      // Child deliberately keeps its accepted proc/thread pipes open and stays
      // alive here. Revoke must release the broker process HANDLE nonetheless.
      uint64_t revoke_deadline = GetTickCount64()+15000;
      DWORD after_revoke;
      do {
        barrier(); after_revoke = handles(c::Handle(startup.broker));
        if (after_revoke < before_revoke) break;
        Sleep(20);
      } while (GetTickCount64() < revoke_deadline);
      Check(after_revoke < before_revoke, "revoke releases lease despite live accepted pipes");
      printf("REVOKE_HANDLES before=%lu after=%lu\n", before_revoke, after_revoke);
      publish.generation[0] = 2;
      Check(Command(publish, child.pi.hProcess) != 0, "reinstall cannot revive live old instance");
      Marker(marker+u".exit"); child.Exit();
      Check(Command(publish, child.pi.hProcess) != 0, "actual terminated instance rejected");
      Check(proof_events_opened == proof_events_closed, "writer proof event balance");
      printf("PROOF_EVENTS opened=%u closed=%u private_process_transfers=%u\n",
             proof_events_opened, proof_events_closed, process_transfers);
      Marker(dir+u"PASS"); return 0;
    }
    Check(!startup.present && argc == 4, "root requires broker, manager and owned empty directory");
    Check(z::JoinJob(0), "fixture root kill-on-close job");
    std::u16string dir = U(argv[3])+u"\\";
    Check(!Exists(dir+u"PASS"), "fresh controlled directory required");
    std::wstring device = L"private-admission-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    Check(SetEnvironmentVariableW(L"GESHER_BINDER", device.c_str()), "fixture device locator");
    auto children = z::StartDevice({Quote(U(argv[1]))+u" "+U(device), Quote(U(argv[2])), Quote(Self())+u" "+Quote(U(argv[3]))},
                                  {dir+u"broker.log", dir+u"manager.log", dir+u"writer.log"}, {u"GESHER_BINDER="+U(device)});
    Check(children.size() == 3, "production initial bootstrap");
    Check(Hello(2000, c::Birth(GetCurrentProcess())), "honest original launcher instance");
    WaitChild(children[2], 0);
    Check(Exists(dir+u"PASS"), "private admission writer result");
    uint64_t deadline = GetTickCount64()+10000;
    while (z::ExitStatus(children[0]) == z::kExitRunning) { Check(GetTickCount64() < deadline, "authority death ends broker"); Sleep(10); }
    z::Kill(children[1]);
    return 0;
  } catch (const std::exception& e) {
    // No control HANDLE, pipe setup name, Proc key or capability enters logs.
    fprintf(stderr, "private admission fixture: %s\n", e.what()); return 1;
  }
}

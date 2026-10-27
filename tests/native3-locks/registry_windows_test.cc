// Actual Windows registry/identity fixture, CI-only. This does not call an
// implemented fcntl path: descriptor/stdio/spawn/signal integration is pending.
#include "../../compat/posix/src/record_lock_liveness.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace gesher::record_locks;

namespace {
unsigned failed = 0;
void Check(bool value, const char* name) {
  std::printf("%s record registry: %s\n", value ? "PASS" : "FAIL", name);
  if (!value) ++failed;
}
void* FailAllocate(size_t) { return nullptr; }
void NoAllocationToRelease(void*) {}
Liveness UnknownProcess(const ProcessIdentity&) { return Liveness::kUnknown; }
Liveness UnknownThread(const ThreadIdentity&) { return Liveness::kUnknown; }

HANDLE OpenFile(const wchar_t* path, DWORD access = GENERIC_READ | GENERIC_WRITE,
                DWORD creation = OPEN_EXISTING) {
  return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
}

Record ProcessRecord(const File& file, const ProcessIdentity& process, uint64_t start) {
  return {file, {Ownership::kProcess, process.pid, process.created}, {start, start + 32},
          Kind::kWrite, static_cast<int32_t>(process.pid)};
}

int Child(const wchar_t* mode, const wchar_t* path) {
  HANDLE file = OpenFile(path);
  File identity{};
  ProcessIdentity process{};
  if (file == INVALID_HANDLE_VALUE || FileIdentity(file, &identity) || CurrentProcessIdentity(&process)) return 91;
  Registry registry;
  RegistryState* state = nullptr;
  if (registry.Open() || registry.Begin(&state)) return 92;
  if (Replace(&state->locks, ProcessRecord(identity, process, 1000), kRecordCapacity)) return 93;
  if (std::wcscmp(mode, L"published") == 0 && registry.Commit()) return 94;
  // The owned child deliberately dies while holding the real named mutex.
  // Before publication the active snapshot must be unchanged; after it the
  // committed record must survive recovery until actual dead-owner pruning.
  ExitProcess(23);
  return 23;
}

struct OwnedChild {
  PROCESS_INFORMATION process{};
  HANDLE job = nullptr;
};

bool RunChild(const wchar_t* mode, const wchar_t* path, OwnedChild* child) {
  wchar_t executable[32768];
  DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  if (length == 0 || length >= std::size(executable)) return false;
  std::wstring command = L"\"" + std::wstring(executable) + L"\" --child " + mode + L" \"" + path + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  child->job = CreateJobObjectW(nullptr, nullptr);
  if (!child->job) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
  // No fixture child executes before it belongs to the owned kill-on-close
  // job. Keep both handles until exit is confirmed even on timeout/setup error.
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child->process)) return false;
  if (!AssignProcessToJobObject(child->job, child->process.hProcess) ||
      ResumeThread(child->process.hThread) == static_cast<DWORD>(-1)) return false;
  DWORD status = WaitForSingleObject(child->process.hProcess, 10000);
  if (status != WAIT_OBJECT_0) {
    bool requested = TerminateProcess(child->process.hProcess, 124) != FALSE;
    DWORD terminate_error = requested ? ERROR_SUCCESS : GetLastError();
    DWORD terminated = WaitForSingleObject(child->process.hProcess, 2000);
    std::fprintf(stderr, "OWNED_REGISTRY_CHILD_TIMEOUT pid=%lu terminate_requested=%d terminate_error=%lu wait=%lu\n",
                 child->process.dwProcessId, requested, terminate_error, terminated);
    // CloseChild still owns the job and process; it must confirm final exit.
    return false;
  }
  DWORD code = 0;
  return GetExitCodeProcess(child->process.hProcess, &code) && code == 23;
}

void CloseChild(OwnedChild* child) {
  if (child->job) {
    if (!CloseHandle(child->job)) {
      std::fprintf(stderr, "FATAL owned registry child job close failed: %lu\n", GetLastError());
      ExitProcess(125);
    }
    child->job = nullptr;  // Windows terminates any still-live assigned child.
  }
  if (child->process.hProcess) {
    DWORD status = WaitForSingleObject(child->process.hProcess, 0);
    if (status != WAIT_OBJECT_0) {
      bool requested = TerminateProcess(child->process.hProcess, 124) != FALSE;
      DWORD terminate_error = requested ? ERROR_SUCCESS : GetLastError();
      status = WaitForSingleObject(child->process.hProcess, 2000);
      std::fprintf(stderr, "OWNED_REGISTRY_CHILD_FINAL_CLEANUP pid=%lu terminate_requested=%d terminate_error=%lu wait=%lu\n",
                   child->process.dwProcessId, requested, terminate_error, status);
    }
    if (status != WAIT_OBJECT_0) {
      std::fputs("FATAL owned registry child termination incomplete; no cleanup proof\n", stderr);
      std::fflush(stderr);
      ExitProcess(126);
    }
    CloseHandle(child->process.hProcess);
  }
  if (child->process.hThread) CloseHandle(child->process.hThread);
  *child = {};
}

std::vector<unsigned char> Bytes(const RegistryState& state) {
  const auto* begin = reinterpret_cast<const unsigned char*>(&state);
  return {begin, begin + sizeof(state)};
}

DWORD WINAPI FinishedThread(void* value) {
  auto* identity = static_cast<ThreadIdentity*>(value);
  return CurrentThreadIdentity(identity) == 0 ? 0 : 95;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  wchar_t ci[16];
  DWORD ci_length = GetEnvironmentVariableW(L"GITHUB_ACTIONS", ci, std::size(ci));
  if (ci_length == 0 || ci_length >= std::size(ci) || std::wcscmp(ci, L"true") != 0) {
    std::fputs("This native registry fixture executes only in CI\n", stderr);
    return 90;
  }
  if (argc > 1) {
    if (argc == 4 && std::wcscmp(argv[1], L"--child") == 0) return Child(argv[2], argv[3]);
    return 96;
  }
  wchar_t temp[32768], path[32768], alias[32768];
  DWORD length = GetTempPathW(std::size(temp), temp);
  if (length == 0 || length >= std::size(temp)) return 2;
  ProcessIdentity process{};
  if (CurrentProcessIdentity(&process)) return 2;
  int written = std::swprintf(path, std::size(path), L"%lsrecord-lock-%lu-%llu.bin", temp,
                 static_cast<unsigned long>(process.pid), static_cast<unsigned long long>(process.created));
  if (written < 0 || static_cast<size_t>(written) >= std::size(path)) return 2;
  written = std::swprintf(alias, std::size(alias), L"%ls.link", path);
  if (written < 0 || static_cast<size_t>(written) >= std::size(alias)) return 2;
  HANDLE file = OpenFile(path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW);
  if (file == INVALID_HANDLE_VALUE) return 2;
  File identity{};
  Check(FileIdentity(file, &identity) == 0, "authoritative volume and 128-bit file identity");
  Check(ProbeProcess(process) == Liveness::kAlive, "actual current process is live");
  ProcessIdentity reused = process;
  ++reused.created;
  Check(ProbeProcess(reused) == Liveness::kDead, "actual matching PID with wrong birth cannot own old locks");
  Check(CreateHardLinkW(alias, path, nullptr), "create independent hardlink spelling");
  HANDLE linked = OpenFile(alias);
  File linked_identity{};
  Check(linked != INVALID_HANDLE_VALUE && FileIdentity(linked, &linked_identity) == 0 && identity == linked_identity,
        "hardlink spellings share the exact file key");
  if (linked != INVALID_HANDLE_VALUE) CloseHandle(linked);
  uint32_t access = 0, handles = 0;
  Check(HandleAccess(file, &access, &handles) == 0 && (access & FILE_READ_DATA) &&
        (access & FILE_WRITE_DATA) && handles > 0, "actual read/write handle granted access");
  HANDLE readonly = OpenFile(path, GENERIC_READ);
  Check(readonly != INVALID_HANDLE_VALUE && HandleAccess(readonly, &access) == 0 &&
        (access & FILE_READ_DATA) && !(access & (FILE_WRITE_DATA | FILE_APPEND_DATA)),
        "read-only handle cannot masquerade as writable");
  if (readonly != INVALID_HANDLE_VALUE) CloseHandle(readonly);
  ThreadIdentity thread_identity{};
  HANDLE thread = CreateThread(nullptr, 0, FinishedThread, &thread_identity, 0, nullptr);
  bool finished = thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
  Check(finished && ProbeThread(thread_identity) == Liveness::kDead,
        "actual exited thread is pruned while its process lives");
  if (thread) CloseHandle(thread);

  Registry registry;
  RegistryState* state = nullptr;
  if (registry.Open() || registry.Begin(&state)) return 3;
  Record parent = ProcessRecord(identity, process, 0);
  Check(Replace(&state->locks, parent, kRecordCapacity) == 0 && registry.Commit() == 0,
        "actual process record committed through inactive bank");
  uint64_t generation = registry.Generation();
  std::vector<unsigned char> original = Bytes(*state);
  registry.Unlock();

  Registry invalid_allocator(FailAllocate, NoAllocationToRelease);
  Check(invalid_allocator.Open() == ENOMEM, "explicit checked staging allocation failure returns ENOMEM");
  if (registry.Begin(&state)) return 3;
  Check(registry.Generation() == generation && Bytes(*state) == original,
        "failed staging allocation preserves actual active bank");
  state->locks.count = kRecordCapacity + 1;
  Check(registry.Commit() == EINVAL, "invalid candidate is rejected before publication");
  registry.Unlock();
  if (registry.Begin(&state)) return 3;
  Check(registry.Generation() == generation && Bytes(*state) == original,
        "rejected candidate preserves generation and every state byte");
  registry.Unlock();

  OwnedChild child{};
  bool crashed = RunChild(L"unpublished", path, &child);
  Check(crashed, "actual owned child exits23 holding named mutex before commit");
  if (!crashed) { CloseChild(&child); return 4; }
  if (registry.Begin(&state)) { CloseChild(&child); return 4; }
  Check(registry.Generation() == generation && Bytes(*state) == original,
        "abandoned precommit writer leaves old active bank authoritative");
  registry.Unlock();
  CloseChild(&child);
  crashed = RunChild(L"published", path, &child);
  Check(crashed, "actual owned child exits23 holding mutex after commit");
  if (!crashed) { CloseChild(&child); return 4; }
  bool recovered = registry.Begin(&state) == 0;
  bool saw_child = false;
  if (recovered) {
    for (const Record& record : state->locks) if (record.pid == static_cast<int32_t>(child.process.dwProcessId) && record.file == identity) {
      saw_child = true;
    }
  }
  Check(recovered && registry.Generation() == generation + 1 && saw_child,
        "abandoned postcommit writer retains its complete committed bank");
  if (!recovered) { CloseChild(&child); CloseHandle(file); return 4; }
  Pruned pruned{};
  PruneDeadOwners(state, &pruned);
  Check(pruned.process_locks >= 1 && registry.Commit() == 0,
        "actual crashed-process identity is removed by conservative pruning");
  registry.Unlock();
  CloseChild(&child);

  if (registry.Begin(&state)) return 5;
  Owner description{Ownership::kDescription, UINT64_C(0x4f46440000000000) | process.pid, process.created};
  Reference reference{description, identity, process, reinterpret_cast<uintptr_t>(file), 42, 0};
  if (state->reference_count >= kReferenceCapacity) return 5;
  state->references[state->reference_count++] = reference;
  Record ofd{identity, description, {2000, 2032}, Kind::kWrite, -1};
  Check(Replace(&state->locks, ofd, kRecordCapacity) == 0, "stage independently owned OFD metadata");
  original = Bytes(*state);
  Probes unknown{UnknownProcess, UnknownThread};
  PruneDeadOwners(state, &pruned, unknown);
  Check(pruned.unknown > 0 && pruned.references == 0 && pruned.process_locks == 0 &&
        pruned.description_locks == 0 && Bytes(*state) == original,
        "explicit denied-query dependency never means dead or drops OFD/process ownership");
  RemoveOwner(&state->locks, description);
  --state->reference_count;
  RemoveOwner(&state->locks, parent.owner, &identity);
  Check(registry.Commit() == 0, "fixture releases only its own file/owner metadata");
  registry.Unlock();
  CloseHandle(file);
  Check(DeleteFileW(alias) && DeleteFileW(path), "fixture-owned temporary file names cleaned");
  std::printf("ACTUAL_RECORD_REGISTRY_SUMMARY failures=%u; scope=storage-identities-only no-fcntl-lifetime-claim\n", failed);
  return failed == 0 ? 0 : 1;
}

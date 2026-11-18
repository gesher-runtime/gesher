// Bounded native capability observations, not a POSIX runtime test.
// Only each controller-created probe process may be retired at its deadline.
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Pair { Handle reader, writer; };
struct PipeInfo { ULONG read_mode, completion_mode; };
struct LocalInfo {
  ULONG type, configuration, maximum_instances, current_instances;
  ULONG inbound_quota, read_available, outbound_quota, write_available, state, end;
};
LONG Query(HANDLE file, void* value, ULONG size, int kind) {
  using Function = LONG (WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
  auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile"));
  if (!function) return static_cast<LONG>(0xc0000002u);
  IO_STATUS_BLOCK status{};
  return function(file, &status, value, size, static_cast<FILE_INFORMATION_CLASS>(kind));
}
bool MakePair(Pair* pair, bool overlapped, bool nowait) {
  wchar_t name[128];
  std::swprintf(name, 128, L"\\\\.\\pipe\\gesher-pr73-capability-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
  DWORD option = overlapped ? FILE_FLAG_OVERLAPPED : 0;
  pair->reader.value = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | option,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS | (nowait ? PIPE_NOWAIT : PIPE_WAIT),
      1, 65536, 65536, 0, nullptr);
  if (pair->reader.value == INVALID_HANDLE_VALUE) return false;
  pair->writer.value = CreateFileW(name, GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
      0, nullptr, OPEN_EXISTING, option, nullptr);
  if (pair->writer.value == INVALID_HANDLE_VALUE) return false;
  Handle event;
  OVERLAPPED operation{};
  if (overlapped) {
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) return false;
    operation.hEvent = event.value;
  }
  if (!ConnectNamedPipe(pair->reader.value, overlapped ? &operation : nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    return false;
  DWORD mode = PIPE_READMODE_BYTE | (nowait ? PIPE_NOWAIT : PIPE_WAIT);
  if (!SetNamedPipeHandleState(pair->writer.value, &mode, nullptr, nullptr)) return false;
  // Match the production public handles' actual directional access.
  HANDLE* ends[] = {&pair->reader.value, &pair->writer.value};
  const DWORD access[] = {FILE_READ_DATA, FILE_WRITE_DATA};
  for (int i = 0; i != 2; ++i) {
    HANDLE reduced = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), *ends[i], GetCurrentProcess(), &reduced,
        access[i] | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, FALSE, 0)) return false;
    CloseHandle(*ends[i]);
    *ends[i] = reduced;
  }
  return true;
}
bool Transfer(HANDLE file, void* bytes, DWORD count, bool writing, bool overlapped, DWORD* transferred) {
  Handle event;
  OVERLAPPED operation{};
  if (overlapped) {
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) return false;
    operation.hEvent = event.value;
  }
  BOOL done = writing ? WriteFile(file, bytes, count, transferred, overlapped ? &operation : nullptr)
                      : ReadFile(file, bytes, count, transferred, overlapped ? &operation : nullptr);
  if (!done && overlapped && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(event.value, 1000) != WAIT_OBJECT_0) {
      CancelIoEx(file, &operation);
      GetOverlappedResult(file, &operation, transferred, TRUE);
      SetLastError(ERROR_TIMEOUT);
      return false;
    }
    done = GetOverlappedResult(file, &operation, transferred, FALSE);
  }
  return done != FALSE;
}
int Reopen(bool original_overlapped, int endpoint, DWORD access) {
  Pair pipe;
  if (!MakePair(&pipe, original_overlapped, true)) {
    std::printf("INFRA make_pair error=%lu\n", GetLastError()); return 2;
  }
  HANDLE original = endpoint ? pipe.writer.value : pipe.reader.value;
  Handle reopened;
  reopened.value = ReOpenFile(original, access | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_OVERLAPPED);
  DWORD open_error = reopened.value == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  std::printf("REOPEN original_async=%d endpoint=%d access=%lu opened=%d error=%lu\n",
      original_overlapped, endpoint, access, reopened.value != INVALID_HANDLE_VALUE, open_error);
  if (reopened.value == INVALID_HANDLE_VALUE) return 0;
  DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  BOOL changed = SetNamedPipeHandleState(reopened.value, &mode, nullptr, nullptr);
  DWORD change_error = changed ? 0 : GetLastError();
  PipeInfo before{}, after{};
  LONG old_status = Query(original, &before, sizeof(before), 23);
  LONG new_status = Query(reopened.value, &after, sizeof(after), 23);
  std::printf("REOPEN_MODE set_wait=%d error=%lu original_status=%08lx original_nowait=%lu new_status=%08lx new_nowait=%lu\n",
      changed != FALSE, change_error, static_cast<unsigned long>(old_status), before.completion_mode,
      static_cast<unsigned long>(new_status), after.completion_mode);
  mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(reopened.value, &mode, nullptr, nullptr)) {
    std::printf("REOPEN_NOWAIT error=%lu\n", GetLastError()); return 0;
  }
  bool read_queue = false, write_queue = false;
  DWORD transferred = 0;
  char byte = 'r', actual = 0;
  if (access & FILE_READ_DATA) {
    bool queued = Transfer(pipe.writer.value, &byte, 1, true, original_overlapped, &transferred) && transferred == 1;
    read_queue = queued && Transfer(reopened.value, &actual, 1, false, true, &transferred) && transferred == 1 && actual == 'r';
    if (!read_queue) Transfer(pipe.reader.value, &actual, 1, false, original_overlapped, &transferred);
  }
  if (access & FILE_WRITE_DATA) {
    byte = 'w';
    bool queued = Transfer(reopened.value, &byte, 1, true, true, &transferred) && transferred == 1;
    write_queue = queued && Transfer(pipe.reader.value, &actual, 1, false, original_overlapped, &transferred) && transferred == 1 && actual == 'w';
  }
  std::printf("REOPEN_QUEUE reads_original=%d writes_original=%d\n", read_queue, write_queue);
  return 0;
}
struct Pending { HANDLE file; char byte = 0; DWORD transferred = 0; BOOL ok = FALSE; };
DWORD WINAPI ReadPending(void* raw) {
  auto* state = static_cast<Pending*>(raw);
  state->ok = ReadFile(state->file, &state->byte, 1, &state->transferred, nullptr);
  return 0;
}
// Microsoft NT OS/2 Named Pipe Specification, sections 5.17.1.1 and
// 5.17.1.5: local assignment uses {HANDLE, ULONG}; query uses the event HANDLE
// as input on an NPFS root handle, returning FILE_PIPE_EVENT_BUFFER entries.
// https://computernewb.com/~lily/files/Documents/NTDesignWorkbook/namepipe.pdf
struct AssignEvent { HANDLE event; ULONG key; };
struct PipeEvent { ULONG state, type, bytes, key, requests; };
bool QueryEvent(HANDLE root, HANDLE assigned, const char* phase) {
  constexpr DWORD query_event = 0x00110010;  // FSCTL_PIPE_QUERY_EVENT (WDK)
  PipeEvent entries[8]{};
  DWORD bytes = 0;
  Handle event;
  event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event.value) return false;
  OVERLAPPED operation{};
  operation.hEvent = event.value;
  BOOL done = DeviceIoControl(root, query_event, &assigned, sizeof(assigned),
      entries, sizeof(entries), &bytes, &operation);
  DWORD error = done ? ERROR_SUCCESS : GetLastError();
  if (!done && error == ERROR_IO_PENDING) {
    const DWORD wait = WaitForSingleObject(event.value, 1000);
    if (wait != WAIT_OBJECT_0) {
      CancelIoEx(root, &operation);
      GetOverlappedResult(root, &operation, &bytes, TRUE);
      std::printf("EVENT_QUERY phase=%s timed_out=1 wait=%lu\n", phase, wait);
      return false;
    }
    done = GetOverlappedResult(root, &operation, &bytes, FALSE);
    error = done ? ERROR_SUCCESS : GetLastError();
  }
  std::printf("EVENT_QUERY phase=%s ok=%d error=%lu bytes=%lu\n", phase, done != FALSE, error, bytes);
  if (!done || bytes > sizeof(entries) || bytes % sizeof(PipeEvent) != 0) return false;
  for (DWORD i = 0; i != bytes / sizeof(PipeEvent); ++i) {
    const auto& item = entries[i];
    std::printf("EVENT_ENTRY phase=%s state=%lu type=%lu bytes=%lu key=%lu requests=%lu\n",
        phase, item.state, item.type, item.bytes, item.key, item.requests);
  }
  return true;
}
int AssignedEventMetadata() {
  Pair pipe;
  if (!MakePair(&pipe, false, false)) {
    std::printf("INFRA make_pair error=%lu\n", GetLastError()); return 2;
  }
  Handle assigned, root, worker;
  assigned.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!assigned.value) return 2;
  root.value = CreateFileW(L"\\\\.\\pipe\\", GENERIC_READ | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  std::printf("EVENT_ROOT opened=%d error=%lu\n", root.value != INVALID_HANDLE_VALUE,
      root.value == INVALID_HANDLE_VALUE ? GetLastError() : 0);
  if (root.value == INVALID_HANDLE_VALUE) return 0;
  const HANDLE ends[] = {pipe.reader.value, pipe.writer.value};
  constexpr DWORD assign_event = 0x00110000;  // FSCTL_PIPE_ASSIGN_EVENT (WDK)
  for (ULONG i = 0; i != 2; ++i) {
    AssignEvent request{assigned.value, i + 1};
    DWORD bytes = 0;
    const BOOL done = DeviceIoControl(ends[i], assign_event, &request, sizeof(request),
        nullptr, 0, &bytes, nullptr);
    std::printf("EVENT_ASSIGN endpoint=%lu ok=%d error=%lu\n", i, done != FALSE, done ? 0 : GetLastError());
    if (!done) return 0;  // An unavailable primitive is an observation, not a pass.
  }
  if (!QueryEvent(root.value, assigned.value, "empty")) return 0;
  Pending state{pipe.reader.value};
  worker.value = CreateThread(nullptr, 0, ReadPending, &state, 0, nullptr);
  if (!worker.value) return 2;
  Sleep(20);
  const bool queried = QueryEvent(root.value, assigned.value, "pending-sync-read");
  char value[] = "data";
  DWORD bytes = 0;
  const BOOL written = WriteFile(pipe.writer.value, value, 4, &bytes, nullptr);
  std::printf("EVENT_TRANSFER write_ok=%d bytes=%lu error=%lu\n", written != FALSE, bytes, written ? 0 : GetLastError());
  if (WaitForSingleObject(worker.value, 1000) != WAIT_OBJECT_0) {
    CancelSynchronousIo(worker.value);
    WaitForSingleObject(worker.value, INFINITE);  // Controller still owns a 3 s deadline.
  }
  std::printf("EVENT_TRANSFER read_ok=%d bytes=%lu value=%u\n", state.ok != FALSE,
      state.transferred, static_cast<unsigned>(static_cast<unsigned char>(state.byte)));
  if (!queried) return 0;
  QueryEvent(root.value, assigned.value, "buffered-three");
  CloseHandle(pipe.writer.value);
  pipe.writer.value = INVALID_HANDLE_VALUE;
  QueryEvent(root.value, assigned.value, "last-writer-closed");
  return 0;
}
int PendingMetadata(bool overlapped) {
  Pair pipe;
  if (!MakePair(&pipe, overlapped, false)) {
    std::printf("INFRA make_pair error=%lu\n", GetLastError()); return 2;
  }
  Handle event, worker;
  event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event.value) return 2;
  OVERLAPPED operation{};
  operation.hEvent = event.value;
  Pending state{pipe.reader.value};
  if (overlapped) {
    BOOL done = ReadFile(state.file, &state.byte, 1, nullptr, &operation);
    if (done || GetLastError() != ERROR_IO_PENDING) {
      std::printf("INFRA expected_pending done=%d error=%lu\n", done != FALSE, GetLastError()); return 2;
    }
  } else {
    worker.value = CreateThread(nullptr, 0, ReadPending, &state, 0, nullptr);
    if (!worker.value) return 2;
    Sleep(20);
  }
  std::printf("PENDING_QUERY begin async=%d\n", overlapped);
  PipeInfo flags{};
  LocalInfo local{};
  LONG flag_status = Query(state.file, &flags, sizeof(flags), 23);
  LONG local_status = Query(state.file, &local, sizeof(local), 24);
  std::printf("PENDING_QUERY end async=%d flag_status=%08lx local_status=%08lx bytes=%lu state=%lu\n",
      overlapped, static_cast<unsigned long>(flag_status), static_cast<unsigned long>(local_status), local.read_available, local.state);
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  BOOL changed = SetNamedPipeHandleState(state.file, &mode, nullptr, nullptr);
  std::printf("PENDING_MODE async=%d changed=%d error=%lu\n", overlapped, changed != FALSE, changed ? 0 : GetLastError());
  if (overlapped) {
    CancelIoEx(state.file, &operation);
    GetOverlappedResult(state.file, &operation, &state.transferred, TRUE);
  } else {
    CancelSynchronousIo(worker.value);
    WaitForSingleObject(worker.value, INFINITE);
  }
  return 0;
}
bool Run(const std::wstring& arguments) {
  wchar_t executable[32768];
  DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
  if (!count || count >= 32768) return false;
  std::wstring command = L"\"" + std::wstring(executable, count) + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  std::printf("CASE_START %ls\n", arguments.c_str());
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) return false;
  Handle child, thread;
  child.value = process.hProcess;
  thread.value = process.hThread;
  DWORD wait = WaitForSingleObject(child.value, 3000);
  bool timeout = wait == WAIT_TIMEOUT;
  if (wait != WAIT_OBJECT_0) {
    // This handle identifies only the child this controller just created.
    if (!TerminateProcess(child.value, 124)) return false;
    if (WaitForSingleObject(child.value, 1000) != WAIT_OBJECT_0) return false;
  }
  DWORD code = MAXDWORD;
  if (!GetExitCodeProcess(child.value, &code)) return false;
  std::printf("CASE_END %ls timeout=%d exit=%lu\n", arguments.c_str(), timeout, code);
  return timeout || code == 0;
}
}
int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 5 && std::strcmp(argv[1], "reopen") == 0)
    return Reopen(std::atoi(argv[2]) != 0, std::atoi(argv[3]), static_cast<DWORD>(std::atoi(argv[4])));
  if (argc == 3 && std::strcmp(argv[1], "pending") == 0) return PendingMetadata(std::atoi(argv[2]) != 0);
  if (argc == 2 && std::strcmp(argv[1], "assigned-event") == 0) return AssignedEventMetadata();
  std::puts("DIAGNOSTIC_ONLY runtime_approval=0 timeouts_are_observations=1");
  bool complete = true;
  complete = Run(L"assigned-event") && complete;
  for (int asynchronous = 0; asynchronous != 2; ++asynchronous) {
    for (int endpoint = 0; endpoint != 2; ++endpoint) {
      for (unsigned access = 1; access <= 3; ++access) {
        complete = Run(L"reopen " + std::to_wstring(asynchronous) + L" " + std::to_wstring(endpoint) + L" " + std::to_wstring(access)) && complete;
      }
    }
    complete = Run(L"pending " + std::to_wstring(asynchronous)) && complete;
  }
  std::printf("OBSERVATIONS_COMPLETE=%d\n", complete);
  return complete ? 0 : 1;
}

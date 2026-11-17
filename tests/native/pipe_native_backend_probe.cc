// Windows transport feasibility checks. This executable deliberately links
// only the native CRT: a Gesher wrapper must not hide the kernel observations.
// It is a diagnostic prerequisite, not proof that the POSIX pipe backend works.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <io.h>
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
void Check(bool ok, const char* name) {
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s (Win32=%lu)\n", name, GetLastError()); }
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Pair { Handle reader, writer; };

bool MakePair(Pair* pair, bool overlapped, bool nowait = false,
              bool change_mode = false, bool server_writer = false) {
  static unsigned sequence = 0;
  wchar_t name[128];
  std::swprintf(name, std::size(name), L"\\\\.\\pipe\\gesher-native-pipe-probe-%lu-%u",
                GetCurrentProcessId(), ++sequence);
  DWORD mode = PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS |
               (nowait ? PIPE_NOWAIT : PIPE_WAIT);
  DWORD options = FILE_FLAG_FIRST_PIPE_INSTANCE | (overlapped ? FILE_FLAG_OVERLAPPED : 0);
  HANDLE* server = server_writer ? &pair->writer.value : &pair->reader.value;
  HANDLE* client = server_writer ? &pair->reader.value : &pair->writer.value;
  // CreateNamedPipe accepts only its documented mode flags, not arbitrary
  // FILE_* access bits. Duplex grants the server mode-changing rights for the
  // separate exploratory mode-change test; fixed-NOWAIT cases stay one-way.
  const DWORD access = change_mode ? PIPE_ACCESS_DUPLEX :
                       server_writer ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
  *server = CreateNamedPipeW(name, access | options, mode,
                              1, server_writer ? 65536 : 0, server_writer ? 0 : 65536, 0, nullptr);
  if (*server == INVALID_HANDLE_VALUE) return false;
  // A client reader needs FILE_WRITE_ATTRIBUTES for its one initial mode
  // transition. This does not grant FILE_WRITE_DATA or change its direction.
  const DWORD client_access = server_writer ? GENERIC_READ | FILE_WRITE_ATTRIBUTES
                                             : GENERIC_WRITE | FILE_READ_ATTRIBUTES;
  *client = CreateFileW(name, client_access, 0, nullptr, OPEN_EXISTING,
                         overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
  if (*client == INVALID_HANDLE_VALUE) return false;
  // Opening the client before ConnectNamedPipe is a documented successful
  // connection: the subsequent connect reports ERROR_PIPE_CONNECTED.
  Handle event;
  OVERLAPPED operation{};
  if (overlapped) {
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) return false;
    operation.hEvent = event.value;
  }
  BOOL connected = ConnectNamedPipe(*server, overlapped ? &operation : nullptr);
  if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) return false;
  if (nowait) {
    DWORD client_mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return SetNamedPipeHandleState(*client, &client_mode, nullptr, nullptr) != FALSE;
  }
  return true;
}

bool Transfer(HANDLE file, void* bytes, DWORD count, bool write, bool overlapped,
              DWORD* transferred) {
  Handle event;
  OVERLAPPED operation{};
  if (overlapped) {
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) return false;
    operation.hEvent = event.value;
  }
  BOOL done = write ? WriteFile(file, bytes, count, transferred, overlapped ? &operation : nullptr)
                    : ReadFile(file, bytes, count, transferred, overlapped ? &operation : nullptr);
  if (!done && overlapped && GetLastError() == ERROR_IO_PENDING) {
    DWORD wait = WaitForSingleObject(event.value, 2000);
    if (wait != WAIT_OBJECT_0) {
      CancelIoEx(file, &operation);
      // Cancellation is a request. Retain every native resource until this
      // exact operation has completed before leaving its stack frame.
      GetOverlappedResult(file, &operation, transferred, TRUE);
      SetLastError(ERROR_TIMEOUT);
      return false;
    }
    done = GetOverlappedResult(file, &operation, transferred, FALSE);
  }
  return done != FALSE;
}

void Modes(bool overlapped) {
  std::printf("BACKEND mode=%s\n", overlapped ? "overlapped" : "synchronous");
  Pair pipe;
  if (!MakePair(&pipe, overlapped, false, true)) {
    Check(false, "create duplex server for the mode-change observation"); return;
  }
  std::array<char, 4096> bytes{};
  DWORD wrote = 0;
  Check(Transfer(pipe.writer.value, bytes.data(), bytes.size(), true, overlapped, &wrote) && wrote == bytes.size(),
        "queue data before changing native mode");
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  BOOL read_mode = SetNamedPipeHandleState(pipe.reader.value, &mode, nullptr, nullptr);
  DWORD read_error = read_mode ? ERROR_SUCCESS : GetLastError();
  BOOL write_mode = SetNamedPipeHandleState(pipe.writer.value, &mode, nullptr, nullptr);
  DWORD write_error = write_mode ? ERROR_SUCCESS : GetLastError();
  std::printf("BUFFERED_MODE_CHANGE reader=%u reader_error=%lu writer=%u writer_error=%lu\n",
              read_mode != FALSE, read_error, write_mode != FALSE, write_error);
  Check(read_mode != FALSE, "reader can change mode while bytes are buffered");
  Check(write_mode != FALSE, "writer can change mode while bytes are buffered");
  if (!read_mode || !write_mode) return;

  Handle alias;
  Check(DuplicateHandle(GetCurrentProcess(), pipe.writer.value, GetCurrentProcess(), &alias.value,
                        0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE, "duplicate writer");
  mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  BOOL alias_changed = SetNamedPipeHandleState(alias.value, &mode, nullptr, nullptr);
  DWORD observed = MAXDWORD;
  BOOL observed_ok = GetNamedPipeHandleStateW(pipe.writer.value, &observed, nullptr, nullptr, nullptr, nullptr, 0);
  std::printf("ALIAS_MODE_CHANGE changed=%u query=%u observed=%lu\n",
              alias_changed != FALSE, observed_ok != FALSE, observed);
  Check(alias_changed && observed_ok && (observed & PIPE_NOWAIT) == 0,
        "native wait mode is shared through a duplicate");
  mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe.writer.value, &mode, nullptr, nullptr)) {
    Check(false, "restore nonblocking writer"); return;
  }
}

void Atomicity(bool overlapped, bool server_writer) {
  std::printf("ATOMICITY mode=%s server=%s\n", overlapped ? "overlapped" : "synchronous",
              server_writer ? "writer" : "reader");
  Pair pipe;
  if (!MakePair(&pipe, overlapped, true, false, server_writer)) {
    Check(false, "create fixed-NOWAIT atomicity pipe"); return;
  }
  DWORD reader_mode = 0, writer_mode = 0;
  const bool query_reader = GetNamedPipeHandleStateW(pipe.reader.value, &reader_mode, nullptr, nullptr, nullptr, nullptr, 0) != FALSE;
  const bool query_writer = GetNamedPipeHandleStateW(pipe.writer.value, &writer_mode, nullptr, nullptr, nullptr, nullptr, 0) != FALSE;
  std::printf("FIXED_NOWAIT reader_query=%u reader_mode=%lu writer_query=%u writer_mode=%lu\n",
              query_reader, reader_mode, query_writer, writer_mode);
  Check(query_reader && query_writer && (reader_mode & PIPE_NOWAIT) && (writer_mode & PIPE_NOWAIT),
        "both native endpoints permanently use NOWAIT");
  char empty_byte = 0;
  DWORD empty_count = MAXDWORD;
  const BOOL empty_ok = Transfer(pipe.reader.value, &empty_byte, 1, false, overlapped, &empty_count);
  const DWORD empty_error = empty_ok ? ERROR_SUCCESS : GetLastError();
  std::printf("FIXED_NOWAIT_EMPTY_READ ok=%u bytes=%lu error=%lu\n", empty_ok, empty_count, empty_error);
  Check(!empty_ok && empty_error == ERROR_NO_DATA && empty_count == 0,
        "empty connected NOWAIT read is immediate with no data");
  std::array<char, 4096> bytes{};
  DWORD wrote = 0;
  DWORD buffered = 0;
  bool full = false;
  for (unsigned attempt = 0; attempt < 256; ++attempt) {
    wrote = MAXDWORD;
    if (!Transfer(pipe.writer.value, bytes.data(), bytes.size(), true, overlapped, &wrote)) {
      Check(false, "nonblocking message write succeeds or commits zero bytes"); return;
    }
    if (wrote == 0) { full = true; break; }
    if (wrote != bytes.size()) { Check(false, "message write is never partially committed"); return; }
    buffered += wrote;
  }
  Check(full, "bounded native quota fills");
  if (!full) return;
  DWORD read = 0;
  Check(Transfer(pipe.reader.value, bytes.data(), 1024, false, overlapped, &read) && read == 1024,
        "drain less than PIPE_BUF");
  wrote = MAXDWORD;
  Check(Transfer(pipe.writer.value, bytes.data(), bytes.size(), true, overlapped, &wrote) && wrote == 0,
        "insufficient space rejects complete 4096-byte write");
  DWORD available = 0;
  Check(PeekNamedPipe(pipe.reader.value, nullptr, 0, nullptr, &available, nullptr) && available == buffered - 1024,
        "rejected write leaves queued data unchanged");
  Check(Transfer(pipe.reader.value, bytes.data(), 3072, false, overlapped, &read) && read == 3072,
        "complete one PIPE_BUF of free quota");
  wrote = MAXDWORD;
  Check(Transfer(pipe.writer.value, bytes.data(), bytes.size(), true, overlapped, &wrote) && wrote == bytes.size(),
        "a whole PIPE_BUF is accepted when space becomes available");
}

struct LocalInformation {
  ULONG type, configuration, maximum_instances, current_instances;
  ULONG inbound_quota, read_available, outbound_quota, write_available, state, end;
};
bool QueryLocal(HANDLE pipe, LocalInformation* information, LONG* result = nullptr) {
  using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
  HMODULE module = GetModuleHandleW(L"ntdll.dll");
  auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(module, "NtQueryInformationFile")));
  if (!query) return false;
  IO_STATUS_BLOCK status{};
  LONG native = query(pipe, &status, information, sizeof(*information), static_cast<FILE_INFORMATION_CLASS>(24));
  if (result) *result = native;
  return native >= 0;
}
void DescribePipe(const char* observation, HANDLE pipe) {
  LocalInformation info{};
  LONG native = static_cast<LONG>(0xc0000002u);
  bool queried = QueryLocal(pipe, &info, &native);
  std::printf("PIPE_LOCAL %s query=%u ntstatus=%08lx type=%lu config=%lu read_available=%lu inbound=%lu outbound=%lu writable=%lu state=%lu end=%lu\n",
              observation, queried, static_cast<unsigned long>(native), info.type, info.configuration,
              info.read_available, info.inbound_quota, info.outbound_quota, info.write_available, info.state, info.end);
}
void QueuedAndClosedPeerInformation(bool nowait, bool server_writer) {
  std::printf("STATE_BACKEND fixed_nowait=%u server=%s\n", nowait, server_writer ? "writer" : "reader");
  Pair pipe;
  if (!MakePair(&pipe, false, nowait, false, server_writer)) { Check(false, "create queued-state pipe"); return; }
  DWORD transferred = 0;
  Check(WriteFile(pipe.writer.value, "abcdef", 6, &transferred, nullptr) && transferred == 6,
        "queue six real bytes for endpoint observations");
  DescribePipe("queued-reader", pipe.reader.value);
  DescribePipe("queued-writer", pipe.writer.value);
  char bytes[64]{};
  Check(ReadFile(pipe.reader.value, bytes, 2, &transferred, nullptr) && transferred == 2,
        "consume two bytes before resampling both endpoints");
  DescribePipe("partial-reader", pipe.reader.value);
  DescribePipe("partial-writer", pipe.writer.value);
  CloseHandle(pipe.reader.value);
  pipe.reader.value = INVALID_HANDLE_VALUE;
  DescribePipe("closed-reader-writer", pipe.writer.value);
  if (nowait) {
    LocalInformation information{};
    const bool query = QueryLocal(pipe.writer.value, &information);
    const ULONG quota = information.end == 0 ? information.inbound_quota : information.outbound_quota;
    Check(query && quota >= information.write_available && quota - information.write_available == 4,
          "Linux FIONREAD retains four queued bytes after the last reader closes");
    transferred = MAXDWORD;
    const BOOL wrote = WriteFile(pipe.writer.value, "x", 1, &transferred, nullptr);
    const DWORD error = wrote ? ERROR_SUCCESS : GetLastError();
    std::printf("FIXED_NOWAIT_BROKEN_WRITE ok=%u bytes=%lu error=%lu\n", wrote, transferred, error);
    Check(!wrote && (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE),
          "NOWAIT writer observes genuine last-reader closure");
  }

  Pair closing;
  if (!MakePair(&closing, false, nowait, false, server_writer)) { Check(false, "create writer-close pipe"); return; }
  Check(WriteFile(closing.writer.value, "abcdef", 6, &transferred, nullptr) && transferred == 6,
        "queue bytes before writer closes");
  CloseHandle(closing.writer.value);
  closing.writer.value = INVALID_HANDLE_VALUE;
  DescribePipe("closed-writer-buffered-reader", closing.reader.value);
  Check(ReadFile(closing.reader.value, bytes, sizeof(bytes), &transferred, nullptr) && transferred == 6,
        "buffer remains readable after genuine last-writer close");
  DescribePipe("closed-writer-drained-reader", closing.reader.value);
}
struct PendingRead { HANDLE file; HANDLE started; char bytes[8191]; DWORD read = 0; BOOL ok = FALSE; };
DWORD WINAPI NativeReader(void* raw) {
  auto* state = static_cast<PendingRead*>(raw);
  SetEvent(state->started);
  state->ok = ReadFile(state->file, state->bytes, sizeof(state->bytes), &state->read, nullptr);
  return 0;
}
void PendingReaderQuota() {
  Pair pipe;
  if (!MakePair(&pipe, false)) { Check(false, "create pending-reader pipe"); return; }
  Handle started;
  started.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!started.value) { Check(false, "create reader rendezvous"); return; }
  PendingRead state{pipe.reader.value, started.value, {}, 0, FALSE};
  Handle reader;
  reader.value = CreateThread(nullptr, 0, NativeReader, &state, 0, nullptr);
  if (!reader.value) { Check(false, "start native reader"); return; }
  WaitForSingleObject(started.value, 2000);
  // Only the actual pending native read can reveal the kernel quota behavior.
  Sleep(20);
  LocalInformation information{};
  bool queried = QueryLocal(pipe.writer.value, &information);
  std::printf("PENDING_READER_QUOTA query=%u read_available=%lu inbound=%lu outbound=%lu writable=%lu state=%lu end=%lu\n",
              queried, information.read_available, information.inbound_quota, information.outbound_quota,
              information.write_available, information.state, information.end);
  Check(queried, "query writer while native reader is pending");
  char byte = 'x';
  DWORD written = 0;
  Check(WriteFile(pipe.writer.value, &byte, 1, &written, nullptr) && written == 1,
        "native write supplies pending reader");
  DWORD wait = WaitForSingleObject(reader.value, 2000);
  if (wait != WAIT_OBJECT_0) {
    CancelSynchronousIo(reader.value);
    WaitForSingleObject(reader.value, INFINITE);
  }
  Check(wait == WAIT_OBJECT_0 && state.ok && state.read == 1 && state.bytes[0] == 'x',
        "pending reader retires with real byte");
}

int Child(const char* mode) {
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
  char bytes[4]{};
  if (std::strcmp(mode, "--child-read") == 0) {
    return _read(0, bytes, sizeof(bytes)) == static_cast<int>(sizeof(bytes)) &&
           std::memcmp(bytes, "pipe", sizeof(bytes)) == 0 ? 0 : 81;
  }
  // Exceed the native quota so a successful short immediate write cannot
  // conceal an incompatible pending-I/O contract in the inherited handle.
  std::array<char, 256 * 1024> output{};
  output.fill('q');
  return _write(1, output.data(), output.size()) == static_cast<int>(output.size()) ? 0 : 82;
}
void NativeChild(bool overlapped, bool child_reads) {
  Pair pipe;
  if (!MakePair(&pipe, overlapped)) { Check(false, "create native-child pipe"); return; }
  Handle inherited, null;
  HANDLE endpoint = child_reads ? pipe.reader.value : pipe.writer.value;
  if (!DuplicateHandle(GetCurrentProcess(), endpoint, GetCurrentProcess(), &inherited.value,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) { Check(false, "duplicate child endpoint"); return; }
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  null.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &security, OPEN_EXISTING, 0, nullptr);
  if (null.value == INVALID_HANDLE_VALUE) { Check(false, "open child unused stdio"); return; }
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<unsigned char> attributes(attribute_bytes);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_bytes)) { Check(false, "initialize child handle list"); return; }
  HANDLE inherited_handles[]{inherited.value, null.value};
  bool listed = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          inherited_handles, sizeof(inherited_handles), nullptr, nullptr) != FALSE;
  wchar_t executable[32768];
  DWORD length = GetModuleFileNameW(nullptr, executable, std::size(executable));
  if (length == 0 || length >= std::size(executable)) {
    DeleteProcThreadAttributeList(list);
    Check(false, "locate native CRT probe executable");
    return;
  }
  std::wstring command = L"\"" + std::wstring(executable, length) + L"\" " +
                         (child_reads ? L"--child-read" : L"--child-write");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_reads ? inherited.value : null.value;
  startup.StartupInfo.hStdOutput = child_reads ? null.value : inherited.value;
  startup.StartupInfo.hStdError = null.value;
  startup.lpAttributeList = list;
  PROCESS_INFORMATION process{};
  BOOL created = listed && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                                           EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                           &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(list);
  if (!created) { Check(false, "launch ordinary CRT child"); return; }
  Handle child, initial_thread;
  child.value = process.hProcess;
  initial_thread.value = process.hThread;
  char bytes[4] = {'p', 'i', 'p', 'e'};
  DWORD transferred = 0;
  if (child_reads) {
    // Leave stdin empty long enough to exercise a pending CRT read. A
    // pre-seeded four-byte pipe would test only immediate native completion.
    WaitForSingleObject(child.value, 100);
    Check(Transfer(pipe.writer.value, bytes, sizeof(bytes), true, overlapped, &transferred) && transferred == sizeof(bytes),
          "supply native CRT child's stdin");
  }
  size_t received = 0;
  bool valid_output = true;
  if (!child_reads) {
    std::array<char, 4096> output{};
    const ULONGLONG deadline = GetTickCount64() + 3000;
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(pipe.reader.value, nullptr, 0, nullptr, &available, nullptr)) break;
      if (available != 0) {
        DWORD take = std::min<DWORD>(available, output.size());
        if (!Transfer(pipe.reader.value, output.data(), take, false, overlapped, &transferred)) {
          valid_output = false;
          break;
        }
        received += transferred;
        valid_output = valid_output &&
            std::all_of(output.begin(), output.begin() + transferred, [](char byte) { return byte == 'q'; });
        continue;
      }
      if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) break;
      Sleep(1);
    }
  }
  DWORD wait = WaitForSingleObject(child.value, 3000);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(child.value, 83);
    WaitForSingleObject(child.value, INFINITE);
  }
  DWORD exit_code = MAXDWORD;
  GetExitCodeProcess(child.value, &exit_code);
  std::printf("NATIVE_CHILD mode=%s direction=%s wait=%lu exit=%lu\n",
              overlapped ? "overlapped" : "synchronous", child_reads ? "read" : "write", wait, exit_code);
  Check(wait == WAIT_OBJECT_0 && exit_code == 0, "native CRT child accepts inherited endpoint");
  if (!child_reads) Check(valid_output && received == 256 * 1024,
                         "native CRT child writes beyond pipe quota without lost data");
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2) return Child(argv[1]);
  std::printf("DIAGNOSTIC_ONLY production_backend_validated=0\n");
  Modes(false);
  Modes(true);
  Atomicity(false, false);
  Atomicity(true, false);
  Atomicity(false, true);
  Atomicity(true, true);
  QueuedAndClosedPeerInformation(false, false);
  QueuedAndClosedPeerInformation(true, false);
  QueuedAndClosedPeerInformation(true, true);
  PendingReaderQuota();
  NativeChild(false, true);
  NativeChild(false, false);
  NativeChild(true, true);
  NativeChild(true, false);
  std::printf("pipe native backend probe: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

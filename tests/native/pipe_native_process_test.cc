// Actual Windows pipe/OFD transport. The component's job runner owns every
// child and supplies a hard deadline; no foreign process or pipe is touched.
#include "exec_spawn.h"
#include "../../compat/posix/src/ofd_internal.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <fcntl.h>
#include <io.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int checks, failures;
static void Check(const char* name, bool result) {
  ++checks;
  if (!result) { ++failures; std::printf("FAIL pipe process: %s errno=%d win32=%lu\n", name, errno, GetLastError()); }
}
static gesher::exec::Launch Target(const std::wstring& path, const wchar_t* mode) {
  gesher::exec::Launch result;
  result.application = path;
  gesher::exec::AppendQuoted(&result.command_line, path);
  result.command_line += L' ';
  result.command_line += mode;
  result.kind = "exe";
  return result;
}
static bool WaitChild(PROCESS_INFORMATION* child) {
  CloseHandle(child->hThread);
  DWORD status = 127;
  bool done = WaitForSingleObject(child->hProcess, 5000) == WAIT_OBJECT_0;
  if (done) GetExitCodeProcess(child->hProcess, &status);
  else { TerminateProcess(child->hProcess, 127); WaitForSingleObject(child->hProcess, 1000); }
  if (!done || status != 0)
    std::printf("PIPE_CHILD_RESULT pid=%lu completed=%d status=%lu\n", child->dwProcessId, done, status);
  CloseHandle(child->hProcess);
  *child = {};
  return done && status == 0;
}
static bool StandardDescriptors(bool diagnostic_stdio) {
  for (int fd : {0, 1, 2}) {
    errno = 0;
    const int flags = fcntl(fd, F_GETFD);
    const int error = errno;
    HANDLE handle = GetStdHandle(fd == 0 ? STD_INPUT_HANDLE :
                                 fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    const bool native_open = handle && handle != INVALID_HANDLE_VALUE;
    if (diagnostic_stdio && fd != 0) {
      if (flags < 0 || !native_open) return false;
    } else if (flags != -1 || error != EBADF || native_open) {
      return false;
    }
  }
  return true;
}
static void NativePeer(const std::wstring& native) {
  int input[2], output[2];
  if (pipe2(input, O_CLOEXEC) < 0) { Check("native input pipe", false); return; }
  if (pipe2(output, O_CLOEXEC) < 0) { close(input[0]); close(input[1]); Check("native output pipe", false); return; }
  auto reader = gesher::CaptureDescriptor(input[0]);
  auto writer = gesher::CaptureDescriptor(output[1]);
  if (!reader || !writer) {
    Check("native descriptors captured", false);
    for (int fd : {input[0], input[1], output[0], output[1]}) close(fd);
    return;
  }
  std::vector<gesher::exec::ChildFd> files{{37, reader->handle, reader}, {38, writer->handle, writer}};
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(Target(native, L"--pipe"), nullptr, native, files, &child);
  Check("invalid directory fails before a child can run", error != 0);
  if (!error) WaitChild(&child);
  Check("failed launch restores known pipe policy", fcntl(input[0], F_GETFL) == O_RDONLY &&
        fcntl(output[1], F_GETFL) == O_WRONLY);
  error = gesher::exec::SpawnWithFds(Target(native, L"--pipe"), nullptr, L"", files, &child);
  Check("ordinary native child receives synchronous directional handles", error == 0);
  if (!error) {
    Check("native-exposed status is explicitly unknown", fcntl(input[0], F_GETFL) == -1 && errno == EOPNOTSUPP);
    pollfd exposed{input[0], POLLIN, 0};
    Check("unmediated poll reports an operation error", poll(&exposed, 1, 0) == -1 && errno == EOPNOTSUPP && exposed.revents == 0);
    files.clear(); reader.reset(); writer.reset();
    close(input[0]); input[0] = -1;
    close(output[1]); output[1] = -1;
    pollfd empty{output[0], POLLIN, 0};
    Check("native writer survives all local writer aliases", poll(&empty, 1, 0) == 0 && empty.revents == 0);
    pollfd writable{input[1], POLLOUT, 0};
    Check("native reader survives all local reader aliases", poll(&writable, 1, 0) == 1 &&
          (writable.revents & POLLOUT) != 0 && (writable.revents & (POLLERR | POLLHUP)) == 0);
    Check("native reader receives actual input", write(input[1], "P", 1) == 1);
    close(input[1]); input[1] = -1;
    char byte = 0;
    Check("native writer delivers bytes before actual EOF", read(output[0], &byte, 1) == 1 && byte == 'N' &&
          read(output[0], &byte, 1) == 0);
    Check("native child observed data and true last-writer closure", WaitChild(&child));
  }
  files.clear(); reader.reset(); writer.reset();
  for (int fd : {input[0], input[1], output[0], output[1]}) if (fd >= 0) close(fd);
}
static void AwarePeer(const std::wstring& self, bool diagnostic_stdio) {
  int ends[2];
  if (pipe2(ends, O_CLOEXEC) < 0) { Check("aware pipe", false); return; }
  auto snapshot = gesher::CaptureDescriptor(ends[0]);
  if (!snapshot) { Check("aware descriptor captured", false); close(ends[0]); close(ends[1]); return; }
  std::vector<gesher::exec::ChildFd> files{{37, snapshot->handle, snapshot}, {7100, snapshot->handle, snapshot}};
  // Exercise the original closed-stdio contract as well as a diagnostic run.
  // Passing stdout/stderr must not hide a failure when those slots are closed.
  if (diagnostic_stdio) {
    for (int fd : {1, 2}) {
      auto output = gesher::CaptureDescriptor(fd);
      if (output) files.push_back({fd, output->handle, output});
    }
  }
  std::printf("PIPE_AWARE_SCENARIO diagnostic_stdio=%d\n", diagnostic_stdio);
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(Target(self, diagnostic_stdio ?
      L"--aware-flags-stdio" : L"--aware-flags"), nullptr, L"", files, &child);
  std::printf("PIPE_AWARE_LAUNCH mode=flags error=%lu\n", error);
  Check("aware child imports high alias and shared pipe policy", !error && WaitChild(&child));
  const int shared_flags = fcntl(ends[0], F_GETFL);
  Check("aware mode update reaches parent", shared_flags >= 0 && (shared_flags & O_NONBLOCK) != 0);
  Check("parent restores actual blocking mode", fcntl(ends[0], F_SETFL, 0) == 0);
  error = gesher::exec::SpawnWithFds(Target(self, diagnostic_stdio ?
      L"--aware-read-stdio" : L"--aware-read"), nullptr, L"", files, &child);
  std::printf("PIPE_AWARE_LAUNCH mode=read error=%lu\n", error);
  Check("aware blocked reader starts", error == 0);
  if (!error) {
    bool busy = false;
    const ULONGLONG admission_start = GetTickCount64();
    while (!busy && GetTickCount64() - admission_start < 2000) {
      pollfd descriptor{ends[0], POLLIN, 0};
      busy = poll(&descriptor, 1, 0) == -1 && errno == EOPNOTSUPP;
      if (!busy) Sleep(1);
    }
    Check("cross-process admission observes real active reader", busy);
    Check("shared cached status remains responsive", fcntl(ends[0], F_GETFL) == O_RDONLY);
    Check("cross-process mode transition refuses active endpoint", fcntl(ends[0], F_SETFL, O_NONBLOCK) == -1 && errno == EOPNOTSUPP);
    int bytes = 717;
    Check("cross-process byte query does not queue behind read", ioctl(ends[0], FIONREAD, &bytes) == -1 && errno == EOPNOTSUPP && bytes == 717);
    Check("actual peer finishes aware read", write(ends[1], "S", 1) == 1);
    Check("aware read completes normally", WaitChild(&child));
    Check("retired admission permits actual idle transition", fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
  }
  files.clear(); snapshot.reset(); close(ends[0]); close(ends[1]);
}
static constexpr unsigned kPipeBuf = 4096;

static int AtomicWriter(const char* name, char marker) {
  if (marker != 'A' && marker != 'B') return 2;
  const std::string base(name);
  HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, (base + "-go").c_str());
  HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, (base + "-" + marker).c_str());
  if (!go || !ready || WaitForSingleObject(go, 4000) != WAIT_OBJECT_0) return 3;
  if (fcntl(37, F_GETFL) != (O_WRONLY | O_NONBLOCK)) return 4;
  std::array<char, kPipeBuf> record;
  record.fill(marker);
  SetEvent(ready);
  const int result = write(37, record.data(), record.size());
  const int error = errno;
  CloseHandle(go);
  CloseHandle(ready);
  if (result == static_cast<int>(record.size())) return 0;
  std::printf("PIPE_ATOMIC_WRITER %c count=%d errno=%d\n", marker, result, error);
  return error == EAGAIN ? 5 : 6;
}

static void AtomicWriters(const std::wstring& self) {
  int ends[2];
  if (pipe2(ends, O_CLOEXEC | O_NONBLOCK) < 0) { Check("atomic writers pipe", false); return; }
  const int alias = dup(ends[1]);
  auto snapshot = gesher::CaptureDescriptor(ends[1]);
  wchar_t base[100];
  std::swprintf(base, 100, L"Local\\gesher-pipe-atomic-%lu-%llu",
                GetCurrentProcessId(), GetTickCount64());
  const std::wstring name(base);
  HANDLE go = CreateEventW(nullptr, TRUE, FALSE, (name + L"-go").c_str());
  const HANDLE ready[] = {
      CreateEventW(nullptr, TRUE, FALSE, (name + L"-A").c_str()),
      CreateEventW(nullptr, TRUE, FALSE, (name + L"-B").c_str())};
  PROCESS_INFORMATION children[2] = {};
  const bool setup = alias >= 0 && snapshot && go && ready[0] && ready[1];
  Check("capture nonblocking writer and create separate process rendezvous", setup);
  if (setup) {
    std::vector<gesher::exec::ChildFd> files{{37, snapshot->handle, snapshot}};
    for (int i = 0; i != 2; ++i) {
      const std::wstring mode = L"--aware-atomic-writer " + name + L" " + wchar_t(L'A' + i);
      const DWORD error = gesher::exec::SpawnWithFds(
          Target(self, mode.c_str()), nullptr, L"", files, &children[i]);
      Check("spawn writer sharing the original open file description", error == 0);
    }
    const HANDLE gate = snapshot->description->pipe_mutex;
    const DWORD status = WaitForSingleObject(gate, 2000);
    const bool held = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    Check("fixture can briefly hold the same admission gate", held);
    SetEvent(go);
    if (children[0].hProcess && children[1].hProcess) {
      Check("both writers begin their real operation",
            WaitForMultipleObjects(2, ready, TRUE, 2000) == WAIT_OBJECT_0);
      if (held) {
        Check("bookkeeping contention does not return EAGAIN before native admission",
              WaitForSingleObject(children[0].hProcess, 50) == WAIT_TIMEOUT &&
              WaitForSingleObject(children[1].hProcess, 50) == WAIT_TIMEOUT);
      }
    }
    if (held) ReleaseMutex(gate);
    for (auto& child : children)
      if (child.hProcess) Check("each nonblocking writer sends its complete record", WaitChild(&child));
  }
  snapshot.reset();
  close(ends[1]);
  if (setup) {
    std::array<char, 2 * kPipeBuf> bytes;
    size_t count = 0;
    while (count < bytes.size()) {
      const int n = read(ends[0], bytes.data() + count, bytes.size() - count);
      if (n <= 0) break;
      count += static_cast<size_t>(n);
    }
    const char first = count == bytes.size() ? bytes.front() : 0;
    const char second = first == 'A' ? 'B' : 'A';
    Check("actual stream contains two intact PIPE_BUF records",
          count == bytes.size() && (first == 'A' || first == 'B') &&
          std::all_of(bytes.begin(), bytes.begin() + kPipeBuf, [first](char c) { return c == first; }) &&
          std::all_of(bytes.begin() + kPipeBuf, bytes.end(), [second](char c) { return c == second; }));
    Check("empty pipe still has the surviving duplicate writer", read(ends[0], bytes.data(), 1) == -1 &&
          errno == EAGAIN);
    close(alias);
    Check("last writer duplicate closure produces EOF", read(ends[0], bytes.data(), 1) == 0);
  } else if (alias >= 0) {
    close(alias);
  }
  for (HANDLE event : {go, ready[0], ready[1]})
    if (event) CloseHandle(event);
  close(ends[0]);
}

struct ReadState { int fd; int result = -1; char byte = 0; };
static void* ReadOne(void* raw) {
  auto* state = static_cast<ReadState*>(raw);
  state->result = read(state->fd, &state->byte, 1);
  return nullptr;
}
static void ExternalPipe(bool asynchronous) {
  HANDLE reader = INVALID_HANDLE_VALUE, writer = INVALID_HANDLE_VALUE;
  if (!asynchronous) {
    if (!CreatePipe(&reader, &writer, nullptr, 4096)) { Check("raw synchronous pipe", false); return; }
  } else {
    wchar_t name[100];
    std::swprintf(name, 100, L"\\\\.\\pipe\\gesher-pipe-component-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    reader = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    if (reader == INVALID_HANDLE_VALUE) { Check("raw asynchronous pipe", false); return; }
    writer = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) { CloseHandle(reader); Check("raw pipe writer", false); return; }
    OVERLAPPED connection{};
    connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connection.hEvent) { CloseHandle(reader); CloseHandle(writer); Check("raw pipe connection event", false); return; }
    bool connected = ConnectNamedPipe(reader, &connection) || GetLastError() == ERROR_PIPE_CONNECTED;
    CloseHandle(connection.hEvent);
    if (!connected) { CloseHandle(reader); CloseHandle(writer); Check("raw pipe connection", false); return; }
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(reader), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  if (fd < 0) { CloseHandle(reader); CloseHandle(writer); Check("raw descriptor", false); return; }
  Check("external status fails without a kernel mode query", fcntl(fd, F_GETFL) == -1 && errno == EOPNOTSUPP);
  pollfd descriptor{fd, POLLIN, 0};
  Check("external readiness has no fabricated bits", poll(&descriptor, 1, 0) == -1 && errno == EOPNOTSUPP && descriptor.revents == 0);
  ReadState state{fd}; pthread_t caller;
  int started = pthread_create(&caller, nullptr, ReadOne, &state);
  Check("external read starts", started == 0);
  if (started) { CloseHandle(writer); close(fd); return; }
  Sleep(10);
  DWORD count = 0;
  Check("real external writer supplies data", WriteFile(writer, "E", 1, &count, nullptr) && count == 1);
  Check("external sync or async request completes accurately", pthread_join(caller, nullptr) == 0 && state.result == 1 && state.byte == 'E');
  started = pthread_create(&caller, nullptr, ReadOne, &state);
  Check("external cancellation read starts", started == 0);
  if (started) { CloseHandle(writer); close(fd); return; }
  Sleep(10);
  Check("external request cancellation accepted", pthread_cancel(caller) == 0);
  void* result = nullptr;
  Check("external request and cancellation callback retire", pthread_join(caller, &result) == 0 && result == PTHREAD_CANCELED);
  CloseHandle(writer);
  char byte;
  Check("external actual last writer closure is EOF", read(fd, &byte, 1) == 0);
  close(fd);
}
int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "--aware-atomic-writer") == 0)
    return AtomicWriter(argv[2], argv[3][0]);
  if (argc == 2 && (std::strcmp(argv[1], "--aware-flags") == 0 ||
                    std::strcmp(argv[1], "--aware-flags-stdio") == 0)) {
    if (!StandardDescriptors(std::strcmp(argv[1], "--aware-flags-stdio") == 0)) return 22;
    const int changed = fcntl(37, F_SETFL, O_NONBLOCK);
    const int changed_error = errno;
    const int flags = fcntl(7100, F_GETFL);
    std::printf("PIPE_AWARE_FLAGS changed=%d error=%d alias_flags=%d alias_errno=%d\n",
                changed, changed_error, flags, errno);
    return changed == 0 && flags >= 0 && (flags & O_NONBLOCK) != 0 ? 0 : 20;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--aware-read") == 0 ||
                    std::strcmp(argv[1], "--aware-read-stdio") == 0)) {
    if (!StandardDescriptors(std::strcmp(argv[1], "--aware-read-stdio") == 0)) return 23;
    char byte = 0;
    const int result = read(37, &byte, 1);
    std::printf("PIPE_AWARE_READ result=%d byte=%d errno=%d\n", result, byte, errno);
    return result == 1 && byte == 'S' ? 0 : 21;
  }
  wchar_t path[32768];
  DWORD length = GetModuleFileNameW(nullptr, path, 32768);
  if (!length || length >= 32768) return 1;
  std::wstring self(path, length);
  std::wstring native = self.substr(0, self.find_last_of(L"/\\") + 1) + L"ofd_native_child.exe";
  NativePeer(native);
  AwarePeer(self, false);
  AwarePeer(self, true);
  AtomicWriters(self);
  ExternalPipe(false);
  ExternalPipe(true);
  std::printf("pipe native process contracts: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

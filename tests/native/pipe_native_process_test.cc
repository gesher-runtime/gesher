// Actual Windows pipe/OFD transport. The component's job runner owns every
// child and supplies a hard deadline; no foreign process or pipe is touched.
#include "exec_spawn.h"
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
static void AwarePeer(const std::wstring& self) {
  int ends[2];
  if (pipe2(ends, O_CLOEXEC) < 0) { Check("aware pipe", false); return; }
  auto snapshot = gesher::CaptureDescriptor(ends[0]);
  if (!snapshot) { Check("aware descriptor captured", false); close(ends[0]); close(ends[1]); return; }
  std::vector<gesher::exec::ChildFd> files{{37, snapshot->handle, snapshot}, {7100, snapshot->handle, snapshot}};
  // Preserve real diagnostic handles so early importer failures in the child
  // cannot disappear into intentionally closed standard descriptors.
  for (int fd : {1, 2}) {
    auto output = gesher::CaptureDescriptor(fd);
    if (output) files.push_back({fd, output->handle, output});
  }
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(Target(self, L"--aware-flags"), nullptr, L"", files, &child);
  std::printf("PIPE_AWARE_LAUNCH mode=flags error=%lu\n", error);
  Check("aware child imports high alias and shared pipe policy", !error && WaitChild(&child));
  const int shared_flags = fcntl(ends[0], F_GETFL);
  Check("aware mode update reaches parent", shared_flags >= 0 && (shared_flags & O_NONBLOCK) != 0);
  Check("parent restores actual blocking mode", fcntl(ends[0], F_SETFL, 0) == 0);
  error = gesher::exec::SpawnWithFds(Target(self, L"--aware-read"), nullptr, L"", files, &child);
  std::printf("PIPE_AWARE_LAUNCH mode=read error=%lu\n", error);
  Check("aware blocked reader starts", error == 0);
  if (!error) {
    bool busy = false;
    for (int attempt = 0; attempt != 2000 && !busy; ++attempt) {
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
  if (argc == 2 && std::strcmp(argv[1], "--aware-flags") == 0) {
    const int changed = fcntl(37, F_SETFL, O_NONBLOCK);
    const int changed_error = errno;
    const int flags = fcntl(7100, F_GETFL);
    std::printf("PIPE_AWARE_FLAGS changed=%d error=%d alias_flags=%d alias_errno=%d\n",
                changed, changed_error, flags, errno);
    return changed == 0 && flags >= 0 && (flags & O_NONBLOCK) != 0 ? 0 : 20;
  }
  if (argc == 2 && std::strcmp(argv[1], "--aware-read") == 0) {
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
  AwarePeer(self);
  ExternalPipe(false);
  ExternalPipe(true);
  std::printf("pipe native process contracts: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

// Real Windows process creation: importer capability, exact descriptor table,
// cross-process flags, high descriptors, devices, and native PE interoperation.
#include "exec_spawn.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static int checks, failures;
static void Check(const char* what, bool result) {
  ++checks;
  if (!result) { ++failures; std::printf("FAIL OFD spawn: %s (errno=%d, win32=%lu)\n", what, errno, GetLastError()); }
}

static bool WaitChild(PROCESS_INFORMATION* child) {
  if (child->hThread) CloseHandle(child->hThread);
  child->hThread = nullptr;
  bool completed = WaitForSingleObject(child->hProcess, 10000) == WAIT_OBJECT_0;
  DWORD code = 127;
  if (completed) GetExitCodeProcess(child->hProcess, &code);
  else TerminateProcess(child->hProcess, 127);
  CloseHandle(child->hProcess);
  child->hProcess = nullptr;
  return completed && code == 0;
}

static bool RefusesNative(const gesher::exec::Launch& launch,
                          const std::vector<gesher::exec::ChildFd>& files) {
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(launch, nullptr, L"", files, &child);
  if (!error) WaitChild(&child);
  return error == ERROR_NOT_SUPPORTED;
}

static int AwareChild() {
  for (int fd = 0; fd < 3; ++fd) if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 20;
  int flags = fcntl(37, F_GETFL);
  if ((flags & O_APPEND) == 0 || flags != fcntl(7100, F_GETFL)) return 21;
  unsigned char bytes[4] = {1, 1, 1, 1};
  if (read(42, bytes, sizeof(bytes)) != 4 || bytes[0] || bytes[1] || bytes[2] || bytes[3]) return 22;
  if (write(37, "X", 1) != 1 || fcntl(7100, F_SETFL, flags & ~O_APPEND) < 0) return 23;
  return 0;
}

static gesher::exec::Launch Target(const std::wstring& path, const wchar_t* mode) {
  gesher::exec::Launch launch;
  launch.application = path;
  gesher::exec::AppendQuoted(&launch.command_line, path);
  launch.command_line += L' ';
  launch.command_line += mode;
  launch.kind = "exe";
  return launch;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--aware") == 0) return AwareChild();
  if (argc == 2 && std::strcmp(argv[1], "--pipe-aware") == 0) {
    if (fcntl(37, F_SETFL, O_NONBLOCK) < 0 || (fcntl(7100, F_GETFL) & O_NONBLOCK) == 0) return 24;
    char byte;
    return read(37, &byte, 1) == -1 && errno == EAGAIN ? 0 : 25;
  }
  wchar_t image[32768];
  DWORD length = GetModuleFileNameW(nullptr, image, sizeof(image) / sizeof(image[0]));
  if (!length || length >= sizeof(image) / sizeof(image[0])) return 1;
  std::wstring self(image, length);
  std::wstring native = self.substr(0, self.find_last_of(L"/\\") + 1) + L"ofd_native_child.exe";
  char path[] = "gesher-ofd-spawn-XXXXXX";
  int fd = mkstemp(path);
  Check("temporary file", fd >= 0);
  if (fd < 0) return 1;
  Check("seed and initial cursor", write(fd, "ABC", 3) == 3 && lseek(fd, 1, SEEK_SET) == 1);
  auto source = gesher::CaptureDescriptor(fd);
  Check("pin file description", static_cast<bool>(source));
  if (!source) { close(fd); unlink(path); return 1; }
  PROCESS_INFORMATION child{};
  auto native_launch = Target(native, L"--file");
  std::vector<gesher::exec::ChildFd> files{{37, source->handle, source}};
  DWORD error = gesher::exec::SpawnWithFds(native_launch, nullptr, L"", files, &child);
  Check("start ordinary native PE with a file and closed standard descriptors", error == 0);
  Check("native child consumes the exact shared kernel cursor", error == 0 && WaitChild(&child));
  char result[5]{};
  Check("native I/O retained the parent's file description", lseek(fd, 0, SEEK_CUR) == 3 &&
      pread(fd, result, 3, 0) == 3 && std::memcmp(result, "ABZ", 3) == 0);
  Check("enable append", fcntl(fd, F_SETFL, O_APPEND) == 0);
  Check("foreign PE rejects emulated append", RefusesNative(native_launch, files));
  int zero = open("/dev/zero", O_RDONLY | O_CLOEXEC);
  Check("open logical zero device", zero >= 0);
  auto zeros = gesher::CaptureDescriptor(zero);
  Check("pin logical zero device", static_cast<bool>(zeros));
  if (zeros) {
    files = {{42, zeros->handle, zeros}};
    Check("foreign PE cannot receive NUL in place of zero", RefusesNative(native_launch, files));
    files = {{37, source->handle, source}, {7100, source->handle, source}, {42, zeros->handle, zeros}};
    auto aware = Target(self, L"--aware");
    error = gesher::exec::SpawnWithFds(aware, nullptr, L"", files, &child);
    Check("discover and launch the real importer export", error == 0);
    Check("aware child imports aliases, high slots, device kind and closed stdio", error == 0 && WaitChild(&child));
    Check("child status mutation propagates to its parent", (fcntl(fd, F_GETFL) & O_APPEND) == 0);
    Check("child append used the same kernel description", lseek(fd, 0, SEEK_CUR) == 4 &&
        pread(fd, result, 4, 0) == 4 && std::memcmp(result, "ABZX", 4) == 0);
  }
  files = {{7100, source->handle, source}};
  Check("foreign PE rejects unsupported high slots", RefusesNative(native_launch, files));
  native_launch = Target(native, L"--empty");
  files.clear();
  error = gesher::exec::SpawnWithFds(native_launch, nullptr, L"", files, &child);
  Check("start native PE without inherited handles", error == 0);
  Check("native child sees closed standard handles", error == 0 && WaitChild(&child));
  int pipes[2] = {-1, -1};
  if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) == 0) {
    auto pipe = gesher::CaptureDescriptor(pipes[0]);
    Check("pin nonblocking pipe", static_cast<bool>(pipe));
    if (pipe) {
      files = {{0, pipe->handle, pipe}};
      Check("foreign PE cannot silently ignore nonblocking pipe status", RefusesNative(native_launch, files));
    }
    files.clear(); pipe.reset(); close(pipes[0]); close(pipes[1]);
  } else Check("nonblocking pipe fixture", false);
  int incoming[2] = {-1, -1}, outgoing[2] = {-1, -1};
  Check("native peer pipes", pipe2(incoming, O_CLOEXEC) == 0 && pipe2(outgoing, O_CLOEXEC) == 0);
  auto incoming_reader = gesher::CaptureDescriptor(incoming[0]);
  auto outgoing_writer = gesher::CaptureDescriptor(outgoing[1]);
  if (incoming_reader && outgoing_writer) {
    files = {{37, incoming_reader->handle, incoming_reader}, {38, outgoing_writer->handle, outgoing_writer}};
    error = gesher::exec::SpawnWithFds(Target(native, L"--pipe"), nullptr, L"", files, &child);
    Check("ordinary UCRT peer starts with real directional pipes", error == 0);
    Check("parent writes actual pipe input", write(incoming[1], "P", 1) == 1);
    close(incoming[1]); incoming[1] = -1;
    files.clear(); incoming_reader.reset(); outgoing_writer.reset();
    close(incoming[0]); incoming[0] = -1;
    close(outgoing[1]); outgoing[1] = -1;
    Check("native UCRT read sees data and real last-writer EOF", error == 0 && WaitChild(&child));
    char byte;
    Check("native UCRT writer reaches parent and closes", read(outgoing[0], &byte, 1) == 1 && byte == 'N' && read(outgoing[0], &byte, 1) == 0);
  } else Check("capture native peer endpoints", false);
  files.clear(); incoming_reader.reset(); outgoing_writer.reset();
  for (int descriptor : {incoming[0], incoming[1], outgoing[0], outgoing[1]}) if (descriptor >= 0) close(descriptor);
  pipes[0] = pipes[1] = -1;
  Check("aware child pipe fixture", pipe2(pipes, O_CLOEXEC) == 0);
  auto shared_pipe = gesher::CaptureDescriptor(pipes[0]);
  if (shared_pipe) {
    files = {{37, shared_pipe->handle, shared_pipe}, {7100, shared_pipe->handle, shared_pipe}};
    error = gesher::exec::SpawnWithFds(Target(self, L"--pipe-aware"), nullptr, L"", files, &child);
    Check("aware child inherits the actual pipe description", error == 0 && WaitChild(&child));
    Check("child pipe mode change reaches parent's native endpoint", (fcntl(pipes[0], F_GETFL) & O_NONBLOCK) != 0);
  } else Check("capture aware child pipe", false);
  files.clear(); shared_pipe.reset(); close(pipes[0]); close(pipes[1]);
  gesher::exec::Launch shell;
  Check("prepare the shell", gesher::exec::PrepareLaunch({"sh", "-c", "printf Y"}, "", &shell) == 0);
  Check("set append for raw shell output", fcntl(fd, F_SETFL, O_APPEND) == 0 && lseek(fd, 0, SEEK_SET) == 0);
  files = {{1, source->handle, source}};
  error = gesher::exec::SpawnWithFds(shell, nullptr, L"", files, &child);
  Check("start shell with an append description", error == 0);
  Check("shell raw HANDLE output honors inherited append status", error == 0 && WaitChild(&child));
  Check("shell append did not overwrite the earlier file", lseek(fd, 0, SEEK_CUR) == 5 &&
      pread(fd, result, 5, 0) == 5 && std::memcmp(result, "ABZXY", 5) == 0);
  files.clear();
  zeros.reset(); source.reset(); close(zero); close(fd); unlink(path);
  std::printf("OFD Windows spawn: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

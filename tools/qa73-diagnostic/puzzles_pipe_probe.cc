// Diagnostic-only transport observation. The JNI/ART boundary is not claimed:
// this uses the same real pipe/OFD admission and SpawnWithFds as ForkAndExec,
// and the unchanged historical gesher-exec plus the manifest-pinned app ELF.
#include "exec_spawn.h"
#include <windows.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include <unistd.h>

namespace ex = gesher::exec;
namespace {
void Close(HANDLE& h) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = nullptr; }

bool RawOutput(const wchar_t* suffix, const std::string& bytes) {
  wchar_t prefix[32768];
  const DWORD length = GetEnvironmentVariableW(L"GESHER_QA73_PIPE_REPORT_PREFIX", prefix, 32768);
  if (!length || length >= 32768) return false;
  const std::wstring path = std::wstring(prefix, length) + suffix;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size();
  CloseHandle(file); return ok;
}

bool Drain(HANDLE h, std::string* output, bool* eof) {
  if (*eof) return true;
  DWORD available = 0;
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) { *eof = true; return true; }
    std::printf("QA73 PIPE peek_error=%lu\n", GetLastError()); return false;
  }
  while (available) {
    char bytes[4096]; DWORD read = 0, count = available < sizeof(bytes) ? available : sizeof(bytes);
    if (!ReadFile(h, bytes, count, &read, nullptr) || !read) {
      std::printf("QA73 PIPE read_error=%lu\n", GetLastError()); return false;
    }
    if (output->size() + read > 256 * 1024) {
      std::printf("QA73 PIPE bounded_output_exceeded\n"); return false;
    }
    output->append(bytes, read); available -= read;
  }
  return true;
}

int Run(const char* mode, const char* launcher, const char* program) {
  if (_putenv_s("GESHER_EXEC_LAUNCHER", launcher)) return 125;
  std::vector<std::string> args;
  if (std::strcmp(mode, "tool") == 0) args = {"sh", "-c", "printf qa73-pipe-control"};
  else args = {program, "keen", "6dn"};
  ex::Launch launch;
  int prepared = ex::PrepareLaunch(args, "", &launch);
  std::printf("QA73 PUZZLES mode=%s prepared=%d kind=%s command=%s\n", mode, prepared,
              launch.kind.c_str(), ex::Narrow(launch.command_line).c_str());
  if (prepared) return 125;
  HANDLE child[3]{}, parent[3]{};
  int parent_fds[3] = {-1, -1, -1};
  gesher::DescriptorSnapshotPtr snapshots[3];
  std::vector<ex::ChildFd> files;
  auto cleanup = [&] {
    files.clear();
    for (int i = 0; i < 3; ++i) {
      snapshots[i].reset(); Close(child[i]); Close(parent[i]);
      if (parent_fds[i] >= 0) { close(parent_fds[i]); parent_fds[i] = -1; }
    }
  };
  for (int i = 0; i < 3; ++i) {
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, nullptr, 256 * 1024)) { cleanup(); return 125; }
    child[i] = i == 0 ? reader : writer; parent[i] = i == 0 ? writer : reader;
    snapshots[i] = gesher::CaptureFileHandle(child[i]);
    auto parent_description = gesher::CaptureFileHandle(parent[i]);
    if (!snapshots[i] || !parent_description ||
        gesher::InitializePipeDescription(snapshots[i], i == 0 ? O_RDONLY : O_WRONLY) < 0 ||
        gesher::InitializePipeDescription(parent_description, i == 0 ? O_WRONLY : O_RDONLY) < 0 ||
        (parent_fds[i] = gesher::AttachFileDescription(parent_description, true)) < 0) {
      std::printf("QA73 PIPE admission_failed slot=%d errno=%d win32=%lu\n", i, errno, GetLastError());
      cleanup(); return 125;
    }
    files.push_back({i, snapshots[i]->handle, snapshots[i]});
  }
  PROCESS_INFORMATION pi{};
  DWORD error = ex::SpawnWithFds(launch, nullptr, L"", files, &pi);
  files.clear();
  for (int i = 0; i < 3; ++i) { snapshots[i].reset(); Close(child[i]); }
  if (error) { std::printf("QA73 PIPE spawn_error=%lu\n", error); cleanup(); return 125; }
  CloseHandle(pi.hThread);
  // Actual last writer closes, providing EOF; no bytes are fabricated for the
  // app generator. Keep the owned read handles until both real pipes reach EOF.
  close(parent_fds[0]); parent_fds[0] = -1; Close(parent[0]);
  std::string output, errors; bool out_eof = false, err_eof = false, healthy = true;
  const ULONGLONG start = GetTickCount64(); ULONGLONG exited_at = 0;
  DWORD code = STILL_ACTIVE, wait = WAIT_TIMEOUT;
  for (;;) {
    healthy = Drain(parent[1], &output, &out_eof) && Drain(parent[2], &errors, &err_eof);
    if (!healthy) break;
    wait = WaitForSingleObject(pi.hProcess, 0);
    if (wait == WAIT_OBJECT_0) {
      if (!GetExitCodeProcess(pi.hProcess, &code)) { healthy = false; break; }
      if (!exited_at) exited_at = GetTickCount64();
      if (out_eof && err_eof) break;
      if (GetTickCount64() - exited_at >= 5000) { healthy = false; break; }
    } else if (wait != WAIT_TIMEOUT) { healthy = false; break; }
    if (GetTickCount64() - start >= 120000) break;
    Sleep(10);
  }
  bool timed_out = wait != WAIT_OBJECT_0;
  if (timed_out) { TerminateProcess(pi.hProcess, 124); WaitForSingleObject(pi.hProcess, 5000); }
  CloseHandle(pi.hProcess); cleanup();
  std::printf("QA73 PUZZLES_RESULT mode=%s pid=%lu wait=%lu code=%lu healthy=%d out_eof=%d err_eof=%d elapsed_ms=%llu stdout_bytes=%zu stderr_bytes=%zu\n",
      mode, pi.dwProcessId, wait, code, healthy, out_eof, err_eof,
      static_cast<unsigned long long>(GetTickCount64() - start), output.size(), errors.size());
  std::printf("QA73 CHILD_STDOUT_BEGIN\n%.*s\nQA73 CHILD_STDOUT_END\n",
              static_cast<int>(output.size()), output.data());
  std::printf("QA73 CHILD_STDERR_BEGIN\n%.*s\nQA73 CHILD_STDERR_END\n",
              static_cast<int>(errors.size()), errors.data());
  if (!RawOutput(L".stdout.bin", output) || !RawOutput(L".stderr.bin", errors)) return 125;
  if (!healthy) return 125;
  if (timed_out) return 124;
  return static_cast<int>(code);
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 4 || (std::strcmp(argv[1], "tool") && std::strcmp(argv[1], "puzzles"))) return 2;
  return Run(argv[1], argv[2], argv[3]);
}

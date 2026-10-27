// Actual UCRT no-console sentinel, shim spawn and a real NDK child.
#include <gesher/bionic_shim.h>

#include <windows.h>
#include <shellapi.h>
#include <io.h>

#include <cstdio>
#include <string>

#include "exec_spawn.h"

int main() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr || argc != 3) {
    LocalFree(argv);
    return 2;
  }
  bool child = std::wstring(argv[1]) == L"--child";
  std::wstring program = argv[2];
  LocalFree(argv);
  if (child) {
    intptr_t before = _get_osfhandle(0);
    if (before != -2) {
      printf("FAIL  spawn no-console: raw stdin %lld, expected -2\n", static_cast<long long>(before));
      return 1;
    }
    using Spawn = int (__attribute__((sysv_abi))*)(int*, const char*, const void*, const void*,
                                                   char* const*, char* const*);
    using Wait = int (__attribute__((sysv_abi))*)(int, int*, int);
    auto spawn = reinterpret_cast<Spawn>(gesher::bionic::LookupBionicSymbol("posix_spawn"));
    auto wait = reinterpret_cast<Wait>(gesher::bionic::LookupBionicSymbol("waitpid"));
    if (spawn == nullptr || wait == nullptr) return 1;
    std::string path = gesher::exec::Narrow(program);
    char name[] = "spawn-no-console", mode[] = "--closed", fd[] = "0";
    char* args[] = {name, mode, fd, nullptr};
    int pid = -1, status = -1;
    int result = spawn(&pid, path.c_str(), nullptr, nullptr, args, nullptr);
    int reaped = result == 0 && pid > 0 ? wait(pid, &status, 0) : -1;
    bool okay = result == 0 && pid > 0 && reaped == pid && status == (42 << 8) &&
                _get_osfhandle(0) == before;
    printf("%s  spawn no-console: raw stdin -2, spawn%d, actual child%d, wait%d, status0x%x, parent unchanged\n",
           okay ? "PASS" : "FAIL", result, pid, reaped, status);
    return okay ? 0 : 1;
  }
  wchar_t self[32768];
  DWORD length = GetModuleFileNameW(nullptr, self, 32768);
  if (length == 0 || length >= 32768) return 1;
  std::wstring command;
  gesher::exec::AppendQuoted(&command, self);
  command += L" --child ";
  gesher::exec::AppendQuoted(&command, program);
  HANDLE output = nullptr, error = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(),
                        &output, 0, TRUE, DUPLICATE_SAME_ACCESS)) return 1;
  if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(),
                        &error, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    CloseHandle(output);
    return 1;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nullptr;
  startup.hStdOutput = output;
  startup.hStdError = error;
  // No lpReserved2/CRT fd block: stdin must actually be the UCRT -2 sentinel.
  PROCESS_INFORMATION process{};
  BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                nullptr, nullptr, &startup, &process);
  CloseHandle(output);
  CloseHandle(error);
  if (!started) return 1;
  CloseHandle(process.hThread);
  DWORD code = 1;
  if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &code);
  }
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

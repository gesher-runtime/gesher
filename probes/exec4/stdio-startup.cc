// Diagnose actual UCRT startup state without Gesher libraries or fd writes.
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <cstdio>
#include <string>

int main() {
  intptr_t first = _get_osfhandle(0);
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return 1;
  bool child = argc == 2;
  LocalFree(argv);
  if (child) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    DWORD mode = 0;
    BOOL console = GetConsoleMode(input, &mode);
    printf("INFO stdio startup: raw%lld win%p type%lu console%d flags%lu reserved%u startup%p\n",
           static_cast<long long>(first), input, GetFileType(input), static_cast<int>(console),
           startup.dwFlags, static_cast<unsigned>(startup.cbReserved2), startup.hStdInput);
    return 0;
  }
  wchar_t self[32768];
  DWORD n = GetModuleFileNameW(nullptr, self, 32768);
  if (!n || n >= 32768) return 1;
  for (DWORD creation : {DWORD{CREATE_NO_WINDOW}, DWORD{DETACHED_PROCESS}}) {
    for (bool invalid : {false, true}) {
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
      startup.hStdInput = invalid ? INVALID_HANDLE_VALUE : nullptr;
      startup.hStdOutput = output;
      startup.hStdError = error;
      std::wstring command = L"\"" + std::wstring(self, n) + L"\" --child";
      PROCESS_INFORMATION process{};
      printf("INFO stdio parent: creation%lu invalid%d\n", creation, static_cast<int>(invalid));
      fflush(stdout);
      BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, creation,
                                    nullptr, nullptr, &startup, &process);
      CloseHandle(output);
      CloseHandle(error);
      if (!started) return 1;
      CloseHandle(process.hThread);
      DWORD result = WaitForSingleObject(process.hProcess, 10000), code = 1;
      if (result == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
      else TerminateProcess(process.hProcess, 124);
      CloseHandle(process.hProcess);
      if (result != WAIT_OBJECT_0 || code != 0) return 1;
    }
  }
  return 0;
}

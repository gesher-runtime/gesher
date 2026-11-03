// gesher-launch.exe: what Explorer starts for Gesher (docs/installer.md): an
// app's Start menu shortcut (`gesher-launch launch <package>`), a double-clicked
// .apk (`gesher-launch <file.apk>`) and a click on a toast (`gesher-launch
// toast-activated`). It runs gesher-app.ps1 with the same arguments.
//
// It is a Windows-subsystem program, so that nothing flashes: powershell.exe is
// a console program, and Explorer starting it opens a console window before its
// -WindowStyle Hidden can hide it. Here powershell runs with CREATE_NO_WINDOW,
// its console never shown, and the runtime it starts shares that console.
//
// It also hands on the foreground: Explorer lets the program it starts for a
// click take the foreground, but the app's window belongs to a process the
// Gesher device's system server starts, which Windows keeps in the background
// (its window opens behind Explorer). AllowSetForegroundWindow(ASFW_ANY) lets
// that window come to the front, until the user's next input.
//
// Its exit code is gesher-app.ps1's. Standard handles it was given (a redirected
// output, as CI reads it) are passed on.
#include <windows.h>

#include <string>

namespace {

// The arguments after the program name, as written (quotes kept).
const wchar_t* Arguments(const wchar_t* line) {
  bool quoted = false;
  for (; *line != L'\0'; ++line) {
    if (*line == L'"') quoted = !quoted;
    if (!quoted && (*line == L' ' || *line == L'\t')) break;
  }
  while (*line == L' ' || *line == L'\t') ++line;
  return line;
}

std::wstring Directory(const wchar_t* path) {
  std::wstring dir(path);
  size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
  if (n == 0 || n >= sizeof(self) / sizeof(self[0])) return 1;
  const std::wstring here = Directory(self);

  wchar_t system[MAX_PATH];
  UINT s = GetSystemDirectoryW(system, MAX_PATH);
  std::wstring powershell = (s > 0 && s < MAX_PATH ? std::wstring(system) : L"C:\\Windows\\System32") +
                            L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::wstring command = L"\"" + powershell +
                         L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                         here + L"\\gesher-app.ps1\" " + Arguments(GetCommandLineW());

  AllowSetForegroundWindow(ASFW_ANY);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE),
         err = GetStdHandle(STD_ERROR_HANDLE);
  bool inherit = (out != nullptr && out != INVALID_HANDLE_VALUE) ||
                 (err != nullptr && err != INVALID_HANDLE_VALUE);
  if (inherit) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
  }
  PROCESS_INFORMATION pi = {};
  std::wstring mutable_command = command;
  if (!CreateProcessW(powershell.c_str(), mutable_command.data(), nullptr, nullptr,
                      inherit ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return 1;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

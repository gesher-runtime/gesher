// gesher: starting programs the way Linux's execvp would (exec_spawn.h).

#include "exec_spawn.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace gesher::exec {

namespace {

constexpr int kEIO = 5;
constexpr int kENAMETOOLONG = 36;
// CreateProcessW's limit on a command line, in characters with the NUL.
constexpr size_t kMaxCommandLine = 32767;

bool IsSlash(char c) { return c == '/' || c == '\\'; }

bool IsAbsolute(const std::string& p) {
  return (!p.empty() && IsSlash(p[0])) ||
         (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')));
}

bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

// The extension of a Windows path's last component, with the dot, or "".
std::wstring Extension(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
  return path.substr(dot);
}

// What a file is, from its attributes and first bytes. 0 or an errno.
int Probe(const std::wstring& path, std::string* head) {
  std::wstring p = path;
  if (p.size() >= MAX_PATH && p.rfind(L"\\\\", 0) != 0) p = L"\\\\?\\" + p;
  DWORD attributes = GetFileAttributesW(p.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return ErrnoFromWin32(GetLastError());
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return kEACCES;
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return ErrnoFromWin32(GetLastError());
  char buf[512];
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
  CloseHandle(h);
  if (!ok) return kEACCES;
  head->assign(buf, got);
  return 0;
}

bool IsShName(const std::string& name) {
  return name == "sh" || name == "/system/bin/sh" || name == "/bin/sh" || name == "/system/xbin/sh" ||
         name == "/vendor/bin/sh";
}

// The directories of PATH: ';'-separated on Windows; a Linux-style ':' list
// is accepted too (a drive letter's colon is not a separator there).
std::vector<std::wstring> PathDirectories() {
  std::vector<std::wstring> dirs;
  const wchar_t* env = _wgetenv(L"PATH");
  std::wstring path = env != nullptr ? env : L"";
  wchar_t sep = path.find(L';') != std::wstring::npos ? L';' : L':';
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(sep, start);
    if (end == std::wstring::npos) end = path.size();
    // "C" followed by ":\..." in a ':' list is a drive, not an entry.
    if (sep == L':' && end - start == 1 && end + 1 < path.size() && (path[end + 1] == L'\\' || path[end + 1] == L'/')) {
      end = path.find(sep, end + 1);
      if (end == std::wstring::npos) end = path.size();
    }
    std::wstring dir = path.substr(start, end - start);
    // An empty entry is the current directory, as on Linux.
    dirs.push_back(dir.empty() ? L"." : dir);
    start = end + 1;
  }
  return dirs;
}

std::wstring ComSpec() {
  const wchar_t* c = _wgetenv(L"ComSpec");
  if (c != nullptr && *c != 0) return c;
  wchar_t sys[MAX_PATH];
  UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  return std::wstring(sys, n) + L"\\cmd.exe";
}

void AppendArgs(std::wstring* cmd, const std::vector<std::string>& argv, size_t from) {
  for (size_t i = from; i < argv.size(); ++i) {
    *cmd += L' ';
    AppendQuoted(cmd, Widen(argv[i]));
  }
}

int Finish(Launch* launch) {
  return launch->command_line.size() + 1 > kMaxCommandLine ? kE2BIG : 0;
}

int PrepareAt(const std::vector<std::string>& argv, const std::string& dir, int depth, Launch* launch);

// The launch for the file found at `path`, run as `argv`.
int LaunchFile(const std::wstring& path, const std::string& head, const std::vector<std::string>& argv,
               const std::string& dir, int depth, Launch* launch) {
  const auto* h = reinterpret_cast<const unsigned char*>(head.data());
  std::wstring ext = Extension(path);
  if (head.size() >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
    // ELF64, little-endian, EM_X86_64: anything else is not runnable here.
    if (h[4] != 2 || h[5] != 1 || h[18] != 62 || h[19] != 0) return kENOEXEC;
    launch->kind = "elf";
    launch->application = LauncherPath();
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, launch->application);
    launch->command_line += L" --elf ";
    AppendQuoted(&launch->command_line, path);
    AppendArgs(&launch->command_line, argv, 0);
    return Finish(launch);
  }
  if (head.size() >= 2 && h[0] == 'M' && h[1] == 'Z') {
    launch->kind = "exe";
    launch->application = path;
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, Widen(argv[0]));
    AppendArgs(&launch->command_line, argv, 1);
    return Finish(launch);
  }
  if (EqualsIgnoreCase(ext, L".cmd") || EqualsIgnoreCase(ext, L".bat")) {
    // cmd /s /c "<line>": cmd strips the outer quotes and runs the rest.
    launch->kind = "batch";
    launch->application = ComSpec();
    launch->command_line = L"cmd.exe /d /s /c \"";
    AppendQuoted(&launch->command_line, path);
    AppendArgs(&launch->command_line, argv, 1);
    launch->command_line += L'"';
    return Finish(launch);
  }
  if (head.size() >= 2 && head[0] == '#' && head[1] == '!') {
    // "#!interpreter [one argument]"
    std::string line = head.substr(2, head.find('\n') == std::string::npos ? std::string::npos : head.find('\n') - 2);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) return kENOEXEC;
    line = line.substr(b);
    size_t sp = line.find_first_of(" \t");
    std::string interpreter = line.substr(0, sp);
    std::string option;
    if (sp != std::string::npos) {
      size_t ob = line.find_first_not_of(" \t", sp);
      if (ob != std::string::npos) option = line.substr(ob);
    }
    std::string base = interpreter.substr(interpreter.find_last_of('/') == std::string::npos ? 0 : interpreter.find_last_of('/') + 1);
    if (base == "sh" || base == "mksh" || base == "ash" || base == "bash") {
      launch->kind = "sh";
      launch->application = LauncherPath();
      launch->command_line.clear();
      AppendQuoted(&launch->command_line, launch->application);
      launch->command_line += L" --sh ";
      AppendQuoted(&launch->command_line, path);
      AppendArgs(&launch->command_line, argv, 1);
      return Finish(launch);
    }
    if (depth > 0) return kENOEXEC;
    std::vector<std::string> args{interpreter};
    if (!option.empty()) args.push_back(option);
    args.push_back(argv[0]);
    args.insert(args.end(), argv.begin() + 1, argv.end());
    Launch inner;
    int r = PrepareAt(args, dir, depth + 1, &inner);
    if (r != 0) return r == kENOENT ? kENOENT : kENOEXEC;
    inner.kind = "script";
    *launch = inner;
    return 0;
  }
  // A text file: execvp (and the JDK) run it with /bin/sh.
  if (head.find('\0') == std::string::npos) {
    launch->kind = "sh";
    launch->application = LauncherPath();
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, launch->application);
    launch->command_line += L" --sh ";
    AppendQuoted(&launch->command_line, path);
    AppendArgs(&launch->command_line, argv, 1);
    return Finish(launch);
  }
  return kENOEXEC;
}

}  // namespace

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& utf16) {
  if (utf16.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

const char* ErrorText(int error) {
  switch (error) {
    case kENOENT: return "No such file or directory";
    case kEIO: return "I/O error";
    case kE2BIG: return "Argument list too long";
    case kENOEXEC: return "Exec format error";
    case kENOMEM: return "Out of memory";
    case kEACCES: return "Permission denied";
    case kENOTDIR: return "Not a directory";
    case kENAMETOOLONG: return "File name too long";
    default: return "Unknown error";
  }
}

int ErrnoFromWin32(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_DIRECTORY:
      return kENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return kEACCES;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
    case ERROR_EXE_MARKED_INVALID:
      return kENOEXEC;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
      return kENOMEM;
    case ERROR_FILENAME_EXCED_RANGE:
      return kENAMETOOLONG;
    default:
      return kEIO;
  }
}

void AppendQuoted(std::wstring* cmd, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    *cmd += arg;
    return;
  }
  *cmd += L'"';
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      cmd->append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      cmd->append(backslashes * 2 + 1, L'\\');
    } else {
      cmd->append(backslashes, L'\\');
    }
    cmd->push_back(*it);
  }
  *cmd += L'"';
}

std::wstring WindowsPath(const std::string& path, const std::string& dir) {
  std::string full = path;
  if (!IsAbsolute(path) && !dir.empty()) full = dir + "/" + path;
  std::wstring w = Widen(full);
  std::replace(w.begin(), w.end(), L'/', L'\\');
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (n == 0) return w;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return out;
}

std::wstring LauncherPath() {
  const wchar_t* env = _wgetenv(L"GESHER_EXEC_LAUNCHER");
  if (env != nullptr && *env != 0) return env;
  std::wstring self(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (n < self.size()) {
      self.resize(n);
      break;
    }
    self.resize(self.size() * 2);
  }
  size_t slash = self.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1)) + L"gesher-exec.exe";
}

namespace {

int PrepareAt(const std::vector<std::string>& argv, const std::string& dir, int depth, Launch* launch) {
  if (argv.empty() || argv[0].empty()) return kENOENT;
  const std::string& prog = argv[0];
  if (IsShName(prog)) {
    launch->kind = "sh";
    launch->application = LauncherPath();
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, launch->application);
    launch->command_line += L" --sh";
    AppendArgs(&launch->command_line, argv, 1);
    return Finish(launch);
  }
  std::string head;
  if (prog.find_first_of("/\\") != std::string::npos) {
    std::wstring path = WindowsPath(prog, dir);
    int r = Probe(path, &head);
    if (r != 0) return r;
    return LaunchFile(path, head, argv, dir, depth, launch);
  }
  // A bare name: PATH, also with Windows' executable extensions when the name
  // has none. As execvp, EACCES is reported only if nothing runnable is found.
  bool has_ext = !Extension(Widen(prog)).empty();
  static const wchar_t* const kExtensions[] = {L"", L".exe", L".com", L".cmd", L".bat"};
  int seen = kENOENT;
  for (const std::wstring& d : PathDirectories()) {
    for (const wchar_t* e : kExtensions) {
      if (has_ext && *e != 0) break;
      std::wstring candidate = d;
      if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/') candidate += L'\\';
      candidate += Widen(prog) + e;
      std::wstring path = WindowsPath(Narrow(candidate));
      int r = Probe(path, &head);
      if (r == 0) {
        int l = LaunchFile(path, head, argv, dir, depth, launch);
        if (l != kENOEXEC) return l;
        seen = kENOEXEC;
        continue;
      }
      if (r == kEACCES) seen = kEACCES;
    }
  }
  return seen;
}

}  // namespace

int PrepareLaunch(const std::vector<std::string>& argv, const std::string& dir, Launch* launch) {
  return PrepareAt(argv, dir, 0, launch);
}

DWORD Spawn(const Launch& launch, const wchar_t* environment, const std::wstring& cwd, HANDLE std_in,
            HANDLE std_out, HANDLE std_err, PROCESS_INFORMATION* info) {
  std::vector<HANDLE> inherit;
  for (HANDLE h : {std_in, std_out, std_err}) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE && std::find(inherit.begin(), inherit.end(), h) == inherit.end()) {
      inherit.push_back(h);
    }
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return GetLastError();
  if (!inherit.empty() &&
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                 inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
    DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    return e;
  }
  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = std_in;
  si.StartupInfo.hStdOutput = std_out;
  si.StartupInfo.hStdError = std_err;
  si.lpAttributeList = attributes;
  std::wstring cmd = launch.command_line;  // CreateProcessW may write to it
  DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
  BOOL ok = CreateProcessW(launch.application.c_str(), cmd.data(), nullptr, nullptr, inherit.empty() ? FALSE : TRUE,
                           flags, const_cast<wchar_t*>(environment), cwd.empty() ? nullptr : cwd.c_str(),
                           &si.StartupInfo, info);
  DWORD e = ok ? 0 : GetLastError();
  DeleteProcThreadAttributeList(attributes);
  return e;
}

std::wstring EnvironmentBlock(const std::vector<std::string>& entries) {
  std::vector<std::wstring> wide;
  for (const std::string& e : entries) {
    if (e.find('=', 1) == std::string::npos) continue;
    wide.push_back(Widen(e));
  }
  auto name = [](const std::wstring& s) {
    size_t eq = s.find(L'=', 1);
    return s.substr(0, eq);
  };
  std::stable_sort(wide.begin(), wide.end(), [&](const std::wstring& a, const std::wstring& b) {
    std::wstring na = name(a), nb = name(b);
    return CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(), static_cast<int>(nb.size()),
                                TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const std::wstring& w : wide) {
    block += w;
    block += L'\0';
  }
  if (block.empty()) block += L'\0';
  block += L'\0';
  return block;
}

int ExitStatus(DWORD code) {
  switch (code) {
    case 0xC0000005:  // STATUS_ACCESS_VIOLATION
    case 0xC0000006:  // STATUS_IN_PAGE_ERROR
    case 0xC00000FD:  // STATUS_STACK_OVERFLOW
    case 0xC0000028:  // STATUS_BAD_STACK
      return 128 + 11;  // SIGSEGV
    case 0xC000001D:  // STATUS_ILLEGAL_INSTRUCTION
    case 0xC0000096:  // STATUS_PRIVILEGED_INSTRUCTION
      return 128 + 4;  // SIGILL
    case 0xC000008D: case 0xC000008E: case 0xC000008F: case 0xC0000090:
    case 0xC0000091: case 0xC0000092: case 0xC0000093:  // floating point
    case 0xC0000094:  // STATUS_INTEGER_DIVIDE_BY_ZERO
    case 0xC0000095:  // STATUS_INTEGER_OVERFLOW
      return 128 + 8;  // SIGFPE
    case 0x80000003:  // STATUS_BREAKPOINT
    case 0x80000004:  // STATUS_SINGLE_STEP
      return 128 + 5;  // SIGTRAP
    case 0x80000002:  // STATUS_DATATYPE_MISALIGNMENT
      return 128 + 7;  // SIGBUS
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (__fastfail)
      return 128 + 6;  // SIGABRT
    default:
      return static_cast<int>(code);
  }
}

}  // namespace gesher::exec

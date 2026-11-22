// gesher: starting programs the way Linux's execvp would (exec_spawn.h).

#include "exec_spawn.h"
#include "pe_exports.h"

#include <gesher/reserved_names.h>
#include <gesher/environment.h>
#include <gesher/environment_utf16.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>
#include <unordered_map>

namespace gesher::exec {

namespace {

constexpr int kEIO = 5;
constexpr int kENAMETOOLONG = 36;
// CreateProcessW's limit on a command line, in characters with the NUL.
constexpr size_t kMaxCommandLine = 32767;

bool IsSlash(char c) { return c == '/' || c == '\\'; }

bool IsAbsolute(const std::string& p) {
  return (!p.empty() && IsSlash(p[0])) ||
         gesher::reserved_names::DriveRoot(p.c_str());
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

// Pin the selected image against replacement until CreateProcess has opened
// it. Export inspection is a data mapping; no target code runs in this process.
DWORD DescriptorTransportSupport(const std::wstring& path, bool* supported, HANDLE* image) {
  *supported = false;
  *image = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (*image == INVALID_HANDLE_VALUE) { *image = nullptr; return GetLastError(); }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(*image, &size)) return GetLastError();
  if (size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > SIZE_MAX) return ERROR_BAD_EXE_FORMAT;
  HANDLE mapping = CreateFileMappingW(*image, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) return GetLastError();
  const void* bytes = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  DWORD error = bytes ? ERROR_SUCCESS : GetLastError();
  if (bytes) {
    *supported = HasFileDescriptionImporter(bytes, static_cast<size_t>(size.QuadPart));
    UnmapViewOfFile(bytes);
  }
  CloseHandle(mapping);
  return error;
}

bool IsShName(const std::string& name) {
  return name == "sh" || name == "/system/bin/sh" || name == "/bin/sh" || name == "/system/xbin/sh" ||
         name == "/vendor/bin/sh";
}

// The tool name of a bare name or a /system/bin path, or "".
std::string ToolName(const std::string& name) {
  static const char* const kTools[] = {"getprop", "setprop", "logcat", "ls",    "cat",   "id",
                                       "ps",      "pm", "am", "kill",   "sleep", "toybox"};
  std::string base = name;
  for (const char* dir : {"/system/bin/", "/system/xbin/", "/bin/"}) {
    if (name.rfind(dir, 0) == 0) {
      base = name.substr(std::strlen(dir));
      break;
    }
  }
  if (base == name && name.find('/') != std::string::npos) return "";
  for (const char* t : kTools) {
    if (base == t) return base;
  }
  return "";
}

// The directories of PATH: ';'-separated on Windows; a Linux-style ':' list
// is accepted too (a drive letter's colon is not a separator there).
std::wstring EnvironmentValue(const char* value) {
  std::wstring wide;
  if (value && !gesher::DecodeEnvironmentUtf8(value, &wide)) errno = EILSEQ;
  return wide;
}
std::vector<std::wstring> PathDirectories(const std::wstring& path) {
  std::vector<std::wstring> dirs;
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
  const char* c = gesher::GetWindowsEnvironment("ComSpec");
  if (c != nullptr && *c != 0) return EnvironmentValue(c);
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
  // A failed environment-controlled application decode must never become a
  // successful launch with CreateProcess choosing an image from its command.
  if (launch->application.empty()) return EILSEQ;
  return launch->command_line.size() + 1 > kMaxCommandLine ? kE2BIG : 0;
}

int PrepareAt(const std::vector<std::string>& argv, const std::string& dir, int depth, Launch* launch,
              const std::string* path_override);

// The launch for the file found at `path`, run as `argv`.
int LaunchFile(const std::wstring& path, const std::string& head, const std::vector<std::string>& argv,
               const std::string& dir, int depth, Launch* launch) {
  const auto* h = reinterpret_cast<const unsigned char*>(head.data());
  std::wstring ext = Extension(path);
  if (head.size() >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
    // ELF64, little-endian, EM_X86_64 or EM_AARCH64 (run by the arm64
    // translator, docs/arm64.md): anything else is not runnable here.
    if (h[4] != 2 || h[5] != 1 || (h[18] != 62 && h[18] != 183) || h[19] != 0) return kENOEXEC;
    // A static executable (ET_EXEC) makes Linux system calls itself: not
    // runnable on Windows (ADR 0055).
    if (h[16] == 2 && h[17] == 0) return kENOEXEC;
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
    int r = PrepareAt(args, dir, depth + 1, &inner, nullptr);
    if (r != 0) return r == kENOENT || r == EILSEQ ? r : kENOEXEC;
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
    case EILSEQ: return "Invalid environment encoding";
    case kE2BIG: return "Argument list too long";
    case kENOEXEC: return "Exec format error";
    case kENOMEM: return "Out of memory";
    case kEACCES: return "Permission denied";
    case kENOTDIR: return "Not a directory";
    case kENAMETOOLONG: return "File name too long";
    case kEBADF: return "Bad file descriptor";
    case kEINVAL: return "Invalid argument";
    case kEOPNOTSUPP: return "Operation not supported";
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
    case ERROR_INVALID_HANDLE:
      return kEBADF;
    case ERROR_INVALID_PARAMETER:
      return kEINVAL;
    case ERROR_NO_UNICODE_TRANSLATION:
      return EILSEQ;
    case ERROR_NOT_SUPPORTED:
      return kEOPNOTSUPP;
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
  gesher::reserved_names::Encode(&w);  // names stored as gesher-base stores them
  std::replace(w.begin(), w.end(), L'/', L'\\');
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (n == 0) return w;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return out;
}

std::wstring LauncherPath() {
  const char* env = gesher::GetWindowsEnvironment("GESHER_EXEC_LAUNCHER");
  if (env != nullptr && *env != 0) return EnvironmentValue(env);
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

int PrepareProgram(const std::string& prog, const std::vector<std::string>& argv, const std::string& dir,
                   int depth, bool search, Launch* launch, const std::string* path_override) {
  if (argv.empty() || prog.empty()) return kENOENT;
  if (IsShName(prog)) {
    launch->kind = "sh";
    launch->application = LauncherPath();
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, launch->application);
    launch->command_line += L" --sh";
    AppendArgs(&launch->command_line, argv, 1);
    return Finish(launch);
  }
  std::string tool = ToolName(prog);
  if (!tool.empty() && (search || prog.find('/') != std::string::npos)) {
    // Android's /system/bin programs that gesher-exec provides (docs/exec.md).
    launch->kind = "tool";
    launch->application = LauncherPath();
    launch->command_line.clear();
    AppendQuoted(&launch->command_line, launch->application);
    launch->command_line += L" --tool ";
    AppendQuoted(&launch->command_line, Widen(tool));
    AppendArgs(&launch->command_line, argv, 1);
    return Finish(launch);
  }
  std::string head;
  if (!search || prog.find_first_of("/\\") != std::string::npos) {
    std::wstring path = WindowsPath(prog, dir);
    int r = Probe(path, &head);
    if (r != 0) return r;
    return LaunchFile(path, head, argv, dir, depth, launch);
  }
  // A bare name: PATH, also with Windows' executable extensions when the name
  // has none. As execvp, EACCES is reported only if nothing runnable is found.
  const char* environment_path = path_override ? path_override->c_str() : gesher::GetWindowsEnvironment("PATH");
  std::wstring path_value;
  if (environment_path && !gesher::DecodeEnvironmentUtf8(environment_path, &path_value)) return EILSEQ;
  bool has_ext = !Extension(Widen(prog)).empty();
  static const wchar_t* const kExtensions[] = {L"", L".exe", L".com", L".cmd", L".bat"};
  int seen = kENOENT;
  for (const std::wstring& d : PathDirectories(path_value)) {
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

int PrepareAt(const std::vector<std::string>& argv, const std::string& dir, int depth, Launch* launch,
              const std::string* path_override) {
  if (argv.empty()) return kENOENT;
  return PrepareProgram(argv[0], argv, dir, depth, true, launch, path_override);
}

}  // namespace

int PrepareLaunch(const std::vector<std::string>& argv, const std::string& dir, Launch* launch) {
  return PrepareAt(argv, dir, 0, launch, nullptr);
}

int PrepareLaunchInPath(const std::vector<std::string>& argv, const std::string& dir, const std::string& path,
                        Launch* launch) {
  return PrepareAt(argv, dir, 0, launch, &path);
}

std::wstring FindInPath(const std::string& name, const std::string& path) {
  if (name.empty() || IsShName(name) || !ToolName(name).empty()) return L"";
  bool has_ext = !Extension(Widen(name)).empty();
  static const wchar_t* const kExtensions[] = {L"", L".exe", L".com", L".cmd", L".bat"};
  for (const std::wstring& d : PathDirectories(Widen(path))) {
    for (const wchar_t* e : kExtensions) {
      if (has_ext && *e != 0) break;
      std::wstring candidate = d;
      if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/') candidate += L'\\';
      candidate += Widen(name) + e;
      std::wstring full = WindowsPath(Narrow(candidate));
      DWORD attributes = GetFileAttributesW(full.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return full;
    }
  }
  return L"";
}

int PrepareLaunchProgram(const std::string& program, const std::vector<std::string>& argv,
                         const std::string& dir, bool search_path, Launch* launch) {
  return PrepareProgram(program, argv, dir, 0, search_path, launch, nullptr);
}

int PrepareLaunchProgramInPath(const std::string& program, const std::vector<std::string>& argv,
                               const std::string& dir, bool search_path, const std::string& path,
                               Launch* launch) {
  return PrepareProgram(program, argv, dir, 0, search_path, launch, &path);
}

bool IsToolName(const std::string& name) { return !ToolName(name).empty(); }

DWORD SpawnWithFds(const Launch& launch, const wchar_t* environment, const std::wstring& cwd,
                   const std::vector<ChildFd>& fds, PROCESS_INFORMATION* info, DWORD extra_flags) try {
  // Default native/Java launches inherit the same live POSIX bytes as ELF
  // execv. Windows' copied environment cannot observe putenv buffer edits.
  std::wstring inherited_environment;
  if (!environment) {
    std::vector<std::string> entries;
    if (!gesher::VisitEnvironment([](const char* entry, void* context) {
          static_cast<std::vector<std::string>*>(context)->emplace_back(entry);
        }, &entries)) return ERROR_NOT_ENOUGH_MEMORY;
    // Native inheritance also includes Windows' per-drive working directories.
    // Keep these native-only UTF16 entries out of POSIX environ, but preserve
    // them when replacing CreateProcess's previous null-environment behavior.
    std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> native(
        GetEnvironmentStringsW(), &FreeEnvironmentStringsW);
    if (!native) {
      const DWORD error = GetLastError();
      return error ? error : ERROR_NOT_ENOUGH_MEMORY;
    }
    inherited_environment = EnvironmentBlock(entries, native.get());
    if (inherited_environment.empty()) return ERROR_NO_UNICODE_TRANSLATION;
    environment = inherited_environment.c_str();
  }
  struct ImagePin {
    HANDLE value = nullptr;
    ~ImagePin() { if (value) CloseHandle(value); }
  } image;
  bool transport = false;
  DWORD image_error = DescriptorTransportSupport(launch.application, &transport, &image.value);
  if (image_error) return image_error;
  struct OwnedHandles {
    std::vector<HANDLE> values;
    ~OwnedHandles() { for (HANDLE handle : values) CloseHandle(handle); }
  } owned;
  auto errno_error = []() -> DWORD {
    switch (errno) {
      case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
      case EBADF: return ERROR_INVALID_HANDLE;
      case EINVAL: return ERROR_INVALID_PARAMETER;
      case EACCES: return ERROR_ACCESS_DENIED;
      case EOPNOTSUPP: case ENOSYS: return ERROR_NOT_SUPPORTED;
      default: return ERROR_GEN_FAILURE;
    }
  };
  int count = 3;
  for (const ChildFd& file : fds) {
    if (file.fd < 0 || static_cast<unsigned>(file.fd) >= gesher::ofd_transfer::kDescriptorLimit)
      return ERROR_INVALID_PARAMETER;
    count = std::max(count, file.fd + 1);
  }
  // Reserve ownership slots before acquiring inheritable handles. At most one
  // file and one shared description exist per selected fd, plus three closed
  // stdio placeholders and the manifest. A bookkeeping allocation must never
  // strand a newly duplicated handle outside the cleanup owner.
  const size_t selected = std::min(fds.size(), static_cast<size_t>(count));
  owned.values.reserve(selected * (transport ? 2 : 1) + (transport ? 4 : 0));
  std::vector<gesher::DescriptorSnapshotPtr> snapshots(static_cast<size_t>(count));
  std::vector<std::unique_ptr<gesher::NativeFileDescriptionExport>> native_exports;
  for (const ChildFd& file : fds) {
    auto snapshot = file.snapshot ? file.snapshot : gesher::CaptureFileHandle(file.handle);
    if (!snapshot) return errno_error();
    if (snapshot->kind == gesher::DescriptorKind::kUnshareable) return ERROR_NOT_SUPPORTED;
    if (file.snapshot && file.handle != snapshot->handle) return ERROR_INVALID_PARAMETER;
    if (!transport) {
      if (file.fd >= 7000) return ERROR_NOT_SUPPORTED;
      auto reservation = std::make_unique<gesher::NativeFileDescriptionExport>();
      if (reservation->Prepare(snapshot) < 0) return errno_error();
      native_exports.push_back(std::move(reservation));
    }
    snapshots[static_cast<size_t>(file.fd)] = std::move(snapshot);
  }
  auto inheritable = [&](HANDLE source) -> HANDLE {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) return nullptr;
    owned.values.push_back(duplicate);
    return duplicate;
  };
  std::vector<HANDLE> by_fd(static_cast<size_t>(count), nullptr);
  std::unordered_map<HANDLE, HANDLE> descriptions;
  std::vector<gesher::ofd_transfer::Entry> entries;
  for (int fd = 0; fd < count; ++fd) {
    const auto& snapshot = snapshots[static_cast<size_t>(fd)];
    if (!snapshot) continue;
    HANDLE file = inheritable(snapshot->handle);
    if (!file) return GetLastError();
    by_fd[static_cast<size_t>(fd)] = file;
    if (!transport) continue;
    HANDLE shared = gesher::ShareFileDescription(snapshot);
    if (!shared) return errno_error();
    HANDLE inherited_description = nullptr;
    auto existing = descriptions.find(shared);
    if (existing != descriptions.end()) {
      inherited_description = existing->second;
    } else {
      inherited_description = inheritable(shared);
      if (!inherited_description) return GetLastError();
      descriptions.emplace(shared, inherited_description);
    }
    entries.push_back({fd, static_cast<uint32_t>(snapshot->kind), reinterpret_cast<uintptr_t>(inherited_description),
                       reinterpret_cast<uintptr_t>(file)});
  }
  uint32_t closed_stdio = 0;
  for (int fd = 0; fd < 3; ++fd) {
    if (!transport) break;
    if (by_fd[static_cast<size_t>(fd)]) continue;
    closed_stdio |= 1u << fd;
    // The CRT may need real standard handles during startup. The description
    // importer closes these temporary placeholders before application code.
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) return GetLastError();
    owned.values.push_back(nul);
    by_fd[static_cast<size_t>(fd)] = nul;
  }
  // UCRT's block has a WORD length. High slots remain valid: the manifest
  // carries their real inherited handles for adoption during early startup.
  const int crt_count = std::min(count, 7000);
  const size_t crt_size = sizeof(int) + static_cast<size_t>(crt_count) * (1 + sizeof(intptr_t));
  constexpr unsigned char kFopen = 0x01, kFpipe = 0x08, kFdev = 0x40;
  std::vector<unsigned char> block(crt_size + (transport ? sizeof(gesher::ofd_transfer::Startup) : 0), 0);
  std::memcpy(block.data(), &crt_count, sizeof(crt_count));
  for (int fd = 0; fd < crt_count; ++fd) {
    HANDLE file = by_fd[static_cast<size_t>(fd)];
    unsigned char flags = 0;
    intptr_t value = -1;
    if (file) {
      DWORD type = GetFileType(file);
      flags = kFopen | (type == FILE_TYPE_PIPE ? kFpipe : 0) | (type == FILE_TYPE_CHAR ? kFdev : 0);
      value = reinterpret_cast<intptr_t>(file);
    }
    block[sizeof(int) + static_cast<size_t>(fd)] = flags;
    std::memcpy(block.data() + sizeof(int) + static_cast<size_t>(crt_count) +
                  static_cast<size_t>(fd) * sizeof(intptr_t), &value, sizeof(value));
  }
  if (transport) {
    const size_t manifest_size = sizeof(gesher::ofd_transfer::Header) +
                                 entries.size() * sizeof(gesher::ofd_transfer::Entry);
    SECURITY_ATTRIBUTES manifest_attributes{sizeof(manifest_attributes), nullptr, TRUE};
    HANDLE manifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &manifest_attributes, PAGE_READWRITE,
                                         0, static_cast<DWORD>(manifest_size), nullptr);
    if (!manifest) return GetLastError();
    owned.values.push_back(manifest);
    void* view = MapViewOfFile(manifest, FILE_MAP_WRITE, 0, 0, manifest_size);
    if (!view) return GetLastError();
    const gesher::ofd_transfer::Header header{gesher::ofd_transfer::kMagic,
        gesher::ofd_transfer::kVersion, static_cast<uint32_t>(entries.size()), closed_stdio, 0};
    std::memcpy(view, &header, sizeof(header));
    if (!entries.empty()) std::memcpy(static_cast<unsigned char*>(view) + sizeof(header),
                                      entries.data(), entries.size() * sizeof(entries[0]));
    UnmapViewOfFile(view);
    const gesher::ofd_transfer::Startup extension{gesher::ofd_transfer::kMagic,
        reinterpret_cast<uintptr_t>(manifest), manifest_size};
    std::memcpy(block.data() + crt_size, &extension, sizeof(extension));
  }

  SIZE_T bytes = 0;
  if (!owned.values.empty()) {
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) return GetLastError();
  }
  std::vector<char> storage(bytes);
  auto* attributes = bytes ? reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data()) : nullptr;
  struct AttributeCleanup {
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~AttributeCleanup() { if (value) DeleteProcThreadAttributeList(value); }
  } attribute_cleanup;
  if (attributes) {
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return GetLastError();
    attribute_cleanup.value = attributes;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   owned.values.data(), owned.values.size() * sizeof(HANDLE), nullptr, nullptr)) {
      return GetLastError();
    }
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = attributes ? sizeof(startup) : sizeof(startup.StartupInfo);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = by_fd[0] ? by_fd[0] : INVALID_HANDLE_VALUE;
  startup.StartupInfo.hStdOutput = by_fd[1] ? by_fd[1] : INVALID_HANDLE_VALUE;
  startup.StartupInfo.hStdError = by_fd[2] ? by_fd[2] : INVALID_HANDLE_VALUE;
  startup.StartupInfo.cbReserved2 = static_cast<WORD>(block.size());
  startup.StartupInfo.lpReserved2 = block.data();
  startup.lpAttributeList = attributes;
  std::wstring command_line = launch.command_line;
  DWORD flags = CREATE_UNICODE_ENVIRONMENT | extra_flags;
  // With no inherited handles, CREATE_NO_WINDOW still supplies fresh console
  // standard handles. Default to detaching this native child, while preserving
  // any console mode the caller explicitly requested.
  if (!(extra_flags & (CREATE_NEW_CONSOLE | CREATE_NO_WINDOW | DETACHED_PROCESS)))
    flags |= owned.values.empty() ? DETACHED_PROCESS : CREATE_NO_WINDOW;
  if (attributes) flags |= EXTENDED_STARTUPINFO_PRESENT;
  BOOL started = CreateProcessW(launch.application.c_str(), command_line.data(), nullptr, nullptr, attributes ? TRUE : FALSE,
      flags, const_cast<wchar_t*>(environment), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, info);
  DWORD error = started ? ERROR_SUCCESS : GetLastError();
  if (started) for (auto& reservation : native_exports) reservation->Commit();
  return error;
} catch (const std::bad_alloc&) {
  // No allocation exception crosses the Win32-status launch API.
  return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD Spawn(const Launch& launch, const wchar_t* environment, const std::wstring& cwd, HANDLE std_in,
            HANDLE std_out, HANDLE std_err, PROCESS_INFORMATION* info) try {
  std::vector<ChildFd> fds;
  const HANDLE standard[] = {std_in, std_out, std_err};
  for (int fd = 0; fd < 3; ++fd) {
    HANDLE handle = standard[fd];
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) fds.push_back({fd, handle});
  }
  return SpawnWithFds(launch, environment, cwd, fds, info);
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

std::wstring EnvironmentBlock(const std::vector<std::string>& entries,
                              const wchar_t* native_defaults) {
  std::vector<std::wstring> wide;
  for (const wchar_t* entry = native_defaults; entry && *entry; entry += std::wcslen(entry) + 1)
    if (*entry == L'=') wide.emplace_back(entry);
  for (const std::string& e : entries) {
    if (e.find('=', 1) == std::string::npos) continue;
    std::wstring value;
    if (!gesher::DecodeEnvironmentUtf8(e, &value)) return errno = EILSEQ, std::wstring{};
    wide.push_back(std::move(value));
  }
  auto name = [](const std::wstring& s) {
    size_t eq = s.find(L'=', 1);
    return std::wstring_view(s).substr(0, eq);
  };
  std::stable_sort(wide.begin(), wide.end(), [&](const std::wstring& a, const std::wstring& b) {
    const auto na = name(a), nb = name(b);
    return CompareStringOrdinal(na.data(), static_cast<int>(na.size()), nb.data(), static_cast<int>(nb.size()),
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

int WaitStatus(DWORD code) {
  int status = ExitStatus(code);
  if (status > 128 && status < 128 + 32) return status - 128;  // WIFSIGNALED, WTERMSIG
  return (status & 0xff) << 8;                                 // WIFEXITED, WEXITSTATUS
}

}  // namespace gesher::exec

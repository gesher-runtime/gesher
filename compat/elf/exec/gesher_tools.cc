// gesher-exec --tool <name> [args]: the Android /system/bin programs apps
// really call, as toybox/AOSP provide them (docs/exec.md, ADR 0055). No root,
// so `su` is absent; these cover ls, cat, id, ps, getprop, setprop, logcat, pm,
// am, kill, sleep, which and mount. exec_spawn.h routes a bare name or a
// /system/bin path here.
//
// They read and write bytes unchanged (binary stdio, set by main()).

#include "exec_spawn.h"

#include <gesher/system_properties.h>
#include <gesher/logging.h>
#include <gesher/reserved_names.h>

#include <fcntl.h>
#include <unistd.h>
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" int gesher_kill_other(int pid, int sig);

namespace gesher::exec {

namespace {

std::string EnvOr(const char* name, const char* fallback) {
  const wchar_t* v = _wgetenv(Widen(name).c_str());
  return v != nullptr && *v != 0 ? Narrow(v) : fallback;
}

// --- cat -----------------------------------------------------------------------

int CatFile(FILE* in) {
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (std::fwrite(buf, 1, n, stdout) != n) return 1;
  }
  return 0;
}

int Cat(const std::vector<std::string>& args) {
  int status = 0;
  bool any = false;
  for (const std::string& a : args) {
    if (a == "-" ) {
      CatFile(stdin);
      any = true;
      continue;
    }
    if (!a.empty() && a[0] == '-') continue;  // ignore options
    any = true;
    FILE* f = _wfopen(WindowsPath(a).c_str(), L"rb");
    if (f == nullptr) {
      std::fprintf(stderr, "cat: %s: No such file or directory\n", a.c_str());
      status = 1;
      continue;
    }
    if (CatFile(f) != 0) status = 1;
    std::fclose(f);
  }
  if (!any) CatFile(stdin);
  std::fflush(stdout);
  return status;
}

// --- ls ------------------------------------------------------------------------

int ListOne(const std::string& path, bool long_form, bool all) {
  std::wstring w = WindowsPath(path);
  DWORD attrs = GetFileAttributesW(w.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    std::fprintf(stderr, "ls: %s: No such file or directory\n", path.c_str());
    return 1;
  }
  std::vector<std::string> names;
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((w + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::string name = Narrow(gesher::reserved_names::Decoded(fd.cFileName));
        if (!all && !name.empty() && name[0] == '.') continue;
        names.push_back(name);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  } else {
    names.push_back(path);
  }
  for (const std::string& n : names) {
    if (long_form) {
      std::wstring name = Widen(n);
      gesher::reserved_names::EncodeName(&name);  // listed decoded, stored encoded
      std::wstring full = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? w + L"\\" + name : w;
      WIN32_FILE_ATTRIBUTE_DATA info = {};
      long long size = 0;
      char type = '-';
      if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info)) {
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) type = 'd';
        size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
      }
      std::fprintf(stdout, "%crw-rw-rw- %10lld %s\n", type, size, n.c_str());
    } else {
      std::fprintf(stdout, "%s\n", n.c_str());
    }
  }
  std::fflush(stdout);
  return 0;
}

int Ls(const std::vector<std::string>& args) {
  bool long_form = false, all = false;
  std::vector<std::string> paths;
  for (const std::string& a : args) {
    if (a.size() >= 2 && a[0] == '-' && a[1] != '-') {
      for (char c : a.substr(1)) {
        if (c == 'l') long_form = true;
        else if (c == 'a') all = true;
      }
    } else if (a != "--") {
      paths.push_back(a);
    }
  }
  if (paths.empty()) paths.push_back(".");
  int status = 0;
  for (const std::string& p : paths) {
    if (ListOne(p, long_form, all) != 0) status = 1;
  }
  return status;
}

// --- id ------------------------------------------------------------------------

int Id(const std::vector<std::string>&) {
  int uid = std::atoi(EnvOr("GESHER_UID", "10000").c_str());
  std::fprintf(stdout, "uid=%d(u0_a%d) gid=%d(u0_a%d) groups=%d(u0_a%d)\n", uid,
               uid >= 10000 ? uid - 10000 : uid, uid, uid >= 10000 ? uid - 10000 : uid, uid,
               uid >= 10000 ? uid - 10000 : uid);
  std::fflush(stdout);
  return 0;
}

// --- ps ------------------------------------------------------------------------

int Ps(const std::vector<std::string>&) {
  std::fprintf(stdout, "%-8s %-8s %s\n", "PID", "PPID", "NAME");
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 1;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      std::fprintf(stdout, "%-8lu %-8lu %s\n", pe.th32ProcessID, pe.th32ParentProcessID,
                   Narrow(pe.szExeFile).c_str());
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  std::fflush(stdout);
  return 0;
}

// --- getprop / setprop ---------------------------------------------------------
//
// The same boot-scoped area as Java SystemProperties and ELF bionic.

int Getprop(const std::vector<std::string>& args) {
  std::vector<std::string> names;
  for (const std::string& a : args) {
    if (a.empty() || a[0] != '-') names.push_back(a);
  }
  if (names.empty()) {
    if (!properties::Initialize()) return 1;
    for (const auto& [name, value] : properties::List()) {
      std::fprintf(stdout, "[%s]: [%s]\n", name.c_str(), value.text.c_str());
    }
    std::fflush(stdout);
    return 0;
  }
  if (!properties::Initialize()) return 1;
  std::string value;
  properties::Get(names[0], &value);
  if (value.empty() && names.size() > 1) value = names[1];
  std::fprintf(stdout, "%s\n", value.c_str());
  std::fflush(stdout);
  return 0;
}

int Setprop(const std::vector<std::string>& args) {
  if (args.size() != 2) {
    std::fprintf(stderr, "usage: setprop NAME VALUE\n");
    return 1;
  }
  if (properties::Set(args[0], args[1])) return 0;
  std::fprintf(stderr, "setprop: %s: property is read-only, invalid, or the store is unavailable\n", args[0].c_str());
  return 1;
}

// --- logcat --------------------------------------------------------------------
//
// Gesher's log for a process is the file in $GESHER_LOG_FILE (the system server
// sets it per app process); -d dumps it and exits, otherwise it follows.

int Logcat(const std::vector<std::string>& args) {
  bool dump = false, clear = false;
  int default_priority = 2;
  std::string format = "brief";
  std::map<std::string, int> filters;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& argument = args[i];
    if (argument == "-d" || argument == "--dump") { dump = true; continue; }
    if (argument == "-c" || argument == "--clear") { clear = true; continue; }
    if (argument == "-s" || argument == "--silent") { default_priority = 8; continue; }
    if (argument == "-v" || argument == "--format") {
      if (++i == args.size()) { std::fprintf(stderr, "logcat: -v needs a format\n"); return 1; }
      format = args[i];
      if (format != "brief" && format != "process" && format != "tag" && format != "thread" &&
          format != "raw" && format != "time" && format != "threadtime" && format != "long" && format != "epoch") {
        std::fprintf(stderr, "logcat: unsupported format %s\n", format.c_str());
        return 1;
      }
      continue;
    }
    if (argument.empty() || argument[0] == '-') {
      std::fprintf(stderr, "logcat: unsupported option %s\n", argument.c_str());
      return 1;
    }
    size_t colon = argument.rfind(':');
    int priority = 2;
    std::string tag = argument;
    if (colon != std::string::npos) {
      if (colon + 2 != argument.size() || (priority = logging::Priority(argument.back())) < 0) {
        std::fprintf(stderr, "logcat: invalid filter %s\n", argument.c_str());
        return 1;
      }
      tag = argument.substr(0, colon);
    }
    if (tag.empty()) { std::fprintf(stderr, "logcat: empty tag\n"); return 1; }
    if (tag == "*") default_priority = priority;
    else filters[tag] = priority;
  }
  std::string error;
  if (clear) {
    if (logging::Clear(&error)) return 0;
    std::fprintf(stderr, "logcat: %s\n", error.c_str());
    return 1;
  }
  size_t consumed = 0;
  uint64_t last_timestamp = 0;
  do {
    std::vector<logging::Entry> entries;
    if (!logging::Read(&entries, &error)) { std::fprintf(stderr, "logcat: %s\n", error.c_str()); return 1; }
    if (entries.size() < consumed || (consumed && entries[consumed - 1].timestamp != last_timestamp)) consumed = 0;
    for (size_t i = consumed; i < entries.size(); ++i) {
      const auto& entry = entries[i];
      auto filter = filters.find(entry.tag);
      if (entry.priority < (filter == filters.end() ? default_priority : filter->second)) continue;
      std::string line = logging::Format(entry, format);
      if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) return 0;
    }
    if (std::fflush(stdout) != 0) return 0;
    consumed = entries.size();
    last_timestamp = consumed ? entries.back().timestamp : 0;
    if (!dump) Sleep(200);
  } while (!dump);
  return 0;
}

// --- kill ----------------------------------------------------------------------

int SignalByName(const std::string& name) {
  static const struct {
    const char* n;
    int sig;
  } kSigs[] = {{"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"KILL", 9}, {"USR1", 10}, {"USR2", 12},
               {"TERM", 15}, {"STOP", 19}, {"CONT", 18}};
  std::string n = name;
  if (n.rfind("SIG", 0) == 0) n = n.substr(3);
  for (const auto& s : kSigs) {
    if (n == s.n) return s.sig;
  }
  return std::atoi(name.c_str());
}

int Kill(const std::vector<std::string>& args) {
  int sig = 15;
  size_t i = 0;
  if (i < args.size() && !args[i].empty() && args[i][0] == '-') {
    sig = SignalByName(args[i].substr(1));
    ++i;
  }
  if (i >= args.size()) {
    std::fprintf(stderr, "usage: kill [-SIGNAL] PID...\n");
    return 1;
  }
  int status = 0;
  for (; i < args.size(); ++i) {
    int pid = std::atoi(args[i].c_str());
    if (gesher_kill_other(pid, sig) != 0) {
      std::fprintf(stderr, "kill: %d: No such process\n", pid);
      status = 1;
    }
  }
  return status;
}

// --- sleep ---------------------------------------------------------------------

int Sleep_(const std::vector<std::string>& args) {
  if (args.empty()) return 0;
  double seconds = std::atof(args[0].c_str());
  if (seconds > 0) Sleep(static_cast<DWORD>(seconds * 1000));
  return 0;
}

// --- pm ------------------------------------------------------------------------
//
// Keep the Binder client in the boot-classpath helper, with the runtime's real
// package/activity interfaces. Native callers inherit the same device namespace.
int DeviceCommand(const std::string& tool, const std::vector<std::string>& args) {
  if (EnvOr("GESHER_BINDER", "").empty()) { std::fprintf(stderr, "%s: no Gesher device namespace\n", tool.c_str()); return 1; }
  std::wstring directory = LauncherPath();
  size_t slash = directory.find_last_of(L"/\\");
  if (slash == std::wstring::npos) return 126;
  directory.resize(slash + 1);
  auto has_image = [](const std::wstring& path) {
    auto file = [](const std::wstring& candidate) {
      DWORD attributes = GetFileAttributesW(candidate.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };
    if (file(path)) return true;
    // ART accepts the logical framework/boot.art option and selects its host
    // instruction-set subdirectory itself. Do not reject that normal layout
    // merely because there is no physical boot.art beside x86_64/boot.art.
    size_t separator = path.find_last_of(L"/\\");
    return separator != std::wstring::npos &&
           file(path.substr(0, separator + 1) + L"x86_64\\" + path.substr(separator + 1));
  };
  std::wstring image = WindowsPath(EnvOr("ANDROID_ROOT", "/system") + "/framework/boot.art");
  if (!has_image(image)) image = directory + L"framework\\boot.art";
  if (!has_image(image)) {
    std::fprintf(stderr, "%s: the runtime boot image is unavailable\n", tool.c_str());
    return 126;
  }
  auto vm_path = [&](std::wstring path) {
    // Boot class paths use ':' separators. Rooted paths on the VM's current
    // drive avoid interpreting the Windows drive colon as another entry.
    if (path.size() > 2 && path[1] == L':' && directory.size() > 2 &&
        (path[0] | 0x20) == (directory[0] | 0x20)) path.erase(0, 2);
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return Narrow(path);
  };
  std::string boot_class_path = "-Xbootclasspath:";
  for (const wchar_t* jar : {L"core-oj.jar", L"core-libart.jar", L"core-icu4j.jar", L"framework.jar", L"gesher-app.jar"}) {
    if (boot_class_path.back() != ':') boot_class_path += ':';
    boot_class_path += vm_path(directory + jar);
  }
  std::vector<std::string> command{Narrow(directory + L"dalvikvm.exe"),
      boot_class_path, "-Ximage:" + vm_path(image), "-Xusejit:false", "com.gesher.server.RuntimeCommands", tool};
  command.insert(command.end(), args.begin(), args.end());
  Launch launch;
  int error = PrepareLaunch(command, Narrow(directory), &launch);
  if (error) { std::fprintf(stderr, "%s: %s\n", tool.c_str(), ErrorText(error)); return 126; }
  PROCESS_INFORMATION process{};
  DWORD failed = SpawnWithFds(launch, nullptr, directory,
      {{0, GetStdHandle(STD_INPUT_HANDLE)}, {1, GetStdHandle(STD_OUTPUT_HANDLE)}, {2, GetStdHandle(STD_ERROR_HANDLE)}}, &process);
  if (failed) { std::fprintf(stderr, "%s: %s\n", tool.c_str(), ErrorText(ErrnoFromWin32(failed))); return 126; }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return ExitStatus(code);
}

// --- which ---------------------------------------------------------------------
//
// toybox which [-a] NAME...: the file each NAME runs from, the first match
// (-a: every match), exit 1 if any NAME has none. A name with a '/' is that
// file if it is one. A bare name exec runs as Gesher's own /system/bin
// program (sh, the tools here) is /system/bin/<name>, found first, as exec
// finds it; otherwise $PATH is searched as exec searches it. `su` is neither:
// Gesher has no root, so `which su` prints nothing and fails.

int Which(const std::vector<std::string>& args) {
  bool all = false, options = true;
  std::vector<std::string> names;
  for (const std::string& a : args) {
    if (options && a == "--") { options = false; continue; }
    if (options && a.size() > 1 && a[0] == '-') {
      for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] != 'a') {
          std::fprintf(stderr, "which: Unknown option '%s'\n", a.c_str());
          return 1;
        }
        all = true;
      }
      continue;
    }
    names.push_back(a);
  }
  if (names.empty()) {
    std::fprintf(stderr, "which: Need 1 argument\n");
    return 1;
  }
  int status = 0;
  for (const std::string& name : names) {
    if (name.find('/') != std::string::npos) {
      if (IsShellName(name) || IsToolName(name)) {
        std::fprintf(stdout, "%s\n", name.c_str());
        continue;
      }
      DWORD attributes = GetFileAttributesW(WindowsPath(name).c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) std::fprintf(stdout, "%s\n", name.c_str());
        else status = 1;
        continue;
      }
    }
    std::vector<std::string> found;
    if (name.find('/') == std::string::npos && (IsShellName(name) || IsToolName(name)))
      found.push_back("/system/bin/" + name);
    for (std::string& path : FindAllInPath(name, EnvOr("PATH", ""))) found.push_back(std::move(path));
    if (found.empty()) status = 1;
    for (const std::string& path : found) {
      std::fprintf(stdout, "%s\n", path.c_str());
      if (!all) break;
    }
  }
  std::fflush(stdout);
  return status;
}

// --- mount ---------------------------------------------------------------------
//
// toybox mount as an app's uid (Gesher has no root). With no device or
// directory it lists /proc/mounts, the same table Gesher's /proc/mounts and
// getmntent serve, as "DEVICE on DIR type TYPE (OPTIONS)". Mounting as
// non-root goes through /etc/fstab first (toybox mount_main): a missing fstab,
// an entry that is not there, or one not marked "user" fails with toybox's
// message and status 1; a "user" entry reaches mount(2), which an app's
// process may not call (EPERM).

struct MountEntry {
  std::string device, dir, type, opts;
};

// The entries of a mount table file (fstab(5)/proc mounts format, with
// getmntent's \ooo escapes), or false with errno set if it cannot be read.
bool ReadMountTable(const char* path, std::vector<MountEntry>* entries) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::string text;
  char buffer[4096];
  for (;;) {
    int n = static_cast<int>(::read(fd, buffer, sizeof(buffer)));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      int saved = errno;
      ::close(fd);
      errno = saved;
      return false;
    }
    if (n == 0) break;
    text.append(buffer, static_cast<size_t>(n));
  }
  ::close(fd);
  auto unescape = [](const std::string& field) {
    std::string out;
    for (size_t i = 0; i < field.size(); ++i) {
      if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' && field[i + 1] <= '3' &&
          field[i + 2] >= '0' && field[i + 2] <= '7' && field[i + 3] >= '0' && field[i + 3] <= '7') {
        out += static_cast<char>(((field[i + 1] - '0') << 6) | ((field[i + 2] - '0') << 3) | (field[i + 3] - '0'));
        i += 3;
      } else {
        out += field[i];
      }
    }
    return out;
  };
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> fields;
    size_t at = 0;
    while (fields.size() < 4) {
      at = line.find_first_not_of(" \t", at);
      if (at == std::string::npos || line[at] == '#') break;
      size_t stop = line.find_first_of(" \t", at);
      if (stop == std::string::npos) stop = line.size();
      fields.push_back(unescape(line.substr(at, stop - at)));
      at = stop;
    }
    if (fields.size() < 3) continue;  // blank, comment or malformed
    if (fields.size() == 3) fields.push_back("defaults");
    entries->push_back({fields[0], fields[1], fields[2], fields[3]});
  }
  return true;
}

// Whether the comma-separated `opts` holds `option` (toybox comma_scan).
bool HasOption(const std::string& opts, const std::string& option) {
  size_t start = 0;
  while (start <= opts.size()) {
    size_t end = opts.find(',', start);
    if (end == std::string::npos) end = opts.size();
    if (opts.compare(start, end - start, option) == 0) return true;
    start = end + 1;
  }
  return false;
}

int Mount(const std::vector<std::string>& args) {
  bool all = false, fake = false;
  std::string type, opts, only;
  std::vector<std::string> operands;
  auto add_opts = [&](const std::string& more) {
    if (more.empty()) return;
    if (!opts.empty()) opts += ',';
    opts += more;
  };
  bool options = true;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (options && a == "--") { options = false; continue; }
    // toybox's "?" option string: a --name is an argument (an -o option below).
    if (!options || a.size() < 2 || a[0] != '-' || a[1] == '-') {
      operands.push_back(a);
      continue;
    }
    for (size_t c = 1; c < a.size(); ++c) {
      char flag = a[c];
      if (flag == 't' || flag == 'o' || flag == 'O') {
        std::string value;
        if (c + 1 < a.size()) value = a.substr(c + 1);
        else if (i + 1 < args.size()) value = args[++i];
        else {
          std::fprintf(stderr, "mount: Missing argument to -%c\n", flag);
          return 1;
        }
        if (flag == 't') type = value;
        else if (flag == 'o') add_opts(value);
        else only = value;
        break;
      }
      if (flag == 'a') all = true;
      else if (flag == 'f') fake = true;
      else if (flag == 'r') add_opts("ro");
      else if (flag == 'w') add_opts("rw");
      else if (flag == 'R') add_opts("rbind");
      else if (flag == 'n' || flag == 'v') {
      } else {
        // toybox passes an unknown -x through as an argument.
        operands.push_back(a);
        break;
      }
    }
  }
  std::string dev, dir;
  for (const std::string& operand : operands) {
    if (operand.size() > 2 && operand[0] == '-' && operand[1] == '-') add_opts(operand.substr(2));
    else if (dev.empty()) dev = operand;
    else if (dir.empty()) dir = operand;
    else {
      std::fprintf(stderr, "mount: Max 2 arguments\n");
      return 1;
    }
  }
  if (all && !dir.empty()) {
    std::fprintf(stderr, "mount: -a with >1 arg\n");
    return 1;
  }
  bool remount = HasOption(opts, "remount");
  std::vector<MountEntry> mounted;
  if (remount && !ReadMountTable("/proc/mounts", &mounted)) {
    std::fprintf(stderr, "mount: bad /proc/mounts: %s\n", ErrorText(errno));
    return 1;
  }
  if (!all && dev.empty()) {
    // List the mounted file systems.
    if (!ReadMountTable("/proc/mounts", &mounted)) {
      std::fprintf(stderr, "mount: bad /proc/mounts: %s\n", ErrorText(errno));
      return 1;
    }
    for (const MountEntry& m : mounted) {
      if (!type.empty() && type != m.type) continue;
      std::fprintf(stdout, "%s on %s type %s (%s)\n", m.device.c_str(), m.dir.c_str(), m.type.c_str(),
                   m.opts.c_str());
    }
    std::fflush(stdout);
    return 0;
  }
  // Not root: every mount is an /etc/fstab (or, remounting, /proc/mounts)
  // entry the user may mount.
  std::vector<MountEntry> table;
  if (remount) {
    table.assign(mounted.rbegin(), mounted.rend());
  } else if (!ReadMountTable("/etc/fstab", &table)) {
    std::fprintf(stderr, "mount: bad /etc/fstab: %s\n", ErrorText(errno));
    return 1;
  }
  int status = 0;
  bool matched = false;
  for (const MountEntry& m : table) {
    if (all) {
      if (!dev.empty()) {
        if (m.dir.compare(0, dev.size(), dev) != 0 || (m.dir.size() > dev.size() && m.dir[dev.size()] != '/'))
          continue;
      } else if (HasOption(m.opts, "noauto")) {
        continue;
      }
      if (!type.empty() && type != m.type) continue;
      if (!only.empty() && !HasOption(m.opts, only)) continue;
    } else {
      if (!dir.empty() && dir != m.dir) continue;
      if (dev != m.device && (!dir.empty() || dev != m.dir)) continue;
    }
    matched = true;
    if (!fake) {
      if (!HasOption(m.opts, "user")) {
        std::fprintf(stderr, "mount: '%s' not user mountable in fstab\n", m.device.c_str());
      } else {
        // mount(2) needs CAP_SYS_ADMIN, which an app's process never has.
        std::fprintf(stderr, "mount: '%s'->'%s': %s\n", m.device.c_str(), m.dir.c_str(), ErrorText(EPERM));
      }
      status = 1;
    }
    if (!all) break;
  }
  if (!matched && !all) {
    std::fprintf(stderr, "mount: '%s' not in %s\n", (dir.empty() ? dev : dir).c_str(),
                 remount ? "/proc/mounts" : "fstab");
    return 1;
  }
  return status;
}

}  // namespace

int RunTool(const std::string& name, const std::vector<std::string>& args) {
  if (name == "toybox") {
    if (args.empty()) {
      std::fprintf(stdout, "toybox (gesher)\nls cat id ps getprop setprop logcat kill sleep pm am\n");
      std::fflush(stdout);
      return 0;
    }
    return RunTool(args[0], std::vector<std::string>(args.begin() + 1, args.end()));
  }
  if (name == "ls") return Ls(args);
  if (name == "cat") return Cat(args);
  if (name == "id") return Id(args);
  if (name == "ps") return Ps(args);
  if (name == "getprop") return Getprop(args);
  if (name == "setprop") return Setprop(args);
  if (name == "logcat") return Logcat(args);
  if (name == "kill") return Kill(args);
  if (name == "sleep") return Sleep_(args);
  if (name == "which") return Which(args);
  if (name == "mount") return Mount(args);
  if (name == "pm" || name == "am") return DeviceCommand(name, args);
  std::fprintf(stderr, "%s: not found\n", name.c_str());
  return 127;
}

}  // namespace gesher::exec

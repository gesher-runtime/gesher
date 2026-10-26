// gesher-exec --tool <name> [args]: the Android /system/bin programs apps
// really call, as toybox/AOSP provide them (docs/exec.md, ADR 0055). No root,
// so `su` is absent; these cover ls, cat, id, ps, getprop, setprop, logcat, pm,
// kill and sleep. exec_spawn.h routes a bare name or a /system/bin path here.
//
// They read and write bytes unchanged (binary stdio, set by main()).

#include "exec_spawn.h"

#include <gesher/system_properties.h>
#include <gesher/logging.h>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
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
        std::string name = Narrow(fd.cFileName);
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
      std::wstring full = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? w + L"\\" + Widen(n) : w;
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
      boot_class_path, "-Ximage:" + vm_path(image), "-Xint", "-Xusejit:false", "com.gesher.server.RuntimeCommands", tool};
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
  if (name == "pm" || name == "am") return DeviceCommand(name, args);
  std::fprintf(stderr, "%s: not found\n", name.c_str());
  return 127;
}

}  // namespace gesher::exec

// gesher-app.exe: native Windows launcher for Gesher (replaces gesher-app.ps1's
// hot path so PowerShell startup and ExecutionPolicy cannot slow down or block
// app launches, while keeping identical profile, runtime-sync, and VM behavior).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "../locale/ui_locale.h"
#include "../locale/ui_messages.h"

namespace {

constexpr wchar_t kMicrogEnv[] = L"GESHER_MICROG_SPOOF";
constexpr wchar_t kCameraRearEnv[] = L"GESHER_CAMERA_REAR_ID";
constexpr wchar_t kLaunchErrorEnv[] = L"GESHER_LAUNCH_ERROR_FILE";
constexpr wchar_t kProfileEnv[] = L"GESHER_PROFILE";
constexpr wchar_t kPortableEnv[] = L"GESHER_PORTABLE";
constexpr wchar_t kSandboxEnv[] = L"GESHER_SANDBOX";

std::wstring Lower(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& ch : out) ch = static_cast<wchar_t>(towlower(ch));
  return out;
}

std::wstring Trim(const std::wstring& s) {
  size_t a = 0;
  while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) {
    --b;
  }
  return s.substr(a, b - a);
}

std::wstring TrimTrailingSlashes(const std::wstring& s) {
  std::wstring out = s;
  while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
  return out;
}

std::wstring DirName(const std::wstring& path) {
  size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return L"";
  if (pos == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
  return path.substr(0, pos);
}

std::wstring BaseName(const std::wstring& path) {
  size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L'\\' + b;
}

std::wstring FullPath(const std::wstring& path) {
  if (path.empty()) return path;
  DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return path;
  std::vector<wchar_t> buf(needed);
  DWORD written = GetFullPathNameW(path.c_str(), needed, buf.data(), nullptr);
  if (written == 0 || written >= needed) return path;
  return std::wstring(buf.data(), written);
}

std::wstring Env(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return L"";
  std::vector<wchar_t> buf(n);
  DWORD got = GetEnvironmentVariableW(name, buf.data(), n);
  return got > 0 && got < n ? std::wstring(buf.data(), got) : L"";
}

bool FileExists(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool CreateDirRecursive(const std::wstring& path) {
  if (path.empty() || DirExists(path)) return true;
  std::wstring parent = DirName(TrimTrailingSlashes(path));
  if (!parent.empty() && parent != path && !DirExists(parent)) {
    if (!CreateDirRecursive(parent)) return false;
  }
  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS && DirExists(path);
}

// Substitutes {0}..{9} once: argument text (paths with braces) is never a template.
std::wstring FormatText(const std::wstring& pattern, const std::vector<std::wstring>& args) {
  std::wstring out;
  for (size_t i = 0; i < pattern.size();) {
    if (i + 2 < pattern.size() && pattern[i] == L'{' && pattern[i + 1] >= L'0' &&
        pattern[i + 1] <= L'9' && pattern[i + 2] == L'}' &&
        static_cast<size_t>(pattern[i + 1] - L'0') < args.size()) {
      out += args[static_cast<size_t>(pattern[i + 1] - L'0')];
      i += 3;
    } else {
      out += pattern[i++];
    }
  }
  return out;
}

void PrintWide(FILE* stream, const std::wstring& text) {
  if (text.empty()) return;
  HANDLE h = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
    DWORD written = 0;
    WriteConsoleW(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    return;
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
  if (n <= 0) return;
  std::string utf8(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), n,
                      nullptr, nullptr);
  fwrite(utf8.data(), 1, utf8.size(), stream);
}

void PrintLine(const std::wstring& text) { PrintWide(stdout, text + L"\n"); }
void PrintErrorLine(const std::wstring& text) { PrintWide(stderr, text + L"\n"); }

std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
    } else if (ch == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(ch);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
  std::wstring cmd = QuoteArg(exe);
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd += QuoteArg(a);
  }
  return cmd;
}

bool HasNonAscii(const std::wstring& s) {
  for (wchar_t ch : s) {
    if (static_cast<unsigned int>(ch) > 0x7F) return true;
  }
  return false;
}

std::wstring ResolveAsciiPath(const std::wstring& path) {
  if (path.empty() || !HasNonAscii(path)) return path;
  std::wstring native = path;
  std::replace(native.begin(), native.end(), L'/', L'\\');
  if (!(native.size() >= 3 && iswalpha(native[0]) && native[1] == L':' && native[2] == L'\\')) {
    return path;
  }
  native = FullPath(native);
  bool trailing_sep = native.size() > 3 && native.back() == L'\\';
  std::wstring probe = TrimTrailingSlashes(native);

  // Keep the leaf's long name (8.3 short names truncate extensions like .xapk -> .XAP).
  std::vector<std::wstring> trailing;
  std::wstring leaf = BaseName(probe);
  std::wstring parent = DirName(probe);
  if (!leaf.empty() && !parent.empty() && parent != probe) {
    trailing.push_back(leaf);
    probe = TrimTrailingSlashes(parent);
  }
  while (!probe.empty() &&
         (!HasNonAscii(BaseName(probe)) || (!DirExists(probe) && !FileExists(probe)))) {
    std::wstring l = BaseName(probe);
    std::wstring p = DirName(probe);
    if (p.empty() || p == probe) break;
    trailing.push_back(l);
    probe = TrimTrailingSlashes(p);
  }
  if (!DirExists(probe) && !FileExists(probe)) return path;

  DWORD needed = GetShortPathNameW(probe.c_str(), nullptr, 0);
  if (needed == 0) return path;
  std::vector<wchar_t> buf(needed);
  DWORD got = GetShortPathNameW(probe.c_str(), buf.data(), needed);
  if (got == 0 || got >= needed) return path;
  std::wstring result(buf.data(), got);
  for (auto it = trailing.rbegin(); it != trailing.rend(); ++it) {
    result = JoinPath(result, *it);
  }
  if (trailing_sep && !result.empty() && result.back() != L'\\') result.push_back(L'\\');
  return HasNonAscii(result) ? path : result;
}

std::wstring ConvertToAndroidPath(const std::wstring& path, wchar_t base_drive,
                                  bool keep_long_path) {
  if (path.empty()) return path;
  std::wstring resolved = keep_long_path ? path : ResolveAsciiPath(path);
  std::replace(resolved.begin(), resolved.end(), L'\\', L'/');
  if (resolved.size() >= 2 && iswalpha(resolved[0]) && resolved[1] == L':') {
    wchar_t drive = static_cast<wchar_t>(towlower(resolved[0]));
    std::wstring rest = resolved.substr(2);
    if (drive == static_cast<wchar_t>(towlower(base_drive))) return rest;
    return std::wstring(L"/mnt/") + drive + rest;
  }
  return resolved;
}

std::wstring PortableRoot(const std::wstring& install_dir) {
  std::wstring p = TrimTrailingSlashes(FullPath(install_dir));
  if (FileExists(JoinPath(p, L"portable.marker"))) return p;
  if (Lower(BaseName(p)) == L"app") {
    std::wstring parent = DirName(p);
    if (!parent.empty() && FileExists(JoinPath(parent, L"portable.marker"))) return parent;
  }
  return L"";
}

std::wstring SetupProfileEnv(const std::wstring& install_dir) {
  std::wstring portable = PortableRoot(install_dir);
  SetEnvironmentVariableW(kPortableEnv, portable.empty() ? L"0" : L"1");

  std::wstring profile = Env(kProfileEnv);
  if (!profile.empty()) {
    profile = FullPath(profile);
  } else if (!portable.empty()) {
    profile = JoinPath(portable, L"Data");
  } else {
    std::wstring local_app_data = Env(L"LOCALAPPDATA");
    if (local_app_data.empty()) local_app_data = JoinPath(Env(L"USERPROFILE"), L"AppData\\Local");
    profile = FullPath(JoinPath(local_app_data, L"Gesher"));
  }
  SetEnvironmentVariableW(kProfileEnv, profile.c_str());
  return profile;
}

bool ReadTextFile(const std::wstring& path, size_t max_bytes, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz = {};
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 &&
            static_cast<unsigned long long>(sz.QuadPart) <= max_bytes;
  if (ok) {
    out->resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    if (sz.QuadPart > 0) {
      ok = ReadFile(h, out->data(), static_cast<DWORD>(sz.QuadPart), &read, nullptr) &&
           read == static_cast<DWORD>(sz.QuadPart);
    }
  }
  CloseHandle(h);
  return ok;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                              nullptr);
  if (n <= 0) return "";
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

// Like gesher-app.ps1's DeviceState: `exists` says whether the file is there at all
// (an existing, even empty, file is a device to stop).
std::map<std::wstring, std::wstring> ReadDeviceState(const std::wstring& root,
                                                     bool* exists = nullptr) {
  std::map<std::wstring, std::wstring> state;
  std::string raw;
  const std::wstring file = JoinPath(root, L"data\\system\\gesher-device");
  if (exists != nullptr) *exists = FileExists(file);
  if (!ReadTextFile(file, 1024 * 1024, &raw)) return state;
  std::wstring text = Utf8ToWide(raw);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find(L'\n', pos);
    std::wstring line = nl == std::wstring::npos ? text.substr(pos) : text.substr(pos, nl - pos);
    pos = nl == std::wstring::npos ? text.size() : nl + 1;
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    std::wstring trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == L'#' || trimmed[0] == L'!') continue;
    size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) continue;
    std::wstring key = Trim(line.substr(0, eq));
    std::wstring raw_val = line.substr(eq + 1);
    std::wstring val;
    for (size_t i = 0; i < raw_val.size(); ++i) {
      if (raw_val[i] == L'\\' && i + 1 < raw_val.size()) ++i;
      val.push_back(raw_val[i]);
    }
    state[key] = val;
  }
  return state;
}

HANDLE OpenDeviceProcess(const std::map<std::wstring, std::wstring>& state,
                         const std::wstring& name, DWORD access) {
  auto it = state.find(name);
  if (it == state.end() || it->second.empty()) return nullptr;
  DWORD pid = static_cast<DWORD>(_wtoi(it->second.c_str()));
  if (pid == 0) return nullptr;
  HANDLE h = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) return nullptr;
  auto sit = state.find(name + L".start");
  if (sit != state.end() && !sit->second.empty()) {
    FILETIME created = {}, ex = {}, kern = {}, user = {};
    if (GetProcessTimes(h, &created, &ex, &kern, &user)) {
      ULARGE_INTEGER actual;
      actual.LowPart = created.dwLowDateTime;
      actual.HighPart = created.dwHighDateTime;
      unsigned long long expected = _wcstoui64(sit->second.c_str(), nullptr, 10);
      if (actual.QuadPart != expected) {
        CloseHandle(h);
        return nullptr;
      }
    }
  }
  return h;
}

int ShutdownCommand(const std::wstring& root, enum GesherUiLanguage lang) {
  bool exists = false;
  auto state = ReadDeviceState(root, &exists);
  if (!exists) {
    PrintLine(std::wstring(L"gesher-app: ") + GesherUiText(lang, "no_device"));
    return 0;
  }
  HANDLE server = OpenDeviceProcess(state, L"system_server", SYNCHRONIZE | PROCESS_TERMINATE);
  if (server != nullptr) {
    auto ev_it = state.find(L"shutdown_event");
    if (ev_it != state.end() && !ev_it->second.empty()) {
      HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, ev_it->second.c_str());
      if (ev != nullptr) {
        SetEvent(ev);
        CloseHandle(ev);
      }
    }
    WaitForSingleObject(server, 30000);
    CloseHandle(server);
  }
  for (const wchar_t* name : {L"system_server", L"servicemanager", L"binderd"}) {
    HANDLE p = OpenDeviceProcess(state, name, PROCESS_TERMINATE);
    if (p != nullptr) {
      TerminateProcess(p, 1);
      CloseHandle(p);
    }
  }
  DeleteFileW(JoinPath(root, L"data\\system\\gesher-device").c_str());
  DeleteFileW(JoinPath(root, L"data\\system\\gesher-device.ready").c_str());
  auto dev_it = state.find(L"device");
  std::wstring dev = dev_it != state.end() ? dev_it->second : L"";
  PrintLine(L"gesher-app: " + FormatText(GesherUiText(lang, "device_stopped"), {dev}));
  return 0;
}

// --- Runtime file sync (matches tools/package/gesher-runtime-files.ps1) ---

struct RuntimeSyncState {
  std::string generation;
  std::wstring stamp_path;
  bool force_refresh = false;
};

bool IsValidVersion(const std::string& v) {
  if (v.empty()) return false;
  int parts = 0;
  size_t pos = 0;
  while (pos <= v.size()) {
    size_t dot = v.find('.', pos);
    std::string part = dot == std::string::npos ? v.substr(pos) : v.substr(pos, dot - pos);
    if (part.empty() || part.size() > 5) return false;
    if (part.size() > 1 && part[0] == '0') return false;
    int val = 0;
    for (char c : part) {
      if (c < '0' || c > '9') return false;
      val = val * 10 + (c - '0');
    }
    if (val > 65535) return false;
    if (parts == 3 && val == 0) return false;
    ++parts;
    if (dot == std::string::npos) break;
    pos = dot + 1;
  }
  return parts == 3 || parts == 4;
}

RuntimeSyncState InitRuntimeSync(const std::wstring& install_dir,
                                 const std::wstring& profile_dir) {
  RuntimeSyncState state;
  state.stamp_path = JoinPath(profile_dir, L"system\\.gesher-runtime-app.version");
  std::string ini;
  if (ReadTextFile(JoinPath(install_dir, L"gesher-version.ini"), 4096, &ini)) {
    // Expect: [Gesher]\r?\nVersion=X.Y.Z(.N)
    size_t p = 0;
    while (p < ini.size() && (ini[p] == ' ' || ini[p] == '\t' || ini[p] == '\r' || ini[p] == '\n'))
      ++p;
    if (ini.compare(p, 8, "[Gesher]") == 0) {
      p += 8;
      while (p < ini.size() && (ini[p] == ' ' || ini[p] == '\t')) ++p;
      if (p < ini.size() && ini[p] == '\r') ++p;
      if (p < ini.size() && ini[p] == '\n') {
        ++p;
        if (ini.compare(p, 8, "Version=") == 0) {
          std::string ver = ini.substr(p + 8);
          while (!ver.empty() &&
                 (ver.back() == ' ' || ver.back() == '\t' || ver.back() == '\r' ||
                  ver.back() == '\n')) {
            ver.pop_back();
          }
          if (IsValidVersion(ver)) {
            state.generation = WideToUtf8(FullPath(install_dir)) + "\n" + ver;
          }
        }
      }
    }
  }
  if (!state.generation.empty()) {
    std::string prev;
    if (!ReadTextFile(state.stamp_path, 8192, &prev) || prev != state.generation) {
      state.force_refresh = true;
    }
  }
  return state;
}

bool FileBytesEqual(const std::wstring& src, const std::wstring& dst,
                    unsigned long long size) {
  if (size == 0) return true;
  HANDLE h1 = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h1 == INVALID_HANDLE_VALUE) return false;
  HANDLE h2 = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h2 == INVALID_HANDLE_VALUE) {
    CloseHandle(h1);
    return false;
  }
  constexpr size_t kBuf = 262144;
  size_t cap = size < kBuf ? static_cast<size_t>(size) : kBuf;
  std::vector<char> b1(cap), b2(cap);
  bool same = true;
  for (;;) {
    DWORD r1 = 0, r2 = 0;
    if (!ReadFile(h1, b1.data(), static_cast<DWORD>(cap), &r1, nullptr) ||
        !ReadFile(h2, b2.data(), static_cast<DWORD>(cap), &r2, nullptr) || r1 != r2) {
      same = false;
      break;
    }
    if (r1 == 0) break;
    if (std::memcmp(b1.data(), b2.data(), r1) != 0) {
      same = false;
      break;
    }
  }
  CloseHandle(h2);
  CloseHandle(h1);
  return same;
}

bool SyncRuntimeFile(const std::wstring& src, const std::wstring& dst,
                     const RuntimeSyncState& state, std::wstring* error) {
  WIN32_FILE_ATTRIBUTE_DATA s_attr = {};
  if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &s_attr) ||
      (s_attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    *error = L"Expected an existing runtime file: " + src;
    return false;
  }
  unsigned long long s_size = (static_cast<unsigned long long>(s_attr.nFileSizeHigh) << 32) |
                              s_attr.nFileSizeLow;
  bool need_copy = false;
  WIN32_FILE_ATTRIBUTE_DATA d_attr = {};
  if (!GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &d_attr) ||
      (d_attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    need_copy = true;
  } else {
    unsigned long long d_size = (static_cast<unsigned long long>(d_attr.nFileSizeHigh) << 32) |
                                d_attr.nFileSizeLow;
    bool same_mtime =
        s_attr.ftLastWriteTime.dwLowDateTime == d_attr.ftLastWriteTime.dwLowDateTime &&
        s_attr.ftLastWriteTime.dwHighDateTime == d_attr.ftLastWriteTime.dwHighDateTime;
    if (s_size != d_size) {
      need_copy = true;
    } else if (state.force_refresh || state.generation.empty() || !same_mtime) {
      need_copy = !FileBytesEqual(src, dst, s_size);
      if (!need_copy && !same_mtime) {
        HANDLE h = CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
          SetFileTime(h, nullptr, nullptr, &s_attr.ftLastWriteTime);
          CloseHandle(h);
        }
      }
    }
  }
  if (need_copy) {
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
      *error = L"Failed to copy runtime file " + src + L" -> " + dst + L" (error " +
               std::to_wstring(GetLastError()) + L")";
      return false;
    }
  }
  return true;
}

void CompleteRuntimeSync(const RuntimeSyncState& state) {
  if (state.generation.empty() || !state.force_refresh) return;
  std::wstring parent = DirName(state.stamp_path);
  CreateDirRecursive(parent);
  std::wstring tmp = JoinPath(parent, L"gesher-runtime-stamp-" +
                                          std::to_wstring(GetCurrentProcessId()) + L".tmp");
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  BOOL ok = WriteFile(h, state.generation.data(), static_cast<DWORD>(state.generation.size()),
                      &written, nullptr) &&
            written == state.generation.size();
  CloseHandle(h);
  if (ok) {
    MoveFileExW(tmp.c_str(), state.stamp_path.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  DeleteFileW(tmp.c_str());
}

bool SyncAllRuntimeFiles(const std::wstring& here, const std::wstring& root,
                         std::wstring* error) {
  for (const wchar_t* d : {L"system\\framework", L"art", L"data", L"i18n\\etc\\icu", L"tzdata"}) {
    CreateDirRecursive(JoinPath(root, d));
  }
  RuntimeSyncState state = InitRuntimeSync(here, root);

  // icudt*l.dat
  WIN32_FIND_DATAW fd = {};
  HANDLE f = FindFirstFileW(JoinPath(here, L"icudt*l.dat").c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) {
    *error = L"Missing icudt*l.dat in " + here;
    return false;
  }
  std::wstring icu_name = fd.cFileName;
  FindClose(f);
  if (!SyncRuntimeFile(JoinPath(here, icu_name), JoinPath(root, L"i18n\\etc\\icu\\" + icu_name),
                       state, error))
    return false;
  if (!SyncRuntimeFile(JoinPath(here, L"framework-res.apk"),
                       JoinPath(root, L"system\\framework\\framework-res.apk"), state, error))
    return false;
  if (!SyncRuntimeFile(JoinPath(here, L"build.prop"), JoinPath(root, L"system\\build.prop"), state,
                       error))
    return false;

  auto sync_optional_lib = [&](const std::wstring& jar, const std::wstring& xml) -> bool {
    if (!FileExists(JoinPath(here, jar))) return true;
    CreateDirRecursive(JoinPath(root, L"system\\etc\\permissions"));
    return SyncRuntimeFile(JoinPath(here, jar), JoinPath(root, L"system\\framework\\" + jar),
                           state, error) &&
           SyncRuntimeFile(JoinPath(here, xml), JoinPath(root, L"system\\" + xml), state, error);
  };
  if (!sync_optional_lib(L"org.apache.http.legacy.jar",
                         L"etc\\permissions\\org.apache.http.legacy.xml"))
    return false;
  if (!sync_optional_lib(L"com.android.location.provider.jar",
                         L"etc\\permissions\\com.android.location.provider.xml"))
    return false;
  for (const wchar_t* lib : {L"android.test.base", L"android.test.mock", L"android.test.runner"}) {
    std::wstring s = lib;
    if (!sync_optional_lib(s + L".jar", L"etc\\permissions\\" + s + L".xml")) return false;
  }
  if (FileExists(JoinPath(here, L"etc\\permissions\\privapp-permissions-microg.xml"))) {
    CreateDirRecursive(JoinPath(root, L"system\\etc\\permissions"));
    if (!SyncRuntimeFile(JoinPath(here, L"etc\\permissions\\privapp-permissions-microg.xml"),
                         JoinPath(root, L"system\\etc\\permissions\\privapp-permissions-microg.xml"),
                         state, error))
      return false;
  }

  if (FileExists(JoinPath(here, L"fonts\\fonts.xml"))) {
    CreateDirRecursive(JoinPath(root, L"system\\fonts"));
    CreateDirRecursive(JoinPath(root, L"system\\etc"));
    if (!SyncRuntimeFile(JoinPath(here, L"fonts\\fonts.xml"),
                         JoinPath(root, L"system\\etc\\fonts.xml"), state, error))
      return false;
    if (FileExists(JoinPath(here, L"fonts\\windows-fonts.xml"))) {
      if (!SyncRuntimeFile(JoinPath(here, L"fonts\\windows-fonts.xml"),
                           JoinPath(root, L"system\\etc\\windows-fonts.xml"), state, error))
        return false;
    }
    HANDLE ff = FindFirstFileW(JoinPath(here, L"fonts\\*.ttf").c_str(), &fd);
    if (ff != INVALID_HANDLE_VALUE) {
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          if (!SyncRuntimeFile(JoinPath(here, L"fonts\\" + std::wstring(fd.cFileName)),
                               JoinPath(root, L"system\\fonts\\" + std::wstring(fd.cFileName)),
                               state, error)) {
            FindClose(ff);
            return false;
          }
        }
      } while (FindNextFileW(ff, &fd));
      FindClose(ff);
    }
  }

  std::wstring audio_root = JoinPath(here, L"media\\audio");
  if (DirExists(audio_root)) {
    std::vector<std::wstring> stack = {L"audio"};
    while (!stack.empty()) {
      std::wstring rel_dir = stack.back();
      stack.pop_back();
      HANDLE mf = FindFirstFileW(JoinPath(here, L"media\\" + rel_dir + L"\\*").c_str(), &fd);
      if (mf == INVALID_HANDLE_VALUE) continue;
      do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring rel = rel_dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          stack.push_back(rel);
        } else {
          std::wstring dst = JoinPath(root, L"system\\media\\" + rel);
          CreateDirRecursive(DirName(dst));
          if (!SyncRuntimeFile(JoinPath(here, L"media\\" + rel), dst, state, error)) {
            FindClose(mf);
            return false;
          }
        }
      } while (FindNextFileW(mf, &fd));
      FindClose(mf);
    }
  }

  CompleteRuntimeSync(state);
  return true;
}

// Whether `path` contains `needle` (read in chunks: the log may be large).
bool FileContains(const std::wstring& path, const std::string& needle) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::string window;
  std::vector<char> buf(65536);
  bool found = false;
  DWORD got = 0;
  while (!found && ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) &&
         got > 0) {
    window.append(buf.data(), got);
    found = window.find(needle) != std::string::npos;
    if (window.size() > needle.size()) window.erase(0, window.size() - needle.size());
  }
  CloseHandle(h);
  return found;
}

int RunProcessWait(const std::wstring& exe, const std::vector<std::wstring>& args) {
  std::wstring cmd = BuildCommandLine(exe, args);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                      &pi)) {
    PrintErrorLine(L"gesher-app: could not start " + exe + L" (error " +
                   std::to_wstring(GetLastError()) + L")");
    return 1;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

HANDLE StartProcessRedirected(const std::wstring& exe, const std::vector<std::wstring>& args,
                              const std::wstring& out_path, const std::wstring& err_path,
                              DWORD* pid_out) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE out_h = CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_h = CreateFileW(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out_h != INVALID_HANDLE_VALUE ? out_h : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = err_h != INVALID_HANDLE_VALUE ? err_h : GetStdHandle(STD_ERROR_HANDLE);
  std::wstring cmd = BuildCommandLine(exe, args);
  PROCESS_INFORMATION pi = {};
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi);
  if (out_h != INVALID_HANDLE_VALUE) CloseHandle(out_h);
  if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
  if (!ok) return nullptr;
  CloseHandle(pi.hThread);
  if (pid_out) *pid_out = pi.dwProcessId;
  return pi.hProcess;
}

// PowerShell's -contains: case-insensitive.
bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
  for (const auto& a : args) {
    if (_wcsicmp(a.c_str(), flag) == 0) return true;
  }
  return false;
}

bool IsKnownCommand(const std::wstring& cmd) {
  static const wchar_t* kCommands[] = {
      L"install",         L"uninstall",      L"view",         L"list",
      L"status",          L"start",          L"shutdown",     L"launch",
      L"toast-activated", L"apk-icon",       L"apk-info",     L"proxy",
      L"windows",         L"special-access", L"accounts",     L"device-admin",
      L"settings-closed", L"settings-shown", L"sync-shortcuts", L"cameras",
      L"spell-checker",   L"mobile-data"};
  for (const wchar_t* c : kCommands) {
    if (cmd == c) return true;
  }
  return false;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  SetEnvironmentVariableW(kSandboxEnv, nullptr);

  std::vector<wchar_t> exe_buf(MAX_PATH);
  DWORD exe_len = 0;
  for (;;) {
    exe_len = GetModuleFileNameW(nullptr, exe_buf.data(), static_cast<DWORD>(exe_buf.size()));
    if (exe_len == 0 || exe_len < exe_buf.size() || exe_buf.size() >= 32768) break;
    exe_buf.resize(exe_buf.size() * 2);
  }
  if (exe_len == 0 || exe_len >= exe_buf.size()) {
    PrintErrorLine(L"gesher-app: could not locate gesher-app.exe");
    return 1;
  }
  std::wstring here = DirName(std::wstring(exe_buf.data(), exe_len));
  enum GesherUiLanguage lang = GesherUiCurrentLanguage();

  std::wstring root = SetupProfileEnv(here);
  bool enable_microg = Env(kMicrogEnv).empty() && FileExists(JoinPath(root, L"microg.enabled"));

  if (argc <= 1) {
    PrintLine(std::wstring(GesherUiText(lang, "usage")) +
              L"gesher-app APK [--window | --hidden-window | --headless [--keep-task]] "
              L"[--activity CLASS] [--ez KEY true|false]... [--proxy host:port|direct|windows] "
              L"[--detach | --wait | --single-process | --system-server]");
    PrintLine(L"       gesher-app launch PACKAGE|APK [options] | install APK [--desktop-shortcut] "
              L"| uninstall PACKAGE | view PACKAGE URL | accounts [list | remove TYPE NAME] | list | "
              L"status | start | shutdown | "
              L"proxy [[PACKAGE] host:port|direct|windows] | windows PACKAGE separate|shared");
    PrintLine(L"       gesher-app spell-checker list|use COMPONENT|automatic|enable|disable|language ID");
    PrintLine(L"       gesher-app cameras [rear CAMERA_ID | reset]");
    PrintLine(L"       gesher-app mobile-data [on|off] | mobile-data roaming [on|off]");
    return 2;
  }

  std::wstring command = argv[1];
  std::vector<std::wstring> rest;
  for (int i = 2; i < argc; ++i) rest.emplace_back(argv[i]);

  std::wstring cmd_lower = Lower(command);
  if (cmd_lower == L"control-panel" || cmd_lower == L"settings" || cmd_lower == L"dashboard" ||
      cmd_lower == L"gui") {
    wchar_t sys[MAX_PATH];
    UINT s = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ps = (s > 0 && s < MAX_PATH ? std::wstring(sys) : L"C:\\Windows\\System32") +
                      L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::vector<std::wstring> ps_args = {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File",
                                         JoinPath(here, L"gesher-control-panel.ps1")};
    ps_args.insert(ps_args.end(), rest.begin(), rest.end());
    return RunProcessWait(ps, ps_args);
  }

  if (cmd_lower == L"shutdown") return ShutdownCommand(root, lang);

  // As gesher-app.ps1: the first two characters (a drive, or \\ of a UNC path) must match.
  if (root.size() < 2 || here.size() < 2 ||
      _wcsicmp(root.substr(0, 2).c_str(), here.substr(0, 2).c_str()) != 0) {
    PrintErrorLine(FormatText(GesherUiText(lang, "same_drive"), {root, here.substr(0, 2)}));
    return 1;
  }

  std::wstring apk_full;
  if (!IsKnownCommand(cmd_lower)) {
    if (!FileExists(command)) {
      PrintErrorLine(L"gesher-app: file not found: " + command);
      return 1;
    }
    apk_full = FullPath(command);
  } else if (!rest.empty() &&
             (cmd_lower == L"install" || cmd_lower == L"apk-icon" || cmd_lower == L"apk-info" ||
              (cmd_lower == L"launch" && FileExists(rest[0])))) {
    if (!FileExists(rest[0])) {
      PrintErrorLine(L"gesher-app: file not found: " + rest[0]);
      return 1;
    }
    apk_full = FullPath(rest[0]);
    rest.erase(rest.begin());
  }

  std::wstring apk_info_out;
  std::wstring apk_snapshot;
  if (cmd_lower == L"apk-info") {
    if (apk_full.empty() || rest.size() < 1 || rest.size() > 2) {
      PrintLine(std::wstring(GesherUiText(lang, "usage")) +
                L"gesher-app apk-info APK OUT [SNAPSHOT_DIR]");
      return 2;
    }
    apk_info_out = FullPath(rest[0]);
    if (rest.size() == 2) {
      if (!DirExists(rest[1])) {
        PrintErrorLine(L"gesher-app: directory not found: " + rest[1]);
        return 1;
      }
      apk_snapshot = FullPath(rest[1]);
    }
    rest.clear();
  }

  SetCurrentDirectoryW(here.c_str());
  wchar_t base_drive = here.size() >= 2 && here[1] == L':' ? here[0] : L'C';
  auto no_drive = [&](const std::wstring& p) { return ConvertToAndroidPath(p, base_drive, false); };
  auto user_path = [&](const std::wstring& p) { return ConvertToAndroidPath(p, base_drive, true); };

  std::wstring sync_error;
  if (!SyncAllRuntimeFiles(here, root, &sync_error)) {
    PrintErrorLine(L"gesher-app: " + sync_error);
    return 1;
  }

  SetEnvironmentVariableW(L"ANDROID_ROOT", no_drive(JoinPath(root, L"system")).c_str());
  SetEnvironmentVariableW(L"ANDROID_ART_ROOT", no_drive(JoinPath(root, L"art")).c_str());
  SetEnvironmentVariableW(L"ANDROID_DATA", no_drive(JoinPath(root, L"data")).c_str());
  SetEnvironmentVariableW(L"ANDROID_I18N_ROOT", no_drive(JoinPath(root, L"i18n")).c_str());
  SetEnvironmentVariableW(L"ANDROID_TZDATA_ROOT", no_drive(JoinPath(here, L"tzdata")).c_str());

  std::wstring bcp;
  for (const wchar_t* jar : {L"core-oj", L"core-libart", L"core-icu4j", L"framework", L"gesher-app"}) {
    if (!bcp.empty()) bcp.push_back(L':');
    bcp += no_drive(JoinPath(here, std::wstring(jar) + L".jar"));
  }
  std::vector<std::wstring> dvm = {
      L"-Xbootclasspath:" + bcp,
      L"-Ximage:" + no_drive(JoinPath(here, L"framework\\boot.art")),
      L"-Xusejit:false",
  };
  std::wstring dalvikvm = JoinPath(here, L"dalvikvm.exe");

  if (enable_microg) SetEnvironmentVariableW(kMicrogEnv, L"1");
  std::wstring rear_camera_file = JoinPath(root, L"camera-rear");
  if (Env(kCameraRearEnv).empty()) {
    std::string raw_cam;
    if (ReadTextFile(rear_camera_file, 65536, &raw_cam)) {
      if (raw_cam.compare(0, 3, "\xEF\xBB\xBF") == 0) raw_cam.erase(0, 3);
      std::wstring cam = Trim(Utf8ToWide(raw_cam));
      if (!cam.empty()) SetEnvironmentVariableW(kCameraRearEnv, cam.c_str());
    }
  }

  std::wstring prev_launch_error = Env(kLaunchErrorEnv);
  SetEnvironmentVariableW(kLaunchErrorEnv, nullptr);

  if (cmd_lower == L"cameras") {
    std::vector<std::wstring> args = dvm;
    args.push_back(L"com.gesher.server.CameraSelection");
    args.push_back(no_drive(rear_camera_file));
    args.insert(args.end(), rest.begin(), rest.end());
    int code = RunProcessWait(dalvikvm, args);
    if (code == 0 && !rest.empty()) {
      auto state = ReadDeviceState(root);
      HANDLE p = OpenDeviceProcess(state, L"system_server", SYNCHRONIZE);
      if (p != nullptr) {
        CloseHandle(p);
        PrintLine(GesherUiText(lang, "camera_selection_restart"));
      }
    }
    return code;
  }

  auto build_launcher_args = [&](const std::wstring& subcmd) {
    std::vector<std::wstring> args = dvm;
    args.push_back(L"com.gesher.app.Launcher");
    if (!prev_launch_error.empty()) {
      args.push_back(L"--launch-error-file");
      args.push_back(prev_launch_error);
    }
    args.push_back(subcmd);
    if (!apk_full.empty()) args.push_back(user_path(apk_full));
    if (!apk_info_out.empty()) args.push_back(user_path(apk_info_out));
    if (!apk_snapshot.empty()) args.push_back(user_path(apk_snapshot));
    args.insert(args.end(), rest.begin(), rest.end());
    return args;
  };

  if (IsKnownCommand(cmd_lower)) {
    return RunProcessWait(dalvikvm, build_launcher_args(cmd_lower));
  }

  bool has_single_process = HasFlag(rest, L"--single-process");
  bool has_system_server = HasFlag(rest, L"--system-server");
  if (!has_single_process && !has_system_server) {
    return RunProcessWait(dalvikvm, build_launcher_args(L"launch"));
  }

  std::vector<std::wstring> driver_args;
  for (const auto& a : rest) {
    if (_wcsicmp(a.c_str(), L"--single-process") != 0) driver_args.push_back(a);
  }
  if (!has_system_server) {
    std::vector<std::wstring> args = dvm;
    args.push_back(L"com.gesher.app.Main");
    args.insert(args.end(), driver_args.begin(), driver_args.end());
    args.push_back(user_path(apk_full));
    return RunProcessWait(dalvikvm, args);
  }

  // --system-server: ephemeral device for this single run
  std::wstring logs = JoinPath(root, L"logs");
  CreateDirRecursive(logs);
  // Unique per run, not only per PID (as gesher-app.ps1): wall-clock milliseconds, hex.
  FILETIME now_ft = {};
  GetSystemTimeAsFileTime(&now_ft);
  ULARGE_INTEGER now;
  now.LowPart = now_ft.dwLowDateTime;
  now.HighPart = now_ft.dwHighDateTime;
  wchar_t stamp[32];
  swprintf(stamp, 32, L"%llx", (now.QuadPart - 116444736000000000ULL) / 10000ULL);
  std::wstring binder =
      L"gesher-app-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(stamp);
  SetEnvironmentVariableW(L"GESHER_BINDER", binder.c_str());

  // The run's device processes have their own (hidden) consoles: Ctrl+C or closing this
  // console ends this process without cleanup, so the job ends them with it.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  std::vector<HANDLE> started;
  auto track = [&](HANDLE h) {
    if (job != nullptr) AssignProcessToJobObject(job, h);
    started.push_back(h);
  };
  auto cleanup = [&]() {
    for (HANDLE h : started) {
      if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) TerminateProcess(h, 1);
      CloseHandle(h);
    }
    SetEnvironmentVariableW(L"GESHER_BINDER", nullptr);
    if (job != nullptr) CloseHandle(job);
  };
  auto start_failed = [&](const std::wstring& exe) {
    PrintErrorLine(L"gesher-app: could not start " + exe + L" (error " +
                   std::to_wstring(GetLastError()) + L")");
    cleanup();
    return 1;
  };

  DWORD binder_pid = 0;
  HANDLE h_binder = StartProcessRedirected(JoinPath(here, L"binderd.exe"), {binder},
                                           JoinPath(logs, L"run-binderd.out"),
                                           JoinPath(logs, L"run-binderd.log"), &binder_pid);
  if (h_binder == nullptr) return start_failed(JoinPath(here, L"binderd.exe"));
  track(h_binder);

  std::vector<std::wstring> wait_args = dvm;
  wait_args.push_back(L"com.gesher.server.RuntimeCommands");
  wait_args.push_back(L"wait-binder");
  wait_args.push_back(binder);
  wait_args.push_back(std::to_wstring(binder_pid));
  wait_args.push_back(L"15000");
  if (RunProcessWait(dalvikvm, wait_args) != 0) {
    PrintErrorLine(std::wstring(L"gesher-app: ") + GesherUiText(lang, "binder_failed"));
    cleanup();
    return 1;
  }

  HANDLE h_sm = StartProcessRedirected(JoinPath(here, L"servicemanager.exe"), {},
                                       JoinPath(logs, L"run-servicemanager.out"),
                                       JoinPath(logs, L"run-servicemanager.log"), nullptr);
  if (h_sm == nullptr) return start_failed(JoinPath(here, L"servicemanager.exe"));
  track(h_sm);

  std::vector<std::wstring> srv_args = dvm;
  srv_args.push_back(L"com.gesher.server.SystemServer");
  srv_args.push_back(user_path(apk_full));
  std::wstring srv_out = JoinPath(logs, L"run-system_server.out");
  std::wstring srv_log = JoinPath(logs, L"run-system_server.log");
  HANDLE h_srv = StartProcessRedirected(dalvikvm, srv_args, srv_out, srv_log, nullptr);
  if (h_srv == nullptr) return start_failed(dalvikvm);
  track(h_srv);
  {
    for (int i = 0; i < 900 && WaitForSingleObject(h_srv, 0) == WAIT_TIMEOUT; ++i) {
      if (FileContains(srv_log, "with servicemanager") ||
          FileContains(srv_out, "with servicemanager")) {
        break;
      }
      Sleep(100);
    }
  }

  std::vector<std::wstring> app_args = dvm;
  app_args.push_back(L"com.gesher.app.Main");
  app_args.insert(app_args.end(), driver_args.begin(), driver_args.end());
  app_args.push_back(user_path(apk_full));
  int code = RunProcessWait(dalvikvm, app_args);
  if (code != 0) {
    PrintLine(L"gesher-app: " + FormatText(GesherUiText(lang, "server_logs"), {logs}));
  }
  cleanup();
  return code;
}

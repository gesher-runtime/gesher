// gesher-base: external storage policy enforcement (scoped storage,
// READ_MEDIA_*, READ/WRITE_EXTERNAL_STORAGE, and MANAGE_EXTERNAL_STORAGE).

#include "storage_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal.h"

namespace gesher {
namespace {

enum class StorageMode {
  kScoped,
  kMediaRo,
  kLegacyRo,
  kLegacyRw,
  kManager,
};

struct PolicyCache {
  std::mutex mu;
  bool valid = false;
  std::wstring policy_path;
  std::string app_pkg;
  DWORD mtime_low = 0;
  DWORD mtime_high = 0;
  DWORD size_low = 0;
  DWORD size_high = 0;
  StorageMode mode = StorageMode::kScoped;
  int media_mask = 0;
};

PolicyCache& GetPolicyCache() {
  static PolicyCache* cache = new PolicyCache();
  return *cache;
}

bool IsDriveLetterChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsDriveLetterW(wchar_t c) {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

std::wstring EnvDirToWindows(const char* env_val) {
  if (env_val == nullptr || env_val[0] == '\0') return {};
  if (strncmp(env_val, "/mnt/", 5) == 0 && IsDriveLetterChar(env_val[5]) &&
      (env_val[6] == '\0' || env_val[6] == '/')) {
    std::wstring rest = Widen(env_val + 6);
    for (wchar_t& c : rest) {
      if (c == L'/') c = L'\\';
    }
    return std::wstring(1, static_cast<wchar_t>(towupper(env_val[5]))) + L":" +
           (rest.empty() ? L"\\" : rest);
  }
  std::wstring out = Widen(env_val);
  for (wchar_t& c : out) {
    if (c == L'/') c = L'\\';
  }
  return out;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

bool StartsWithIgnoreCase(std::wstring_view s, std::wstring_view prefix) {
  return s.size() >= prefix.size() && _wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
}

bool IsUnsafeDeviceOrLoopbackPath(std::wstring_view p) {
  if (StartsWithIgnoreCase(p, L"\\\\?\\GLOBALROOT") ||
      StartsWithIgnoreCase(p, L"\\??\\GLOBALROOT") ||
      StartsWithIgnoreCase(p, L"\\\\.\\") ||
      StartsWithIgnoreCase(p, L"\\??\\")) {
    return true;
  }
  if (StartsWithIgnoreCase(p, L"\\\\localhost\\") ||
      StartsWithIgnoreCase(p, L"\\\\127.0.0.1\\") ||
      StartsWithIgnoreCase(p, L"\\\\[::1]\\")) {
    return true;
  }
  return false;
}

std::wstring ToWin32OpenPath(const std::wstring& p) {
  if (p.size() < MAX_PATH - 12 || p.rfind(L"\\\\?\\", 0) == 0) return p;
  if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
  if (p.size() >= 3 && IsDriveLetterW(p[0]) && p[1] == L':' && p[2] == L'\\') {
    return L"\\\\?\\" + p;
  }
  return p;
}

std::wstring LexicalCanonicalize(std::wstring p) {
  if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    p = L"\\\\" + p.substr(8);
  } else if (p.rfind(L"\\\\?\\", 0) == 0) {
    p = p.substr(4);
  }
  for (wchar_t& c : p) {
    if (c == L'/') c = L'\\';
  }
  const bool is_drive_abs =
      p.size() >= 3 && IsDriveLetterW(p[0]) && p[1] == L':' && p[2] == L'\\';
  const bool is_unc = p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\';
  if (!is_drive_abs && !is_unc) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n > 0) {
      std::wstring full(n, L'\0');
      DWORD written = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
      if (written > 0 && written < n) {
        full.resize(written);
        p = std::move(full);
      }
    }
  }

  std::wstring root;
  size_t pos = 0;
  if (p.size() >= 2 && IsDriveLetterW(p[0]) && p[1] == L':') {
    root.push_back(static_cast<wchar_t>(towupper(p[0])));
    root += L":\\";
    pos = (p.size() >= 3 && p[2] == L'\\') ? 3 : 2;
  } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
    size_t s1 = p.find(L'\\', 2);
    if (s1 != std::wstring::npos) {
      size_t s2 = p.find(L'\\', s1 + 1);
      if (s2 == std::wstring::npos) {
        root = p + L'\\';
        pos = p.size();
      } else {
        root = p.substr(0, s2 + 1);
        pos = s2 + 1;
      }
    } else {
      root = L"\\\\";
      pos = 2;
    }
  }

  std::vector<std::wstring_view> parts;
  std::wstring_view view(p);
  while (pos < view.size()) {
    size_t next = view.find(L'\\', pos);
    std::wstring_view part =
        next == std::wstring_view::npos ? view.substr(pos) : view.substr(pos, next - pos);
    pos = next == std::wstring_view::npos ? view.size() : next + 1;
    if (part.empty() || part == L".") continue;
    if (part == L"..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }

  std::wstring out = root;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.push_back(L'\\');
    out.append(parts[i]);
  }
  if (out.size() > 3 && out.back() == L'\\') out.pop_back();
  return out;
}

std::wstring CanonicalizeWindowsPath(std::wstring_view input) {
  if (input.empty()) return {};
  if (IsUnsafeDeviceOrLoopbackPath(input)) return {};
  std::wstring lex = LexicalCanonicalize(std::wstring(input));
  if (lex.empty() || IsUnsafeDeviceOrLoopbackPath(lex)) return {};

  // Resolve the longest existing ancestor via GetFinalPathNameByHandleW to defeat
  // symlinks, NTFS junctions, and 8.3 short names.
  std::wstring cur = lex;
  std::vector<std::wstring> tail;
  for (;;) {
    std::wstring open_path = ToWin32OpenPath(cur);
    HANDLE h = CreateFileW(open_path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      wchar_t final_buf[32768];
      DWORD n = GetFinalPathNameByHandleW(h, final_buf, 32768,
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      CloseHandle(h);
      if (n > 0 && n < 32768) {
        std::wstring resolved(final_buf, n);
        for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
          if (resolved.back() != L'\\') resolved.push_back(L'\\');
          resolved.append(*it);
        }
        return LexicalCanonicalize(std::move(resolved));
      }
      break;
    }
    size_t slash = cur.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash <= 2) break;
    tail.push_back(cur.substr(slash + 1));
    cur.resize(slash);
  }
  return lex;
}

struct ProcessIdentity {
  std::string package;
  std::string uid;
  std::wstring data_root;
  std::wstring media_root;
};

extern "C" __declspec(dllexport) ProcessIdentity* gesher_storage_policy_owner_v1() {
  // The launcher calls this before entering guest code. Never consult mutable
  // process environment again to decide an application's authorization.
  static ProcessIdentity* identity = [] {
    auto* value = new ProcessIdentity();
    const char* package = getenv("GESHER_APP_PACKAGE");
    if (package) value->package = package;
    const char* uid = getenv("GESHER_UID");
    if (uid) value->uid = uid;
    value->data_root = EnvDirToWindows(getenv("ANDROID_DATA"));
    if (!value->data_root.empty())
      value->media_root = CanonicalizeWindowsPath(value->data_root + L"\\media\\0");
    return value;
  }();
  return identity;
}

const ProcessIdentity& Identity() {
  using Resolve = ProcessIdentity* (*)();
  static const auto resolve = [] {
    auto shared = reinterpret_cast<Resolve>(GetProcAddress(GetModuleHandleW(nullptr),
                                                            "gesher_storage_policy_owner_v1"));
    return shared ? shared : gesher_storage_policy_owner_v1;
  }();
  return *resolve();
}

bool ParsePolicyContent(std::string_view content, std::string_view expected_pkg,
                        std::string_view expected_uid, StorageMode* out_mode, int* out_media_mask) {
  std::string_view version;
  std::string_view pkg;
  std::string_view uid;
  std::string_view mode_str;
  int media_mask = 0;
  bool has_media_mask = false;

  size_t pos = 0;
  while (pos < content.size()) {
    size_t nl = content.find('\n', pos);
    std::string_view line =
        nl == std::string_view::npos ? content.substr(pos) : content.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? content.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = line.substr(0, eq);
    std::string_view val = line.substr(eq + 1);
    if (key == "v") {
      version = val;
    } else if (key == "pkg") {
      pkg = val;
    } else if (key == "uid") {
      uid = val;
    } else if (key == "mode") {
      mode_str = val;
    } else if (key == "media_mask") {
      if (has_media_mask) return false;
      has_media_mask = true;
      std::string tmp(val);
      char* end = nullptr;
      long parsed = strtol(tmp.c_str(), &end, 10);
      if (end != tmp.c_str() && *end == '\0' && parsed >= 0 && parsed <= 7) {
        media_mask = static_cast<int>(parsed);
      } else return false;
    }
  }

  if (version != "1" || pkg != expected_pkg || expected_uid.empty() || uid != expected_uid ||
      !has_media_mask)
    return false;

  StorageMode mode;
  if (mode_str == "MANAGER") {
    mode = StorageMode::kManager;
  } else if (mode_str == "LEGACY_RW") {
    mode = StorageMode::kLegacyRw;
  } else if (mode_str == "LEGACY_RO") {
    mode = StorageMode::kLegacyRo;
  } else if (mode_str == "MEDIA_RO") {
    mode = StorageMode::kMediaRo;
  } else if (mode_str == "SCOPED") {
    mode = StorageMode::kScoped;
  } else {
    return false;
  }

  *out_mode = mode;
  *out_media_mask = media_mask;
  return true;
}

void LoadPolicy(const std::wstring& policy_path, std::string_view app_pkg, std::string_view app_uid,
                StorageMode* out_mode, int* out_media_mask) {
  PolicyCache& cache = GetPolicyCache();
  std::lock_guard<std::mutex> lock(cache.mu);

  HANDLE h = CreateFileW(policy_path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    cache.valid = false;
    *out_mode = StorageMode::kScoped;
    *out_media_mask = 0;
    return;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    CloseHandle(h);
    cache.valid = false;
    *out_mode = StorageMode::kScoped;
    *out_media_mask = 0;
    return;
  }

  FILETIME now_ft{};
  GetSystemTimeAsFileTime(&now_ft);
  ULARGE_INTEGER now_u{{now_ft.dwLowDateTime, now_ft.dwHighDateTime}};
  ULARGE_INTEGER mtime_u{{info.ftLastWriteTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime}};
  const bool recent_write =
      now_u.QuadPart >= mtime_u.QuadPart && (now_u.QuadPart - mtime_u.QuadPart) < 20000000ULL;

  if (!recent_write && cache.valid && cache.policy_path == policy_path &&
      cache.app_pkg == app_pkg &&
      cache.mtime_low == info.ftLastWriteTime.dwLowDateTime &&
      cache.mtime_high == info.ftLastWriteTime.dwHighDateTime &&
      cache.size_low == info.nFileSizeLow && cache.size_high == info.nFileSizeHigh) {
    CloseHandle(h);
    *out_mode = cache.mode;
    *out_media_mask = cache.media_mask;
    return;
  }

  StorageMode mode = StorageMode::kScoped;
  int media_mask = 0;
  bool read_complete = false;
  if (info.nFileSizeHigh == 0 && info.nFileSizeLow <= 65536) {
    std::string buf(info.nFileSizeLow, '\0');
    DWORD total = 0;
    while (total < info.nFileSizeLow) {
      DWORD chunk = 0;
      if (!ReadFile(h, buf.data() + total, info.nFileSizeLow - total, &chunk, nullptr) ||
          chunk == 0) {
        break;
      }
      total += chunk;
    }
    read_complete = total == info.nFileSizeLow;
    if (!read_complete || !ParsePolicyContent(buf, app_pkg, app_uid, &mode, &media_mask)) {
      mode = StorageMode::kScoped;
      media_mask = 0;
    }
  }
  CloseHandle(h);

  cache.valid = !recent_write && read_complete;
  cache.policy_path = policy_path;
  cache.app_pkg = std::string(app_pkg);
  cache.mtime_low = info.ftLastWriteTime.dwLowDateTime;
  cache.mtime_high = info.ftLastWriteTime.dwHighDateTime;
  cache.size_low = info.nFileSizeLow;
  cache.size_high = info.nFileSizeHigh;
  cache.mode = mode;
  cache.media_mask = media_mask;

  *out_mode = mode;
  *out_media_mask = media_mask;
}

bool MatchesExtensionList(std::wstring_view ext, const wchar_t* const* list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (EqualsIgnoreCase(ext, list[i])) return true;
  }
  return false;
}

bool IsAllowedMediaFile(const std::wstring& canon_path, int media_mask, AccessKind kind) {
  std::wstring open_path = ToWin32OpenPath(canon_path);
  DWORD attrs = GetFileAttributesW(open_path.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return true;
  }
  if (media_mask == 0) return false;

  size_t slash = canon_path.find_last_of(L'\\');
  std::wstring_view name =
      slash == std::wstring::npos ? std::wstring_view(canon_path)
                                  : std::wstring_view(canon_path).substr(slash + 1);
  size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring_view::npos || dot + 1 >= name.size()) {
    // Missing path without an extension: let stat/opendir surface ENOENT instead of EACCES.
    DWORD err = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    return attrs == INVALID_FILE_ATTRIBUTES &&
           (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) &&
           kind == AccessKind::kStat;
  }
  std::wstring_view ext = name.substr(dot);

  static constexpr const wchar_t* kImages[] = {
      L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp", L".heic", L".heif", L".dng", L".avif",
  };
  static constexpr const wchar_t* kVideo[] = {
      L".mp4", L".m4v", L".mkv", L".webm", L".3gp", L".avi", L".mov", L".ts",
  };
  static constexpr const wchar_t* kAudio[] = {
      L".mp3", L".m4a", L".aac", L".ogg", L".oga", L".opus", L".wav", L".flac", L".mid", L".midi",
  };

  if ((media_mask & 1) != 0 && MatchesExtensionList(ext, kImages, sizeof(kImages) / sizeof(kImages[0]))) {
    return true;
  }
  if ((media_mask & 2) != 0 && MatchesExtensionList(ext, kVideo, sizeof(kVideo) / sizeof(kVideo[0]))) {
    return true;
  }
  if ((media_mask & 4) != 0 && MatchesExtensionList(ext, kAudio, sizeof(kAudio) / sizeof(kAudio[0]))) {
    return true;
  }
  return false;
}

bool AllowSharedStorage(const std::wstring& canon_path, bool is_root_or_android_dir,
                        StorageMode mode, int media_mask, AccessKind kind) {
  if (kind == AccessKind::kWrite) {
    return mode == StorageMode::kManager || mode == StorageMode::kLegacyRw;
  }
  switch (mode) {
    case StorageMode::kManager:
    case StorageMode::kLegacyRw:
    case StorageMode::kLegacyRo:
      return true;
    case StorageMode::kMediaRo:
      return is_root_or_android_dir || IsAllowedMediaFile(canon_path, media_mask, kind);
    case StorageMode::kScoped:
      return kind == AccessKind::kStat && is_root_or_android_dir;
  }
  return false;
}

bool CheckCanonicalPath(const ProcessIdentity& identity, const std::wstring& canon_path,
                        AccessKind kind) {
  const std::wstring& media_root = identity.media_root;
  if (media_root.empty() || canon_path.empty()) {
    errno = EACCES;
    return false;
  }

  std::wstring_view rel;
  if (canon_path.size() == media_root.size() &&
      _wcsicmp(canon_path.c_str(), media_root.c_str()) == 0) {
    rel = L"";
  } else if (canon_path.size() > media_root.size() &&
             canon_path[media_root.size()] == L'\\' &&
             _wcsnicmp(canon_path.c_str(), media_root.c_str(), media_root.size()) == 0) {
    rel = std::wstring_view(canon_path).substr(media_root.size() + 1);
  } else {
    return true;  // Outside $ANDROID_DATA/media/0.
  }

  std::vector<std::wstring_view> comps;
  size_t pos = 0;
  while (pos < rel.size()) {
    size_t next = rel.find(L'\\', pos);
    std::wstring_view part =
        next == std::wstring_view::npos ? rel.substr(pos) : rel.substr(pos, next - pos);
    pos = next == std::wstring_view::npos ? rel.size() : next + 1;
    if (!part.empty()) comps.push_back(part);
  }

  std::wstring pkg_w = Widen(identity.package.c_str());

  // Android/data, Android/obb, and Android/media handling.
  bool is_root_or_android_dir = comps.empty();
  if (!comps.empty() && EqualsIgnoreCase(comps[0], L"Android")) {
    if (comps.size() == 1) {
      is_root_or_android_dir = true;
    } else if (EqualsIgnoreCase(comps[1], L"data") || EqualsIgnoreCase(comps[1], L"obb")) {
      if (comps.size() == 2) {
        if (kind == AccessKind::kStat) return true;
        errno = EACCES;
        return false;
      }
      if (EqualsIgnoreCase(comps[2], pkg_w)) return true;
      errno = EACCES;
      return false;
    } else if (EqualsIgnoreCase(comps[1], L"media")) {
      if (comps.size() >= 3 && EqualsIgnoreCase(comps[2], pkg_w)) return true;
      // Android/media and Android/media/<other> fall through to shared storage rules.
    }
  }

  std::wstring policy_path = identity.data_root + L"\\system\\storage-policy\\" + pkg_w;
  StorageMode mode = StorageMode::kScoped;
  int media_mask = 0;
  LoadPolicy(policy_path, identity.package, identity.uid, &mode, &media_mask);

  if (!AllowSharedStorage(canon_path, is_root_or_android_dir, mode, media_mask, kind)) {
    errno = EACCES;
    return false;
  }
  return true;
}

}  // namespace

extern "C" void gesher_storage_policy_anchor_v1() {}

void InitializeExternalStoragePolicy() { (void)Identity(); }

std::wstring ExternalStorageDataRoot() {
  const ProcessIdentity& identity = Identity();
  return identity.package.empty() ? EnvDirToWindows(getenv("ANDROID_DATA")) : identity.data_root;
}

std::wstring ExternalStorageChildEnvironment(const wchar_t* environment) {
  const ProcessIdentity& identity = Identity();
  if (identity.package.empty()) return {};
  std::vector<std::wstring> entries;
  for (const wchar_t* item = environment; item && *item; item += wcslen(item) + 1) {
    std::wstring_view entry(item);
    const auto name = entry.substr(0, entry.find(L'=', 1));
    if (!EqualsIgnoreCase(name, L"GESHER_APP_PACKAGE") &&
        !EqualsIgnoreCase(name, L"GESHER_UID") && !EqualsIgnoreCase(name, L"ANDROID_DATA"))
      entries.emplace_back(entry);
  }
  entries.push_back(L"GESHER_APP_PACKAGE=" + Widen(identity.package.c_str()));
  entries.push_back(L"GESHER_UID=" + Widen(identity.uid.c_str()));
  entries.push_back(L"ANDROID_DATA=" + identity.data_root);
  std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  std::wstring block;
  for (const auto& entry : entries) { block += entry; block += L'\0'; }
  block += L'\0';
  return block;
}

bool CheckExternalStorageAccess(const std::wstring& windows_path, AccessKind kind) {
  const ProcessIdentity& identity = Identity();
  if (identity.package.empty()) return true;
  return CheckCanonicalPath(identity, CanonicalizeWindowsPath(windows_path), kind);
}

bool CheckExternalStorageHandle(HANDLE handle, AccessKind kind) {
  const ProcessIdentity& identity = Identity();
  if (identity.package.empty() || GetFileType(handle) != FILE_TYPE_DISK) return true;
  wchar_t path[32768];
  DWORD length = GetFinalPathNameByHandleW(handle, path, 32768,
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= 32768) return errno = EACCES, false;
  return CheckCanonicalPath(identity, LexicalCanonicalize(std::wstring(path, length)), kind);
}

}  // namespace gesher

#ifdef GESHER_FORK_LOCK
// The policy cache is process-local state. Rebuild its lock in a forked child
// just like the filesystem caches registered by the rest of gesher_posix.
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::GetPolicyCache().mu);
#endif

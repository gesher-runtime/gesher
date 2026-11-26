// The system CA directory Gesher writes (compat/net/trust_store.cc) holds only
// certificates, <hash>.<n>, as Android's /system/etc/security/cacerts does:
// Conscrypt's DirectoryCertificateSource reads every file in it as a
// certificate. The manifest that makes an unchanged sync cheap lives next to
// the directory, and the manifest and temporary files earlier versions left in
// it are removed.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#include <set>
#include <string>

#include "trust_store.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(cond)) {                                                                       \
      fprintf(stderr, "FAIL: %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                                        \
    }                                                                                    \
  } while (0)

static bool WriteFile(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size();
  CloseHandle(h);
  return ok;
}

static bool ReadFile(const std::wstring& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out->clear();
  char buffer[16384];
  DWORD n = 0;
  while (::ReadFile(h, buffer, sizeof(buffer), &n, nullptr) && n > 0) out->append(buffer, n);
  CloseHandle(h);
  return true;
}

static bool IsCertName(const std::wstring& name) {
  if (name.size() < 10 || name[8] != L'.') return false;
  for (size_t i = 0; i < 8; i++) {
    if (!iswxdigit(name[i])) return false;
  }
  for (size_t i = 9; i < name.size(); i++) {
    if (!iswdigit(name[i])) return false;
  }
  return true;
}

static std::set<std::wstring> List(const std::wstring& dir) {
  std::set<std::wstring> names;
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return names;
  do {
    std::wstring name = entry.cFileName;
    if (name != L"." && name != L"..") names.insert(name);
  } while (FindNextFileW(find, &entry));
  FindClose(find);
  return names;
}

static void RemoveTree(const std::wstring& dir) {
  for (const std::wstring& name : List(dir)) {
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveTree(path);
    } else {
      DeleteFileW(path.c_str());
    }
  }
  RemoveDirectoryW(dir.c_str());
}

int main() {
  wchar_t temp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return 2;
  std::wstring root = std::wstring(temp) + L"gesher-trust-store-" +
                      std::to_wstring(GetCurrentProcessId());
  RemoveTree(root);
  std::wstring security = root + L"\\etc\\security";
  std::wstring cacerts = security + L"\\cacerts";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW((root + L"\\etc").c_str(), nullptr);
  CreateDirectoryW(security.c_str(), nullptr);
  CreateDirectoryW(cacerts.c_str(), nullptr);

  // What an earlier version left in the directory: its manifest, a temporary
  // file of an interrupted write, and a certificate no longer trusted.
  CHECK(WriteFile(cacerts + L"\\.gesher-cacerts", "00000000.0 00\n"));
  CHECK(WriteFile(cacerts + L"\\.00000000.0.tmp-1-2", "partial"));
  CHECK(WriteFile(cacerts + L"\\00000000.7", "stale"));

  std::string utf8(root.size() * 3, '\0');
  int len = WideCharToMultiByte(CP_UTF8, 0, root.c_str(), static_cast<int>(root.size()),
                                utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
  utf8.resize(len > 0 ? len : 0);
  _putenv_s("ANDROID_ROOT", utf8.c_str());
  // The sync also writes the user-added CAs under $ANDROID_DATA: keep them in
  // the temporary root too.
  std::string android_data = utf8 + "\\data";
  _putenv_s("ANDROID_DATA", android_data.c_str());

  // A different object type on the writer's mutex name makes CreateMutexW
  // fail immediately. A failed admission must never start the migration.
  HANDLE blocker = CreateEventW(nullptr, FALSE, FALSE, L"Local\\gesher-system-cacerts");
  CHECK(blocker != nullptr);
  if (blocker != nullptr) {
    const auto legacy = List(cacerts);
    const auto layout = List(security);
    gesher::net::SyncSystemCaCerts();
    CHECK(List(cacerts) == legacy);
    CHECK(List(security) == layout);
    CloseHandle(blocker);
  }

  gesher::net::SyncSystemCaCerts();

  std::string manifest;
  CHECK(ReadFile(security + L"\\.gesher-cacerts", &manifest));
  std::set<std::wstring> files = List(cacerts);
  for (const std::wstring& name : files) {
    if (!IsCertName(name)) fprintf(stderr, "not a certificate name in cacerts: %ls\n", name.c_str());
    CHECK(IsCertName(name));
  }
  CHECK(files.count(L"00000000.7") == 0);
  // Every manifest line names a written file, and every file is listed.
  size_t lines = 0;
  for (size_t at = 0; at < manifest.size();) {
    size_t end = manifest.find('\n', at);
    if (end == std::string::npos) end = manifest.size();
    std::string line = manifest.substr(at, end - at);
    size_t space = line.find(' ');
    CHECK(space != std::string::npos);
    std::wstring name(line.begin(), line.begin() + (space == std::string::npos ? 0 : space));
    CHECK(files.count(name) == 1);
    lines++;
    at = end + 1;
  }
  CHECK(lines == files.size());
  // Nothing staged next to the directory stays behind.
  for (const std::wstring& name : List(security)) {
    CHECK(name == L"cacerts" || name == L".gesher-cacerts");
  }

  // Unchanged: the manifest still matches, nothing is rewritten.
  FILETIME before = {}, after = {};
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (GetFileAttributesExW((security + L"\\.gesher-cacerts").c_str(), GetFileExInfoStandard, &data)) {
    before = data.ftLastWriteTime;
  }
  gesher::net::SyncSystemCaCerts();
  if (GetFileAttributesExW((security + L"\\.gesher-cacerts").c_str(), GetFileExInfoStandard, &data)) {
    after = data.ftLastWriteTime;
  }
  CHECK(CompareFileTime(&before, &after) == 0);
  CHECK(List(cacerts) == files);

  // Temporary files of a writer that died before renaming them, next to the
  // directory and (an earlier version's) in it, go at the next sync, even an
  // unchanged one.
  CHECK(WriteFile(security + L"\\.0000abcd.0.tmp-11-22", "partial"));
  CHECK(WriteFile(security + L"\\..gesher-cacerts.tmp-11-22", "partial"));
  const std::wstring unrelated = security + L"\\.unrelated-config.tmp-11-22";
  CHECK(WriteFile(unrelated, "must remain"));
  CHECK(WriteFile(cacerts + L"\\.0000abcd.0.tmp-11-22", "partial"));
  gesher::net::SyncSystemCaCerts();
  CHECK(List(cacerts) == files);
  std::string unrelated_contents;
  CHECK(ReadFile(unrelated, &unrelated_contents) && unrelated_contents == "must remain");
  CHECK(DeleteFileW(unrelated.c_str()));
  for (const std::wstring& name : List(security)) {
    CHECK(name == L"cacerts" || name == L".gesher-cacerts");
  }

  // The manifest is outside the directory: a deleted directory, or a deleted
  // certificate, is written again although the manifest still matches.
  if (!files.empty()) {
    DeleteFileW((cacerts + L"\\" + *files.begin()).c_str());
    gesher::net::SyncSystemCaCerts();
    CHECK(List(cacerts) == files);
  }
  RemoveTree(cacerts);
  CHECK(GetFileAttributesW(cacerts.c_str()) == INVALID_FILE_ATTRIBUTES);
  gesher::net::SyncSystemCaCerts();
  CHECK(List(cacerts) == files);
  std::string again;
  CHECK(ReadFile(security + L"\\.gesher-cacerts", &again) && again == manifest);

  RemoveTree(root);
  if (g_failed == 0) {
    printf("trust store layout: all passed (%d checks, %zu certificates)\n", g_checks,
           files.size());
    return 0;
  }
  fprintf(stderr, "FAILED: %d/%d checks failed\n", g_failed, g_checks);
  return 1;
}

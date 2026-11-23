#include <gesher/rooted_storage.h>

#include <windows.h>
#include <aclapi.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
namespace storage = gesher::rooted_storage;
using storage::Access;
using storage::Area;
using storage::OpenRequest;
constexpr auto kCE = Area::kCredential;
constexpr auto kDE = Area::kDevice;

int failures;
void Check(bool ok, const char* test) {
  std::printf("%s: %s (Win32 %lu)\n", test, ok ? "ok" : "FAILED", GetLastError());
  if (!ok) ++failures;
}

HANDLE Pin(const fs::path& path) {
  return CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

std::string Contents(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void FileOperations(const storage::Roots& roots, const fs::path& dir) {
  Check(storage::MakeDirectory(roots, kCE, L"files") == 0, "create CE directory");
  Check(storage::MakeDirectory(roots, kDE, L"files") == 0, "create separate DE directory");
  HANDLE file = INVALID_HANDLE_VALUE;
  OpenRequest create{Access::kReadWrite, true, false, false, false, false, true};
  bool created = false;
  const int open = storage::Open(roots, kCE, L"files/state", create, &file, &created);
  Check(open == 0 && created, "open produced the actual new file");
  if (open != 0) return;
  DWORD count = 0;
  Check(WriteFile(file, "data", 4, &count, nullptr) && count == 4, "native handle writes real CE bytes");
  HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  const auto* bytes = map == nullptr ? nullptr :
      static_cast<const char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4));
  Check(bytes != nullptr && std::string_view(bytes, 4) == "data", "file handle supports a real mapping");
  if (bytes != nullptr) UnmapViewOfFile(bytes);
  if (map != nullptr) CloseHandle(map);
  OVERLAPPED range{};
  Check(LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range) &&
        UnlockFileEx(file, 0, 1, 0, &range), "file handle supports kernel range lock");
  PSECURITY_DESCRIPTOR existing_dacl = nullptr;
  auto native_file = (dir / L"CE/files/state").wstring();
  Check(GetNamedSecurityInfoW(native_file.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, nullptr, nullptr, &existing_dacl) == ERROR_SUCCESS &&
        !SetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION, existing_dacl) &&
        GetLastError() == ERROR_ACCESS_DENIED, "data handle provides no DACL write authority");
  if (existing_dacl != nullptr) LocalFree(existing_dacl);
  BY_HANDLE_FILE_INFORMATION ce_info{}, de_info{};
  Check(storage::Stat(roots, kCE, L"files/state", &ce_info) == 0 && ce_info.nFileSizeLow == 4 &&
        storage::Stat(roots, kDE, L"files/state", &de_info) == ENOENT,
        "CE and DE use exact independent roots");
  HANDLE again = INVALID_HANDLE_VALUE;
  created = true;
  Check(storage::Open(roots, kCE, L"files/state", create, &again, &created) == 0 && !created,
        "kernel distinguishes opening an existing file");
  if (again != INVALID_HANDLE_VALUE) CloseHandle(again);
  create.exclusive = true;
  Check(storage::Open(roots, kCE, L"files/state", create, &again) == EEXIST, "exclusive creation preserves EEXIST");
  Check(storage::Rename(roots, kCE, L"files/state", kDE, L"files/state", true) == EXDEV,
        "CE and DE rename cannot cross the selected area");
  Check(storage::Rename(roots, kCE, L"files/state", kCE, L"files/new-state", false) == 0 &&
        Contents(dir / L"CE/files/new-state") == "data", "same-root rename changes real namespace");
  Check(storage::Remove(roots, kCE, L"files", true) == ENOTEMPTY, "nonempty directory returns ENOTEMPTY");
  Check(storage::Remove(roots, kCE, L"files/new-state", false) == 0 &&
        storage::Stat(roots, kCE, L"files/new-state", &ce_info) == ENOENT,
        "unlink removes the name while the existing handle remains valid");
  char data[4]{};
  LARGE_INTEGER start{};
  Check(SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && ReadFile(file, data, 4, &count, nullptr) &&
        count == 4 && std::string_view(data, 4) == "data", "unlinked file handle retains exact bytes");
  CloseHandle(file);
  Check(storage::Remove(roots, kCE, L"files", true) == 0, "remove an empty directory");
}

void WinVfsHandles(const storage::Roots& roots, const fs::path& dir) {
  OpenRequest options;
  options.access = Access::kReadWrite;
  options.create = true;
  options.exclusive = true;
  options.share_mode = 0;
  HANDLE file = INVALID_HANDLE_VALUE;
  const int result = storage::Open(roots, kCE, L"sqlite-exclusive", options, &file);
  Check(result == 0, "open a real exclusive database handle");
  if (result != 0) return;
  HANDLE other = INVALID_HANDLE_VALUE;
  Check(storage::Open(roots, kCE, L"sqlite-exclusive", {}, &other) == EACCES &&
        other == INVALID_HANDLE_VALUE, "exclusive database sharing blocks concurrent open");
  CloseHandle(file);

  options.share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
  options.delete_on_close = true;
  options.overlapped = true;
  file = INVALID_HANDLE_VALUE;
  const int temp = storage::Open(roots, kCE, L"sqlite-temp", options, &file);
  Check(temp == 0, "open a real overlapped delete-on-close temp file");
  if (temp != 0) return;
  OVERLAPPED io{};
  DWORD transferred = 0;
  const bool queued = WriteFile(file, "safe", 4, nullptr, &io) || GetLastError() == ERROR_IO_PENDING;
  Check(queued && GetOverlappedResult(file, &io, &transferred, TRUE) && transferred == 4,
        "overlapped file preserves real I/O");
  CloseHandle(file);
  Check(!fs::exists(dir / L"CE/sqlite-temp"), "last close retires the real temp file");
}

void BoundaryOperations(const storage::Roots& roots, const fs::path& dir) {
  std::ofstream(dir / L"outside" / L"sentinel", std::ios::binary) << "outside";
  Check(CreateHardLinkW((dir / L"CE/aliased").c_str(), (dir / L"outside/sentinel").c_str(), nullptr),
        "fixture has real out-of-root hard link");
  OpenRequest truncate{Access::kReadWrite, true, false, true};
  HANDLE file = INVALID_HANDLE_VALUE;
  Check(storage::Open(roots, kCE, L"aliased", truncate, &file) == EMLINK &&
        Contents(dir / L"outside/sentinel") == "outside", "outside hard link rejected before truncation");
  const bool linked = CreateSymbolicLinkW((dir / L"CE/redirect").c_str(), (dir / L"outside").c_str(),
      SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  Check(linked, "fixture has real Windows directory reparse point");
  if (linked) {
    Check(storage::Open(roots, kCE, L"redirect/sentinel", truncate, &file) == ELOOP &&
          Contents(dir / L"outside/sentinel") == "outside", "outside directory reparse fails closed");
  }
  for (const auto* bad : {L"../outside/sentinel", L"/outside/sentinel", L"C:/tmp", L"foo\\bar",
                          L"foo:stream", L"x//y", L"x/../y", L"x/./y", L"x/"}) {
    Check(storage::Open(roots, kCE, bad, truncate, &file) == EINVAL, "unsafe path form rejected");
  }
  Check(storage::Rename(roots, kCE, L"", kCE, L"renamed", true) == EBUSY &&
        storage::Remove(roots, kCE, L"", true) == EBUSY, "guest cannot replace or delete pinned root");
  HANDLE directory = INVALID_HANDLE_VALUE;
  Check(storage::Open(roots, kCE, L"", OpenRequest{Access::kRead, false, false, false, true}, &directory) == 0,
        "open the real ensured CE directory");
  if (directory != INVALID_HANDLE_VALUE) {
    std::vector<storage::DirectoryEntry> entries;
    Check(storage::ReadDirectory(directory, true, &entries) == 0 &&
          std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.name == L"aliased"; }) &&
          std::none_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.name == L"outside"; }),
          "enumeration uses the pinned kernel directory handle");
    CloseHandle(directory);
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  fs::path dir = fs::absolute(argv[1]) / (L"rooted-" + std::to_wstring(GetCurrentProcessId()));
  for (const auto* child : {L"CE", L"DE", L"outside"}) fs::create_directories(dir / child);
  storage::Roots roots{Pin(dir / L"CE"), Pin(dir / L"DE")};
  if (roots.credential == INVALID_HANDLE_VALUE || roots.device == INVALID_HANDLE_VALUE) return 2;
  FileOperations(roots, dir);
  WinVfsHandles(roots, dir);
  BoundaryOperations(roots, dir);
  CloseHandle(roots.credential);
  CloseHandle(roots.device);
  return failures == 0 ? 0 : 1;
}

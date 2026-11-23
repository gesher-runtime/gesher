#include <gesher/rooted_storage.h>

#include <winternl.h>

#include <array>
#include <cstddef>
#include <cstring>

#include <gesher/reserved_names.h>

#include "internal.h"

namespace gesher::rooted_storage {
namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD kSharePinnedDirectory = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr ACCESS_MASK kDirectoryQuery = FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE;

class File {
 public:
  File() = default;
  explicit File(HANDLE handle) : handle_(handle) {}
  File(File&& other) noexcept : File(other.release()) {}
  File& operator=(const File&) = delete;
  ~File() {
    if (valid()) CloseHandle(handle_);
  }
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
  HANDLE get() const { return handle_; }
  HANDLE release() {
    HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return handle;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

HANDLE Root(const Roots& roots, Area area) {
  switch (area) {
    case Area::kCredential: return roots.credential;
    case Area::kDevice: return roots.device;
  }
  return INVALID_HANDLE_VALUE;
}

int NativeError(NTSTATUS status) {
  constexpr NTSTATUS kFileIsDirectory = static_cast<NTSTATUS>(0xc00000ba);
  if (status == kFileIsDirectory) return EISDIR;
  using Translate = ULONG(WINAPI*)(NTSTATUS);
  static const auto translate = reinterpret_cast<Translate>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError")));
  if (translate == nullptr) return EIO;
  const DWORD code = translate(status);
  if (code == ERROR_STOPPED_ON_SYMLINK || code == ERROR_REPARSE_POINT_ENCOUNTERED) return ELOOP;
  return ErrnoFromWin32(code);
}

int GetInfo(HANDLE file, BY_HANDLE_FILE_INFORMATION* info) {
  if (GetFileType(file) != FILE_TYPE_DISK) return EBADF;
  if (!GetFileInformationByHandle(file, info)) return ErrnoFromWin32(GetLastError());
  if (info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ELOOP;
  if (!(info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info->nNumberOfLinks != 1) return EMLINK;
  return 0;
}

int CheckDirectory(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};
  int error = GetInfo(file, &info);
  if (error != 0) return error;
  return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ENOTDIR;
}

int OpenChild(HANDLE parent, std::wstring_view name, ACCESS_MASK access, ULONG disposition,
              ULONG options, ULONG sharing, HANDLE* result, ULONG file_attributes = FILE_ATTRIBUTE_NORMAL,
              bool* created = nullptr, bool overlapped = false) {
  using Create = NTSTATUS(WINAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                   PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  static const auto create = reinterpret_cast<Create>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile")));
  if (create == nullptr) return EOPNOTSUPP;
  auto bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  UNICODE_STRING object_name{bytes, bytes, const_cast<wchar_t*>(name.data())};
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE, parent, nullptr);
  IO_STATUS_BLOCK io{};
  if (!overlapped) options |= FILE_SYNCHRONOUS_IO_NONALERT;
  NTSTATUS status = create(result, access | SYNCHRONIZE, &attributes, &io, nullptr,
                           file_attributes, sharing, disposition,
                           options | FILE_OPEN_REPARSE_POINT,
                           nullptr, 0);
  if (status < 0) return NativeError(status);
  if (created != nullptr) *created = io.Information == FILE_CREATED;
  return 0;
}

struct Resolved {
  HANDLE parent = INVALID_HANDLE_VALUE;
  std::wstring_view leaf;
  std::vector<File> ancestors;
};

int ValidatePath(std::wstring_view path) {
  if (path.size() > 4096) return ENAMETOOLONG;
  while (!path.empty()) {
    size_t slash = path.find(L'/');
    std::wstring_view name = path.substr(0, slash);
    if (name.size() > 255) return ENAMETOOLONG;
    if (name.empty() || name == L"." || name == L".." || name.back() == L' ' || name.back() == L'.') {
      return EINVAL;
    }
    for (wchar_t character : name) {
      if (character == L'\\' || character == L'\0' || reserved_names::Reserved(character)) return EINVAL;
    }
    if (slash == std::wstring_view::npos) break;
    path.remove_prefix(slash + 1);
    if (path.empty()) return EINVAL;
  }
  return 0;
}

int Resolve(const Roots& roots, Area area, std::wstring_view path, Resolved* scope) {
  int error = ValidatePath(path);
  if (error != 0) return error;
  scope->parent = Root(roots, area);
  error = CheckDirectory(scope->parent);
  if (error != 0) return error;
  for (size_t slash = path.find(L'/'); slash != std::wstring_view::npos; slash = path.find(L'/')) {
    HANDLE opened = INVALID_HANDLE_VALUE;
    error = OpenChild(scope->parent, path.substr(0, slash), kDirectoryQuery,
                      FILE_OPEN, FILE_DIRECTORY_FILE, kSharePinnedDirectory, &opened);
    if (error != 0) return error;
    File ancestor(opened);
    error = CheckDirectory(opened);
    if (error != 0) return error;
    scope->ancestors.emplace_back(std::move(ancestor));
    scope->parent = opened;
    path.remove_prefix(slash + 1);
  }
  scope->leaf = path;
  return 0;
}

int DuplicateDirectory(HANDLE file, ACCESS_MASK access, HANDLE* result) {
  HANDLE process = GetCurrentProcess();
  return DuplicateHandle(process, file, process, result, access | SYNCHRONIZE, FALSE, 0)
      ? 0 : ErrnoFromWin32(GetLastError());
}

int CheckRemoveType(bool is_directory, bool requested_directory) {
  if (is_directory == requested_directory) return 0;
  return is_directory ? EISDIR : ENOTDIR;
}

}  // namespace

int Open(const Roots& roots, Area area, std::wstring_view path, OpenRequest request, HANDLE* result,
         bool* created) {
  if (result == nullptr) return EFAULT;
  *result = INVALID_HANDLE_VALUE;
  if (created != nullptr) *created = false;
  ACCESS_MASK rights = FILE_READ_ATTRIBUTES;
  switch (request.access) {
    case Access::kRead: rights |= FILE_READ_DATA; break;
      case Access::kWrite: rights |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES; break;
      case Access::kReadWrite:
        rights |= FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES;
        break;
    case Access::kMetadata: break;
    case Access::kMetadataWrite: rights |= FILE_WRITE_ATTRIBUTES; break;
    default: return EINVAL;
  }
  const bool writes = request.access == Access::kReadWrite || request.access == Access::kWrite;
  const bool metadata = request.access == Access::kMetadata || request.access == Access::kMetadataWrite;
  if ((request.share_mode & ~kShareAll) != 0 || (request.exclusive && !request.create) ||
      (request.truncate && !writes) ||
      (request.read_only_create && !request.create) || (metadata && request.create) ||
      (request.directory && (writes || request.create || request.delete_on_close)) ||
      (metadata && request.delete_on_close)) return EINVAL;
  if (request.delete_on_close) rights |= DELETE;
  Resolved scope;
  int error = Resolve(roots, area, path, &scope);
  if (error != 0) return error;
  if (scope.leaf.empty()) {
    if (request.create && request.exclusive) return EEXIST;
    if (request.create || writes || request.delete_on_close) return EISDIR;
    if (request.share_mode != kShareAll || request.overlapped) return EOPNOTSUPP;
    return DuplicateDirectory(scope.parent, rights, result);
  }
  const ULONG disposition = !request.create ? FILE_OPEN : request.exclusive ? FILE_CREATE : FILE_OPEN_IF;
  ULONG options = request.directory ? FILE_DIRECTORY_FILE :
      (request.create || writes || request.delete_on_close ? FILE_NON_DIRECTORY_FILE : 0);
  if (request.write_through) options |= FILE_WRITE_THROUGH;
  const ULONG file_attributes = request.read_only_create ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
  HANDLE opened = INVALID_HANDLE_VALUE;
  bool fresh = false;
  error = OpenChild(scope.parent, scope.leaf, rights, disposition, options, request.share_mode, &opened,
                    file_attributes, &fresh, request.overlapped);
  if (error != 0) return error;
  File owned(opened);
  BY_HANDLE_FILE_INFORMATION info{};
  error = GetInfo(opened, &info);
  if (error != 0) return error;
  // Native overwrite dispositions can truncate before the link check.
  FILE_END_OF_FILE_INFO end{};
  if (request.truncate && !SetFileInformationByHandle(opened, FileEndOfFileInfo, &end, sizeof(end))) {
    return ErrnoFromWin32(GetLastError());
  }
  if (request.delete_on_close) {
    HANDLE narrowed = INVALID_HANDLE_VALUE;
    const HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process, opened, process, &narrowed, (rights & ~DELETE) | SYNCHRONIZE,
                         FALSE, 0)) return ErrnoFromWin32(GetLastError());
    File guest(narrowed);
    FILE_DISPOSITION_INFO deletion{TRUE};
    if (!SetFileInformationByHandle(opened, FileDispositionInfo, &deletion, sizeof(deletion))) {
      return ErrnoFromWin32(GetLastError());
    }
    // The duplicate keeps this exact file object and its pending deletion,
    // without receiving the right to change its disposition or name.
    *result = guest.release();
  } else {
    *result = owned.release();
  }
  if (created != nullptr) *created = fresh;
  return 0;
}

int Stat(const Roots& roots, Area area, std::wstring_view path, BY_HANDLE_FILE_INFORMATION* result) {
  if (result == nullptr) return EFAULT;
  Resolved scope;
  int error = Resolve(roots, area, path, &scope);
  if (error != 0) return error;
  if (scope.leaf.empty()) return GetInfo(scope.parent, result);
  HANDLE opened = INVALID_HANDLE_VALUE;
  error = OpenChild(scope.parent, scope.leaf, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, kShareAll, &opened);
  if (error != 0) return error;
  File owned(opened);
  return GetInfo(opened, result);
}

int MakeDirectory(const Roots& roots, Area area, std::wstring_view path) {
  Resolved scope;
  int error = Resolve(roots, area, path, &scope);
  if (error != 0) return error;
  if (scope.leaf.empty()) return EEXIST;
  HANDLE opened = INVALID_HANDLE_VALUE;
  error = OpenChild(scope.parent, scope.leaf, kDirectoryQuery, FILE_CREATE, FILE_DIRECTORY_FILE,
                    kSharePinnedDirectory, &opened);
  File owned(opened);
  return error;
}

int Remove(const Roots& roots, Area area, std::wstring_view path, bool directory) {
  Resolved scope;
  int error = Resolve(roots, area, path, &scope);
  if (error != 0) return error;
  if (scope.leaf.empty()) return EBUSY;
  HANDLE opened = INVALID_HANDLE_VALUE;
  error = OpenChild(scope.parent, scope.leaf, FILE_READ_ATTRIBUTES | DELETE, FILE_OPEN, 0, kShareAll, &opened);
  if (error != 0) return error;
  File owned(opened);
  BY_HANDLE_FILE_INFORMATION info{};
  error = GetInfo(opened, &info);
  if (error != 0) return error;
  error = CheckRemoveType((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, directory);
  if (error != 0) return error;
  FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                       FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
  return SetFileInformationByHandle(opened, FileDispositionInfoEx, &disposition, sizeof(disposition))
      ? 0 : ErrnoFromWin32(GetLastError());
}

int Rename(const Roots& roots, Area source_area, std::wstring_view source,
           Area target_area, std::wstring_view target, bool replace) {
  if (source_area != target_area) return EXDEV;
  Resolved from, to;
  int error = Resolve(roots, source_area, source, &from);
  if (error != 0) return error;
  error = Resolve(roots, target_area, target, &to);
  if (error != 0) return error;
  if (from.leaf.empty() || to.leaf.empty()) return EBUSY;
  HANDLE source_file = INVALID_HANDLE_VALUE;
  error = OpenChild(from.parent, from.leaf, FILE_READ_ATTRIBUTES | DELETE, FILE_OPEN, 0, kShareAll, &source_file);
  if (error != 0) return error;
  File source_owner(source_file);
  BY_HANDLE_FILE_INFORMATION source_info{};
  error = GetInfo(source_file, &source_info);
  if (error != 0) return error;
  HANDLE target_file = INVALID_HANDLE_VALUE;
  error = OpenChild(to.parent, to.leaf, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, kShareAll, &target_file);
  File target_owner(target_file);
  if (error != 0 && error != ENOENT) return error;
  if (target_owner.valid()) {
    BY_HANDLE_FILE_INFORMATION target_info{};
    error = GetInfo(target_file, &target_info);
    if (error != 0) return error;
    error = CheckRemoveType((target_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                            (source_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if (error != 0) return error;
  }
  const size_t bytes = to.leaf.size() * sizeof(wchar_t);
  alignas(FILE_RENAME_INFO) std::array<std::byte, sizeof(FILE_RENAME_INFO) + 255 * sizeof(wchar_t)> storage{};
  auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
  information->Flags = replace ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS : 0;
  information->RootDirectory = to.parent;
  information->FileNameLength = static_cast<DWORD>(bytes);
  std::memcpy(information->FileName, to.leaf.data(), bytes);
  // Use the native handle-relative operation. The Win32 wrapper may require
  // a DOS path even when FILE_RENAME_INFO describes a relative target.
  using Set = NTSTATUS(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  static const auto set = reinterpret_cast<Set>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile")));
  if (set == nullptr) return EOPNOTSUPP;
  constexpr ULONG kFileRenameInformationEx = 65;
  IO_STATUS_BLOCK io{};
  NTSTATUS status = set(source_file, &io, information,
                        static_cast<ULONG>(sizeof(FILE_RENAME_INFO) + bytes), kFileRenameInformationEx);
  return status < 0 ? NativeError(status) : 0;
}

int ReadDirectory(HANDLE directory, bool restart, std::vector<DirectoryEntry>* entries) {
  if (entries == nullptr) return EFAULT;
  entries->clear();
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 16384> buffer{};
  if (!GetFileInformationByHandleEx(directory, restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                                    buffer.data(), static_cast<DWORD>(buffer.size()))) {
    DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? 0 : ErrnoFromWin32(error);
  }
  size_t position = 0;
  while (position + offsetof(FILE_ID_BOTH_DIR_INFO, FileName) <= buffer.size()) {
    const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + position);
    const size_t name_len = entry->FileNameLength;
    const size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    if (name_len % sizeof(wchar_t) != 0 || name_len > buffer.size() - position - header) break;
    entries->push_back({std::wstring(entry->FileName, name_len / sizeof(wchar_t)),
                        entry->FileAttributes, static_cast<ULONGLONG>(entry->FileId.QuadPart)});
    if (entry->NextEntryOffset == 0) return 0;
    if (entry->NextEntryOffset < header + name_len || entry->NextEntryOffset > buffer.size() - position ||
        entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0) break;
    position += entry->NextEntryOffset;
  }
  entries->clear();
  return EIO;
}

}  // namespace gesher::rooted_storage

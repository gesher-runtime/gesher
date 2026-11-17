// Android user.* extended attributes backed by local Windows file EAs.
#include <unistd.h>
#include <sys/xattr.h>
#include <windows.h>
#include <aclapi.h>
#include <gesher/xattr.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <new>
#include <string_view>

#include "internal.h"
#include "record_lock_descriptors.h"
#include "record_lock_liveness.h"
#include "xattr_access.h"
#include "xattr_security_policy.h"

namespace {

constexpr char kNativeName[] = "GESHER.USER.XATTR";
constexpr char kSignature[] = {'G', 'X', 'A', 1};
constexpr std::string_view kPrefix = "user.";
constexpr size_t kMaxName = 255;
constexpr size_t kMaxValue = 65536;
// Leave room for the native EA name, headers and alignment in NTFS's shared
// 64 KiB EA budget. The filesystem may enforce a lower capacity.
constexpr size_t kMaxStore = 65500;
constexpr DWORD kSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD kOpenFlags = FILE_FLAG_BACKUP_SEMANTICS;
using Attributes = std::map<std::string_view, std::string_view, std::less<>>;

class Handle {
 public:
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (*this) CloseHandle(value);
  }
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
  HANDLE value = INVALID_HANDLE_VALUE;
};

struct NativeEa {
  ULONG next;
  UCHAR flags;
  UCHAR name_size;
  USHORT value_size;
};
static_assert(sizeof(NativeEa) == 8);
constexpr size_t kValueOffset = sizeof(NativeEa) + sizeof(kNativeName);

struct NativeCalls {
  using Query = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, BOOLEAN,
                             void*, ULONG, ULONG*, BOOLEAN);
  using Set = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG);
  using QueryVolume = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FS_INFORMATION_CLASS);
  using Open = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
  using Error = ULONG(NTAPI*)(LONG);

  NativeCalls() {
    HMODULE dll = GetModuleHandleW(L"ntdll.dll");
    if (!dll) return;
    query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(dll, "NtQueryEaFile")));
    set = reinterpret_cast<Set>(reinterpret_cast<void*>(GetProcAddress(dll, "NtSetEaFile")));
    volume = reinterpret_cast<QueryVolume>(
        reinterpret_cast<void*>(GetProcAddress(dll, "NtQueryVolumeInformationFile")));
    open = reinterpret_cast<Open>(reinterpret_cast<void*>(GetProcAddress(dll, "NtOpenFile")));
    error = reinterpret_cast<Error>(
        reinterpret_cast<void*>(GetProcAddress(dll, "RtlNtStatusToDosError")));
  }

  Query query = nullptr;
  Set set = nullptr;
  QueryVolume volume = nullptr;
  Open open = nullptr;
  Error error = nullptr;
};

const NativeCalls& Native() {
  static const NativeCalls calls;
  return calls;
}

int NativeError(LONG status) {
  switch (static_cast<ULONG>(status)) {
    case 0xc0000010:  // STATUS_INVALID_DEVICE_REQUEST
    case 0xc000004f:  // STATUS_EAS_NOT_SUPPORTED
    case 0xc00000bb:  // STATUS_NOT_SUPPORTED
      return EOPNOTSUPP;
    case 0xc0000044:  // STATUS_QUOTA_EXCEEDED
      return EDQUOT;
    case 0xc0000050:  // STATUS_EA_TOO_LARGE
      return ENOSPC;
    case 0xc0000053:  // STATUS_EA_CORRUPT_ERROR
    case 0x80000005:  // STATUS_BUFFER_OVERFLOW
    case 0xc0000023:  // STATUS_BUFFER_TOO_SMALL
      return EIO;
    case 0xc000009a:  // STATUS_INSUFFICIENT_RESOURCES
      return ENOMEM;
    case 0xc0000056:  // STATUS_DELETE_PENDING
      return ENOENT;
    default:
      return status < 0 && Native().error ? gesher::ErrnoFromWin32(Native().error(status)) : EIO;
  }
}

int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }

bool ReadUser(const void* source, void* destination, size_t bytes) {
  SIZE_T copied = 0;
  return bytes == 0 ||
         (source && ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &copied) &&
          copied == bytes);
}

int ReadString(const char* source, char* destination, size_t capacity, int too_long) {
  if (!source) return EFAULT;
  static const DWORD page_size = [] {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    return system.dwPageSize;
  }();
  // Only probe the next page if no terminator was present in this one.
  uintptr_t cursor = reinterpret_cast<uintptr_t>(source);
  while (capacity) {
    const size_t count = std::min(capacity, static_cast<size_t>(page_size - cursor % page_size));
    if (!ReadUser(reinterpret_cast<const void*>(cursor), destination, count)) return EFAULT;
    if (std::memchr(destination, 0, count)) return 0;
    if (__builtin_add_overflow(cursor, count, &cursor)) return EFAULT;
    destination += count;
    capacity -= count;
  }
  return too_long;
}

bool WriteUser(void* destination, std::string_view value) {
  if (value.empty()) return true;
  uintptr_t cursor = reinterpret_cast<uintptr_t>(destination);
  uintptr_t end;
  if (!cursor || __builtin_add_overflow(cursor, value.size(), &end)) return false;
  // WriteProcessMemory can make readonly pages writable for debuggers.
  // Check each region before using it as Linux's copy_to_user equivalent.
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD)) return false;
    const DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    uintptr_t next;
    if (__builtin_add_overflow(reinterpret_cast<uintptr_t>(region.BaseAddress),
                               region.RegionSize, &next) || next <= cursor) return false;
    cursor = next;
  }
  SIZE_T copied = 0;
  return WriteProcessMemory(GetCurrentProcess(), destination, value.data(), value.size(), &copied) &&
         copied == value.size();
}

int CheckName(std::string_view name) {
  if (name.empty()) return ERANGE;
  if (!name.starts_with(kPrefix)) return EOPNOTSUPP;
  return name.size() == kPrefix.size() ? EINVAL : 0;
}

enum class Operation { kGet, kList, kSet, kRemove };

struct Request {
  Operation operation;
  const char* name = nullptr;
  void* output = nullptr;
  const void* input = nullptr;
  size_t length = 0;
  int flags = 0;

  bool Mutates() const { return operation == Operation::kSet || operation == Operation::kRemove; }
};

DWORD RequiredAccess(const Request& request) {
  // Android checks current inode read/write permission, not the mode of the
  // descriptor used to reach it. EA rights alone are separate Windows rights.
  return FILE_READ_EA | FILE_READ_ATTRIBUTES |
         (request.operation == Operation::kGet ? FILE_READ_DATA : 0) |
         (request.Mutates() ? FILE_WRITE_EA | FILE_WRITE_DATA : 0);
}

}  // namespace

namespace gesher::xattr {

// A native empty name opens the held file itself, including directories.
// ReOpenFile can reject directory objects even with backup flags.
int ReopenFileAccess(HANDLE file, DWORD access, HANDLE* result, LONG* native_status) {
  if (!Native().open) {
    if (native_status) *native_status = static_cast<LONG>(0xc0000002);  // STATUS_NOT_IMPLEMENTED
    return EOPNOTSUPP;
  }
  wchar_t empty = L'\0';
  UNICODE_STRING name{0, sizeof(empty), &empty};
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = file;
  attributes.ObjectName = &name;
  IO_STATUS_BLOCK io{};
  const LONG status = Native().open(result, access | SYNCHRONIZE, &attributes, &io, kSharing,
                                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (native_status) *native_status = status;
  return status == 0 ? 0 : NativeError(status);
}

}  // namespace gesher::xattr

namespace {

// Only the ordinary allow/deny DACL and mandatory label can be evaluated by
// this fallback. CAP, resource attributes, callback/conditional and other ACE
// policies need the filesystem's authorization machinery. If native reopening
// fails with STATUS_DELETE_PENDING, refuse them instead of omitting them.
int CheckFallbackPolicy(PSECURITY_DESCRIPTOR descriptor) {
  if (!descriptor || !IsValidSecurityDescriptor(descriptor)) return EIO;
  for (bool system_acl : {false, true}) {
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    const BOOL ok = system_acl ? GetSecurityDescriptorSacl(descriptor, &present, &acl, &defaulted)
                              : GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted);
    if (!ok) return EIO;
    if (!present || !acl) continue;
    if (!IsValidAcl(acl)) return EIO;
    const auto support = gesher::xattr::CheckFallbackAcl(
        {reinterpret_cast<const unsigned char*>(acl), acl->AclSize},
        system_acl ? gesher::xattr::AclKind::kSacl : gesher::xattr::AclKind::kDacl);
    if (support == gesher::xattr::PolicySupport::kUnsupported) return EOPNOTSUPP;
    if (support == gesher::xattr::PolicySupport::kMalformed) return EIO;
  }
  return 0;
}

}  // namespace

namespace gesher::xattr {

// Reopening a delete-pending object can fail even though its admitted handle
// remains valid. When it already has the native EA rights, check the object's
// current DACL against the effective token and keep using that pinned handle.
// Never use the old grant as evidence of current inode authorization.
int CheckHeldFileAccess(HANDLE file, DWORD desired) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  // ATTRIBUTE and SCOPE are separate READ_CONTROL queries: querying LABEL
  // alone does not reveal resource claims or an applicable Central Access Policy.
  constexpr SECURITY_INFORMATION information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
      DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION |
      ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION |
      PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION;
  // Ask Windows for the query rights of this complete metadata selection. Do
  // not enable audit privileges or omit a policy class when it is inaccessible.
  DWORD query_access = 0;
  QuerySecurityAccessMask(information, &query_access);
  if (query_access != READ_CONTROL) return EOPNOTSUPP;
  const DWORD query_error = GetSecurityInfo(file, SE_FILE_OBJECT, information,
      nullptr, nullptr, nullptr, nullptr, &descriptor);
  if (query_error != ERROR_SUCCESS) return gesher::ErrnoFromWin32(query_error);
  const auto free_descriptor = [](void* value) { LocalFree(value); };
  std::unique_ptr<void, decltype(free_descriptor)> security(descriptor, free_descriptor);
  const int policy_error = CheckFallbackPolicy(descriptor);
  if (policy_error) return policy_error;
  Handle token;
  // OpenAsSelf controls admission to the thread token; the token being checked
  // is still the impersonating thread's, never the process token on failure.
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value)) {
    if (GetLastError() != ERROR_NO_TOKEN) return WinError();
    Handle process_token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token.value) ||
        !DuplicateToken(process_token.value, SecurityImpersonation, &token.value)) return WinError();
  }
  // AccessCheck evaluates the DACL. A held handle must not bypass mandatory
  // integrity policy when a lower-integrity thread impersonates its opener.
  TOKEN_MANDATORY_POLICY policy{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token.value, TokenMandatoryPolicy, &policy, sizeof(policy), &bytes))
    return WinError();
  if (policy.Policy & TOKEN_MANDATORY_POLICY_NO_WRITE_UP) {
    alignas(TOKEN_MANDATORY_LABEL) unsigned char label_buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, label_buffer, sizeof(label_buffer), &bytes))
      return WinError();
    const PSID token_sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label_buffer)->Label.Sid;
    if (!IsValidSid(token_sid) || *GetSidSubAuthorityCount(token_sid) != 1) return EIO;
    const DWORD token_level = *GetSidSubAuthority(token_sid, 0);
    DWORD object_level = SECURITY_MANDATORY_MEDIUM_RID;
    DWORD mask = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
    PACL sacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted)) return WinError();
    if (present && sacl) {
      for (DWORD i = 0; i < sacl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(sacl, i, &entry)) return WinError();
        const auto* ace = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(entry);
        if (ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
        if (!(ace->Header.AceFlags & INHERIT_ONLY_ACE)) {
          PSID sid = const_cast<DWORD*>(&ace->SidStart);
          if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) != 1) return EIO;
          object_level = *GetSidSubAuthority(sid, 0);
          mask = ace->Mask;
        }
        break;
      }
    }
    if (token_level < object_level) {
      // These are the file-specific rights in the generic read/write/execute
      // mappings; their common READ_CONTROL/SYNCHRONIZE bits are not writes.
      const DWORD denied = ((mask & SYSTEM_MANDATORY_LABEL_NO_READ_UP) ?
          FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES : 0) |
          ((mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) ?
          FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER : 0) |
          ((mask & SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP) ? FILE_EXECUTE : 0);
      if (desired & denied) return EACCES;
    }
  }
  GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
  PRIVILEGE_SET privileges{};
  DWORD privilege_size = sizeof(privileges), granted = 0;
  BOOL allowed = FALSE;
  if (!AccessCheck(descriptor, token.value, desired, &mapping, &privileges,
                   &privilege_size, &granted, &allowed)) return WinError();
  return allowed ? 0 : EACCES;
}

}  // namespace gesher::xattr

namespace {

struct Target {
  Handle original;
  Handle reopened;
  bool eligible = true;

  HANDLE File() const { return reopened ? reopened.value : original.value; }
};

int CheckTarget(Target* target, const Request& request) {
  if (GetFileType(target->File()) != FILE_TYPE_DISK || gesher::IsSymlinkHandle(target->File())) {
    target->eligible = false;
    return 0;
  }
  if (request.Mutates()) {
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(target->File(), FileBasicInfo, &info, sizeof(info))) return WinError();
    if ((info.FileAttributes & FILE_ATTRIBUTE_READONLY) && !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      return EACCES;
  }
  // Different Windows hosts cannot share the compare-and-update mutex.
  // Local redirectors and volumes without real EAs are not emulated.
  if (!Native().volume) return EOPNOTSUPP;
  IO_STATUS_BLOCK io{};
  FILE_FS_DEVICE_INFORMATION device{};
  const LONG status = Native().volume(target->File(), &io, &device, sizeof(device), FileFsDeviceInformation);
  if (status != 0) return NativeError(status);
  constexpr ULONG kFileRemoteDevice = 0x10;
  if (device.Characteristics & kFileRemoteDevice) return EOPNOTSUPP;
  return 0;
}

int OpenPath(const char* path, bool follow, const Request& request, Target* target) {
  char stable_path[4096];
  int error = ReadString(path, stable_path, sizeof(stable_path), ENAMETOOLONG);
  if (error) return error;
  std::wstring windows;
  if (!gesher::DiskPath(stable_path, &windows, EOPNOTSUPP)) return errno;
  // Open the link itself when requested. Inspect the resulting object before
  // asking for write rights, so lsetxattr cannot ever tag the link target.
  DWORD flags = kOpenFlags | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
  // The initial open only pins/classifies the object. The native reopen below
  // checks data and EA rights without backup-intent privilege overrides.
  DWORD access = FILE_READ_ATTRIBUTES | FILE_READ_EA;
  target->original.value = CreateFileW(windows.c_str(), access, kSharing, nullptr, OPEN_EXISTING, flags, nullptr);
  if (!target->original) return WinError();
  error = CheckTarget(target, request);
  if (error || !target->eligible) return error;
  return gesher::xattr::ReopenFileAccess(target->original.value, RequiredAccess(request), &target->reopened.value);
}

int OpenFd(int fd, const Request& request, bool allow_path, Target* target) {
  if (fd < 0) return EBADF;
  {
    // Pin the opened object before any participating close/dup2 can reuse fd.
    gesher::record_locks::DescriptorGuard guard;
    HANDLE file = gesher::HandleFromFd(fd);
    if (!file || file == INVALID_HANDLE_VALUE) return EBADF;
    HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process, file, process, &target->original.value, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return WinError();
  }
  uint32_t granted = 0;
  int error = gesher::record_locks::HandleAccess(target->original.value, &granted);
  if (error) return error;
  // Our O_PATH descriptors lack all of the data and EA rights of an open fd.
  const bool path_only = !(granted &
      (FILE_READ_EA | FILE_WRITE_EA | FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA));
  if (path_only) {
    if (!allow_path) return EBADF;
    // Bionic's fget/fset/flist fallback follows /proc/self/fd, which pins the
    // referred object, including an O_PATH|O_NOFOLLOW symlink itself. Open the
    // held object for classification without ever resolving its current name.
    error = gesher::xattr::ReopenFileAccess(target->original.value, FILE_READ_ATTRIBUTES | FILE_READ_EA,
                             &target->reopened.value);
    if (error) return error;
  }
  error = CheckTarget(target, request);
  if (error || !target->eligible) return error;
  const DWORD native_access = FILE_READ_EA | FILE_READ_ATTRIBUTES | READ_CONTROL |
                              (request.Mutates() ? FILE_WRITE_EA : 0);
  // Prefer the filesystem's current-token access check, including its native
  // security policies. A descriptor's access mode does not limit this request.
  Handle authorized;
  LONG native_status = 0;
  error = gesher::xattr::ReopenFileAccess(target->original.value, RequiredAccess(request), &authorized.value, &native_status);
  if (!error) std::swap(target->reopened.value, authorized.value);
  // Only a native STATUS_DELETE_PENDING permits using the held object. It must
  // already have the actual EA rights, and its current security is rechecked.
  if (static_cast<ULONG>(native_status) == 0xc0000056 && !path_only && (granted & native_access) == native_access)
    return gesher::xattr::CheckHeldFileAccess(target->original.value, RequiredAccess(request));
  return error;
}

int ReadStore(HANDLE file, unsigned char* buffer, std::string_view* store) {
  struct Query {
    ULONG next = 0;
    UCHAR name_size = sizeof(kNativeName) - 1;
    char name[sizeof(kNativeName)];
  } request;
  std::memcpy(request.name, kNativeName, sizeof(request.name));
  if (!Native().query) return EOPNOTSUPP;
  IO_STATUS_BLOCK io{};
  constexpr ULONG kQuerySize = offsetof(Query, name) + sizeof(kNativeName);
  const LONG status = Native().query(file, &io, buffer, kValueOffset + kMaxStore, TRUE,
                                    &request, kQuerySize, nullptr, TRUE);
  const ULONG code = static_cast<ULONG>(status);
  if (code == 0xc0000051 || code == 0xc0000052 || code == 0x80000012) {
    // No matching entry, no EAs on this file, or end of the EA list.
    *store = {};
    return 0;
  }
  if (status != 0) return NativeError(status);
  NativeEa header{};
  if (io.Information < kValueOffset) return EIO;
  std::memcpy(&header, buffer, sizeof(header));
  if (header.next || header.name_size != sizeof(kNativeName) - 1 ||
      header.value_size > kMaxStore || io.Information < kValueOffset + header.value_size ||
      std::memcmp(buffer + sizeof(header), kNativeName, sizeof(kNativeName)) != 0) return EIO;
  *store = {reinterpret_cast<const char*>(buffer + kValueOffset), header.value_size};
  return 0;
}

int Decode(std::string_view store, Attributes* attributes) {
  if (store.empty()) return 0;
  if (!store.starts_with(std::string_view(kSignature, sizeof(kSignature)))) return EIO;
  store.remove_prefix(sizeof(kSignature));
  while (!store.empty()) {
    if (store.size() < 3) return EIO;
    const size_t name_size = static_cast<unsigned char>(store[0]);
    const size_t value_size = static_cast<unsigned char>(store[1]) |
                              (static_cast<unsigned char>(store[2]) << 8);
    store.remove_prefix(3);
    if (name_size + value_size > store.size()) return EIO;
    const auto name = store.substr(0, name_size);
    if (CheckName(name) || name.find('\0') != std::string_view::npos ||
        !attributes->emplace(name, store.substr(name_size, value_size)).second) return EIO;
    store.remove_prefix(name_size + value_size);
  }
  return 0;
}

int Commit(HANDLE file, const Attributes& attributes) {
  if (!Native().set) return EOPNOTSUPP;
  const size_t start = attributes.empty() ? 0 : sizeof(kSignature);
  size_t length = start;
  for (const auto& [name, value] : attributes) length += 3 + name.size() + value.size();
  if (length > kMaxStore) return ENOSPC;
  std::unique_ptr<unsigned char[]> buffer(new (std::nothrow) unsigned char[kValueOffset + length]);
  if (!buffer) return ENOMEM;
  const NativeEa header{0, 0, sizeof(kNativeName) - 1, static_cast<USHORT>(length)};
  std::memcpy(buffer.get(), &header, sizeof(header));
  std::memcpy(buffer.get() + sizeof(header), kNativeName, sizeof(kNativeName));
  unsigned char* next = buffer.get() + kValueOffset;
  if (start) {
    std::memcpy(next, kSignature, start);
    next += start;
  }
  for (const auto& [name, value] : attributes) {
    *next++ = static_cast<unsigned char>(name.size());
    *next++ = static_cast<unsigned char>(value.size());
    *next++ = static_cast<unsigned char>(value.size() >> 8);
    std::memcpy(next, name.data(), name.size());
    next += name.size();
    if (!value.empty()) std::memcpy(next, value.data(), value.size());
    next += value.size();
  }
  // A single native EA write publishes the complete change. A zero-length
  // native value deletes the record after the last Android attribute is gone.
  IO_STATUS_BLOCK io{};
  const LONG status = Native().set(file, &io, buffer.get(), static_cast<ULONG>(kValueOffset + length));
  return status == 0 ? 0 : NativeError(status);
}

class FileMutex {
 public:
  int Lock(HANDLE file) {
    gesher::record_locks::File identity{};
    const int error = gesher::record_locks::FileIdentity(file, &identity);
    if (error) return error;
    wchar_t name[80];
    const int length = swprintf(name, sizeof(name) / sizeof(*name), L"Global\\Gesher.Xattr.v1.%016llX.%016llX.%016llX",
                                static_cast<unsigned long long>(identity.volume),
                                static_cast<unsigned long long>(identity.id_low),
                                static_cast<unsigned long long>(identity.id_high));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(name) / sizeof(*name)) return EIO;
    handle_.value = CreateMutexExW(nullptr, name, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    if (!handle_) return WinError();
    const DWORD status = WaitForSingleObject(handle_.value, INFINITE);
    locked_ = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    return locked_ ? 0 : WinError();
  }
  ~FileMutex() {
    if (locked_) ReleaseMutex(handle_.value);
  }

 private:
  Handle handle_;
  bool locked_ = false;
};

ssize_t CopyResult(const Request& request, std::string_view value) {
  if (request.length != 0 && request.length < value.size()) return errno = ERANGE, -1;
  if (request.length != 0 && !WriteUser(request.output, value)) return errno = EFAULT, -1;
  return static_cast<ssize_t>(value.size());
}

ssize_t Perform(const Target& target, const Request& request) {
  char stable_name[kMaxName + 1];
  std::string_view name;
  if (request.operation != Operation::kList) {
    const int read_error = ReadString(request.name, stable_name, sizeof(stable_name), ERANGE);
    if (read_error) return errno = read_error, -1;
    name = stable_name;
    const int name_error = CheckName(name);
    if (name_error) return errno = name_error, -1;
  }
  if (!target.eligible) {
    if (request.operation == Operation::kList) return 0;
    return errno = request.Mutates() ? EPERM : ENODATA, -1;
  }
  std::unique_ptr<char[]> input;
  if (request.operation == Operation::kSet) {
    if (request.flags & ~(XATTR_CREATE | XATTR_REPLACE)) return errno = EINVAL, -1;
    if (request.length > kMaxValue || request.length > kMaxStore - sizeof(kSignature) - 3 - name.size())
      return errno = E2BIG, -1;
    if (request.length != 0) {
      input.reset(new (std::nothrow) char[request.length]);
      if (!input) return errno = ENOMEM, -1;
      if (!ReadUser(request.input, input.get(), request.length)) return errno = EFAULT, -1;
    }
  }
  std::unique_ptr<unsigned char[]> buffer(new (std::nothrow) unsigned char[kValueOffset + kMaxStore]);
  if (!buffer) return errno = ENOMEM, -1;
  FileMutex mutex;
  if (request.Mutates()) {
    const int error = mutex.Lock(target.File());
    if (error) return errno = error, -1;
  }
  std::string_view store;
  Attributes attributes;
  int error = ReadStore(target.File(), buffer.get(), &store);
  if (!error) error = Decode(store, &attributes);
  if (error) return errno = error, -1;

  if (request.operation == Operation::kList) {
    std::string names;
    for (const auto& attribute : attributes) {
      names.append(attribute.first);
      names.push_back('\0');
    }
    return CopyResult(request, names);
  }
  const auto existing = attributes.find(name);
  if (request.operation == Operation::kGet)
    return existing == attributes.end() ? (errno = ENODATA, -1) : CopyResult(request, existing->second);
  if (request.operation == Operation::kRemove) {
    if (existing == attributes.end()) return errno = ENODATA, -1;
    attributes.erase(existing);
  } else {
    if (existing != attributes.end() && (request.flags & XATTR_CREATE)) return errno = EEXIST, -1;
    if (existing == attributes.end() && (request.flags & XATTR_REPLACE)) return errno = ENODATA, -1;
    attributes.insert_or_assign(name, std::string_view(input ? input.get() : "", request.length));
  }
  error = Commit(target.File(), attributes);
  return error ? (errno = error, -1) : 0;
}

ssize_t PathRequest(const char* path, bool follow, const Request& request) {
  Target target;
  const int error = OpenPath(path, follow, request, &target);
  return error ? (errno = error, -1) : Perform(target, request);
}

ssize_t FdRequest(int fd, const Request& request, bool allow_path = false) {
  Target target;
  const int error = OpenFd(fd, request, allow_path, &target);
  return error ? (errno = error, -1) : Perform(target, request);
}

}  // namespace

extern "C" {

ssize_t getxattr(const char* path, const char* name, void* value, size_t size) {
  return PathRequest(path, true, {Operation::kGet, name, value, nullptr, size});
}
ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size) {
  return PathRequest(path, false, {Operation::kGet, name, value, nullptr, size});
}
ssize_t fgetxattr(int fd, const char* name, void* value, size_t size) {
  return FdRequest(fd, {Operation::kGet, name, value, nullptr, size});
}
int setxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  return static_cast<int>(PathRequest(path, true, {Operation::kSet, name, nullptr, value, size, flags}));
}
int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  return static_cast<int>(PathRequest(path, false, {Operation::kSet, name, nullptr, value, size, flags}));
}
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
  return static_cast<int>(FdRequest(fd, {Operation::kSet, name, nullptr, value, size, flags}));
}
ssize_t listxattr(const char* path, char* list, size_t size) {
  return PathRequest(path, true, {Operation::kList, nullptr, list, nullptr, size});
}
ssize_t llistxattr(const char* path, char* list, size_t size) {
  return PathRequest(path, false, {Operation::kList, nullptr, list, nullptr, size});
}
ssize_t flistxattr(int fd, char* list, size_t size) {
  return FdRequest(fd, {Operation::kList, nullptr, list, nullptr, size});
}
int removexattr(const char* path, const char* name) {
  return static_cast<int>(PathRequest(path, true, {Operation::kRemove, name}));
}
int lremovexattr(const char* path, const char* name) {
  return static_cast<int>(PathRequest(path, false, {Operation::kRemove, name}));
}
int fremovexattr(int fd, const char* name) {
  return static_cast<int>(FdRequest(fd, {Operation::kRemove, name}));
}

}  // extern "C"

namespace gesher {

// The public Bionic provider has an O_PATH fallback for these three calls.
// Keep the POSIX entry points above as the legacy raw-kernel contracts; both
// syscall-number tables route to them through Bionic's __f*xattr shims.
ssize_t BionicFgetxattr(int fd, const char* name, void* value, size_t size) {
  return FdRequest(fd, {Operation::kGet, name, value, nullptr, size}, true);
}
int BionicFsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
  return static_cast<int>(FdRequest(fd, {Operation::kSet, name, nullptr, value, size, flags}, true));
}
ssize_t BionicFlistxattr(int fd, char* list, size_t size) {
  return FdRequest(fd, {Operation::kList, nullptr, list, nullptr, size}, true);
}

}  // namespace gesher

// Revocation is a shared capability, not a flag attached to a recycled fd.
// The section and mutex have random names so Binder/exec aliases can open the
// same lifetime without exporting a process-local pointer or closing a remote
// handle. Consumers can read the section, but only its controller writes it.
#include <gesher/revocable_file.h>
#include "revocable_internal.h"
#include "ofd_internal.h"
#include "internal.h"

#include <bcrypt.h>
#include <fcntl.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

namespace gesher {
namespace {
constexpr uint64_t kMagic = 0x3156455246485347ull;  // GSHFREV1
struct SharedRevocation {
  uint64_t magic;
  uint32_t version;
  volatile LONG revoked;
};

bool ValidName(const char* id) {
  for (unsigned i = 0; i != 32; ++i) {
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }
  return id[32] == '\0';
}
std::wstring Name(const char* id, const wchar_t* suffix) {
  std::wstring name = L"Local\\GesherRevocable-";
  for (unsigned i = 0; i != 32; ++i) name += static_cast<wchar_t>(id[i]);
  return name + suffix;
}
}  // namespace

struct RevocableFileGroup {
  char id[33]{};
  HANDLE mapping = nullptr;
  HANDLE mutex = nullptr;
  SharedRevocation* view = nullptr;

  ~RevocableFileGroup() {
    if (view) UnmapViewOfFile(view);
    if (mutex) CloseHandle(mutex);
    if (mapping) CloseHandle(mapping);
  }
  int Lock() {
    DWORD result = WaitForSingleObject(mutex, INFINITE);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return 0;
    return ErrnoFromWin32(GetLastError());
  }
  bool Revoked() const {
    // Reading a naturally aligned LONG is atomic on supported Windows hosts.
    // The kernel mutex supplies the inter-process acquire/release boundary.
    return view->revoked != 0;
  }
};

namespace {
std::mutex controllers_mutex;
std::unordered_map<uint64_t, std::shared_ptr<RevocableFileGroup>> controllers;
uint64_t next_controller = 0;

std::shared_ptr<RevocableFileGroup> Controller(uint64_t id) {
  std::lock_guard<std::mutex> lock(controllers_mutex);
  const auto found = controllers.find(id);
  return found == controllers.end() ? nullptr : found->second;
}

std::shared_ptr<RevocableFileGroup> OpenGroup(const char* id) {
  if (!ValidName(id)) { errno = EINVAL; return {}; }
  auto group = std::make_shared<RevocableFileGroup>();
  std::memcpy(group->id, id, sizeof(group->id));
  group->mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, Name(id, L"-state").c_str());
  if (!group->mapping) { errno = EPERM; return {}; }  // retired before adoption
  group->view = static_cast<SharedRevocation*>(MapViewOfFile(group->mapping, FILE_MAP_READ,
                                                             0, 0, sizeof(SharedRevocation)));
  if (!group->view) { FailWithLastError(); return {}; }
  if (group->view->magic != kMagic || group->view->version != 1) { errno = EINVAL; return {}; }
  group->mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, Name(id, L"-io").c_str());
  if (!group->mutex) { errno = EPERM; return {}; }
  return group;
}
}  // namespace

uint64_t CreateRevocableFileGroup() try {
  auto group = std::make_shared<RevocableFileGroup>();
  unsigned char random[16];
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    errno = EIO;
    return 0;
  }
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i != sizeof(random); ++i) {
    group->id[i * 2] = digits[random[i] >> 4];
    group->id[i * 2 + 1] = digits[random[i] & 15];
  }
  group->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(SharedRevocation), Name(group->id, L"-state").c_str());
  if (!group->mapping) { FailWithLastError(); return 0; }
  if (GetLastError() == ERROR_ALREADY_EXISTS) { errno = EEXIST; return 0; }
  group->view = static_cast<SharedRevocation*>(MapViewOfFile(group->mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                                             0, 0, sizeof(SharedRevocation)));
  if (!group->view) { FailWithLastError(); return 0; }
  group->view->magic = kMagic;
  group->view->version = 1;
  group->view->revoked = 0;
  group->mutex = CreateMutexW(nullptr, FALSE, Name(group->id, L"-io").c_str());
  if (!group->mutex) { FailWithLastError(); return 0; }
  if (GetLastError() == ERROR_ALREADY_EXISTS) { errno = EEXIST; return 0; }
  std::lock_guard<std::mutex> lock(controllers_mutex);
  // Exhaustion must not reuse a still-live or previously released token.
  if (next_controller == UINT64_MAX) { errno = EOVERFLOW; return 0; }
  const uint64_t id = ++next_controller;
  controllers.emplace(id, std::move(group));
  return id;
} catch (const std::bad_alloc&) {
  errno = ENOMEM;
  return 0;
}

int AttachRevocableFileGroup(uint64_t controller, int fd) try {
  auto group = Controller(controller);
  if (!group) return errno = EINVAL, -1;
  auto snapshot = CaptureDescriptor(fd);
  if (!snapshot) return -1;
  if (snapshot->kind != DescriptorKind::kFile || GetFileType(snapshot->handle) != FILE_TYPE_DISK)
    return errno = EINVAL, -1;
  if (FileDescriptionFlags(snapshot) < 0) return -1;
  const int error = group->Lock();
  if (error) return errno = error, -1;
  struct Unlock { HANDLE mutex; ~Unlock() { ReleaseMutex(mutex); } } unlock{group->mutex};
  if (group->Revoked()) return errno = EPERM, -1;
  std::lock_guard<std::mutex> lock(snapshot->description->mutex);
  auto& description = *snapshot->description;
  if (!description.Share()) return -1;
  auto* shared = description.shared.load(std::memory_order_acquire);
  if (shared->revocation[0] != '\0') return errno = EBUSY, -1;
  std::memcpy(shared->revocation, group->id, sizeof(shared->revocation));
  description.revocation = std::move(group);
  return 0;
} catch (const std::bad_alloc&) {
  return errno = ENOMEM, -1;
}

int RevokeFileGroup(uint64_t controller) {
  auto group = Controller(controller);
  if (!group) return errno = EINVAL, -1;
  // Stop new admission before waiting for a currently executing operation.
  // Otherwise an app issuing continuous writes could keep winning the mutex
  // and indefinitely postpone session finalization.
  InterlockedExchange(&group->view->revoked, 1);
  const int error = group->Lock();
  if (error) return errno = error, -1;
  ReleaseMutex(group->mutex);
  return 0;
}

void ReleaseRevocableFileGroup(uint64_t controller) {
  std::shared_ptr<RevocableFileGroup> retired;
  {
    std::lock_guard<std::mutex> lock(controllers_mutex);
    auto found = controllers.find(controller);
    if (found == controllers.end()) return;
    retired = std::move(found->second);
    controllers.erase(found);
  }
  // Dropping the controller never converts a capability into an ordinary fd.
  // Normal callers already drained I/O through RevokeFileGroup().
  InterlockedExchange(&retired->view->revoked, 1);
}

FileOperationLease::FileOperationLease(const DescriptorSnapshotPtr& snapshot) {
  try {
    if (!snapshot || !snapshot->description) { error_ = EBADF; return; }
    auto& description = *snapshot->description;
    auto* shared = description.shared.load(std::memory_order_acquire);
    if (!shared || shared->revocation[0] == '\0') return;
    {
      std::lock_guard<std::mutex> lock(description.mutex);
      if (!description.revocation) description.revocation = OpenGroup(shared->revocation);
      group_ = description.revocation;
    }
    if (!group_) { error_ = errno ? errno : EPERM; return; }
    if (group_->Revoked()) { error_ = EPERM; return; }
    error_ = group_->Lock();
    if (error_) return;
    locked_ = true;
    if (group_->Revoked()) error_ = EPERM;
  } catch (const std::bad_alloc&) {
    error_ = ENOMEM;
  }
}

FileOperationLease::~FileOperationLease() {
  if (locked_) ReleaseMutex(group_->mutex);
}

int ReopenRevocableFileDescription(const DescriptorSnapshotPtr& source, int flags) try {
  FileOperationLease operation(source);
  if (operation.Error()) return errno = operation.Error(), -1;
  if (!FileDescriptionIsRevocable(source)) return errno = EINVAL, -1;
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  if ((flags & O_DIRECT) != 0 && (flags & O_PATH) == 0) return errno = EINVAL, -1;
  if ((flags & O_NOATIME) != 0 && (flags & O_PATH) == 0) return errno = EOPNOTSUPP, -1;
  const int original = FileDescriptionFlags(source);
  if (original < 0) return -1;
  // The capability only grants its actual data rights. ReOpenFile must not
  // upgrade a read-only session reader into a writer of its backing file.
  const bool read = (flags & O_PATH) == 0 && (flags & O_ACCMODE) != O_WRONLY;
  const bool write = (flags & O_PATH) == 0 &&
                     ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC) != 0);
  if ((read && ((original & O_PATH) != 0 || (original & O_ACCMODE) == O_WRONLY)) ||
      (write && ((original & O_PATH) != 0 || (original & O_ACCMODE) == O_RDONLY)))
    return errno = EACCES, -1;
  DWORD access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
  if (read) access |= GENERIC_READ;
  if (write) access |= GENERIC_WRITE;
  const DWORD attributes = (flags & O_DSYNC) != 0 ? FILE_FLAG_WRITE_THROUGH : 0;
  HANDLE file = ReOpenFile(source->handle, access,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, attributes);
  if (file == INVALID_HANDLE_VALUE) return FailWithLastError();
  struct Owned { HANDLE file; ~Owned() { if (file) CloseHandle(file); } } owned{file};
  if ((flags & O_TRUNC) != 0 && (flags & O_PATH) == 0) {
    FILE_END_OF_FILE_INFO end{};
    if (!SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end))) return FailWithLastError();
  }
  auto result = std::make_shared<DescriptorSnapshot>();
  result->description = std::make_shared<OpenFileDescription>();
  result->handle = file;
  owned.file = nullptr;
  result->description->SetFlags(flags | ((flags & O_PATH) == 0 ? O_LARGEFILE : 0));
  if (!result->description->Share()) return -1;
  auto* from = source->description->shared.load(std::memory_order_acquire);
  auto* to = result->description->shared.load(std::memory_order_acquire);
  std::memcpy(to->revocation, from->revocation, sizeof(to->revocation));
  {
    std::lock_guard<std::mutex> lock(source->description->mutex);
    result->description->revocation = source->description->revocation;
  }
  return AttachFileDescription(result, (flags & O_CLOEXEC) != 0);
} catch (const std::bad_alloc&) {
  return errno = ENOMEM, -1;
}
}  // namespace gesher

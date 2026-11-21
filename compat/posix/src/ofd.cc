// Status belongs to the open description, not to an fd number. The executable
// owns one table for all modules; explicitly inherited descriptions share a
// kernel section rather than copying their status into the child process.
#include "ofd_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"
#include "record_lock_liveness.h"
#include "pipe_io.h"

#include <gesher/file_description.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <winternl.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

namespace gesher {

OpenFileDescription::~OpenFileDescription() {
  if (pipe_mutex) CloseHandle(pipe_mutex);
  if (Shared* view = shared.load(std::memory_order_relaxed)) UnmapViewOfFile(view);
  if (mapping) CloseHandle(mapping);
}

bool OpenFileDescription::OwnedPipe() const {
  auto* view = shared.load(std::memory_order_acquire);
  return view && view->pipe_gate[0] != 0;
}
bool OpenFileDescription::KnownPipeStatus() const {
  auto* view = shared.load(std::memory_order_acquire);
  return view && view->pipe_gate[0] != 0 &&
      InterlockedCompareExchange(&view->pipe_native, 0, 0) == 0 &&
      InterlockedCompareExchange(&view->pipe_transition, 0, 0) == 0;
}
void OpenFileDescription::ExposePipe(bool exposed) {
  InterlockedExchange(&shared.load(std::memory_order_acquire)->pipe_native, exposed ? 1 : 0);
}
void OpenFileDescription::PipeTransition(bool changing) {
  InterlockedExchange(&shared.load(std::memory_order_acquire)->pipe_transition, changing ? 1 : 0);
}

int OpenFileDescription::Flags() const {
  if (Shared* view = shared.load(std::memory_order_acquire)) {
    return static_cast<int>(InterlockedCompareExchange(&view->flags, 0, 0));
  }
  return local_flags.load(std::memory_order_acquire);
}

void OpenFileDescription::SetFlags(int flags) {
  if (Shared* view = shared.load(std::memory_order_acquire)) {
    InterlockedExchange(&view->flags, static_cast<LONG>(flags));
  } else {
    local_flags.store(flags, std::memory_order_release);
  }
  initialized.store(true, std::memory_order_release);
}

HANDLE OpenFileDescription::Share() {
  if (mapping) return mapping;
  HANDLE candidate = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(Shared), nullptr);
  if (!candidate) { FailWithLastError(); return nullptr; }
  auto* view = static_cast<Shared*>(MapViewOfFile(candidate, FILE_MAP_READ | FILE_MAP_WRITE,
                                                 0, 0, sizeof(Shared)));
  if (!view) {
    DWORD error = GetLastError();
    CloseHandle(candidate);
    errno = ErrnoFromWin32(error);
    return nullptr;
  }
  std::memset(view, 0, sizeof(*view));
  view->magic = ofd_transfer::kMagic;
  view->version = ofd_transfer::kVersion;
  std::memset(view->revocation, 0, sizeof(view->revocation));
  InterlockedExchange(&view->flags, static_cast<LONG>(local_flags.load()));
  mapping = candidate;
  shared.store(view, std::memory_order_release);
  return mapping;
}

bool OpenFileDescription::Adopt(HANDLE inherited) {
  auto* view = static_cast<Shared*>(MapViewOfFile(inherited, FILE_MAP_READ | FILE_MAP_WRITE,
                                                 0, 0, sizeof(Shared)));
  if (!view) { FailWithLastError(); return false; }
  if (view->magic != ofd_transfer::kMagic || view->version != ofd_transfer::kVersion) {
    UnmapViewOfFile(view);
    errno = EINVAL;
    return false;
  }
  HANDLE gate = nullptr;
  if (view->pipe_gate[0]) {
    bool valid = view->pipe_gate[32] == 0;
    std::wstring name = L"Local\\gesher-pipe-";
    for (unsigned i = 0; i != 32; ++i) {
      char digit = view->pipe_gate[i];
      valid = valid && ((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'));
      name += static_cast<wchar_t>(digit);
    }
    if (!valid) { UnmapViewOfFile(view); errno = EINVAL; return false; }
    // The sender can retire its last local description after transferring
    // the file and shared state, before this process starts importing them.
    // The named gate then has no handles and Windows destroys it. Every
    // active operation pins its description (and therefore its gate), so a
    // missing gate has no active owner. Open or recreate that same gate,
    // without resetting the shared flags or an existing mutex's ownership.
    gate = CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    if (!gate) {
      DWORD error = GetLastError();
      UnmapViewOfFile(view);
      errno = ErrnoFromWin32(error);
      return false;
    }
  }
  // A descendant receives another explicit duplicate. Never allow the private
  // bookkeeping handle to escape through an unrelated native process launch.
  if (!SetHandleInformation(inherited, HANDLE_FLAG_INHERIT, 0)) {
    DWORD error = GetLastError();
    if (gate) CloseHandle(gate);
    UnmapViewOfFile(view);
    errno = ErrnoFromWin32(error);
    return false;
  }
  mapping = inherited;
  pipe_mutex = gate;
  shared.store(view, std::memory_order_release);
  initialized.store(true, std::memory_order_release);
  return true;
}

DescriptorSnapshot::~DescriptorSnapshot() {
  if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

namespace {
struct Table {
  std::mutex mutex;
  // UCRT's real low-I/O descriptor ceiling. Fixed slots avoid a heap node and
  // allocator contention for every dup; the shared object is allocated once.
  std::array<std::shared_ptr<OpenFileDescription>, ofd_transfer::kDescriptorLimit> entries;
  INIT_ONCE imported = INIT_ONCE_STATIC_INIT;
  int import_error = 0;
};
INIT_ONCE table_once = INIT_ONCE_STATIC_INIT;
Table* table = nullptr;

BOOL CALLBACK CreateTable(PINIT_ONCE, void*, void**) {
  // MinGW calls our setvbuf(stderr) before its invalid-parameter handler or
  // C++ initializers run. That call lazily imports the child's descriptors,
  // including closing requested stdio slots. Its subsequent descriptor probe
  // must return EBADF, not invoke the UCRT's default process termination.
  InitializeCrtDescriptorErrors();
  void* storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Table));
  if (!storage) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  // Process lifetime: no static destructor closes descriptions while another
  // DLL or a CRT exit handler still uses the process's descriptors.
  table = new (storage) Table{};
  return TRUE;
}

bool ValidSlot(int fd) {
  return fd >= 0 && static_cast<unsigned>(fd) < ofd_transfer::kDescriptorLimit;
}

int NativeFlags(HANDLE file) {
  uint32_t access = 0;
  int error = record_locks::HandleAccess(file, &access);
  if (error) return errno = error, -1;
  const DWORD type = GetFileType(file);
  const bool read = (access & FILE_READ_DATA) != 0;
  // FILE_APPEND_DATA aliases FILE_CREATE_PIPE_INSTANCE for named pipes;
  // instance-creation permission is not permission to write pipe data.
  const bool write = (access & FILE_WRITE_DATA) != 0 ||
                     (type == FILE_TYPE_DISK && (access & FILE_APPEND_DATA) != 0);
  int flags = read && write ? O_RDWR : write ? O_WRONLY : read ? O_RDONLY : O_PATH;
  if (type == FILE_TYPE_DISK && write && (access & FILE_WRITE_DATA) == 0) flags |= O_APPEND;
  if (type == FILE_TYPE_DISK && (flags & O_PATH) == 0) {
    flags |= O_LARGEFILE;
    using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
    using Error = ULONG(WINAPI*)(LONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationFile"))) : nullptr;
    auto convert = ntdll ? reinterpret_cast<Error>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError"))) : nullptr;
    if (!query || !convert) return errno = EOPNOTSUPP, -1;
    IO_STATUS_BLOCK status{};
    FILE_MODE_INFORMATION mode{};
    LONG result = query(file, &status, &mode, sizeof(mode), FileModeInformation);
    if (result < 0) return errno = ErrnoFromWin32(convert(result)), -1;
    if ((mode.Mode & FILE_WRITE_THROUGH) != 0) flags |= O_SYNC;
    if ((mode.Mode & FILE_NO_INTERMEDIATE_BUFFERING) != 0) flags |= O_DIRECT;
  }
  // Pipe wait-mode queries serialize behind synchronous I/O. A raw external
  // handle supplies actual access, but not an assumed O_NONBLOCK policy.
  return flags;
}

int InitializeSnapshotFlags(const DescriptorSnapshotPtr& snapshot) {
  if (!snapshot || !snapshot->description || !snapshot->handle || snapshot->handle == INVALID_HANDLE_VALUE)
    return errno = EBADF, -1;
  auto& description = *snapshot->description;
  if (description.initialized.load(std::memory_order_acquire)) return 0;
  std::lock_guard<std::mutex> lock(description.mutex);
  if (description.initialized.load(std::memory_order_acquire)) return 0;
  const int flags = NativeFlags(snapshot->handle);
  if (flags < 0) return -1;
  description.SetFlags(flags);
  return 0;
}

int ImportStartup(Table* state) try {
  STARTUPINFOW startup{};
  GetStartupInfoW(&startup);
  if (!startup.lpReserved2 || startup.cbReserved2 < sizeof(int) + sizeof(ofd_transfer::Startup)) return 0;
  ofd_transfer::Startup extension{};
  std::memcpy(&extension, startup.lpReserved2 + startup.cbReserved2 - sizeof(extension), sizeof(extension));
  if (extension.magic != ofd_transfer::kMagic) return 0;
  int descriptor_count = 0;
  std::memcpy(&descriptor_count, startup.lpReserved2, sizeof(descriptor_count));
  if (descriptor_count < 0 || static_cast<unsigned>(descriptor_count) > ofd_transfer::kDescriptorLimit) return EINVAL;
  const size_t crt_size = sizeof(int) + static_cast<size_t>(descriptor_count) * (1 + sizeof(intptr_t));
  if (crt_size > startup.cbReserved2 || startup.cbReserved2 - crt_size != sizeof(ofd_transfer::Startup)) return EINVAL;
  const size_t maximum = sizeof(ofd_transfer::Header) +
                         ofd_transfer::kDescriptorLimit * sizeof(ofd_transfer::Entry);
  if (extension.manifest_size < sizeof(ofd_transfer::Header) || extension.manifest_size > maximum ||
      extension.manifest_handle == 0 || extension.manifest_handle > UINTPTR_MAX) return EINVAL;
  HANDLE manifest = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(extension.manifest_handle));
  auto* bytes = static_cast<const unsigned char*>(MapViewOfFile(manifest, FILE_MAP_READ, 0, 0,
                                                               static_cast<SIZE_T>(extension.manifest_size)));
  if (!bytes) return ErrnoFromWin32(GetLastError());
  struct StartupView {
    const void* bytes;
    HANDLE manifest;
    ~StartupView() { UnmapViewOfFile(bytes); CloseHandle(manifest); }
  } cleanup{bytes, manifest};
  ofd_transfer::Header header{};
  std::memcpy(&header, bytes, sizeof(header));
  int error = 0;
  if (header.magic != ofd_transfer::kMagic || header.version != ofd_transfer::kVersion ||
      header.count > ofd_transfer::kDescriptorLimit || header.reserved != 0 || (header.closed_stdio & ~7u) != 0 ||
      extension.manifest_size != sizeof(header) + static_cast<size_t>(header.count) * sizeof(ofd_transfer::Entry)) {
    error = EINVAL;
  }
  std::unordered_map<HANDLE, std::shared_ptr<OpenFileDescription>> adopted;
  for (uint32_t index = 0; !error && index < header.count; ++index) {
    ofd_transfer::Entry entry{};
    std::memcpy(&entry, bytes + sizeof(header) + static_cast<size_t>(index) * sizeof(entry), sizeof(entry));
    if (!ValidSlot(entry.descriptor) || entry.kind > static_cast<uint32_t>(DescriptorKind::kRandom) ||
        entry.shared_handle == 0 || entry.shared_handle > UINTPTR_MAX ||
        entry.file_handle == 0 || entry.file_handle > UINTPTR_MAX || state->entries[entry.descriptor]) {
      error = EINVAL;
      break;
    }
    HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(entry.shared_handle));
    auto found = adopted.find(mapping);
    std::shared_ptr<OpenFileDescription> description;
    const bool already_adopted = found != adopted.end();
    if (already_adopted) {
      description = found->second;
    } else {
      description = std::make_shared<OpenFileDescription>();
      if (!description->Adopt(mapping)) { error = errno; break; }
      adopted.emplace(mapping, description);
    }
    if (already_adopted && description->kind.load() != static_cast<DescriptorKind>(entry.kind)) {
      error = EINVAL;
      break;
    }
    description->kind = static_cast<DescriptorKind>(entry.kind);
    HANDLE inherited = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(entry.file_handle));
    if (entry.descriptor < descriptor_count) {
      if (HandleFromFd(entry.descriptor) != inherited) { error = EBADF; break; }
    } else {
      // The UCRT block has a WORD byte length, smaller than its 8192-slot fd
      // table. Preserve high descriptors through the explicit handle manifest.
      int temporary = _open_osfhandle(reinterpret_cast<intptr_t>(inherited), _O_BINARY);
      if (temporary < 0) { error = gesher_errno_from_crt(errno); break; }
      if (temporary != entry.descriptor) {
        if (_dup2(temporary, entry.descriptor) < 0) error = gesher_errno_from_crt(errno);
        _close(temporary);
        if (error) break;
      }
      if (!SetHandleInformation(HandleFromFd(entry.descriptor), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        error = ErrnoFromWin32(GetLastError());
        break;
      }
    }
    state->entries[entry.descriptor] = std::move(description);
  }
  for (int fd = 0; !error && fd < 3; ++fd) {
    if ((header.closed_stdio & (1u << fd)) == 0) continue;
    _close(fd);
    state->entries[fd].reset();
    SetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,
                 INVALID_HANDLE_VALUE);
  }
  return error;
} catch (const std::bad_alloc&) {
  return ENOMEM;
}

BOOL CALLBACK ImportTable(PINIT_ONCE, void* parameter, void**) {
  auto* state = static_cast<Table*>(parameter);
  state->import_error = ImportStartup(state);
  return TRUE;
}

int InitializeLocal() {
  if (!InitOnceExecuteOnce(&table_once, CreateTable, nullptr, nullptr)) return FailWithLastError();
  if (!InitOnceExecuteOnce(&table->imported, ImportTable, table, nullptr)) return FailWithLastError();
  return table->import_error ? (errno = table->import_error, -1) : 0;
}

std::shared_ptr<OpenFileDescription> GetOrCreateLocked(int fd) try {
  auto& entry = table->entries[fd];
  if (entry) return entry;
  // Capture/dup must not wait for native I/O while holding the process-wide
  // transaction. A later snapshot operation discovers the real native flags
  // under this description's own mutex, with a retained native handle.
  auto description = std::make_shared<OpenFileDescription>();
  entry = description;
  return description;
} catch (const std::bad_alloc&) {
  errno = ENOMEM;
  return {};
}

int RegisterLocal(int fd, int flags) try {
  if (!ValidSlot(fd)) return errno = EBADF, -1;
  if (InitializeLocal() < 0) return -1;
  auto description = std::make_shared<OpenFileDescription>();
  const int mask = (flags & O_PATH) != 0 ? O_PATH | O_DIRECTORY | O_NOFOLLOW : kOpenDescriptionFlagsMask;
  description->SetFlags(flags & mask);
  std::lock_guard<std::mutex> lock(table->mutex);
  table->entries[fd] = std::move(description);
  return 0;
} catch (const std::bad_alloc&) {
  return errno = ENOMEM, -1;
}

int DupLocal(int old_fd, int new_fd) {
  if (!ValidSlot(old_fd) || !ValidSlot(new_fd)) return errno = EBADF, -1;
  if (InitializeLocal() < 0) return -1;
  HANDLE file = HandleFromFd(old_fd);
  if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  std::lock_guard<std::mutex> lock(table->mutex);
  auto source = GetOrCreateLocked(old_fd);
  if (!source) return -1;
  table->entries[new_fd] = std::move(source);
  return 0;
}

void ForgetLocal(int fd) {
  if (!ValidSlot(fd) || InitializeLocal() < 0) return;
  std::lock_guard<std::mutex> lock(table->mutex);
  table->entries[fd].reset();
}

bool LookupLocal(int fd, std::shared_ptr<OpenFileDescription>* out) {
  if (!ValidSlot(fd) || InitializeLocal() < 0) return false;
  std::lock_guard<std::mutex> lock(table->mutex);
  if (!table->entries[fd]) return false;
  if (out) *out = table->entries[fd];
  return true;
}

int GetFlagsLocal(int fd) {
  auto snapshot = CaptureDescriptor(fd);
  return snapshot ? FileDescriptionFlags(snapshot) : -1;
}

int SetFlagsLocal(int fd, int arg) {
  auto snapshot = CaptureDescriptor(fd);
  return snapshot ? SetFileDescriptionFlags(snapshot, arg) : -1;
}

DescriptorSnapshotPtr Snapshot(HANDLE file, const std::shared_ptr<OpenFileDescription>& description) try {
  DWORD native_flags = 0;
  if (!GetHandleInformation(file, &native_flags)) { FailWithLastError(); return {}; }
  // Allocate the control block before acquiring a native duplicate, so an
  // allocation failure cannot leak a hidden handle or prolong peer lifetime.
  auto snapshot = std::make_shared<DescriptorSnapshot>();
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) { FailWithLastError(); return {}; }
  snapshot->handle = duplicate;
  snapshot->descriptor_flags = (native_flags & HANDLE_FLAG_INHERIT) != 0 ? 0 : FD_CLOEXEC;
  snapshot->description = description;
  snapshot->kind = description->kind.load();
  return snapshot;
} catch (const std::bad_alloc&) {
  errno = ENOMEM;
  return {};
}

DescriptorSnapshotPtr CaptureLocal(int fd) {
  record_locks::DescriptorGuard guard;
  if (!ValidSlot(fd)) { errno = EBADF; return {}; }
  if (InitializeLocal() < 0) return {};
  HANDLE file = HandleFromFd(fd);
  if (file == INVALID_HANDLE_VALUE) { errno = EBADF; return {}; }
  std::shared_ptr<OpenFileDescription> description;
  {
    std::lock_guard<std::mutex> lock(table->mutex);
    description = GetOrCreateLocked(fd);
  }
  return description ? Snapshot(file, description) : DescriptorSnapshotPtr{};
}

DescriptorSnapshotPtr CaptureHandleLocal(HANDLE file) try {
  record_locks::DescriptorGuard guard;
  if (!file || file == INVALID_HANDLE_VALUE) { errno = EBADF; return {}; }
  if (InitializeLocal() < 0) return {};
  std::shared_ptr<OpenFileDescription> description;
  {
    std::lock_guard<std::mutex> lock(table->mutex);
    for (size_t fd = 0; fd < table->entries.size(); ++fd) {
      if (!table->entries[fd]) continue;
      HANDLE current = HandleFromFd(static_cast<int>(fd));
      if (current == INVALID_HANDLE_VALUE) continue;
      bool same = current == file;
      if (!same) {
        int error = record_locks::SameDescription(file, current, &same);
        if (error) { errno = error; return {}; }
      }
      if (same) { description = table->entries[fd]; break; }
    }
    if (!description) {
      description = std::make_shared<OpenFileDescription>();
    }
  }
  return Snapshot(file, description);
} catch (const std::bad_alloc&) {
  errno = ENOMEM;
  return {};
}

int AttachLocal(const DescriptorSnapshotPtr& snapshot, bool close_on_exec) {
  if (!snapshot || !snapshot->description || !snapshot->handle || snapshot->handle == INVALID_HANDLE_VALUE)
    return errno = EBADF, -1;
  if (snapshot->kind == DescriptorKind::kUnshareable) return errno = EOPNOTSUPP, -1;
  // Probe the pinned file before taking the descriptor-table locks. A file
  // attribute query must not hold up unrelated descriptor operations.
  const bool memfd = FileDescriptionIsMemfd(snapshot);
  record_locks::DescriptorGuard guard;
  if (InitializeLocal() < 0) return -1;
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), snapshot->handle, GetCurrentProcess(), &duplicate,
                       0, close_on_exec ? FALSE : TRUE, DUPLICATE_SAME_ACCESS)) return FailWithLastError();
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate),
                           _O_BINARY | (close_on_exec ? _O_NOINHERIT : 0));
  if (fd < 0) {
    int error = gesher_errno_from_crt(errno);
    CloseHandle(duplicate);
    return errno = error, -1;
  }
  if (!ValidSlot(fd)) {
    _close(fd);
    return errno = EMFILE, -1;
  }
  if (!SetHandleInformation(HandleFromFd(fd), HANDLE_FLAG_INHERIT,
                            close_on_exec ? 0 : HANDLE_FLAG_INHERIT)) {
    int error = ErrnoFromWin32(GetLastError());
    _close(fd);
    return errno = error, -1;
  }
  std::lock_guard<std::mutex> lock(table->mutex);
  table->entries[fd] = snapshot->description;
  if (memfd) RegisterMemfd(fd);
  return fd;
}

int SetKindLocal(int fd, DescriptorKind kind) {
  record_locks::DescriptorGuard guard;
  if (!ValidSlot(fd) || InitializeLocal() < 0) return errno = EBADF, -1;
  HANDLE file = HandleFromFd(fd);
  if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  std::lock_guard<std::mutex> lock(table->mutex);
  auto description = GetOrCreateLocked(fd);
  if (!description) return -1;
  description->kind = kind;
  return 0;
}

DescriptorKind GetKindLocal(int fd) {
  std::shared_ptr<OpenFileDescription> description;
  return LookupLocal(fd, &description) ? description->kind.load() : DescriptorKind::kFile;
}

HANDLE ShareLocal(const DescriptorSnapshotPtr& snapshot) {
  if (!snapshot || !snapshot->description) { errno = EINVAL; return nullptr; }
  if (InitializeSnapshotFlags(snapshot) < 0) return nullptr;
  std::lock_guard<std::mutex> lock(snapshot->description->mutex);
  return snapshot->description->Share();
}

int CaptureAllLocal(std::vector<std::pair<int, DescriptorSnapshotPtr>>* out, bool include_close_on_exec) try {
  if (!out) return errno = EINVAL, -1;
  const int saved = errno;
  record_locks::DescriptorGuard guard;
  if (InitializeLocal() < 0) return -1;
  out->clear();
  for (unsigned fd = 0; fd < ofd_transfer::kDescriptorLimit; ++fd) {
    HANDLE file = HandleFromFd(static_cast<int>(fd));
    if (file == INVALID_HANDLE_VALUE) continue;
    DWORD native_flags = 0;
    if (!GetHandleInformation(file, &native_flags)) {
      int error = ErrnoFromWin32(GetLastError());
      out->clear();
      return errno = error, -1;
    }
    if (!include_close_on_exec && (native_flags & HANDLE_FLAG_INHERIT) == 0) continue;
    auto snapshot = CaptureLocal(static_cast<int>(fd));
    if (!snapshot) {
      int error = errno;
      out->clear();
      return errno = error, -1;
    }
    out->emplace_back(static_cast<int>(fd), std::move(snapshot));
  }
  errno = saved;
  return 0;
} catch (const std::bad_alloc&) {
  out->clear();
  return errno = ENOMEM, -1;
}

struct OwnerApi {
  int (*initialize)();
  int (*register_fd)(int, int);
  int (*duplicate)(int, int);
  void (*forget)(int);
  bool (*lookup)(int, std::shared_ptr<OpenFileDescription>*);
  int (*set_kind)(int, DescriptorKind);
  DescriptorKind (*get_kind)(int);
  int (*get_flags)(int);
  int (*set_flags)(int, int);
  DescriptorSnapshotPtr (*capture_fd)(int);
  DescriptorSnapshotPtr (*capture_handle)(HANDLE);
  int (*attach)(const DescriptorSnapshotPtr&, bool);
  int (*capture_all)(std::vector<std::pair<int, DescriptorSnapshotPtr>>*, bool);
  HANDLE (*share)(const DescriptorSnapshotPtr&);
};

const OwnerApi* Owner() {
  using Factory = const OwnerApi* (*)();
  static auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_ofd_owner_v1")));
  if (!factory) { errno = ENOSYS; return nullptr; }
  return factory();
}
}  // namespace

extern "C" void gesher_ofd_owner_anchor_v1() {}
extern "C" __declspec(dllexport) const OwnerApi* gesher_ofd_owner_v1() {
  static const OwnerApi api{InitializeLocal, RegisterLocal, DupLocal, ForgetLocal,
                            LookupLocal, SetKindLocal, GetKindLocal, GetFlagsLocal, SetFlagsLocal, CaptureLocal,
                            CaptureHandleLocal, AttachLocal, CaptureAllLocal, ShareLocal};
  return &api;
}

int InitializeInheritedFileDescriptions() {
  const auto* owner = Owner();
  return owner ? owner->initialize() : -1;
}
int RegisterOfd(int fd, int flags) {
  const auto* owner = Owner();
  return owner ? owner->register_fd(fd, flags) : -1;
}
int DupOfd(int old_fd, int new_fd) {
  const auto* owner = Owner();
  return owner ? owner->duplicate(old_fd, new_fd) : -1;
}
void ForgetOfd(int fd) {
  if (const auto* owner = Owner()) owner->forget(fd);
}
bool LookupOfd(int fd, std::shared_ptr<OpenFileDescription>* out) {
  const auto* owner = Owner();
  return owner && owner->lookup(fd, out);
}
int GetFileStatusFlags(int fd) {
  const auto* owner = Owner();
  return owner ? owner->get_flags(fd) : -1;
}
int SetFileStatusFlags(int fd, int flags) {
  const auto* owner = Owner();
  return owner ? owner->set_flags(fd, flags) : -1;
}
DescriptorSnapshotPtr CaptureDescriptor(int fd) {
  const auto* owner = Owner();
  return owner ? owner->capture_fd(fd) : DescriptorSnapshotPtr{};
}
DescriptorSnapshotPtr CaptureFileHandle(HANDLE file) {
  const auto* owner = Owner();
  return owner ? owner->capture_handle(file) : DescriptorSnapshotPtr{};
}
int AttachFileDescription(const DescriptorSnapshotPtr& snapshot, bool close_on_exec) {
  const auto* owner = Owner();
  return owner ? owner->attach(snapshot, close_on_exec) : -1;
}
int FileDescriptionFlags(const DescriptorSnapshotPtr& snapshot) {
  if (InitializeSnapshotFlags(snapshot) < 0) return -1;
  if (GetFileType(snapshot->handle) == FILE_TYPE_PIPE && !snapshot->description->KnownPipeStatus())
    return errno = EOPNOTSUPP, -1;
  const int flags = snapshot->description->Flags();
  if (GetFileType(snapshot->handle) == FILE_TYPE_PIPE && !snapshot->description->KnownPipeStatus())
    return errno = EOPNOTSUPP, -1;
  return flags;
}
int FileDescriptionIoFlags(const DescriptorSnapshotPtr& snapshot) {
  return InitializeSnapshotFlags(snapshot) < 0 ? -1 : snapshot->description->Flags();
}
int InitializePipeDescription(const DescriptorSnapshotPtr& snapshot, int flags) try {
  if (!snapshot || !snapshot->description || GetFileType(snapshot->handle) != FILE_TYPE_PIPE)
    return errno = EINVAL, -1;
  const int native = NativeFlags(snapshot->handle);
  if (native < 0) return -1;
  if (((native ^ flags) & (O_ACCMODE | O_PATH)) != 0) return errno = EINVAL, -1;
  auto& description = *snapshot->description;
  std::lock_guard<std::mutex> lock(description.mutex);
  if (description.OwnedPipe()) return errno = EALREADY, -1;
  unsigned char nonce[16];
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    return errno = EIO, -1;
  char capability[33]{};
  constexpr char hex[] = "0123456789abcdef";
  std::wstring name = L"Local\\gesher-pipe-";
  for (unsigned i = 0; i != 16; ++i) {
    capability[2 * i] = hex[nonce[i] >> 4];
    capability[2 * i + 1] = hex[nonce[i] & 15];
    name += static_cast<wchar_t>(capability[2 * i]);
    name += static_cast<wchar_t>(capability[2 * i + 1]);
  }
  HANDLE gate = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!gate) return FailWithLastError();
  if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(gate); return errno = EEXIST, -1; }
  if (!description.Share()) { const int error = errno; CloseHandle(gate); return errno = error, -1; }
  description.SetFlags(flags & kOpenDescriptionFlagsMask);
  description.pipe_mutex = gate;
  std::memcpy(description.shared.load()->pipe_gate, capability, sizeof(capability));
  return 0;
} catch (const std::bad_alloc&) {
  return errno = ENOMEM, -1;
}
int SetFileDescriptionFlags(const DescriptorSnapshotPtr& snapshot, int arg) {
  if (InitializeSnapshotFlags(snapshot) < 0) return -1;
  auto& description = *snapshot->description;
  const HANDLE file = snapshot->handle;
  std::lock_guard<std::mutex> lock(description.mutex);
  for (;;) {
    const int old_flags = description.Flags();
    if ((old_flags & O_PATH) != 0) return errno = EBADF, -1;
    const DWORD type = GetFileType(file);
    if (type == FILE_TYPE_PIPE && !description.KnownPipeStatus()) return errno = EOPNOTSUPP, -1;
    // A raw native handle can already be unbuffered. Preserve that real mode;
    // this backend cannot change buffering on an existing FILE_OBJECT.
    if (((arg ^ old_flags) & O_DIRECT) != 0) return errno = EINVAL, -1;
    if (type == FILE_TYPE_DISK && (old_flags & O_APPEND) != 0 && (arg & O_APPEND) == 0) {
      uint32_t access = 0;
      int error = record_locks::HandleAccess(file, &access);
      if (error) return errno = error, -1;
      if ((access & FILE_APPEND_DATA) != 0 && (access & FILE_WRITE_DATA) == 0) return errno = EPERM, -1;
    }
    // Timestamp suppression and SIGIO delivery have no implementation here.
    // Failure is preferable to claiming a flag whose effect never occurs.
    if (((arg ^ old_flags) & O_NOATIME) != 0) return errno = EOPNOTSUPP, -1;
    if (type == FILE_TYPE_PIPE && ((arg ^ old_flags) & O_ASYNC) != 0) return errno = EOPNOTSUPP, -1;
    int flags = (old_flags & ~kModifiableStatusFlags) | (arg & kModifiableStatusFlags);
    // A normal Linux file has no fasync callback: this bit stays unchanged.
    if (type == FILE_TYPE_DISK) flags = (flags & ~O_ASYNC) | (old_flags & O_ASYNC);
    if (type == FILE_TYPE_PIPE && ((flags ^ old_flags) & O_NONBLOCK) != 0) {
      // Never queue a mode query/change behind a pending synchronous transfer,
      // including one in another runtime process. NPFS can also reject an idle
      // transition while bytes remain buffered; retain the old flags on failure.
      DWORD wait = WaitForSingleObject(description.pipe_mutex, 0);
      if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        return wait == WAIT_TIMEOUT ? (errno = EOPNOTSUPP, -1) : FailWithLastError();
      if (!description.KnownPipeStatus()) {
        ReleaseMutex(description.pipe_mutex);
        return errno = EOPNOTSUPP, -1;
      }
      // Another aware process has its own description.mutex. It may have
      // completed a transition before this shared endpoint admission succeeded.
      const int current_flags = description.Flags();
      flags = (current_flags & ~kModifiableStatusFlags) | (arg & kModifiableStatusFlags);
      if (((flags ^ current_flags) & O_NONBLOCK) == 0) {
        description.SetFlags(flags);
        ReleaseMutex(description.pipe_mutex);
        return 0;
      }
      description.PipeTransition(true);
      int result = SetPipeNonBlocking(file, (flags & O_NONBLOCK) != 0);
      const int error = errno;
      if (result == 0) description.SetFlags(flags);
      description.PipeTransition(false);
      ReleaseMutex(description.pipe_mutex);
      return result == 0 ? 0 : (errno = error, -1);
    }
    if (type == FILE_TYPE_PIPE) {
      // No wait-mode change is needed, so keep no-op/metadata updates usable
      // during active I/O. Never publish a stale mode after another process
      // changed the native endpoint; retry against its complete current flags.
      auto* shared = description.shared.load(std::memory_order_acquire);
      if (InterlockedCompareExchange(&shared->flags, static_cast<LONG>(flags),
                                      static_cast<LONG>(old_flags)) != old_flags) continue;
      return 0;
    }
    description.SetFlags(flags);
    return 0;
  }
}
int ValidateNativeFileDescription(const DescriptorSnapshotPtr& snapshot) {
  int flags = FileDescriptionIoFlags(snapshot);
  if (flags < 0) return -1;
  if (snapshot->kind != DescriptorKind::kFile) return errno = EOPNOTSUPP, -1;
  if (FileDescriptionIsRevocable(snapshot)) return errno = EOPNOTSUPP, -1;
  int native = NativeFlags(snapshot->handle);
  if (native < 0) return -1;
  if (GetFileType(snapshot->handle) == FILE_TYPE_PIPE) {
    if (((flags ^ native) & (O_ACCMODE | O_PATH)) != 0) return errno = EOPNOTSUPP, -1;
    if (snapshot->description->KnownPipeStatus() && (flags & (O_NONBLOCK | O_ASYNC)) != 0)
      return errno = EOPNOTSUPP, -1;
    if (snapshot->description->OwnedPipe()) {
      auto* shared = snapshot->description->shared.load(std::memory_order_acquire);
      if (InterlockedCompareExchange(&shared->pipe_transition, 0, 0) != 0) return errno = EOPNOTSUPP, -1;
    }
    // An external handle is passed unchanged. No blocking policy or logical
    // status is synthesized for a handle the runtime did not create.
    return 0;
  }
  // The Windows program will use the handle directly. There is no importer
  // to translate logical devices, access metadata or dynamically emulated
  // write/pipe policy on its behalf.
  constexpr int kernel_policy = O_ACCMODE | O_PATH | O_APPEND | O_DIRECT | O_SYNC | O_NOATIME;
  if (((flags ^ native) & kernel_policy) != 0) return errno = EOPNOTSUPP, -1;
  return 0;
}

NativeFileDescriptionExport::~NativeFileDescriptionExport() {
  if (!description_) return;
  if (!committed_) description_->ExposePipe(false);
  ReleaseMutex(description_->pipe_mutex);
}
int NativeFileDescriptionExport::Prepare(const DescriptorSnapshotPtr& snapshot) {
  if (description_) return errno = EINVAL, -1;
  if (ValidateNativeFileDescription(snapshot) < 0) return -1;
  auto& description = *snapshot->description;
  if (!description.OwnedPipe() || !description.KnownPipeStatus()) return 0;
  DWORD wait = WaitForSingleObject(description.pipe_mutex, 0);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    return wait == WAIT_TIMEOUT ? (errno = EOPNOTSUPP, -1) : FailWithLastError();
  if (ValidateNativeFileDescription(snapshot) < 0) {
    ReleaseMutex(description.pipe_mutex);
    return -1;
  }
  if (!description.KnownPipeStatus()) {
    ReleaseMutex(description.pipe_mutex);
    return 0;
  }
  description_ = snapshot->description;
  description_->ExposePipe(true);
  return 0;
}
bool FileDescriptionIsMemfd(const DescriptorSnapshotPtr& snapshot) {
  if (!snapshot || !snapshot->description || snapshot->kind != DescriptorKind::kFile) return false;
  auto& description = *snapshot->description;
  if (!description.memfd_known.load(std::memory_order_acquire)) {
    description.memfd.store(GetFileType(snapshot->handle) == FILE_TYPE_DISK &&
                            FileHasMemfdSeals(snapshot->handle), std::memory_order_relaxed);
    description.memfd_known.store(true, std::memory_order_release);
  }
  return description.memfd.load(std::memory_order_acquire);
}

bool FileDescriptionIsRevocable(const DescriptorSnapshotPtr& snapshot) {
  if (!snapshot || !snapshot->description) return false;
  auto* view = snapshot->description->shared.load(std::memory_order_acquire);
  return view && view->revocation[0] != '\0';
}
int AdoptFileDescription(HANDLE file, HANDLE shared, DescriptorKind kind, bool close_on_exec) try {
  struct Owned {
    HANDLE file, shared;
    ~Owned() {
      if (file && file != INVALID_HANDLE_VALUE) CloseHandle(file);
      if (shared && shared != INVALID_HANDLE_VALUE) CloseHandle(shared);
    }
  } cleanup{file, shared};
  if (!file || file == INVALID_HANDLE_VALUE || !shared || shared == INVALID_HANDLE_VALUE ||
      kind > DescriptorKind::kRandom) return errno = EINVAL, -1;
  auto description = std::make_shared<OpenFileDescription>();
  if (!description->Adopt(shared)) return -1;
  cleanup.shared = nullptr;
  description->kind = kind;
  auto snapshot = std::make_shared<DescriptorSnapshot>();
  snapshot->handle = file;
  cleanup.file = nullptr;
  snapshot->description = std::move(description);
  snapshot->kind = kind;
  return AttachFileDescription(snapshot, close_on_exec);
} catch (const std::bad_alloc&) {
  return errno = ENOMEM, -1;
}
int CaptureDescriptors(std::vector<std::pair<int, DescriptorSnapshotPtr>>* out, bool include_close_on_exec) {
  const auto* owner = Owner();
  return owner ? owner->capture_all(out, include_close_on_exec) : -1;
}
int SetDescriptorKind(int fd, DescriptorKind kind) {
  const auto* owner = Owner();
  return owner ? owner->set_kind(fd, kind) : -1;
}
DescriptorKind GetDescriptorKind(int fd) {
  const auto* owner = Owner();
  return owner ? owner->get_kind(fd) : DescriptorKind::kUnshareable;
}
HANDLE ShareFileDescription(const DescriptorSnapshotPtr& snapshot) {
  const auto* owner = Owner();
  return owner ? owner->share(snapshot) : nullptr;
}
bool IsAppendMode(int fd) {
  const int flags = GetFileStatusFlags(fd);
  return flags >= 0 && (flags & O_APPEND) != 0;
}
bool IsNonBlockingMode(int fd) {
  const int flags = GetFileStatusFlags(fd);
  return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

namespace {
struct StartupImport {
  StartupImport() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&gesher_ofd_owner_anchor_v1), &self) ||
        self != GetModuleHandleW(nullptr)) return;
    if (InitializeInheritedFileDescriptions() == 0) return;
    // Starting guest code without its descriptions would fabricate status and
    // destroy sharing. A failed runtime bootstrap must remain a failed start.
    char message[96] = "gesher: cannot import inherited file descriptions errno=";
    const size_t prefix = std::strlen(message);
    auto formatted = std::to_chars(message + prefix, message + sizeof(message) - 1, errno);
    char* end = formatted.ec == std::errc{} ? formatted.ptr : message + prefix;
    *end++ = '\n';
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(end - message), &written, nullptr);
    ExitProcess(127);
  }
};
StartupImport startup_import;
}  // namespace
}  // namespace gesher

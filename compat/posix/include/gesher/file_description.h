// File-description ownership shared by descriptor I/O and process launch.
#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace gesher {
// NUL-backed resources must never be exported as ordinary files. Stateless
// devices have an explicit kind; resources without a cross-process transport
// fail export instead of silently losing their behavior.
enum class DescriptorKind : uint32_t { kFile = 0, kZero = 1, kRandom = 2, kUnshareable = 3 };

struct OpenFileDescription;

// An exact kernel-object reference captured before its descriptor can be
// closed or replaced. The status description remains shared with all aliases.
struct DescriptorSnapshot {
  HANDLE handle = nullptr;
  int descriptor_flags = 0;
  DescriptorKind kind = DescriptorKind::kFile;
  std::shared_ptr<OpenFileDescription> description;
  ~DescriptorSnapshot();
};

using DescriptorSnapshotPtr = std::shared_ptr<DescriptorSnapshot>;
DescriptorSnapshotPtr CaptureDescriptor(int fd);
DescriptorSnapshotPtr CaptureFileHandle(HANDLE file);
// A new descriptor alias with independent descriptor flags. The snapshot
// retains its reference; the new slot owns an explicit native duplicate.
int AttachFileDescription(const DescriptorSnapshotPtr& snapshot, bool close_on_exec);
// Owned pipe status is cached with its actual kernel mode. Unmediated pipe
// status may be unavailable; never query behind pending synchronous I/O.
int FileDescriptionFlags(const DescriptorSnapshotPtr& snapshot);
int SetFileDescriptionFlags(const DescriptorSnapshotPtr& snapshot, int flags);
// For I/O admission only: retains exact access even if an external pipe's
// wait policy is unknown. Do not use this to report F_GETFL status.
int FileDescriptionIoFlags(const DescriptorSnapshotPtr& snapshot);
// Call only for a freshly created, unpublished synchronous pipe endpoint.
// Its access and wait mode are supplied by the creator, before any I/O.
int InitializePipeDescription(const DescriptorSnapshotPtr& snapshot, int flags);
bool FileDescriptionIsRevocable(const DescriptorSnapshotPtr& snapshot);
// Consumes both owned handles on every outcome. Used after a broker has
// transferred the file and its exact shared description as one capability.
int AdoptFileDescription(HANDLE file, HANDLE shared, DescriptorKind kind, bool close_on_exec);
// Native Windows programs do not import Gesher metadata. Reject descriptions
// whose current behavior cannot be supplied by the inherited kernel handle.
int ValidateNativeFileDescription(const DescriptorSnapshotPtr& snapshot);
// Reserve the known pipe policy through CreateProcess. Failure rolls back;
// success records that native code can now use/change the endpoint directly.
class NativeFileDescriptionExport {
 public:
  NativeFileDescriptionExport() = default;
  ~NativeFileDescriptionExport();
  NativeFileDescriptionExport(const NativeFileDescriptionExport&) = delete;
  NativeFileDescriptionExport& operator=(const NativeFileDescriptionExport&) = delete;
  int Prepare(const DescriptorSnapshotPtr& snapshot);
  void Commit() { committed_ = true; }
 private:
  std::shared_ptr<OpenFileDescription> description_;
  bool committed_ = false;
};
// Raw-handle consumers such as the shell retain this snapshot and use the
// same I/O policy as descriptor callers, including shared status and devices.
int ReadFileDescription(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count);
// Real pipe input with an additional borrowed manual-reset interruption event.
// The event remains alive until this call returns; native cancellation retires
// before returning EINTR, so the caller may run its handler on its own thread.
int ReadPipeDescriptionInterruptible(const DescriptorSnapshotPtr& snapshot, void* bytes,
                                     unsigned count, HANDLE interrupted);
int WriteFileDescription(const DescriptorSnapshotPtr& snapshot, const void* bytes, unsigned count);
// Linux pwrite policy (including O_APPEND) while retaining the shared cursor.
int64_t WriteFileDescriptionAt(const DescriptorSnapshotPtr& snapshot, const void* bytes,
                              unsigned count, int64_t offset);
// One transaction captures all open slots and their descriptor flags. Each
// snapshot owns its native reference after the transaction is released.
int CaptureDescriptors(std::vector<std::pair<int, DescriptorSnapshotPtr>>* out,
                       bool include_close_on_exec);

int SetDescriptorKind(int fd, DescriptorKind kind);
DescriptorKind GetDescriptorKind(int fd);

// Returns a borrowed, noninherited shared-state handle. The snapshot keeps it
// alive; a launcher duplicates it explicitly into its child's handle list.
HANDLE ShareFileDescription(const DescriptorSnapshotPtr& snapshot);

// Initialize before guest or host application code can recycle inherited fds.
// The executable owns the process-wide table, including calls from DLLs.
int InitializeInheritedFileDescriptions();

namespace ofd_transfer {
inline constexpr uint64_t kMagic = 0x3144464f52485347ull;  // GSHROFD1
// Version 3 combines pipe admission metadata and revocation identity. Neither
// standalone version-2 layout is wire-compatible with this shared structure.
inline constexpr uint32_t kVersion = 3;
inline constexpr uint32_t kDescriptorLimit = 8192;
struct Header {
  uint64_t magic;
  uint32_t version;
  uint32_t count;
  uint32_t closed_stdio;
  uint32_t reserved;
};
struct Entry {
  int32_t descriptor;
  uint32_t kind;
  uint64_t shared_handle;
  uint64_t file_handle;
};
// Appended after the standard UCRT inherited-fd block. A separate mapping
// holds the variable-sized manifest without reducing the descriptor ceiling.
struct Startup {
  uint64_t magic;
  uint64_t manifest_handle;
  uint64_t manifest_size;
};
}  // namespace ofd_transfer
}  // namespace gesher

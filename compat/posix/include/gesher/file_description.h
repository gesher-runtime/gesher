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
// Native inspection and mode changes can wait behind synchronous I/O. Call
// these after releasing descriptor publication/registry locks; the snapshot
// pins the exact object even when its original fd is closed or replaced.
int FileDescriptionFlags(const DescriptorSnapshotPtr& snapshot);
int SetFileDescriptionFlags(const DescriptorSnapshotPtr& snapshot, int flags);
bool FileDescriptionIsRevocable(const DescriptorSnapshotPtr& snapshot);
// Consumes both owned handles on every outcome. Used after a broker has
// transferred the file and its exact shared description as one capability.
int AdoptFileDescription(HANDLE file, HANDLE shared, DescriptorKind kind, bool close_on_exec);
// Native Windows programs do not import Gesher metadata. Reject descriptions
// whose current behavior cannot be supplied by the inherited kernel handle.
int ValidateNativeFileDescription(const DescriptorSnapshotPtr& snapshot);
// Raw-handle consumers such as the shell retain this snapshot and use the
// same I/O policy as descriptor callers, including shared status and devices.
int ReadFileDescription(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count);
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
inline constexpr uint32_t kVersion = 2;
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

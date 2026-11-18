// gesher-base: Open File Description (OFD) semantics and status flags tracking.
#pragma once

#include <fcntl.h>
#include <gesher/file_description.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <windows.h>

namespace gesher {

struct RevocableFileGroup;
struct OpenFileDescription {
  struct Shared {
    uint64_t magic;
    uint32_t version;
    volatile LONG flags;
    // Only runtime-created endpoints have an admission capability. This is
    // immutable after publication and does not retain either pipe peer.
    char pipe_gate[33];
    volatile LONG pipe_native;
    volatile LONG pipe_transition;
    // Immutable after a fresh descriptor is attached, before publication.
    // Empty for ordinary descriptors; otherwise a shared revocation group ID.
    char revocation[33];
  };
  std::atomic<DescriptorKind> kind{DescriptorKind::kFile};
  std::atomic<int> local_flags{0};
  // Native handles discovered through the CRT are initialized only after the
  // caller has pinned them and released the descriptor/table transaction.
  std::atomic<bool> initialized{false};
  std::atomic<Shared*> shared{nullptr};
  HANDLE mapping = nullptr;
  HANDLE pipe_mutex = nullptr;
  std::mutex mutex;
  std::shared_ptr<RevocableFileGroup> revocation;
  ~OpenFileDescription();
  int Flags() const;
  void SetFlags(int flags);
  HANDLE Share();  // caller holds mutex; borrowed handle, nullptr on error
  bool Adopt(HANDLE inherited);  // takes ownership only on success
  bool OwnedPipe() const;
  bool KnownPipeStatus() const;
  void ExposePipe(bool exposed);
  void PipeTransition(bool changing);
};

// Flags retained by Linux F_GETFL, excluding creation-only options.
inline constexpr int kFileStatusFlagsMask =
    O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC | O_DIRECT | O_NOATIME | O_ASYNC | O_LARGEFILE;
inline constexpr int kOpenDescriptionFlagsMask =
    O_ACCMODE | O_PATH | O_DIRECTORY | O_NOFOLLOW | kFileStatusFlagsMask;
inline constexpr int kModifiableStatusFlags =
    O_APPEND | O_NONBLOCK | O_DIRECT | O_NOATIME | O_ASYNC;

// Registers an OFD for fd with the given initial flags.
int RegisterOfd(int fd, int flags);

// Duplicates the OFD from old_fd to new_fd (shared ownership).
int DupOfd(int old_fd, int new_fd);

// Drops the OFD mapping for fd (decrements shared ownership).
void ForgetOfd(int fd);

// Looks up the OFD for fd.
bool LookupOfd(int fd, std::shared_ptr<OpenFileDescription>* ofd);

// Gets status flags for fd (creating lazily from handle access if not yet registered).
int GetFileStatusFlags(int fd);

// Sets status flags for fd via F_SETFL (modifying only kModifiableStatusFlags).
int SetFileStatusFlags(int fd, int arg);

// Checks if fd has O_APPEND set on its OFD.
bool IsAppendMode(int fd);

// Checks if fd has O_NONBLOCK set on its OFD.
bool IsNonBlockingMode(int fd);

}  // namespace gesher

// Internal descriptor/registry transaction boundary. Public fcntl/stdio/fd
// routing is installed only with its independently reviewed integration.
#pragma once
#include "record_lock_operation.h"

namespace gesher::record_locks {

// Serializes participating descriptor publication/replacement. Reentrant for
// internal F_DUPFD helpers; no callback or blocking record-lock wait may run
// while this guard is held.
class DescriptorGuard {
 public:
  DescriptorGuard();
  ~DescriptorGuard();
  DescriptorGuard(const DescriptorGuard&) = delete;
  DescriptorGuard& operator=(const DescriptorGuard&) = delete;
};

// POSIX result/errno for the four nonblocking commands, using a single checked
// user-input snapshot. Does not implement F_SETLKW or install any C symbols.
int DescriptorFcntl(int descriptor, int command, void* user_flock);

// Hold across the ACTUAL CRT close/reopen/dup2 replacement, not merely an
// attempted close. Finish inspects the captured FILE_OBJECT versus the fd's
// actual post-operation object even if the CRT returned an error.
class ClosingDescriptor {
 public:
  explicit ClosingDescriptor(int descriptor);
  ~ClosingDescriptor();
  ClosingDescriptor(const ClosingDescriptor&) = delete;
  ClosingDescriptor& operator=(const ClosingDescriptor&) = delete;
  int Error() const { return error_; }
  // Successful close/replacement is evidence even if a dup2 target's new
  // handle still names the SAME FILE_OBJECT. Failed operations use the actual
  // captured-versus-current object check to discover whether the fd survived.
  int Finish(bool closed_or_replaced = false);  // Linux error, or0.

 private:
  DescriptorGuard guard_;
  int descriptor_, error_ = 0;
  bool begun_ = false, completed_ = false;
  HANDLE probe_ = nullptr;
  File file_{};
  RegistryState* staged_ = nullptr;
};

}  // namespace gesher::record_locks

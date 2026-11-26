#pragma once
#include <gesher/file_description.h>
#include <memory>

namespace gesher {
struct RevocableFileGroup;
// /proc/self/fd reopens a new file description, retaining the capability while
// giving its caller an independent cursor and requested status/access mode.
int ReopenRevocableFileDescription(const DescriptorSnapshotPtr& snapshot, int flags);

// Holds only the selected group's kernel mutex, never a descriptor-global or
// registry lock. A revoker acquires the same mutex before publishing revocation.
class FileOperationLease {
 public:
  explicit FileOperationLease(const DescriptorSnapshotPtr& snapshot);
  ~FileOperationLease();
  FileOperationLease(const FileOperationLease&) = delete;
  FileOperationLease& operator=(const FileOperationLease&) = delete;
  int Error() const { return error_; }
 private:
  std::shared_ptr<RevocableFileGroup> group_;
  bool locked_ = false;
  int error_ = 0;
};
}  // namespace gesher

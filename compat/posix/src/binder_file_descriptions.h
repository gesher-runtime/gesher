#pragma once
#include "binder_wire.h"
#include <linux/android/binder.h>
#include <cstring>
#include <vector>

namespace gesher::binder_wire {
// Every transportable FD has exactly one bounded metadata record. Reject a
// partial/foreign table rather than silently adopting an unrestricted file.
inline bool ValidFileDescriptions(const std::vector<uint8_t>& data,
                                  const std::vector<uint64_t>& offsets,
                                  const std::vector<FileDescription>& descriptions) {
  if (descriptions.size() > kMaxFileDescriptions) return false;
  size_t next = 0;
  for (uint64_t offset : offsets) {
    if (offset > data.size() || data.size() - offset < sizeof(binder_object_header)) return false;
    binder_object_header header{};
    std::memcpy(&header, data.data() + offset, sizeof(header));
    if (header.type != BINDER_TYPE_FD) continue;
    if (data.size() - offset < sizeof(binder_fd_object) || next == descriptions.size()) return false;
    const auto& description = descriptions[next++];
    if (description.object_offset != offset || description.shared_handle == 0 ||
        description.shared_handle > UINTPTR_MAX || description.kind > 2 || description.reserved != 0)
      return false;
  }
  return next == descriptions.size();
}
}  // namespace gesher::binder_wire

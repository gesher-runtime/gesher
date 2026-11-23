#pragma once
#include "binder_wire.h"
#include <linux/android/binder.h>
#include <cstring>
#include <vector>

namespace gesher::binder_wire {
// Every transportable FD has exactly one bounded metadata record. Reject a
// partial/foreign table rather than silently adopting an unrestricted file.
inline bool ValidFileDescriptions(const void* data, size_t data_size,
                                  const void* offsets, size_t offsets_size,
                                  const std::vector<FileDescription>& descriptions) {
  if (descriptions.size() > kMaxFileDescriptions || offsets_size % sizeof(uint64_t) != 0 ||
      (data_size != 0 && data == nullptr) || (offsets_size != 0 && offsets == nullptr)) return false;
  const auto* bytes = static_cast<const uint8_t*>(data);
  const auto* offset_bytes = static_cast<const uint8_t*>(offsets);
  size_t next = 0;
  for (size_t at = 0; at < offsets_size; at += sizeof(uint64_t)) {
    uint64_t offset;
    std::memcpy(&offset, offset_bytes + at, sizeof(offset));
    if (offset > data_size || data_size - offset < sizeof(binder_object_header)) return false;
    binder_object_header header{};
    std::memcpy(&header, bytes + offset, sizeof(header));
    if (header.type != BINDER_TYPE_FD) continue;
    if (data_size - offset < sizeof(binder_fd_object) || next == descriptions.size()) return false;
    const auto& description = descriptions[next++];
    binder_fd_object object{};
    std::memcpy(&object, bytes + offset, sizeof(object));
    const bool socket = description.kind == kSocketFileDescription;
    const bool fence = description.kind == kNativeFenceFileDescription;
    if (description.object_offset != offset || description.shared_handle > UINTPTR_MAX ||
        description.reserved != 0 ||
        (socket ? description.shared_handle != 0 || (object.pad_flags & 1) == 0
         : fence ? description.shared_handle != 0 || object.pad_flags != kCompletedNativeFenceMetadata
                 : description.shared_handle == 0 || description.kind > 2 || object.pad_flags != 0))
      return false;
  }
  return next == descriptions.size();
}

inline bool ValidFileDescriptions(const std::vector<uint8_t>& data,
                                  const std::vector<uint64_t>& offsets,
                                  const std::vector<FileDescription>& descriptions) {
  return ValidFileDescriptions(data.data(), data.size(), offsets.data(),
                               offsets.size() * sizeof(uint64_t), descriptions);
}
}  // namespace gesher::binder_wire

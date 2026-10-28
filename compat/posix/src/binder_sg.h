// Binder PTR scatter/gather shape, shared by the client and broker.
// All wire pointers are offsets into owned extra bytes; only the receiving
// process rebases them into its single transaction allocation.
#pragma once

#include <linux/android/binder.h>
#include <stdint.h>
#include <string.h>

#include <vector>

namespace gesher::binder_sg {

// Matches the existing binder pipe message cap; validate before allocation.
constexpr uint64_t kMaxBytes = 64u << 20;
constexpr size_t kNoParent = SIZE_MAX;
static_assert(sizeof(binder_uintptr_t) == 8 && sizeof(binder_size_t) == 8);
static_assert(sizeof(binder_buffer_object) == 40);

inline bool Range(uint64_t at, uint64_t size, uint64_t limit) {
  return at <= limit && size <= limit - at;
}

inline bool Dimensions(uint64_t data, uint64_t offsets, uint64_t extra) {
  if (data > kMaxBytes) return false;
  const uint64_t aligned_data = (data + 7) & ~uint64_t{7};
  return offsets % sizeof(binder_size_t) == 0 && extra % 8 == 0 &&
      Range(aligned_data, offsets, kMaxBytes) && Range(aligned_data + offsets, extra, kMaxBytes);
}

inline size_t ObjectSize(uint32_t type) {
  switch (type) {
    case BINDER_TYPE_BINDER:
    case BINDER_TYPE_WEAK_BINDER:
    case BINDER_TYPE_HANDLE:
    case BINDER_TYPE_WEAK_HANDLE: return sizeof(flat_binder_object);
    case BINDER_TYPE_FD: return sizeof(binder_fd_object);
    case BINDER_TYPE_PTR: return sizeof(binder_buffer_object);
    default: return 0;  // FDA and other remaining object contracts stay unsupported.
  }
}

struct Pointer {
  size_t object_index;
  size_t data_offset;
  size_t extra_offset;
  size_t parent = kNoParent;  // index in Layout::pointers
  binder_buffer_object object;
};

struct Layout {
  std::vector<Pointer> pointers;
};

// Validate the ordered UAPI object table before touching a pointed-to byte.
// Parent fixups may move only forwards in the previous pointer's ancestry,
// following binder_validate_fixup's ordering contract at the pinned source.
inline bool Describe(const uint8_t* data, size_t data_size,
                     const std::vector<uint64_t>& offsets, size_t extra_size,
                     Layout* layout) {
  if (!Dimensions(data_size, offsets.size() * sizeof(uint64_t), extra_size)) return false;
  layout->pointers.clear();
  std::vector<size_t> pointer_by_object(offsets.size(), kNoParent);
  uint64_t previous_end = 0;
  uint64_t next_extra = 0;
  size_t last_pointer = kNoParent;
  for (size_t index = 0; index < offsets.size(); ++index) {
    const uint64_t offset = offsets[index];
    if (offset % 4 != 0 || offset < previous_end ||
        !Range(offset, sizeof(binder_object_header), data_size)) return false;
    uint32_t type;
    memcpy(&type, data + offset, sizeof(type));
    const size_t size = ObjectSize(type);
    if (size == 0 || !Range(offset, size, data_size)) return false;
    previous_end = offset + size;
    if (type != BINDER_TYPE_PTR) continue;
    binder_buffer_object object;
    memcpy(&object, data + offset, sizeof(object));
    if (!Range(next_extra, object.length, extra_size)) return false;
    const uint64_t padded = (object.length + 7) & ~uint64_t{7};
    if (!Range(next_extra, padded, extra_size)) return false;
    Pointer pointer{index, static_cast<size_t>(offset), static_cast<size_t>(next_extra),
                    kNoParent, object};
    if (object.flags & BINDER_BUFFER_FLAG_HAS_PARENT) {
      if (object.parent >= index) return false;
      const size_t parent = pointer_by_object[object.parent];
      if (parent == kNoParent || last_pointer == kNoParent) return false;
      // The pinned driver's last-fixup offset uses zero as its sentinel.
      if (layout->pointers[last_pointer].data_offset == 0) return false;
      const auto& parent_object = layout->pointers[parent].object;
      if (!Range(object.parent_offset, sizeof(binder_uintptr_t), parent_object.length)) return false;
      size_t ancestor = last_pointer;
      uint64_t minimum = 0;
      while (ancestor != parent) {
        const auto& prior = layout->pointers[ancestor];
        if (prior.parent == kNoParent) return false;
        minimum = prior.object.parent_offset + sizeof(binder_uintptr_t);
        ancestor = prior.parent;
      }
      if (object.parent_offset < minimum) return false;
      pointer.parent = parent;
    }
    pointer_by_object[index] = layout->pointers.size();
    last_pointer = layout->pointers.size();
    layout->pointers.push_back(pointer);
    next_extra += padded;
  }
  return true;
}

inline bool WireLayout(const std::vector<uint8_t>& data,
                       const std::vector<uint64_t>& offsets,
                       size_t extra_size, Layout* layout) {
  if (!Describe(data.data(), data.size(), offsets, extra_size, layout)) return false;
  for (const auto& pointer : layout->pointers) {
    if (pointer.object.buffer != pointer.extra_offset) return false;
  }
  return true;
}

// The original sender pointers are read once through the supplied safe user
// copy operation. The wire owns all bytes and normalizes each PTR object's
// buffer address; application payload bytes otherwise retain their meaning.
template <typename Copy>
bool Capture(std::vector<uint8_t>* data, const std::vector<uint64_t>& offsets,
             size_t extra_size, std::vector<uint8_t>* extra, Copy copy) {
  Layout layout;
  if (!Describe(data->data(), data->size(), offsets, extra_size, &layout)) return false;
  extra->assign(extra_size, 0);
  for (auto pointer : layout.pointers) {
    if (pointer.object.length != 0 &&
        !copy(extra->data() + pointer.extra_offset,
              reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer.object.buffer)),
              static_cast<size_t>(pointer.object.length))) return false;
    pointer.object.buffer = pointer.extra_offset;
    memcpy(data->data() + pointer.data_offset, &pointer.object, sizeof(pointer.object));
  }
  return true;
}

inline void Rebase(uint8_t* data, uint8_t* extra, const Layout& layout) {
  for (auto pointer : layout.pointers) {
    pointer.object.buffer = reinterpret_cast<binder_uintptr_t>(extra + pointer.extra_offset);
    memcpy(data + pointer.data_offset, &pointer.object, sizeof(pointer.object));
    if (pointer.parent != kNoParent) {
      const auto& parent = layout.pointers[pointer.parent];
      memcpy(extra + parent.extra_offset + pointer.object.parent_offset,
             &pointer.object.buffer, sizeof(pointer.object.buffer));
    }
  }
}

}  // namespace gesher::binder_sg

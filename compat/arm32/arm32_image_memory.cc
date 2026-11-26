// gesher: publish an independently relocated ELF32 image into guest memory.
#include <gesher/arm32_executor.h>
#include <gesher/arm32_elf.h>

#include <new>
#include <vector>

namespace gesher::arm32 {
namespace {
bool Fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}
}

bool MapElfImage(const ElfImage& image, GuestMemory* memory, std::string* error) {
  constexpr uint64_t page = GuestMemory::kPageSize;
  constexpr uint64_t limit = GuestMemory::kAddressLimit;
  if (!memory || image.bytes.empty() || image.guest_base < page ||
      image.guest_base % page || image.bytes.size() > limit - image.guest_base ||
      image.segments.empty())
    return Fail(error, "invalid ARM32 image mapping");
  const uint64_t size = (uint64_t(image.bytes.size()) + page - 1) & ~(page - 1);
  if (size > limit - image.guest_base)
    return Fail(error, "ARM32 rounded image mapping overflows");
  std::vector<MemoryPermission> permissions;
  try {
    permissions.resize(static_cast<size_t>(size / page), MemoryPermission::kNone);
  } catch (const std::bad_alloc&) {
    return Fail(error, "cannot allocate ARM32 image permission map");
  }
  for (const auto& segment : image.segments) {
    if (segment.address < image.guest_base || segment.flags & ~uint32_t{7} ||
        uint64_t(segment.address) - image.guest_base + segment.size > image.bytes.size())
      return Fail(error, "ARM32 segment lies outside its owned image");
    if (!segment.size) continue;
    auto access = MemoryPermission::kNone;
    if (segment.flags & 4) access = access | MemoryPermission::kRead;
    if (segment.flags & 2) access = access | MemoryPermission::kWrite;
    if (segment.flags & 1) access = access | MemoryPermission::kExecute;
    const uint64_t begin = uint64_t(segment.address) - image.guest_base;
    const uint64_t end = begin + segment.size;
    for (size_t i = static_cast<size_t>(begin / page); i < (end + page - 1) / page; ++i)
      permissions[i] = permissions[i] | access;
  }
  // Bionic protects RELRO from page_start(begin) through page_end(end), after
  // relocations have been applied. Keep the same protection in guest memory.
  for (const auto& range : image.relro) {
    if (range.address < image.guest_base ||
        uint64_t(range.address) - image.guest_base + range.size > image.bytes.size())
      return Fail(error, "ARM32 RELRO range lies outside its owned image");
    if (!range.size) continue;
    const uint64_t begin = uint64_t(range.address) - image.guest_base;
    const uint64_t end = begin + range.size;
    for (size_t i = static_cast<size_t>(begin / page); i < (end + page - 1) / page; ++i)
      permissions[i] = MemoryPermission::kRead;
  }
  if (!memory->Map(image.guest_base, size,
                   MemoryPermission::kRead | MemoryPermission::kWrite, error)) return false;
  if (!memory->Write(image.guest_base, image.bytes.data(), image.bytes.size(), error)) {
    memory->Unmap(image.guest_base, size);
    return false;
  }
  for (size_t i = 0; i < permissions.size(); ++i) {
    if (!memory->Protect(static_cast<uint32_t>(uint64_t(image.guest_base) + i * page),
                         page, permissions[i], error)) {
      memory->Unmap(image.guest_base, size);
      return false;
    }
  }
  if (error) error->clear();
  return true;
}
}  // namespace gesher::arm32

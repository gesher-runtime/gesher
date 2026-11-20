#pragma once
// Bounded admission of the native ACL forms understood by the delete-pending
// xattr fallback. This classifies policy; Windows still evaluates DACL and MIC.
#include <cstddef>
#include <cstdint>
#include <span>

namespace gesher::xattr {

enum class AclKind { kDacl, kSacl };
enum class PolicySupport { kSimple, kUnsupported, kMalformed };

inline PolicySupport CheckFallbackAcl(std::span<const unsigned char> bytes, AclKind kind) {
  constexpr size_t kAclHeader = 8;
  constexpr size_t kAceHeader = 4;
  if (bytes.size() < kAclHeader) return PolicySupport::kMalformed;
  const auto word = [&](size_t at) {
    return static_cast<size_t>(bytes[at]) | (static_cast<size_t>(bytes[at + 1]) << 8);
  };
  const size_t length = word(2), count = word(4);
  if (length < kAclHeader || length > bytes.size()) return PolicySupport::kMalformed;
  if (bytes[0] != 2 && bytes[0] != 4) return PolicySupport::kUnsupported;
  size_t cursor = kAclHeader;
  for (size_t i = 0; i < count; ++i) {
    if (length - cursor < kAceHeader) return PolicySupport::kMalformed;
    const unsigned char type = bytes[cursor], flags = bytes[cursor + 1];
    const size_t size = word(cursor + 2);
    if (size < kAceHeader || size % 4 || size > length - cursor) return PolicySupport::kMalformed;
    // INHERIT_ONLY ACEs do not apply to this object. Unknown types are still
    // refused rather than assuming that future policy has today's semantics.
    constexpr unsigned char kInheritOnly = 0x08;
    const bool known_inherit_only = (flags & kInheritOnly) && type <= 0x15;
    if (!known_inherit_only) {
      const bool plain_dacl = kind == AclKind::kDacl && (type == 0x00 || type == 0x01);
      const bool mandatory_label = kind == AclKind::kSacl && type == 0x11;
      if (!plain_dacl && !mandatory_label) return PolicySupport::kUnsupported;
      // The admitted ACE forms share MASK + SID after the ACE_HEADER.
      // Check the SID's bounded wire shape before Windows consumes it.
      if (size < 16 || bytes[cursor + 8] != 1 || bytes[cursor + 9] > 15 ||
          size < 16 + 4 * static_cast<size_t>(bytes[cursor + 9])) return PolicySupport::kMalformed;
    }
    cursor += size;
  }
  // AclSize includes free capacity after the final ACE; it is not another ACE.
  return PolicySupport::kSimple;
}

}  // namespace gesher::xattr

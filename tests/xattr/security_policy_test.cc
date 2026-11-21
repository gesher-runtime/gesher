// Tests the exact production ACL policy classifier; no Windows access decision
// is simulated. The native fixture separately sets actual file security policy.
#include "../../compat/posix/src/xattr_security_policy.h"
#include <cstdio>
#include <vector>
using gesher::xattr::AclKind;
using gesher::xattr::CheckFallbackAcl;
using gesher::xattr::PolicySupport;
static int checks, failures;
static void Check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::fprintf(stderr, "FAIL xattr policy: %s\n", message); }
}
static std::vector<unsigned char> Acl(unsigned char type, unsigned char flags = 0) {
  // Native ACL + MASK/SID ACE, SID=S-1-1-0. Policy payload isn't interpreted.
  return {2, 0, 28, 0, 1, 0, 0, 0, type, flags, 20, 0, 0, 0, 0, 0,
          1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
}
int main() {
  Check(CheckFallbackAcl({(const unsigned char*)"\2\0\10\0\0\0\0\0", 8}, AclKind::kDacl) == PolicySupport::kSimple,
        "empty DACL is eligible for the actual AccessCheck denial");
  for (auto type : {0x00, 0x01}) {
    auto data = Acl(static_cast<unsigned char>(type));
    Check(CheckFallbackAcl(data, AclKind::kDacl) == PolicySupport::kSimple, "plain allow/deny DACL remains supported");
  }
  auto label = Acl(0x11);
  Check(CheckFallbackAcl(label, AclKind::kSacl) == PolicySupport::kSimple, "mandatory label remains supported");
  for (auto type : {0x05, 0x06, 0x09, 0x0a, 0x0b, 0x0c}) {
    auto data = Acl(static_cast<unsigned char>(type));
    Check(CheckFallbackAcl(data, AclKind::kDacl) == PolicySupport::kUnsupported,
          "object-specific and callback/conditional DACL policies fail closed");
  }
  for (auto type : {0x12, 0x13, 0x14, 0x15}) {
    auto data = Acl(static_cast<unsigned char>(type));
    Check(CheckFallbackAcl(data, AclKind::kSacl) == PolicySupport::kUnsupported,
          "resource/CAP/trust/filter SACL metadata fails closed");
    data[9] = 0x10;
    Check(CheckFallbackAcl(data, AclKind::kSacl) == PolicySupport::kUnsupported,
          "inherited policy still applies to the current object");
    data[9] = 0x08;
    Check(CheckFallbackAcl(data, AclKind::kSacl) == PolicySupport::kSimple,
          "inherit-only policy does not apply to the current object");
  }
  auto unknown = Acl(0xff, 0x08);
  Check(CheckFallbackAcl(unknown, AclKind::kDacl) == PolicySupport::kUnsupported, "future ACE types are not guessed");
  auto valid = Acl(0x00);
  for (size_t length = 0; length < valid.size(); ++length)
    Check(CheckFallbackAcl({valid.data(), length}, AclKind::kDacl) == PolicySupport::kMalformed,
          "every truncated ACL is rejected");
  for (unsigned size : {0, 3, 4, 15, 19, 24, 255}) {
    auto data = valid; data[10] = static_cast<unsigned char>(size);
    Check(CheckFallbackAcl(data, AclKind::kDacl) == PolicySupport::kMalformed, "invalid ACE extent is rejected");
  }
  auto counted = valid; counted[4] = 2;
  Check(CheckFallbackAcl(counted, AclKind::kDacl) == PolicySupport::kMalformed, "missing second ACE is rejected");
  auto sid = valid; sid[17] = 15;
  Check(CheckFallbackAcl(sid, AclKind::kDacl) == PolicySupport::kMalformed, "SID cannot exceed its ACE");
  auto capacity = valid; capacity.resize(64); capacity[2] = 64;
  Check(CheckFallbackAcl(capacity, AclKind::kDacl) == PolicySupport::kSimple, "unused ACL capacity is permitted");
  // Bounds/overflow controls over hostile storage; only policy classification,
  // not a claim that these bytes are accepted by the Windows object manager.
  unsigned state = 0x81624a31;
  for (unsigned i = 0; i < 10000; ++i) {
    auto bytes = valid;
    for (auto& byte : bytes) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; byte = state; }
    (void)CheckFallbackAcl(bytes, i % 2 ? AclKind::kDacl : AclKind::kSacl);
  }
  std::printf("xattr policy classifier: %d checks, %d failures; 10000 bounded corrupt inputs\n", checks, failures);
  return failures ? 1 : 0;
}

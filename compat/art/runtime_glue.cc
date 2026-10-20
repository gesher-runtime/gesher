// Gesher: small pieces libart-runtime expects from modules Gesher does not
// build (yet).

#include <ostream>

#include "base/flags.h"
#include "com_android_art_rw_flags.h"
#include "intrinsics_enum.h"
#include "jit_create.h"

namespace art {

// libartbase/base/flags.cc leaves this out on Windows, where AOSP has no
// aconfig; Gesher generates the aconfig headers itself.
bool is_test_rw_flag_enabled() { return COM_ANDROID_ART_RW_FLAGS_TEST_RW_FLAG; }

// Same as compiler/optimizing/intrinsics.cc, which needs the whole compiler.
std::ostream& operator<<(std::ostream& os, const Intrinsics& intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kNone:
      os << "None";
      break;
#define GESHER_INTRINSIC_NAME(Name, ...) \
    case Intrinsics::k##Name:            \
      os << #Name;                       \
      break;
    ART_INTRINSICS_LIST(GESHER_INTRINSIC_NAME)
#undef GESHER_INTRINSIC_NAME
  }
  return os;
}

namespace jit {

// The JIT compiler (libart-compiler) is not built yet, so Gesher runs ART
// with the JIT off (-Xusejit:false) and this is never reached in practice.
JitCompilerInterface* jit_create() { return nullptr; }

}  // namespace jit
}  // namespace art

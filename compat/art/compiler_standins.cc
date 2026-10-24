// Gesher: what libart-runtime takes from libart-compiler, for dalvikvm, which
// runs without the compiler (the JIT is off: -Xusejit:false). dex2oat links
// the real libart-compiler instead (cmake/aosp/dex2oat.cmake).

#include <ostream>

#include "intrinsics_enum.h"
#include "jit_create.h"

namespace art {

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

// The JIT compiler is not linked into dalvikvm, so Gesher runs ART with the
// JIT off (-Xusejit:false) and this is never reached in practice.
JitCompilerInterface* jit_create() { return nullptr; }

}  // namespace jit
}  // namespace art

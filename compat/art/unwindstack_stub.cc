// Gesher: libunwindstack stand-in.
//
// ART uses unwindstack only to print native stacks in thread dumps. Windows
// has its own unwinder (RtlVirtualUnwind); until Gesher adapts it, local
// unwinding reports "unsupported" and dumps show no native frames.

#include <unwindstack/AndroidUnwinder.h>

namespace unwindstack {

bool AndroidLocalUnwinder::InternalInitialize(ErrorData& error) {
  error.code = ERROR_UNSUPPORTED;
  return false;
}

bool AndroidLocalUnwinder::InternalUnwind(std::optional<pid_t>, AndroidUnwinderData& data) {
  data.error.code = ERROR_UNSUPPORTED;
  return false;
}

}  // namespace unwindstack

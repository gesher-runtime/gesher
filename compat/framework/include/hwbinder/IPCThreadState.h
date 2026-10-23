// Gesher: stands in for libhwbinder's IPCThreadState (system/libhwbinder),
// which binderthreadstate/CallerUtils.h asks whether a HIDL call is being
// served. Gesher has no hwbinder, so never.
#pragma once

namespace android::hardware {

class IPCThreadState {
 public:
  static IPCThreadState* selfOrNull() { return nullptr; }
  const void* getServingStackPointer() const { return nullptr; }
};

}  // namespace android::hardware

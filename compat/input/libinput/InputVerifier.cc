// Gesher: libinput's InputVerifier without its Rust implementation
// (input_verifier.rs; no Rust toolchain). The verifier checks the motion
// streams an InputPublisher sends, and only when the
// enable_outbound_event_verification flag or the "InputTransportVerifyEvents"
// log tag asks for it (both off here); this one accepts every stream.
// docs/input.md.

#include <input/InputVerifier.h>

namespace android {
namespace input {
namespace verifier {
struct InputVerifier {
  std::string name;
};
}  // namespace verifier
}  // namespace input

InputVerifier::InputVerifier(const std::string& name)
    : mVerifier(std::make_shared<input::verifier::InputVerifier>(
          input::verifier::InputVerifier{name})) {}

android::base::Result<void> InputVerifier::processMovement(DeviceId, int32_t, int32_t, int32_t,
                                                           uint32_t, const PointerProperties*,
                                                           const PointerCoords*, int32_t,
                                                           int32_t) {
  return {};
}

std::string InputVerifier::dump() const {
  return "InputVerifier '" + mVerifier->name + "': not built (Gesher has no Rust)\n";
}

void InputVerifier::resetDevice(DeviceId) {}

}  // namespace android

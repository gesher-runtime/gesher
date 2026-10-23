// Gesher: the virtual keyboard's key character map (docs/input.md).
#pragma once

#include <memory>

#include <input/KeyCharacterMap.h>

// Generic.kcm (frameworks/base/data/keyboards, android-16.0.0_r4), built in:
// generic_kcm.cc, which cmake/aosp/input.cmake writes from
// compat/input/keychars/Generic.kcm.
extern "C" const char gesher_generic_kcm[];

namespace gesher {
namespace input {

// $ANDROID_ROOT/usr/keychars/Generic.kcm when there is one, as on a device;
// otherwise the built-in copy. Null (logged) if neither loads.
std::unique_ptr<android::KeyCharacterMap> LoadGenericKeyCharacterMap();

}  // namespace input
}  // namespace gesher

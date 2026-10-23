// Gesher: hwui's HWUIProperties.sysprop, as Soong's sysprop C++ generator
// would write it for its two read-only properties (module android.uirenderer).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <android-base/parseint.h>
#include <android-base/properties.h>

namespace android::uirenderer {

inline std::optional<bool> use_vulkan() {
  std::string v = android::base::GetProperty("ro.hwui.use_vulkan", "");
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return std::nullopt;
}

inline std::optional<std::int32_t> render_ahead() {
  std::int32_t value;
  if (android::base::ParseInt(android::base::GetProperty("ro.hwui.render_ahead", ""), &value)) {
    return value;
  }
  return std::nullopt;
}

}  // namespace android::uirenderer

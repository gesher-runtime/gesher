// Test-only stand-in for android-base properties.
#pragma once
#include <cstdlib>
#include <string>
namespace android::base {
inline std::string GetProperty(const std::string& name, const std::string& fallback) {
  if (name == "debug.hwui.renderer") {
    const char* value = std::getenv("GESHER_TEST_RENDERER_PROPERTY");
    if (value != nullptr) return value;
  }
  return fallback;
}
inline bool GetBoolProperty(const std::string& name, bool fallback) {
  if (name == "renderthread.skia.reduceopstasksplitting") {
    const char* value = std::getenv("GESHER_TEST_REDUCE_OPS");
    if (value != nullptr) return value[0] != '0';
  }
  return fallback;
}
}

// Platform-independent policy for choosing between SkiaGL and SkiaCpu.
// ANGLE's D3D11 WARP device is software, not an integrated Intel GPU.
#pragma once
#include <algorithm>
#include <cctype>
#include <string_view>

namespace gesher::hwui {

inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                     [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                     }) != haystack.end();
}

// ANGLE may successfully initialize on Microsoft Basic Render Driver even
// when the "hardware" device was requested. Only automatically enable GL
// after probing a known real D3D11 adapter; other devices remain opt-in.
inline bool HardwareAngleD3D11(std::string_view renderer) {
  if (!ContainsIgnoreCase(renderer, "ANGLE (") ||
      !ContainsIgnoreCase(renderer, "Direct3D11")) {
    return false;
  }
  for (std::string_view software : {"Microsoft Basic Render Driver", "WARP",
                                    "SwiftShader", "llvmpipe", "software rasterizer",
                                    "GDI Generic"}) {
    if (ContainsIgnoreCase(renderer, software)) return false;
  }
  for (std::string_view vendor : {"ANGLE (Intel", "ANGLE (AMD", "ANGLE (ATI",
                                  "ANGLE (NVIDIA", "ANGLE (Qualcomm", "ANGLE (ARM",
                                  "ANGLE (Imagination"}) {
    if (renderer.size() >= vendor.size() &&
        ContainsIgnoreCase(renderer.substr(0, vendor.size()), vendor)) {
      return true;
    }
  }
  return false;
}
}  // namespace gesher::hwui

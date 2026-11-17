// Portable tests of the actual hardware renderer selection policy.
#include "../../compat/hwui/gl/hardware_renderer_policy.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>
using gesher::hwui::HardwareAngleD3D11;
void Check(bool ok, const char* reason) {
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", reason); std::exit(1); }
}
int main() {
  for (std::string_view gl : {
       "ANGLE (Intel, Intel(R) UHD Graphics 620 Direct3D11 vs_5_0 ps_5_0)",
       "ANGLE (Intel, Intel(R) Iris Xe Graphics Direct3D11 vs_5_0 ps_5_0)",
       "ANGLE (Intel, Intel(R) Arc Graphics Direct3D11 vs_5_0 ps_5_0)",
       "ANGLE (AMD, Radeon RX 6800 Direct3D11 vs_5_0 ps_5_0)",
       "ANGLE (NVIDIA, GeForce RTX 4060 Direct3D11 vs_5_0 ps_5_0)",
       "ANGLE (Qualcomm, Adreno Direct3D11 vs_5_0 ps_5_0)"})
    Check(HardwareAngleD3D11(gl), "real D3D11 hardware recognized");
  for (std::string_view gl : {
       "", "Mesa llvmpipe", "ANGLE (Microsoft, Microsoft Basic Render Driver Direct3D11)",
       "ANGLE (Microsoft, WaRp Direct3D11)", "ANGLE (Google, SwiftShader Direct3D11)",
       "ANGLE (Mesa, llvmpipe Direct3D11)", "ANGLE (Microsoft, software rasterizer Direct3D11)",
       "ANGLE (Intel, Iris Xe Vulkan)", "OpenGL on Intel Direct3D11",
       "ANGLE (Unknown, D3D11 Compatible Direct3D11 vs_5_0)"})
    Check(!HardwareAngleD3D11(gl), "software or unknown backend rejected");
  std::puts("hwui renderer policy all passed");
}

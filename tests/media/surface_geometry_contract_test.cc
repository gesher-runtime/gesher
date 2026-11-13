// Real Windows Window/QueueSurface objects and real AOSP native-window dispatch.
#include "compat/media/surface/queue_surface.h"
#include "compat/hwui/window/win32_surface.h"
#include "compat/hwui/window/gesher_window.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system/window.h>
#include "surface_geometry_assertions.h"

int main() {
  auto queue = android::sp<gesher::media::QueueSurface>::make(4, 6);
  surface_contract::Run(queue.get(), 4, 6);
  auto* host_window = gesher::Window::Create("Surface geometry contracts", 80, 60);
  if (!surface_contract::Check(host_window != nullptr, "create a real hidden Win32 window")) return 1;
  auto window = android::sp<gesher::Win32Surface>::make(host_window);
  surface_contract::Run(window.get(), host_window->width(), host_window->height());
  std::printf("surface geometry contracts: %d checks, %d failures\n",
              surface_contract::checks, surface_contract::failures);
  return surface_contract::failures != 0;
}

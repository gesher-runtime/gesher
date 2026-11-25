// DEVMODEW and EnumDisplaySettingsW require GDI declarations. Enable them only
// in this translation unit, before the first Windows include. Consumers keep
// the build's NOGDI; no SDK structs or WINAPI declarations are reproduced here.
#undef NOGDI
#include "display_refresh.h"

#include <algorithm>
#include <cstdint>

namespace gesher::bionic::gl {

std::chrono::nanoseconds DisplayRefresh::Period(HWND helper,
    std::chrono::steady_clock::time_point now) {
  // Queue surfaces have no HWND or consumer-to-display association. Prefer
  // this process's foreground window, otherwise use the helper's/default
  // monitor. Never follow another application's foreground monitor.
  HWND window = GetForegroundWindow();
  DWORD process = 0;
  if (window != nullptr) GetWindowThreadProcessId(window, &process);
  if (process != GetCurrentProcessId()) window = helper;
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
  // Moving between monitors takes effect on the next frame. Re-query an
  // unchanged monitor once per second for mode changes, not every swap.
  if (!queried_ || monitor != monitor_ || now - queried_at_ >= std::chrono::seconds(1)) {
    monitor_ = monitor;
    queried_at_ = now;
    queried_ = true;
    period_ = std::chrono::nanoseconds(16666667);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (monitor != nullptr &&
        GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
      const int64_t hz = mode.dmDisplayFrequency;
      period_ = std::chrono::nanoseconds(std::max<int64_t>(1, (1000000000 + hz / 2) / hz));
    }
  }
  return period_;
}

}  // namespace gesher::bionic::gl

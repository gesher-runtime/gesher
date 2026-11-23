// Host-display pacing for offscreen EGL helper windows.
#pragma once

#include <windows.h>
#include <chrono>

namespace gesher::bionic::gl {

class DisplayRefresh {
 public:
  std::chrono::nanoseconds Period(HWND helper,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

 private:
  HMONITOR monitor_ = nullptr;
  std::chrono::steady_clock::time_point queried_at_{};
  std::chrono::nanoseconds period_{16666667};
  bool queried_ = false;
};

}  // namespace gesher::bionic::gl

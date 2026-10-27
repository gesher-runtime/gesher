// Leaf regression for the standalone CI counterfactual; no ART/AOSP dependency.
#include "gesher_window.h"
#include <windows.h>
#include <cstdio>
int main() {
  auto* window = gesher::Window::Create("Windows4 initial client size", 400, 4000);
  int bounds[4]{};
  if (window != nullptr) window->ScreenBounds(bounds);
  bool exact = window != nullptr && bounds[2] - bounds[0] == 400 && bounds[3] - bounds[1] == 4000;
  std::printf("%s windows4 initial size: a client larger than the desktop retains its exact requested size\n",
      exact ? "PASS" : "FAIL");
  bool activated = false;
  if (window != nullptr) {
    HWND hwnd = static_cast<HWND>(window->hwnd());
    window->Activate();
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    for (int i = 0; i < 100; ++i) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      if (GetGUIThreadInfo(thread, &info) && info.hwndFocus == hwnd) break;
      Sleep(10);
    }
    bool focused = info.hwndFocus == hwnd;
    gesher::WindowEvent event;
    while (window->PollEvent(&event)) {}
    SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    for (int i = 0; i < 10; ++i) {
      if (!window->WaitEvent(&event, 100)) continue;
      if (event.type == gesher::WindowEvent::kActivated) { activated = true; break; }
    }
    activated = activated && focused && GetGUIThreadInfo(thread, &info) && info.hwndFocus == hwnd;
  }
  std::printf("%s windows4 initial size: top-level activation is delivered separately from keyboard focus\n",
      activated ? "PASS" : "FAIL");
  delete window;
  int failed = (exact ? 0 : 1) + (activated ? 0 : 1);
  std::printf("%d windows4 initial size check(s) failed\n", failed);
  return failed;
}

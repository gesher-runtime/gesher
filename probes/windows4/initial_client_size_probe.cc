// Leaf regression for the standalone CI counterfactual; no ART/AOSP dependency.
#include "gesher_window.h"
#include <cstdio>
int main() {
  auto* window = gesher::Window::Create("Windows4 initial client size", 400, 4000);
  int bounds[4]{};
  if (window != nullptr) window->ScreenBounds(bounds);
  bool exact = window != nullptr && bounds[2] - bounds[0] == 400 && bounds[3] - bounds[1] == 4000;
  std::printf("%s windows4 initial size: a client larger than the desktop retains its exact requested size\n",
      exact ? "PASS" : "FAIL");
  delete window;
  std::printf("%d windows4 initial size check(s) failed\n", exact ? 0 : 1);
  return exact ? 0 : 1;
}

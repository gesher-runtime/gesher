// Gesher: popups of a Gesher window (gesher_window.h, "Popups"):
// dialogs, menus, dropdowns, tooltips and toasts are popup windows owned by
// the window they belong to (docs/graphics.md, "Dialogs, menus and other
// popups"; ADR 0040). This file has what popups add to gesher::Window:
//
// - placement: a popup's client area sits at a point of its anchor's client
//   area (the app window: Android's display) and follows it when it moves;
// - translucency: a popup the CPU path draws with alpha (a dialog's rounded
//   card and shadow, a menu's) is a layered window updated with
//   UpdateLayeredWindow, so what is behind shows through;
// - dimming: a window dims what it shows behind a dialog (FLAG_DIM_BEHIND);
// - input routes: a window's input can go to another window's queue (a
//   modal dialog gets the touches outside it, the focused window the keys),
//   with coordinates translated through the screen.
//
// Routes and anchors name other windows; the registry (one mutex for all)
// keeps them consistent as windows go away.

#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "gesher_window.h"

namespace gesher {
namespace {

std::mutex& RegistryMutex() {
  static std::mutex* m = new std::mutex();
  return *m;
}

// Windows a route or an anchor may name (with RegistryMutex held).
std::unordered_set<Window*>& Registered() {
  static auto* s = new std::unordered_set<Window*>();
  return *s;
}

POINT ClientOrigin(HWND hwnd) {
  POINT p = {0, 0};
  ClientToScreen(hwnd, &p);
  return p;
}

bool IsPointer(int32_t type) {
  switch (type) {
    case WindowEvent::kMouseDown:
    case WindowEvent::kMouseUp:
    case WindowEvent::kMouseMove:
    case WindowEvent::kWheel:
    case WindowEvent::kTouchDown:
    case WindowEvent::kTouchMove:
    case WindowEvent::kTouchUp:
    case WindowEvent::kTouchCancel:
    case WindowEvent::kMouseLeave:
      return true;
    default:
      return false;
  }
}

bool IsKey(int32_t type) {
  return type == WindowEvent::kKeyDown || type == WindowEvent::kKeyUp ||
         type == WindowEvent::kChar;
}

// `event` (in `from`'s client coordinates) in `to`'s.
WindowEvent Translated(const WindowEvent& event, HWND from, HWND to) {
  WindowEvent e = event;
  const POINT a = ClientOrigin(from), b = ClientOrigin(to);
  e.x += static_cast<float>(a.x - b.x);
  e.y += static_cast<float>(a.y - b.y);
  return e;
}

}  // namespace

bool Window::RouteEvent(const WindowEvent& event) {
  const bool pointer = IsPointer(event.type);
  if (!pointer && !IsKey(event.type)) return false;  // focus, size, close: the window's own
  std::lock_guard<std::mutex> lock(RegistryMutex());
  if (!pointer) {
    if (route_keys_ == nullptr || route_keys_ == this) return false;
    route_keys_->QueueOwn(event);
    return true;
  }
  // A gesture (from the first press or contact until the last release) goes
  // where it started, as InputDispatcher keeps a touch with its window.
  const bool mouse_start = event.type == WindowEvent::kMouseDown && !gesture_active_;
  const bool touch_start = event.type == WindowEvent::kTouchDown && gesture_touches_ == 0;
  bool routed;
  Window* target;
  if (mouse_start || touch_start) {
    gesture_active_ = true;
    gesture_target_ = route_pointer_ != this ? route_pointer_ : nullptr;
    gesture_routed_ = gesture_target_ != nullptr;
    // Windows watching touches outside them get ACTION_OUTSIDE where this one
    // starts, as InputDispatcher sends it to FLAG_WATCH_OUTSIDE_TOUCH windows.
    for (Window* w : route_outside_) {
      if (w == this || w == gesture_target_) continue;
      WindowEvent outside = Translated(event, static_cast<HWND>(hwnd_), static_cast<HWND>(w->hwnd_));
      outside.type = WindowEvent::kOutside;
      outside.tool = event.type == WindowEvent::kTouchDown ? (event.tool == 2 ? 2 : 1) : 0;
      w->QueueOwn(outside);
    }
  }
  if (gesture_active_) {
    routed = gesture_routed_;
    target = gesture_target_;
  } else {
    target = route_pointer_ != this ? route_pointer_ : nullptr;
    routed = target != nullptr;
  }
  // Where the gesture ends.
  if (event.type == WindowEvent::kTouchDown) gesture_touches_++;
  if ((event.type == WindowEvent::kTouchUp || event.type == WindowEvent::kTouchCancel) &&
      gesture_touches_ > 0) {
    gesture_touches_--;
  }
  if (gesture_active_ && gesture_touches_ == 0 && buttons_ == 0 &&
      (event.type == WindowEvent::kMouseUp || event.type == WindowEvent::kTouchUp ||
       event.type == WindowEvent::kTouchCancel)) {
    gesture_active_ = false;
    gesture_target_ = nullptr;
  }
  if (!routed) return false;
  if (target != nullptr) {
    target->QueueOwn(Translated(event, static_cast<HWND>(hwnd_), static_cast<HWND>(target->hwnd_)));
  }
  return true;  // routed to a window that went away: dropped
}

void Window::SetInputRoute(Window* pointer, Window* keys, const std::vector<Window*>& outside) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto& registered = Registered();
  registered.insert(this);
  if (pointer != nullptr) registered.insert(pointer);
  if (keys != nullptr) registered.insert(keys);
  for (Window* w : outside) registered.insert(w);
  route_pointer_ = pointer;
  route_keys_ = keys;
  route_outside_ = outside;
}

void Window::Unregister() {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto& registered = Registered();
  if (registered.erase(this) == 0) return;
  for (Window* w : registered) {
    if (w->route_pointer_ == this) w->route_pointer_ = nullptr;
    if (w->route_keys_ == this) w->route_keys_ = nullptr;
    w->route_outside_.erase(std::remove(w->route_outside_.begin(), w->route_outside_.end(), this),
                            w->route_outside_.end());
    if (w->gesture_target_ == this) w->gesture_target_ = nullptr;  // the rest is dropped
    if (w->anchor_ == this) w->anchor_ = nullptr;
    w->anchored_.erase(std::remove(w->anchored_.begin(), w->anchored_.end(), this),
                       w->anchored_.end());
  }
  anchored_.clear();
  anchor_ = nullptr;
}

void Window::SetPopupBounds(Window* anchor, int x, int y, int width, int height) {
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& registered = Registered();
    registered.insert(this);
    if (anchor != nullptr) registered.insert(anchor);
    if (anchor_ != anchor) {
      if (anchor_ != nullptr) {
        anchor_->anchored_.erase(
            std::remove(anchor_->anchored_.begin(), anchor_->anchored_.end(), this),
            anchor_->anchored_.end());
      }
      if (anchor != nullptr) anchor->anchored_.push_back(this);
      anchor_ = anchor;
    }
    anchor_x_ = x;
    anchor_y_ = y;
  }
  const POINT origin = anchor != nullptr ? ClientOrigin(static_cast<HWND>(anchor->hwnd_))
                                         : POINT{0, 0};
  HWND hwnd = static_cast<HWND>(hwnd_);
  RECT r = {0, 0, width, height};
  AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                           GetDpiForWindow(hwnd));
  // SetWindowPos from another thread waits for the window's thread (WM_SIZE).
  SetWindowPos(hwnd, nullptr, origin.x + x + r.left, origin.y + y + r.top, r.right - r.left,
               r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::MoveAnchoredPopups() {
  struct Placed {
    HWND hwnd;
    int x, y;
  };
  std::vector<Placed> popups;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    for (Window* w : anchored_) {
      popups.push_back({static_cast<HWND>(w->hwnd_), w->anchor_x_, w->anchor_y_});
    }
  }
  if (popups.empty()) return;
  const POINT origin = ClientOrigin(static_cast<HWND>(hwnd_));
  for (const Placed& p : popups) {
    RECT wr;
    GetWindowRect(p.hwnd, &wr);
    const POINT co = ClientOrigin(p.hwnd);
    // Asynchronous: the popup's thread moves it (this thread never waits on another).
    SetWindowPos(p.hwnd, nullptr, origin.x + p.x - (co.x - wr.left),
                 origin.y + p.y - (co.y - wr.top), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
  }
}

void Window::ShowPopup(bool activate) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE));
  if (activate) PostMessageW(hwnd, kMsgActivate, 0, 0);
}

void Window::Activate() {
  PostMessageW(static_cast<HWND>(hwnd_), kMsgActivate, 0, 0);
}

void Window::Raise() {
  SetWindowPos(static_cast<HWND>(hwnd_), HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Window::SetTranslucent(bool translucent) {
  if (translucent_.exchange(translucent) == translucent) return;
  // The window's thread makes it layered (or not) and presents the buffer.
  PostMessageW(static_cast<HWND>(hwnd_), kMsgPresentLayered, 0, 0);
}

void Window::SetNoActivate(bool no_activate, bool passthrough) {
  no_activate_ = no_activate;
  passthrough_ = passthrough;
  // The window's thread sets its extended style (PresentLayered sets it too: one writer).
  PostMessageW(static_cast<HWND>(hwnd_), kMsgApplyStyle, 0, 0);
}

void Window::ApplyExStyle() {
  HWND hwnd = static_cast<HWND>(hwnd_);
  const LONG_PTR managed = WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  LONG_PTR want = ex & ~managed;
  if (no_activate_) want |= WS_EX_NOACTIVATE;
  if (passthrough_) want |= WS_EX_TRANSPARENT;
  if (layered_) want |= WS_EX_LAYERED;
  if (want != ex) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
}

void Window::SetDim(float amount) {
  int d = static_cast<int>(amount * 255.0f + 0.5f);
  d = d < 0 ? 0 : (d > 255 ? 255 : d);
  if (dim_.exchange(d) == d) return;
  if (layered_) {
    PostMessageW(static_cast<HWND>(hwnd_), kMsgPresentLayered, 0, 0);
  } else {
    InvalidateRect(static_cast<HWND>(hwnd_), nullptr, FALSE);
  }
}

const uint8_t* Window::DimmedPixels() {
  const int d = dim_;
  if (d == 0) return pixels_.data();
  // Android's dim layer: black at the dim amount over what is behind.
  dimmed_.resize(pixels_.size());
  const unsigned keep = 255u - static_cast<unsigned>(d);
  for (size_t i = 0; i < pixels_.size(); i += 4) {
    dimmed_[i] = static_cast<uint8_t>((pixels_[i] * keep + 127u) / 255u);
    dimmed_[i + 1] = static_cast<uint8_t>((pixels_[i + 1] * keep + 127u) / 255u);
    dimmed_[i + 2] = static_cast<uint8_t>((pixels_[i + 2] * keep + 127u) / 255u);
    dimmed_[i + 3] = pixels_[i + 3];
  }
  return dimmed_.data();
}

void Window::ScreenBounds(int out[4]) const {
  HWND hwnd = static_cast<HWND>(hwnd_);
  RECT rc = {0, 0, 0, 0};
  GetClientRect(hwnd, &rc);
  const POINT o = ClientOrigin(hwnd);
  out[0] = o.x;
  out[1] = o.y;
  out[2] = o.x + (rc.right - rc.left);
  out[3] = o.y + (rc.bottom - rc.top);
}

void Window::SetScreenPosition(int x, int y) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  RECT wr;
  GetWindowRect(hwnd, &wr);
  const POINT o = ClientOrigin(hwnd);
  SetWindowPos(hwnd, nullptr, x - (o.x - wr.left), y - (o.y - wr.top), 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::PresentLayered() {
  HWND hwnd = static_cast<HWND>(hwnd_);
  if (!translucent_) {
    if (layered_) {
      layered_ = false;
      ApplyExStyle();
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    return;
  }
  if (!layered_) {
    layered_ = true;
    ApplyExStyle();
  }
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  const int w = buffer_width_, h = buffer_height_;
  if (w < 1 || h < 1) return;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp != nullptr) {
    // The buffer is premultiplied BGRA, what UpdateLayeredWindow takes.
    std::memcpy(bits, DimmedPixels(), static_cast<size_t>(w) * h * 4);
    GdiFlush();
    HGDIOBJ old = SelectObject(dc, bmp);
    SIZE size = {w, h};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, nullptr, &size, dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
}

bool Window::HandlePopupMessage(unsigned msg, uintptr_t wparam, intptr_t lparam,
                                intptr_t* result) {
  (void)wparam;
  switch (msg) {
    case kMsgPresentLayered: {
      // Coalesce: one update for the frames queued meanwhile.
      MSG pending;
      while (PeekMessageW(&pending, static_cast<HWND>(hwnd_), kMsgPresentLayered,
                          kMsgPresentLayered, PM_REMOVE)) {
      }
      PresentLayered();
      *result = 0;
      return true;
    }
    case kMsgApplyStyle:
      ApplyExStyle();
      *result = 0;
      return true;
    case kMsgActivate: {
      HWND hwnd = static_cast<HWND>(hwnd_);
      SetForegroundWindow(hwnd);
      SetActiveWindow(hwnd);
      SetFocus(hwnd);
      *result = 0;
      return true;
    }
    case WM_WINDOWPOSCHANGED: {
      const auto* pos = reinterpret_cast<const WINDOWPOS*>(lparam);
      if ((pos->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
        MoveAnchoredPopups();  // the client area's origin may have moved
      }
      return false;  // DefWindowProc: WM_SIZE and WM_MOVE
    }
    default:
      return false;
  }
}

}  // namespace gesher

// Gesher's input (compat/input, docs/input.md) without Java: the translation
// of window events into Android events; Generic.kcm; then an InputChannel
// pair over gesher-base's socketpair, a Gesher window attached to its server
// end (gesher_input_attach), Win32 messages posted to the window, and
// libinput's InputConsumer on the client end reading MotionEvents and
// KeyEvents and finishing them. Run in CI on Windows
// (tests/windows/tracks/33-input.ps1); prints "all passed" when every check
// holds.
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <android/input.h>
#include <android/keycodes.h>
#include <input/Input.h>
#include <input/InputConsumer.h>
#include <input/InputTransport.h>
#include <input/KeyCharacterMap.h>

#include "gesher_input.h"
#include "gesher_window.h"
#include "keymap.h"
#include "translate.h"
#include "win32.h"

using gesher::WindowEvent;
using namespace gesher::input;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  std::fflush(stdout);
  if (!ok) failures++;
}

bool Near(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

// Win32 (winuser.h), without windows.h next to AOSP's headers.
constexpr uint32_t kWmKeyDown = 0x0100, kWmKeyUp = 0x0101, kWmMouseMove = 0x0200,
                   kWmLButtonDown = 0x0201, kWmLButtonUp = 0x0202, kWmMouseWheel = 0x020A,
                   kWmPointerUpdate = 0x0245, kWmPointerDown = 0x0246, kWmPointerUp = 0x0247;
constexpr uint64_t kMkLButton = 0x0001;
// POINTER_MESSAGE_FLAG_NEW | INRANGE | INCONTACT | FIRSTBUTTON | PRIMARY
constexpr uint64_t kPointerContact = 0x1 | 0x2 | 0x4 | 0x10 | 0x2000;

int64_t Point(int x, int y) {
  return (static_cast<int64_t>(static_cast<uint16_t>(y)) << 16) | static_cast<uint16_t>(x);
}

WindowEvent Event(int type, float x = 0, float y = 0) {
  WindowEvent e;
  e.type = type;
  e.x = x;
  e.y = y;
  return e;
}

void TestTranslation() {
  std::vector<AndroidEvent> out;
  TranslateOptions options;
  options.scale = 2.0f;
  options.offset_x = 5;
  Translator t(options);

  WindowEvent down = Event(WindowEvent::kMouseDown, 10, 20);
  down.button = 1;
  down.modifiers = 1 | 2;  // shift, ctrl
  t.Translate(down, 1000, &out);
  check(out.size() == 2 && out[0].action == AMOTION_EVENT_ACTION_DOWN &&
            out[1].action == AMOTION_EVENT_ACTION_BUTTON_PRESS &&
            out[1].action_button == AMOTION_EVENT_BUTTON_PRIMARY &&
            out[0].button_state == AMOTION_EVENT_BUTTON_PRIMARY &&
            out[0].source == AINPUT_SOURCE_MOUSE &&
            Near(out[0].coords[0].getX(), 25) && Near(out[0].coords[0].getY(), 40) &&
            out[0].meta_state == (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON | AMETA_CTRL_ON |
                                  AMETA_CTRL_LEFT_ON),
        "translate: left button down is DOWN + BUTTON_PRESS, scaled, with shift+ctrl meta");
  out.clear();
  WindowEvent right = Event(WindowEvent::kMouseDown, 10, 20);
  right.button = 2;
  t.Translate(right, 1001, &out);
  check(out.size() == 1 && out[0].action == AMOTION_EVENT_ACTION_BUTTON_PRESS &&
            out[0].button_state ==
                (AMOTION_EVENT_BUTTON_PRIMARY | AMOTION_EVENT_BUTTON_SECONDARY) &&
            out[0].down_time == 1000,
        "translate: a second button is BUTTON_PRESS within the gesture");
  out.clear();
  WindowEvent up = Event(WindowEvent::kMouseUp, 10, 20);
  up.button = 1;
  t.Translate(up, 1002, &out);
  up.button = 2;
  t.Translate(up, 1003, &out);
  check(out.size() == 3 && out[0].action == AMOTION_EVENT_ACTION_BUTTON_RELEASE &&
            out[1].action == AMOTION_EVENT_ACTION_BUTTON_RELEASE &&
            out[2].action == AMOTION_EVENT_ACTION_UP && out[2].button_state == 0 &&
            out[2].down_time == 1000,
        "translate: releases, then UP with the gesture's down time");
  out.clear();
  WindowEvent wheel = Event(WindowEvent::kWheel, 1, 2);
  wheel.delta = -2;
  wheel.delta_x = 1;
  t.Translate(wheel, 1004, &out);
  check(out.size() == 1 && out[0].action == AMOTION_EVENT_ACTION_SCROLL &&
            Near(out[0].coords[0].getAxisValue(AMOTION_EVENT_AXIS_VSCROLL), -2) &&
            Near(out[0].coords[0].getAxisValue(AMOTION_EVENT_AXIS_HSCROLL), 1),
        "translate: wheel is SCROLL with VSCROLL and HSCROLL");
  out.clear();

  // Two fingers: DOWN, POINTER_DOWN(1), MOVE, POINTER_UP(0), UP.
  Translator touch(TranslateOptions{});
  WindowEvent f = Event(WindowEvent::kTouchDown, 10, 10);
  f.pointer = 100;
  f.tool = 1;
  touch.Translate(f, 1, &out);
  f.pointer = 200;
  f.x = 50;
  touch.Translate(f, 2, &out);
  f.type = WindowEvent::kTouchMove;
  f.x = 60;
  touch.Translate(f, 3, &out);
  f.type = WindowEvent::kTouchUp;
  f.pointer = 100;
  f.x = 10;
  touch.Translate(f, 4, &out);
  f.pointer = 200;
  f.x = 60;
  touch.Translate(f, 5, &out);
  check(out.size() == 5 && out[0].action == AMOTION_EVENT_ACTION_DOWN &&
            out[0].source == AINPUT_SOURCE_TOUCHSCREEN &&
            out[0].properties[0].toolType == android::ToolType::FINGER &&
            out[1].action == (AMOTION_EVENT_ACTION_POINTER_DOWN | (1 << 8)) &&
            out[1].pointer_count == 2 && out[1].properties[1].id == 1 &&
            out[2].action == AMOTION_EVENT_ACTION_MOVE && Near(out[2].coords[1].getX(), 60) &&
            out[3].action == (AMOTION_EVENT_ACTION_POINTER_UP | (0 << 8)) &&
            out[4].action == AMOTION_EVENT_ACTION_UP && out[4].pointer_count == 1 &&
            out[4].properties[0].id == 1 && out[4].down_time == 1,
        "translate: two fingers (DOWN, POINTER_DOWN, MOVE, POINTER_UP, UP; ids 0 and 1)");
  out.clear();

  Translator keys(TranslateOptions{});
  WindowEvent k = Event(WindowEvent::kKeyDown);
  k.key = 'Q';
  k.scan = 0x10;
  k.modifiers = 1;
  keys.Translate(k, 10, &out);
  k.repeat = 1;
  keys.Translate(k, 20, &out);
  keys.Translate(k, 30, &out);
  k.type = WindowEvent::kKeyUp;
  keys.Translate(k, 40, &out);
  check(out.size() == 4 && out[0].key_code == AKEYCODE_Q && out[0].scan_code == 16 &&
            out[0].device_id == -1 && out[0].repeat_count == 0 && out[2].repeat_count == 2 &&
            out[3].action == AKEY_EVENT_ACTION_UP && out[3].down_time == 10 &&
            (out[0].meta_state & AMETA_SHIFT_ON) != 0,
        "translate: key down, repeats counted, up with the down time, shift meta");
  check(KeyCodeFromVirtualKey(0x0D, 0x1C) == AKEYCODE_ENTER &&
            KeyCodeFromVirtualKey(0x0D, 0x11C) == AKEYCODE_NUMPAD_ENTER &&
            KeyCodeFromVirtualKey(0x11, 0x11D) == AKEYCODE_CTRL_RIGHT &&
            KeyCodeFromVirtualKey(0x10, 0x36) == AKEYCODE_SHIFT_RIGHT &&
            KeyCodeFromVirtualKey(0x74, 0x3F) == AKEYCODE_F5 &&
            KeyCodeFromVirtualKey(0xBA, 0x27) == AKEYCODE_SEMICOLON &&
            KeyCodeFromVirtualKey(0x2E, 0x153) == AKEYCODE_FORWARD_DEL &&
            KeyCodeFromVirtualKey(0x08, 0x0E) == AKEYCODE_DEL &&
            LinuxScanCode(0x148) == 103 && LinuxScanCode(0x1E) == 30,
        "translate: virtual keys and scan codes (Enter, keypad Enter, right Ctrl/Shift, F5, ;, "
        "Delete, Backspace)");
}

void TestKeyMap() {
  std::unique_ptr<android::KeyCharacterMap> map = LoadGenericKeyCharacterMap();
  check(map != nullptr && map->getKeyboardType() == android::KeyCharacterMap::KeyboardType::FULL &&
            map->getCharacter(AKEYCODE_A, 0) == u'a' &&
            map->getCharacter(AKEYCODE_A, AMETA_SHIFT_ON) == u'A' &&
            map->getCharacter(AKEYCODE_1, AMETA_SHIFT_ON) == u'!' &&
            map->getDisplayLabel(AKEYCODE_Z) == u'Z',
        "Generic.kcm (built in): a full keyboard; a, A, !, label Z");
}

struct Received {
  android::InputEventType type;
  int32_t action = 0;
  int32_t source = 0;
  int32_t device_id = 0;
  float x = 0, y = 0;
  float vscroll = 0;
  int32_t button_state = 0, action_button = 0;
  int32_t key_code = 0, scan_code = 0, repeat = 0;
  int32_t pointer_count = 0;
  android::ToolType tool = android::ToolType::UNKNOWN;
};

void TestChannel() {
  std::unique_ptr<android::InputChannel> server, client;
  android::status_t status = android::InputChannel::openInputChannelPair("input probe", server,
                                                                         client);
  check(status == android::OK && server != nullptr && client != nullptr,
        "InputChannel::openInputChannelPair over gesher-base's socketpair");
  if (status != android::OK) return;

  gesher::Window* window = gesher::Window::Create("Gesher input probe", 320, 240);
  check(window != nullptr, "a Gesher window");
  if (window == nullptr) return;
  void* hwnd = window->hwnd();
  check(gesher_input_attach(reinterpret_cast<void*>(1), server->getFd(), "x", nullptr) == nullptr,
        "gesher_input_attach refuses what is not a Gesher window");
  int chars = 0;
  gesher_input_options options = {};
  options.on_window_event = [](void* cookie, const void* event) {
    if (static_cast<const WindowEvent*>(event)->type == WindowEvent::kChar) {
      (*static_cast<int*>(cookie))++;
    }
  };
  options.cookie = &chars;
  gesher_input_connection* connection =
      gesher_input_attach(hwnd, server->getFd(), "input probe", &options);
  check(connection != nullptr, "gesher_input_attach: the window's input on the server end");
  if (connection == nullptr) return;
  server.reset();  // the connection has its own fd

  int32_t sx = 30, sy = 40;
  ClientToScreenPoint(hwnd, &sx, &sy);
  int32_t px = 100, py = 110, qx = 120, qy = 130;
  ClientToScreenPoint(hwnd, &px, &py);
  ClientToScreenPoint(hwnd, &qx, &qy);
  bool posted = PostWindowMessage(hwnd, kWmMouseMove, 0, Point(10, 20)) &&
                PostWindowMessage(hwnd, kWmLButtonDown, kMkLButton, Point(40, 50)) &&
                PostWindowMessage(hwnd, kWmMouseMove, kMkLButton, Point(60, 70)) &&
                PostWindowMessage(hwnd, kWmLButtonUp, 0, Point(60, 70)) &&
                PostWindowMessage(hwnd, kWmMouseWheel, 120ull << 16, Point(sx, sy)) &&
                PostWindowMessage(hwnd, kWmKeyDown, 'A', 0x001E0001) &&
                PostWindowMessage(hwnd, kWmKeyDown, 'A', 0x401E0001) &&
                PostWindowMessage(hwnd, kWmKeyUp, 'A', 0xC01E0001ll) &&
                PostWindowMessage(hwnd, kWmPointerDown, 4242 | (kPointerContact << 16), Point(px, py)) &&
                PostWindowMessage(hwnd, kWmPointerUpdate, 4242 | (kPointerContact << 16),
                                  Point(qx, qy)) &&
                PostWindowMessage(hwnd, kWmPointerUp, 4242 | (0x2ull << 16), Point(qx, qy));
  check(posted, "PostMessage: mouse, wheel, key and pointer messages to the window");

  // The client end: libinput's consumer, as InputEventReceiver uses it.
  std::shared_ptr<android::InputChannel> clientChannel = std::move(client);
  android::InputConsumer consumer(clientChannel);
  android::PreallocatedInputEventFactory factory;
  std::vector<Received> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int touch_up = 0;
  while (std::chrono::steady_clock::now() < deadline && touch_up == 0) {
    struct pollfd p = {clientChannel->getFd(), POLLIN, 0};
    poll(&p, 1, 100);
    for (;;) {
      uint32_t seq = 0;
      android::InputEvent* event = nullptr;
      status = consumer.consume(&factory, true, -1, &seq, &event);
      if (status != android::OK || event == nullptr) break;
      Received r;
      r.type = event->getType();
      r.source = static_cast<int32_t>(event->getSource());
      r.device_id = event->getDeviceId();
      if (r.type == android::InputEventType::MOTION) {
        auto* m = static_cast<android::MotionEvent*>(event);
        r.action = m->getAction();
        r.x = m->getX(0);
        r.y = m->getY(0);
        r.vscroll = m->getAxisValue(AMOTION_EVENT_AXIS_VSCROLL, 0);
        r.button_state = m->getButtonState();
        r.action_button = m->getActionButton();
        r.pointer_count = static_cast<int32_t>(m->getPointerCount());
        r.tool = m->getToolType(0);
        if (r.source == AINPUT_SOURCE_TOUCHSCREEN && r.action == AMOTION_EVENT_ACTION_UP) {
          touch_up++;
        }
      } else if (r.type == android::InputEventType::KEY) {
        auto* k = static_cast<android::KeyEvent*>(event);
        r.action = k->getAction();
        r.key_code = k->getKeyCode();
        r.scan_code = k->getScanCode();
        r.repeat = k->getRepeatCount();
      }
      got.push_back(r);
      consumer.sendFinishedSignal(seq, true);
    }
  }

  // Hover events may come and go with the mouse (a hidden window has no
  // cursor over it: Windows may report WM_MOUSELEAVE at once); the rest is
  // in order.
  std::vector<Received> seq;
  bool hover_enter = false;
  for (const Received& r : got) {
    if (r.type == android::InputEventType::FOCUS) continue;
    if (r.type == android::InputEventType::MOTION) {
      int a = r.action & AMOTION_EVENT_ACTION_MASK;
      if (a == AMOTION_EVENT_ACTION_HOVER_ENTER && Near(r.x, 10) && Near(r.y, 20)) {
        hover_enter = true;
      }
      if (a == AMOTION_EVENT_ACTION_HOVER_ENTER || a == AMOTION_EVENT_ACTION_HOVER_MOVE ||
          a == AMOTION_EVENT_ACTION_HOVER_EXIT) {
        continue;
      }
    }
    seq.push_back(r);
  }
  std::printf("  %zu events received, %zu after hover events\n", got.size(), seq.size());
  for (const Received& r : seq) {
    std::printf("    type %d action 0x%x source 0x%x (%.0f, %.0f) key %d scan %d repeat %d\n",
                static_cast<int>(r.type), r.action, r.source, r.x, r.y, r.key_code, r.scan_code,
                r.repeat);
  }
  check(hover_enter, "WM_MOUSEMOVE without buttons: HOVER_ENTER at (10, 20)");
  bool mouse = seq.size() >= 5 && seq[0].action == AMOTION_EVENT_ACTION_DOWN &&
               seq[0].source == AINPUT_SOURCE_MOUSE && Near(seq[0].x, 40) && Near(seq[0].y, 50) &&
               seq[0].button_state == AMOTION_EVENT_BUTTON_PRIMARY &&
               seq[0].tool == android::ToolType::MOUSE &&
               seq[1].action == AMOTION_EVENT_ACTION_BUTTON_PRESS &&
               seq[1].action_button == AMOTION_EVENT_BUTTON_PRIMARY &&
               seq[2].action == AMOTION_EVENT_ACTION_MOVE && Near(seq[2].x, 60) &&
               Near(seq[2].y, 70) && seq[3].action == AMOTION_EVENT_ACTION_BUTTON_RELEASE &&
               seq[4].action == AMOTION_EVENT_ACTION_UP && seq[4].button_state == 0;
  check(mouse, "WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP: DOWN, BUTTON_PRESS, MOVE, BUTTON_RELEASE, UP "
               "(SOURCE_MOUSE, window pixels)");
  bool wheel = seq.size() >= 6 && seq[5].action == AMOTION_EVENT_ACTION_SCROLL &&
               Near(seq[5].x, 30) && Near(seq[5].y, 40) && Near(seq[5].vscroll, 1);
  check(wheel, "WM_MOUSEWHEEL: SCROLL at the client point, VSCROLL 1");
  bool keys = seq.size() >= 9 && seq[6].type == android::InputEventType::KEY &&
              seq[6].action == AKEY_EVENT_ACTION_DOWN && seq[6].key_code == AKEYCODE_A &&
              seq[6].scan_code == 30 && seq[6].repeat == 0 && seq[6].device_id == -1 &&
              seq[7].action == AKEY_EVENT_ACTION_DOWN && seq[7].repeat == 1 &&
              seq[8].action == AKEY_EVENT_ACTION_UP && seq[8].key_code == AKEYCODE_A;
  check(keys, "WM_KEYDOWN/KEYDOWN (repeat)/KEYUP 'A': KEYCODE_A, scan 30, repeat 0 then 1, UP");
  bool touch = seq.size() >= 12 && seq[9].action == AMOTION_EVENT_ACTION_DOWN &&
               seq[9].source == AINPUT_SOURCE_TOUCHSCREEN &&
               seq[9].tool == android::ToolType::FINGER && Near(seq[9].x, 100) &&
               Near(seq[9].y, 110) && seq[10].action == AMOTION_EVENT_ACTION_MOVE &&
               Near(seq[10].x, 120) && seq[11].action == AMOTION_EVENT_ACTION_UP;
  check(touch, "WM_POINTERDOWN/UPDATE/UP (touch): DOWN, MOVE, UP (SOURCE_TOUCHSCREEN, finger)");
  std::printf("  WM_CHAR events to the window-event callback: %d\n", chars);

  // The connection reads the finished signals.
  gesher_input_stats stats = {};
  for (int i = 0; i < 100; i++) {
    gesher_input_get_stats(connection, &stats);
    if (stats.finished >= got.size() && stats.in_flight == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::printf("  published %llu, finished %llu (handled %llu), in flight %llu, dropped %llu\n",
              static_cast<unsigned long long>(stats.published),
              static_cast<unsigned long long>(stats.finished),
              static_cast<unsigned long long>(stats.handled),
              static_cast<unsigned long long>(stats.in_flight),
              static_cast<unsigned long long>(stats.dropped));
  check(stats.finished >= got.size() && stats.handled == stats.finished && stats.dropped == 0,
        "finished signals: every event received was finished (handled), nothing dropped");

  // Injected events with modifiers (posted messages carry no keyboard state).
  WindowEvent shiftA = Event(WindowEvent::kKeyDown);
  shiftA.key = 'A';
  shiftA.scan = 0x1E;
  shiftA.modifiers = 1;
  gesher_input_inject(connection, &shiftA);
  bool injected = false;
  for (int i = 0; i < 100 && !injected; i++) {
    struct pollfd p = {clientChannel->getFd(), POLLIN, 0};
    poll(&p, 1, 50);
    uint32_t seq_no = 0;
    android::InputEvent* event = nullptr;
    if (consumer.consume(&factory, true, -1, &seq_no, &event) == android::OK && event != nullptr) {
      if (event->getType() == android::InputEventType::KEY) {
        auto* k = static_cast<android::KeyEvent*>(event);
        injected = k->getKeyCode() == AKEYCODE_A &&
                   (k->getMetaState() & (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON)) ==
                       (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON);
      }
      consumer.sendFinishedSignal(seq_no, true);
    }
  }
  check(injected, "gesher_input_inject: shift+A arrives with META_SHIFT_ON|META_SHIFT_LEFT_ON");

  gesher_input_detach(connection);
  delete window;
}

}  // namespace

int main() {
  TestTranslation();
  TestKeyMap();
  TestChannel();
  if (failures == 0) std::printf("all passed\n");
  return failures == 0 ? 0 : 1;
}

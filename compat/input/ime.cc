// Gesher: a Gesher window's input method side. See ime.h, docs/input.md
// ("Text input") and ADR 0039.

#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

#include "ime.h"
#include "touch_keyboard.h"

namespace gesher {
namespace input {

namespace {

const wchar_t kProp[] = L"GesherImeWindow";

// Window-thread work posted by the other threads (wParam: an Op).
UINT UpdateMessage() {
  static const UINT message = RegisterWindowMessageW(L"GesherImeUpdate");
  return message;
}
// Sent by ~ImeWindow: restore the procedure (lParam: the Impl).
UINT DetachMessage() {
  static const UINT message = RegisterWindowMessageW(L"GesherImeDetach");
  return message;
}

enum Op : int {
  kOpState = 1, kOpCaret = 2, kOpComplete = 3, kOpCancel = 4, kOpShowKeyboard = 5,
  kOpHideKeyboard = 6,
};

LRESULT CALLBACK ImeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// The thread's modifiers as TranslateMessage saw them (gesher::WindowEvent's bits).
int32_t KeyModifiers() {
  int32_t m = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= 2;
  if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
  if (GetKeyState(VK_CAPITAL) & 1) m |= 8;
  return m;
}

std::u16string ReadString(HIMC imc, DWORD index) {
  LONG bytes = ImmGetCompositionStringW(imc, index, nullptr, 0);
  if (bytes <= 0) return {};
  std::wstring buffer(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
  ImmGetCompositionStringW(imc, index, buffer.data(), static_cast<DWORD>(bytes));
  return std::u16string(buffer.begin(), buffer.end());
}

bool IsText(uint32_t ch) { return ch >= 0x20 && ch != 0x7f; }

}  // namespace

struct ImeWindow::Impl {
  HWND hwnd = nullptr;
  WNDPROC original = nullptr;
  // The procedure could not be restored (another subclass is on top): pass
  // everything on.
  bool detached = false;

  // Shared with the other threads.
  std::mutex mutex;
  std::deque<TextEvent> events;
  TextInputState state;
  int32_t caret[4] = {0, 0, 0, 0};
  bool has_caret = false;

  // The window's thread only.
  std::unordered_set<WPARAM> consumed;  // keys whose WM_KEYDOWN the input method took
  std::deque<uint32_t> own_chars;       // WM_CHARs to come whose keys' KeyEvents type them
  wchar_t high_surrogate = 0;
  bool composing = false;
  bool associated = true;  // the window has the thread's default input context
  bool caret_created = false;
  int32_t caret_size[2] = {0, 0};

  LRESULT CallOriginal(UINT msg, WPARAM wparam, LPARAM lparam) {
    return CallWindowProcW(original, hwnd, msg, wparam, lparam);
  }

  TextInputState State() {
    std::lock_guard<std::mutex> lock(mutex);
    return state;
  }

  // Queues `event` and hands the window's procedure its marker.
  void Emit(TextEvent event) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back(std::move(event));
    }
    CallOriginal(WM_CHAR, kTextEventMarker, 1);
  }

  void EmitText(std::u16string text) {
    if (text.empty()) return;
    TextEvent e;
    e.kind = TextEvent::kCommit;
    e.text = std::move(text);
    Emit(std::move(e));
  }

  // A UTF-16 unit of text (WM_CHAR, WM_IME_CHAR): surrogate pairs joined,
  // control characters (their keys' KeyEvents do what they do) dropped.
  void Unit(wchar_t c) {
    if (IS_HIGH_SURROGATE(c)) {
      high_surrogate = c;
      return;
    }
    std::u16string text;
    if (IS_LOW_SURROGATE(c)) {
      if (high_surrogate != 0) {
        text.push_back(static_cast<char16_t>(high_surrogate));
        text.push_back(static_cast<char16_t>(c));
      }
    } else if (IsText(c)) {
      text.push_back(static_cast<char16_t>(c));
    }
    high_surrogate = 0;
    EmitText(std::move(text));
  }

  LRESULT KeyDown(UINT msg, WPARAM vk, LPARAM lparam) {
    if (vk == VK_PROCESSKEY && associated) {
      // An IME took the key; its key-up may come as either code.
      UINT real = ImmGetVirtualKey(hwnd);
      if (real != 0 && real != VK_PROCESSKEY) consumed.insert(real);
      consumed.insert(VK_PROCESSKEY);
      return 0;
    }
    if (vk == VK_PACKET) {
      // SendInput's Unicode input (KEYEVENTF_UNICODE): its WM_CHAR is the text.
      consumed.insert(VK_PACKET);
      return 0;
    }
    // What TranslateMessage made of the key in the thread's layout and key
    // state, asked again without touching that state (flag 4: no dead key is
    // consumed). Peeking at the queued WM_CHAR would see an earlier key's when
    // keys come in a burst of posted messages.
    const int32_t scan = static_cast<int32_t>((lparam >> 16) & 0x1ff);
    uint32_t ch = 0;
    bool dead = false;
    {
      BYTE keys[256];
      wchar_t out[8];
      if (GetKeyboardState(keys)) {
        int n = ToUnicodeEx(static_cast<UINT>(vk), static_cast<UINT>(scan & 0xff), keys, out, 8,
                            4, GetKeyboardLayout(0));
        if (n < 0) dead = true;
        if (n >= 1) ch = static_cast<uint32_t>(out[0]);
      }
    }
    const int32_t modifiers = KeyModifiers();
    const TextInputState s = State();
    if (s.active) {
      if (s.enter_action != 0 && vk == VK_RETURN && (modifiers & 7) == 0) {
        consumed.insert(vk);
        if ((lparam & (1 << 30)) == 0) {  // not a repeat
          TextEvent e;
          e.kind = TextEvent::kEditorAction;
          e.cursor = s.enter_action;
          Emit(std::move(e));
        }
        return 0;
      }
      if (dead || (IsText(ch) &&
                   !KeyEventTypes(static_cast<int32_t>(vk), scan, modifiers, ch))) {
        consumed.insert(vk);
        return 0;
      }
    }
    consumed.erase(vk);  // a fresh press: its key-up is the window's
    if (IsText(ch) && KeyEventTypes(static_cast<int32_t>(vk), scan, modifiers, ch)) {
      own_chars.push_back(ch);
      if (own_chars.size() > 32) own_chars.pop_front();
    }
    return CallOriginal(msg, vk, lparam);
  }

  LRESULT Char(WPARAM wparam) {
    const uint32_t ch = static_cast<uint32_t>(wparam);
    // The characters of keys whose KeyEvents type them come in the keys'
    // order; one that never came (a key TranslateMessage did not see) is
    // passed over.
    for (size_t i = 0; i < own_chars.size() && i < 8; i++) {
      if (own_chars[i] == ch) {
        own_chars.erase(own_chars.begin(), own_chars.begin() + static_cast<long>(i) + 1);
        return 0;
      }
    }
    Unit(static_cast<wchar_t>(ch));
    return 0;
  }

  void Composition(LPARAM flags) {
    HIMC imc = ImmGetContext(hwnd);
    if (imc == nullptr) return;
    bool any = false;
    if (flags & GCS_RESULTSTR) {
      EmitText(ReadString(imc, GCS_RESULTSTR));
      // Committed: nothing is composed unless a new string comes with it.
      composing = false;
      any = true;
    }
    if (flags & GCS_COMPSTR) {
      TextEvent e;
      e.kind = TextEvent::kCompose;
      e.text = ReadString(imc, GCS_COMPSTR);
      const int32_t length = static_cast<int32_t>(e.text.size());
      e.cursor = length;
      if (flags & GCS_CURSORPOS) {
        LONG pos = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
        if (pos >= 0 && pos <= length) e.cursor = static_cast<int32_t>(pos);
      }
      if (flags & GCS_COMPATTR) {
        LONG n = ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
        if (n > 0) {
          std::string attrs(static_cast<size_t>(n), '\0');
          ImmGetCompositionStringW(imc, GCS_COMPATTR, attrs.data(), static_cast<DWORD>(n));
          for (int32_t i = 0; i < n && i < length; i++) {
            if (attrs[i] == ATTR_TARGET_CONVERTED || attrs[i] == ATTR_TARGET_NOTCONVERTED) {
              if (e.target_start < 0) e.target_start = i;
              e.target_end = i + 1;
            }
          }
        }
      }
      composing = !e.text.empty();
      Emit(std::move(e));
      any = true;
    }
    if (!any) {
      // Neither string: the composition was cancelled.
      TextEvent e;
      e.kind = TextEvent::kCompose;
      composing = false;
      Emit(std::move(e));
    }
    ImmReleaseContext(hwnd, imc);
  }

  LRESULT QueryCharPosition(LPARAM lparam) {
    auto* position = reinterpret_cast<IMECHARPOSITION*>(lparam);
    if (position == nullptr || position->dwSize < sizeof(IMECHARPOSITION)) return 0;
    int32_t c[4];
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!has_caret) return 0;
      std::copy(caret, caret + 4, c);
    }
    POINT p = {c[0], c[1]};
    ClientToScreen(hwnd, &p);
    position->pt = p;
    position->cLineHeight = static_cast<UINT>(c[3]);
    RECT client;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
    position->rcDocument = client;
    return 1;
  }

  void Apply(int op) {
    const TextInputState s = State();
    switch (op) {
      case kOpState: {
        const bool want = s.active && s.ime;
        if (want != associated) {
          if (!want && composing) {
            // The editor is gone (or takes no compositions): the client keeps
            // what was composed (InputMethodManager finishes it); the IME drops it.
            composing = false;
            if (HIMC imc = ImmGetContext(hwnd)) {
              ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
              ImmReleaseContext(hwnd, imc);
            }
          }
          ImmAssociateContextEx(hwnd, nullptr, want ? IACE_DEFAULT : 0);
          associated = want;
        }
        if (!s.active && caret_created) {
          DestroyCaret();
          caret_created = false;
        }
        if (s.active) Apply(kOpCaret);
        break;
      }
      case kOpCaret: {
        int32_t c[4];
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!has_caret) return;
          std::copy(caret, caret + 4, c);
        }
        if (!s.active) return;
        const int32_t w = c[2] > 0 ? c[2] : 1, h = c[3] > 0 ? c[3] : 1;
        if (!caret_created || caret_size[0] != w || caret_size[1] != h) {
          if (caret_created) DestroyCaret();
          caret_created = CreateCaret(hwnd, nullptr, w, h) != 0;  // never shown
          caret_size[0] = w;
          caret_size[1] = h;
        }
        if (caret_created) SetCaretPos(c[0], c[1]);
        if (HIMC imc = ImmGetContext(hwnd)) {
          COMPOSITIONFORM composition = {};
          composition.dwStyle = CFS_POINT;
          composition.ptCurrentPos = {c[0], c[1]};
          ImmSetCompositionWindow(imc, &composition);
          CANDIDATEFORM candidate = {};
          candidate.dwIndex = 0;
          candidate.dwStyle = CFS_EXCLUDE;
          candidate.ptCurrentPos = {c[0], c[1] + h};
          candidate.rcArea = {c[0], c[1], c[0] + w, c[1] + h};
          ImmSetCandidateWindow(imc, &candidate);
          ImmReleaseContext(hwnd, imc);
        }
        break;
      }
      case kOpComplete:
      case kOpCancel:
        if (associated) {
          if (HIMC imc = ImmGetContext(hwnd)) {
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, op == kOpComplete ? CPS_COMPLETE : CPS_CANCEL,
                         0);
            ImmReleaseContext(hwnd, imc);
          }
        }
        break;
      default:
        break;
    }
  }

  // On the window's thread: 1 when the procedure is the window's own again.
  LRESULT Detach() {
    if (caret_created) {
      DestroyCaret();
      caret_created = false;
    }
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == ImeProc) {
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
      RemovePropW(hwnd, kProp);
      return 1;
    }
    detached = true;
    return 2;
  }

  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (detached) return CallOriginal(msg, wparam, lparam);
    switch (msg) {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        return KeyDown(msg, wparam, lparam);
      case WM_KEYUP:
      case WM_SYSKEYUP:
        if (consumed.erase(wparam) != 0) return 0;
        break;
      case WM_CHAR:
        return Char(wparam);
      case WM_UNICHAR: {
        if (wparam == UNICODE_NOCHAR) return TRUE;  // WM_UNICHAR is understood
        const uint32_t cp = static_cast<uint32_t>(wparam);
        std::u16string text;
        if (cp >= 0x10000 && cp <= 0x10ffff) {
          text.push_back(static_cast<char16_t>(0xd800 + ((cp - 0x10000) >> 10)));
          text.push_back(static_cast<char16_t>(0xdc00 + ((cp - 0x10000) & 0x3ff)));
        } else if (cp < 0x10000 && IsText(cp) && (cp < 0xd800 || cp > 0xdfff)) {
          text.push_back(static_cast<char16_t>(cp));
        }
        EmitText(std::move(text));
        return FALSE;
      }
      case WM_IME_SETCONTEXT:
        // The editor draws the composition (inline): no composition window.
        if (wparam && State().active) lparam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        break;
      case WM_IME_STARTCOMPOSITION:
        if (State().active && associated) return 0;
        break;
      case WM_IME_COMPOSITION:
        if (State().active && associated) {
          Composition(lparam);
          return 0;
        }
        break;
      case WM_IME_ENDCOMPOSITION:
        if (State().active && associated) {
          composing = false;
          TextEvent e;
          e.kind = TextEvent::kComposeEnd;
          Emit(std::move(e));
          return 0;
        }
        break;
      case WM_IME_CHAR:
        if (State().active && associated) {
          Unit(static_cast<wchar_t>(wparam));
          return 0;
        }
        break;
      case WM_IME_REQUEST:
        if (wparam == IMR_QUERYCHARPOSITION && State().active) return QueryCharPosition(lparam);
        break;
      case WM_NCDESTROY: {
        // Destroyed while attached: the window's own procedure gets the rest.
        WNDPROC original_proc = original;
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_proc));
        RemovePropW(hwnd, kProp);
        detached = true;
        return CallWindowProcW(original_proc, hwnd, msg, wparam, lparam);
      }
      default:
        if (msg == UpdateMessage()) {
          if (wparam == kOpShowKeyboard || wparam == kOpHideKeyboard) {
            const bool show = wparam == kOpShowKeyboard;
            return static_cast<LRESULT>(input::ShowTouchKeyboard(hwnd, show));
          }
          Apply(static_cast<int>(wparam));
          return 0;
        }
        if (msg == DetachMessage() && reinterpret_cast<Impl*>(lparam) == this) return Detach();
        break;
    }
    return CallOriginal(msg, wparam, lparam);
  }
};

namespace {

LRESULT CALLBACK ImeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* impl = static_cast<ImeWindow::Impl*>(GetPropW(hwnd, kProp));
  if (impl == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);
  return impl->Handle(msg, wparam, lparam);
}

}  // namespace

std::unique_ptr<ImeWindow> ImeWindow::Attach(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (hwnd == nullptr || !IsWindow(hwnd)) return nullptr;
  auto* impl = new Impl;
  impl->hwnd = hwnd;
  impl->original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (impl->original == nullptr || !SetPropW(hwnd, kProp, impl)) {
    delete impl;
    return nullptr;
  }
  SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ImeProc));
  std::unique_ptr<ImeWindow> ime(new ImeWindow(hwnd, impl));
  // No editor yet: no input context.
  ime->Post(kOpState);
  return ime;
}

ImeWindow::~ImeWindow() {
  HWND hwnd = static_cast<HWND>(hwnd_);
  DWORD_PTR result = 0;
  if (IsWindow(hwnd) &&
      SendMessageTimeoutW(hwnd, DetachMessage(), 0, reinterpret_cast<LPARAM>(impl_),
                          SMTO_NORMAL | SMTO_ABORTIFHUNG, 5000, &result) != 0 &&
      result == 1) {
    delete impl_;
  }
  // Otherwise the Impl stays: the window's thread may still call into it.
}

void ImeWindow::Post(int op) {
  PostMessageW(static_cast<HWND>(hwnd_), UpdateMessage(), static_cast<WPARAM>(op), 0);
}

bool ImeWindow::TakeTextEvent(TextEvent* out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->events.empty()) return false;
  *out = std::move(impl_->events.front());
  impl_->events.pop_front();
  return true;
}

void ImeWindow::SetTextInput(const TextInputState& state) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state = state;
  }
  Post(kOpState);
}

void ImeWindow::SetCaret(int32_t x, int32_t y, int32_t width, int32_t height) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->caret[0] = x;
    impl_->caret[1] = y;
    impl_->caret[2] = width;
    impl_->caret[3] = height;
    impl_->has_caret = true;
  }
  Post(kOpCaret);
}

int32_t ImeWindow::ShowTouchKeyboard(bool show) {
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(static_cast<HWND>(hwnd_), UpdateMessage(),
                           show ? kOpShowKeyboard : kOpHideKeyboard, 0, SMTO_NORMAL, 5000,
                           &result)) {
    return static_cast<int32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
  }
  return static_cast<int32_t>(static_cast<LRESULT>(result));
}

void ImeWindow::CompleteComposition(bool cancel) {
  Post(cancel ? kOpCancel : kOpComplete);
}

}  // namespace input
}  // namespace gesher

// Own Win32/COM fixture for the production IME adapter, without an AOSP build.
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ime.h"

// This fixture exercises TSF, not the key map or the touch keyboard service.
namespace gesher::input {
bool KeyEventTypes(int32_t, int32_t, int32_t, uint32_t) { return false; }
int32_t ShowTouchKeyboard(void*, bool) { return 0; }
}
using namespace gesher::input;
namespace {
int failures = 0;
bool detach_on_text = false;
std::unique_ptr<ImeWindow> ime;
void Check(bool okay, const char* what) {
  std::printf("%s windows4 IME TSF: %s\n", okay ? "PASS" : "FAIL", what);
  if (!okay) failures++;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CHAR && w == kTextEventMarker) {
    if (detach_on_text) { detach_on_text = false; ime.reset(); }
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
void Drain() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}
void Editor(HWND window, int epoch) {
  ime = ImeWindow::Attach(window);
  TextInputState state; state.active = state.ime = true; state.editor_epoch = epoch;
  ime->SetTextInput(state);
  TextDocument doc; doc.text = u"before \u05e9\u05dc\u05d5\u05dd"; doc.start = 7; doc.end = 11;
  doc.available = true; doc.epoch = epoch;
  ime->SetDocument(doc);
  Drain();
}
}
int main() {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 1;
  WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = Procedure;
  type.lpszClassName = L"GesherWindows4ImeTsfProbe";
  RegisterClassW(&type);
  HWND window = CreateWindowW(type.lpszClassName, L"TSF fixture", WS_OVERLAPPEDWINDOW,
      0, 0, 300, 300, nullptr, nullptr, type.hInstance, nullptr);
  if (!window) { CoUninitialize(); return 1; }
  wchar_t module[32768]; DWORD length = GetModuleFileNameW(nullptr, module, 32768);
  std::wstring path(module, module + length);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"windows4_tsf_service.dll";
  SetEnvironmentVariableW(L"GESHER_TSF_TEST_SERVICE", path.c_str());
  Editor(window, 1);
  int32_t result = ime->RunTextServiceTest();
  TextEvent event; std::vector<TextEvent> edits;
  while (ime->TakeTextEvent(&event)) edits.push_back(std::move(event));
  bool replaced = false, ended = false; int revision = 0;
  for (const auto& edit : edits) {
    Check(edit.epoch == 1 && edit.revision > revision, "ordered edits retain the editor epoch and increasing revisions");
    revision = edit.revision;
    replaced |= edit.kind == TextEvent::kDocumentReplace && edit.text == u"\u65e5\u672c"
        && edit.start == 7 && edit.end == 11;
    ended |= edit.kind == TextEvent::kDocumentCompositionEnd;
  }
  Check(result == S_OK && replaced && ended, "the production IME adapter emits exact native reconversion/finish events");
  TextInputState restricted; restricted.active = true; restricted.editor_epoch = 2;
  ime->SetTextInput(restricted);
  // Sent work can run before the posted state update. Shared state must already
  // deny service access, rather than exposing the previous editor's context.
  Check(ime->RunTextServiceTest() == static_cast<int32_t>(E_UNEXPECTED), "a restriction takes effect before its queued window-thread update");
  Drain();
  Check(ime->RunTextServiceTest() == static_cast<int32_t>(E_UNEXPECTED), "a restricted editor destroys its TSF document before service access");
  ime.reset();
  Editor(window, 3);
  detach_on_text = true;
  result = ime->RunTextServiceTest();
  Check(ime == nullptr, "a nested native text callback can detach the owner during a real COM edit session");
  Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == Procedure,
      "reentrant detach restores the original window procedure");
  Drain(); DestroyWindow(window); CoUninitialize();
  std::printf("%d windows4 IME TSF check(s) failed\n", failures);
  return failures;
}

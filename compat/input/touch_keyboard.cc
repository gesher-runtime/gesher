// Gesher: Windows' touch keyboard for a window (docs/input.md, "Text input";
// ADR 0039): InputPane through IInputPaneInterop::GetForWindow, TryShow and
// TryHide. The WinRT entry points are looked up in combase.dll at run time,
// so nothing more is linked. Called on the window's thread (ime.cc).

#include <windows.h>

#include "touch_keyboard.h"

namespace gesher {
namespace input {

namespace {

// IInspectable's slots, then the interface's own (windows.ui.viewmanagement.h).
struct IInspectableVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(void*);
  ULONG(STDMETHODCALLTYPE* Release)(void*);
  HRESULT(STDMETHODCALLTYPE* GetIids)(void*, ULONG*, IID**);
  HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(void*, int*);
};
struct IInputPaneInteropVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* GetForWindow)(void*, HWND, REFIID, void**);
};
struct IInputPane2Vtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* TryShow)(void*, unsigned char*);
  HRESULT(STDMETHODCALLTYPE* TryHide)(void*, unsigned char*);
};
template <typename V>
struct Object {
  const V* vtbl;
};

// 75CF2C57-9195-4931-8332-F0B409E916AF
const IID kIidInputPaneInterop = {
    0x75cf2c57, 0x9195, 0x4931, {0x83, 0x32, 0xf0, 0xb4, 0x09, 0xe9, 0x16, 0xaf}};
// 8A6B3F26-7090-4793-944C-C3F2CDE26276
const IID kIidInputPane2 = {
    0x8a6b3f26, 0x7090, 0x4793, {0x94, 0x4c, 0xc3, 0xf2, 0xcd, 0xe2, 0x62, 0x76}};

using RoInitializeFn = HRESULT(WINAPI*)(int);
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(void*);
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(void*, REFIID, void**);

template <typename F>
F Lookup(HMODULE module, const char* name) {
  return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

}  // namespace

int32_t ShowTouchKeyboard(void* hwnd_ptr, bool show) {
  HMODULE combase = LoadLibraryW(L"combase.dll");
  if (combase == nullptr) return static_cast<int32_t>(HRESULT_FROM_WIN32(GetLastError()));
  auto ro_initialize =
      Lookup<RoInitializeFn>(combase, "RoInitialize");
  auto create_string =
      Lookup<WindowsCreateStringFn>(combase, "WindowsCreateString");
  auto delete_string =
      Lookup<WindowsDeleteStringFn>(combase, "WindowsDeleteString");
  auto get_factory = Lookup<RoGetActivationFactoryFn>(combase, "RoGetActivationFactory");
  if (ro_initialize == nullptr || create_string == nullptr || delete_string == nullptr ||
      get_factory == nullptr) {
    return static_cast<int32_t>(E_NOTIMPL);
  }
  // The window's thread is single-threaded (it has a message loop); an
  // earlier initialization in another mode is kept.
  ro_initialize(0 /* RO_INIT_SINGLETHREADED */);
  static const wchar_t kClass[] = L"Windows.UI.ViewManagement.InputPane";
  void* name = nullptr;
  HRESULT hr = create_string(kClass, static_cast<UINT32>(wcslen(kClass)), &name);
  if (FAILED(hr)) return static_cast<int32_t>(hr);
  void* interop = nullptr;
  hr = get_factory(name, kIidInputPaneInterop, &interop);
  delete_string(name);
  if (FAILED(hr)) return static_cast<int32_t>(hr);
  auto* factory = static_cast<Object<IInputPaneInteropVtbl>*>(interop);
  void* pane = nullptr;
  hr = factory->vtbl->GetForWindow(factory, static_cast<HWND>(hwnd_ptr), kIidInputPane2, &pane);
  factory->vtbl->base.Release(factory);
  if (FAILED(hr)) return static_cast<int32_t>(hr);
  auto* input_pane = static_cast<Object<IInputPane2Vtbl>*>(pane);
  unsigned char done = 0;
  hr = show ? input_pane->vtbl->TryShow(input_pane, &done)
            : input_pane->vtbl->TryHide(input_pane, &done);
  input_pane->vtbl->base.Release(input_pane);
  if (FAILED(hr)) return static_cast<int32_t>(hr);
  return done ? 1 : 0;
}

}  // namespace input
}  // namespace gesher

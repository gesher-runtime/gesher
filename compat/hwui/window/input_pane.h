// IFrameworkInputPane subscription, separate from ART/AOSP so a real COM test
// source can exercise AdviseWithHWND, handler callbacks and teardown. ADR 0061.
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <memory>

namespace gesher {
struct FrameworkInputPaneHandler : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Showing(RECT* screen, BOOL ensure_focused) = 0;
  virtual HRESULT STDMETHODCALLTYPE Hiding(BOOL ensure_focused) = 0;
};
struct FrameworkInputPane : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Advise(IUnknown* window, FrameworkInputPaneHandler* handler,
                                           DWORD* cookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE AdviseWithHWND(HWND hwnd, FrameworkInputPaneHandler* handler,
                                                   DWORD* cookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE Location(RECT* screen) = 0;
};
extern const IID kInputPaneIID;
extern const IID kInputPaneHandlerIID;

class InputPane {
 public:
  // The optional factory is used only by native probes. It returns one owned
  // FrameworkInputPane reference. Apps always use Windows' COM implementation.
  using Factory = void* (*)();
  static std::unique_ptr<InputPane> Attach(void* hwnd, Factory factory = nullptr);
  ~InputPane();
  void Screen(int out[4]) const;
 private:
  InputPane() = default;
  class Handler;
  FrameworkInputPane* source_ = nullptr;
  Handler* handler_ = nullptr;
  DWORD cookie_ = 0;
  bool advised_ = false;
};
}  // namespace gesher

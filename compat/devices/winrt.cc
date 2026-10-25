// Gesher (devices track): the Windows Runtime helpers of winrt.h.

#include "winrt.h"

#include <mutex>

namespace gesher::devices::winrt {

const IID IID_IAsyncInfo = {0x00000036, 0x0000, 0x0000,
                            {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID IID_IClosable = {0x30D5A829, 0x7FA4, 0x4026,
                           {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};

namespace {

struct Api {
  HRESULT(WINAPI* RoInitialize)(int type) = nullptr;
  HRESULT(WINAPI* RoActivateInstance)(HSTRING id, IInspectable** instance) = nullptr;
  HRESULT(WINAPI* RoGetActivationFactory)(HSTRING id, const IID& iid, void** factory) = nullptr;
  HRESULT(WINAPI* WindowsCreateString)(const wchar_t* s, UINT32 length, HSTRING* out) = nullptr;
  HRESULT(WINAPI* WindowsDeleteString)(HSTRING s) = nullptr;
  const wchar_t*(WINAPI* WindowsGetStringRawBuffer)(HSTRING s, UINT32* length) = nullptr;
  bool ok = false;
};

template <typename F>
void Find(HMODULE module, const char* name, F& f) {
  f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

const Api& GetApi() {
  static Api api;
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (combase == nullptr) return;
    Find(combase, "RoInitialize", api.RoInitialize);
    Find(combase, "RoActivateInstance", api.RoActivateInstance);
    Find(combase, "RoGetActivationFactory", api.RoGetActivationFactory);
    Find(combase, "WindowsCreateString", api.WindowsCreateString);
    Find(combase, "WindowsDeleteString", api.WindowsDeleteString);
    Find(combase, "WindowsGetStringRawBuffer", api.WindowsGetStringRawBuffer);
    api.ok = api.RoInitialize && api.RoActivateInstance && api.RoGetActivationFactory &&
             api.WindowsCreateString && api.WindowsDeleteString &&
             api.WindowsGetStringRawBuffer;
  });
  return api;
}

// An HSTRING for a class name, deleted with it.
class Name {
 public:
  explicit Name(const wchar_t* s) {
    const Api& api = GetApi();
    hr_ = api.WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &s_);
  }
  ~Name() {
    if (s_ != nullptr) GetApi().WindowsDeleteString(s_);
  }
  Name(const Name&) = delete;
  Name& operator=(const Name&) = delete;
  HRESULT hr() const { return hr_; }
  HSTRING get() const { return s_; }

 private:
  HSTRING s_ = nullptr;
  HRESULT hr_ = E_FAIL;
};

}  // namespace

HRESULT EnsureApartment() {
  const Api& api = GetApi();
  if (!api.ok) return E_NOTIMPL;
  thread_local HRESULT result = [&api] {
    HRESULT hr = api.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    return hr == RPC_E_CHANGED_MODE ? S_FALSE : hr;
  }();
  return result;
}

HRESULT ActivateInstance(const wchar_t* className, IInspectable** instance) {
  *instance = nullptr;
  HRESULT hr = EnsureApartment();
  if (FAILED(hr)) return hr;
  Name name(className);
  if (FAILED(name.hr())) return name.hr();
  return GetApi().RoActivateInstance(name.get(), instance);
}

HRESULT GetActivationFactory(const wchar_t* className, const IID& iid, void** factory) {
  *factory = nullptr;
  HRESULT hr = EnsureApartment();
  if (FAILED(hr)) return hr;
  Name name(className);
  if (FAILED(name.hr())) return name.hr();
  return GetApi().RoGetActivationFactory(name.get(), iid, factory);
}

std::wstring StringOf(HSTRING s) {
  const Api& api = GetApi();
  if (s == nullptr || !api.ok) return std::wstring();
  UINT32 length = 0;
  const wchar_t* chars = api.WindowsGetStringRawBuffer(s, &length);
  return chars != nullptr ? std::wstring(chars, length) : std::wstring();
}

void DeleteString(HSTRING s) {
  if (s != nullptr && GetApi().ok) GetApi().WindowsDeleteString(s);
}

HRESULT Wait(IUnknown* async, int64_t timeoutMs) {
  IAsyncInfo* info = nullptr;
  HRESULT hr = async->QueryInterface(IID_IAsyncInfo, reinterpret_cast<void**>(&info));
  if (FAILED(hr)) return hr;
  ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
  DWORD pause = 1;
  int status = 0;
  for (;;) {
    hr = info->get_Status(&status);
    if (FAILED(hr) || status != 0 /* Started */) break;
    if (GetTickCount64() >= deadline) {
      info->Cancel();
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      break;
    }
    Sleep(pause);
    if (pause < 20) pause *= 2;
  }
  if (SUCCEEDED(hr)) {
    switch (status) {
      case 1:  // Completed
        hr = S_OK;
        break;
      case 2:  // Canceled
        hr = E_ABORT;
        break;
      default: {  // Error
        HRESULT code = E_FAIL;
        info->get_ErrorCode(&code);
        hr = FAILED(code) ? code : E_FAIL;
        break;
      }
    }
  }
  info->Release();
  return hr;
}

}  // namespace gesher::devices::winrt

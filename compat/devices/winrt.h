// Gesher (devices track): the Windows Runtime from plain C++, for the device
// natives (docs/devices.md). combase's Ro*/Windows*String functions are
// looked up at run time, so the runtime starts on Windows editions without
// them; the interfaces the natives call are declared in their own sources by
// their ABI (IIDs and vtables), as the Windows SDK's headers describe them.
#pragma once

#include <windows.h>
#include <objbase.h>
#include <stdint.h>

#include <string>

namespace gesher::devices::winrt {

typedef struct HSTRING_PRIVATE* HSTRING;

// IInspectable's methods after IUnknown's.
struct IInspectable : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* level) = 0;
};

// Windows.Foundation.IAsyncInfo {00000036-0000-0000-C000-000000000046}.
struct IAsyncInfo : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
  virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// Windows.Foundation.IAsyncOperation<T>: T is a pointer or a 32-bit value.
template <typename T>
struct IAsyncOperation : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;
};

// Windows.Foundation.IReference<T>.
template <typename T>
struct IReference : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Value(T* value) = 0;
};

// Windows.Foundation.IClosable {30D5A829-7FA4-4026-83BB-D75BAE4EA99E}.
struct IClosable : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

extern const IID IID_IAsyncInfo;
extern const IID IID_IClosable;

// Joins this thread to the multithreaded apartment (once per thread). S_OK,
// S_FALSE (already) or RPC_E_CHANGED_MODE (an STA thread: calls still work
// for agile objects) are all usable; another failure means no WinRT.
HRESULT EnsureApartment();

// RoActivateInstance / RoGetActivationFactory by class name; E_NOTIMPL when
// combase lacks them.
HRESULT ActivateInstance(const wchar_t* className, IInspectable** instance);
HRESULT GetActivationFactory(const wchar_t* className, const IID& iid, void** factory);

// The characters of an HSTRING (empty for null), and releasing one.
std::wstring StringOf(HSTRING s);
void DeleteString(HSTRING s);

// Waits for an IAsyncOperation/IAsyncAction to finish, polling its
// IAsyncInfo; on time-out it is canceled. Returns S_OK when it completed,
// its error code when it failed, E_ABORT when canceled, HRESULT_FROM_WIN32
// (ERROR_TIMEOUT) on time-out.
HRESULT Wait(IUnknown* async, int64_t timeoutMs);

// Releases a COM pointer if set.
template <typename T>
void Release(T*& p) {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

// Ticks (100 ns) between 1601-01-01 and 1970-01-01.
constexpr int64_t kUnixEpochTicks = 116444736000000000LL;

}  // namespace gesher::devices::winrt

// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows Hello keys through their ABI (ADR 0092).
// Interfaces, IIDs and vtable orders are those of the Windows SDK's
// windows.security.credentials.idl, windows.storage.streams.idl and
// windows.security.cryptography.idl (as in microsoft/win32metadata and
// microsoft/windows-rs).

#include "hello_key_win.h"

#include <stdio.h>

#include <atomic>
#include <mutex>

#include "hello_win.h"
#include "winrt.h"

namespace gesher::devices::hello_key {

using winrt::HSTRING;
using winrt::IAsyncInfo;
using winrt::IAsyncOperation;
using winrt::IInspectable;

namespace {

constexpr wchar_t kManager[] = L"Windows.Security.Credentials.KeyCredentialManager";
constexpr wchar_t kCryptographicBuffer[] = L"Windows.Security.Cryptography.CryptographicBuffer";

// Windows.Storage.Streams.IBuffer.
struct IBuffer : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 value) = 0;
};

// Windows.Storage.Streams.IBufferByteAccess (robuffer.h): IUnknown-based.
struct IBufferByteAccess : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) = 0;
};
const IID IID_IBufferByteAccess = {
    0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

// Windows.Security.Cryptography.ICryptographicBufferStatics, up to the method used.
struct ICryptographicBufferStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Compare(IBuffer* a, IBuffer* b, BOOLEAN* equal) = 0;
  virtual HRESULT STDMETHODCALLTYPE GenerateRandom(UINT32 length, IBuffer** buffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE GenerateRandomNumber(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateFromByteArray(UINT32 size, const BYTE* value,
                                                        IBuffer** buffer) = 0;
};
const IID IID_ICryptographicBufferStatics = {
    0x320b7e22, 0x3cb0, 0x4cdf, {0x86, 0x63, 0x1d, 0x28, 0x91, 0x00, 0x65, 0xeb}};

// KeyCredentialOperationResult's default interface.
struct IKeyCredentialOperationResult : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Result(IBuffer** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int* value) = 0;
};

// KeyCredential's default interface.
struct IKeyCredential : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithDefaultBlobType(IBuffer** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithBlobType(int blobType,
                                                                  IBuffer** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestSignAsync(
      IBuffer* data, IAsyncOperation<IKeyCredentialOperationResult*>** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAttestationAsync(IUnknown** operation) = 0;
};

// KeyCredentialRetrievalResult's default interface.
struct IKeyCredentialRetrievalResult : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Credential(IKeyCredential** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int* value) = 0;
};

struct IKeyCredentialManagerStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE IsSupportedAsync(IAsyncOperation<BOOLEAN>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RenewAttestationAsync(IUnknown** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestCreateAsync(
      HSTRING name, int option, IAsyncOperation<IKeyCredentialRetrievalResult*>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenAsync(
      HSTRING name, IAsyncOperation<IKeyCredentialRetrievalResult*>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeleteAsync(HSTRING name, IUnknown** operation) = 0;
};
const IID IID_IKeyCredentialManagerStatics = {
    0x6aac468b, 0x0ef1, 0x4ce0, {0x82, 0x90, 0x41, 0x06, 0xda, 0x6a, 0x63, 0xb5}};

// Windows.UI.WindowId: for a Win32 window, the HWND's value.
struct WindowId {
  UINT64 Value;
};

// UniversalApiContract 20.0 (SDK 10.0.26100.7627): the same requests, owned by a window.
struct IKeyCredentialManagerCreateWithWindowStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RequestCreateForWindowAsync(
      WindowId window, HSTRING name, int option,
      IAsyncOperation<IKeyCredentialRetrievalResult*>** value) = 0;
};
const IID IID_IKeyCredentialManagerCreateWithWindowStatics = {
    0x30b1b9c9, 0x61ef, 0x43e8, {0x88, 0xac, 0xcc, 0x43, 0x3b, 0x38, 0xd1, 0xa6}};

struct IKeyCredentialWithWindow : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RequestSignForWindowAsync(
      WindowId window, IBuffer* data,
      IAsyncOperation<IKeyCredentialOperationResult*>** value) = 0;
};
const IID IID_IKeyCredentialWithWindow = {
    0x050deb3b, 0xb19c, 0x4635, {0x9d, 0xf6, 0x56, 0x50, 0xd6, 0x6c, 0x62, 0xb1}};

constexpr int kFailIfExists = 1;  // KeyCredentialCreationOption
constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2;  // AsyncStatus
constexpr int kUserCanceled = 3;  // KeyCredentialStatus
constexpr int64_t kQuietTimeoutMs = 5000;
constexpr size_t kMaxName = 256;

void Log(const char* call, HRESULT hr) {
  fprintf(stderr, "gesher hello key: %s failed: 0x%08lx\n", call,
          static_cast<unsigned long>(hr));
}

HRESULT Manager(IKeyCredentialManagerStatics** statics) {
  HRESULT hr = winrt::EnsureApartment();
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
  return winrt::GetActivationFactory(kManager, IID_IKeyCredentialManagerStatics,
                                     reinterpret_cast<void**>(statics));
}

HRESULT Name(const wchar_t* name, size_t length, HSTRING* out) {
  return winrt::CreateString(name, static_cast<UINT32>(length < kMaxName ? length : kMaxName),
                             out);
}

HRESULT BytesOf(IBuffer* buffer, std::vector<uint8_t>* out) {
  UINT32 length = 0;
  HRESULT hr = buffer->get_Length(&length);
  if (FAILED(hr)) return hr;
  IBufferByteAccess* access = nullptr;
  hr = buffer->QueryInterface(IID_IBufferByteAccess, reinterpret_cast<void**>(&access));
  if (FAILED(hr)) return hr;
  BYTE* bytes = nullptr;
  hr = access->Buffer(&bytes);
  if (SUCCEEDED(hr)) {
    if (bytes == nullptr && length != 0) {
      hr = E_POINTER;
    } else {
      out->assign(bytes, bytes + length);
    }
  }
  winrt::Release(access);
  return hr;
}

HRESULT BufferOf(const uint8_t* data, size_t size, IBuffer** out) {
  ICryptographicBufferStatics* statics = nullptr;
  HRESULT hr = winrt::GetActivationFactory(kCryptographicBuffer, IID_ICryptographicBufferStatics,
                                           reinterpret_cast<void**>(&statics));
  if (FAILED(hr)) return hr;
  hr = statics->CreateFromByteArray(static_cast<UINT32>(size), data, out);
  winrt::Release(statics);
  return hr;
}

HRESULT PublicKeyOf(IKeyCredential* credential, std::vector<uint8_t>* out) {
  IBuffer* buffer = nullptr;
  HRESULT hr = credential->RetrievePublicKeyWithDefaultBlobType(&buffer);
  if (FAILED(hr)) return hr;
  hr = BytesOf(buffer, out);
  winrt::Release(buffer);
  return hr;
}

// OpenAsync, waited for: a status, and with kSuccess the credential.
int OpenCredential(const wchar_t* name, size_t length, IKeyCredential** credential,
                   HRESULT* hr) {
  *credential = nullptr;
  IKeyCredentialManagerStatics* statics = nullptr;
  HRESULT r = Manager(&statics);
  if (FAILED(r)) {
    Log("GetActivationFactory(KeyCredentialManager)", r);
    *hr = r;
    return kFailed;
  }
  HSTRING text = nullptr;
  r = Name(name, length, &text);
  IAsyncOperation<IKeyCredentialRetrievalResult*>* operation = nullptr;
  if (SUCCEEDED(r)) r = statics->OpenAsync(text, &operation);
  winrt::DeleteString(text);
  winrt::Release(statics);
  if (SUCCEEDED(r)) r = winrt::Wait(operation, kQuietTimeoutMs);
  IKeyCredentialRetrievalResult* result = nullptr;
  if (SUCCEEDED(r)) r = operation->GetResults(&result);
  winrt::Release(operation);
  int status = kFailed;
  if (SUCCEEDED(r)) r = result->get_Status(&status);
  if (SUCCEEDED(r) && status == kSuccess) r = result->get_Credential(credential);
  winrt::Release(result);
  if (FAILED(r) || (status == kSuccess && *credential == nullptr)) {
    Log("KeyCredentialManager.OpenAsync", r);
    winrt::Release(*credential);
    *hr = FAILED(r) ? r : E_POINTER;
    return kFailed;
  }
  *hr = S_OK;
  return status;
}

}  // namespace

struct Operation {
  bool sign = false;
  int immediate = kSuccess;  // not kSuccess: why nothing started
  // The result is a KeyCredentialRetrievalResult (create) or a
  // KeyCredentialOperationResult (sign), read through its own interface.
  IAsyncOperation<IInspectable*>* operation = nullptr;
  IAsyncInfo* info = nullptr;
  std::mutex lock;
  // GetTickCount64 at the first Cancel (never 0 once requested): the start of
  // Await's grace period.
  std::atomic<ULONGLONG> cancel_tick{0};
  // Await gave up on a request Windows did not end after Cancel.
  std::atomic<bool> abandoned{false};
  bool released = false;
};

namespace {

Operation* Immediate(int status) {
  Operation* operation = new Operation;
  operation->immediate = status;
  return operation;
}

// Takes `async` (released on failure) and makes it the operation's.
Operation* Started(bool sign, IUnknown* async, HRESULT hr, const char* call) {
  if (FAILED(hr)) {
    Log(call, hr);
    winrt::Release(async);
    return Immediate(kFailed);
  }
  return internal::Wrap(sign, async);
}

}  // namespace

namespace internal {

Operation* Wrap(bool sign, IUnknown* async) {
  IAsyncInfo* info = nullptr;
  HRESULT hr = async->QueryInterface(winrt::IID_IAsyncInfo, reinterpret_cast<void**>(&info));
  if (FAILED(hr)) {
    Log("IAsyncOperation::QueryInterface(IAsyncInfo)", hr);
    winrt::Release(async);
    return Immediate(kFailed);
  }
  Operation* operation = new Operation;
  operation->sign = sign;
  operation->operation = reinterpret_cast<IAsyncOperation<IInspectable*>*>(async);
  operation->info = info;
  return operation;
}

}  // namespace internal

int Supported(HRESULT* hr) {
  *hr = S_OK;
  IKeyCredentialManagerStatics* statics = nullptr;
  HRESULT r = Manager(&statics);
  if (FAILED(r)) {
    Log("GetActivationFactory(KeyCredentialManager)", r);
    *hr = r;
    return -1;
  }
  IAsyncOperation<BOOLEAN>* operation = nullptr;
  r = statics->IsSupportedAsync(&operation);
  winrt::Release(statics);
  BOOLEAN supported = FALSE;
  if (SUCCEEDED(r)) r = winrt::Wait(operation, kQuietTimeoutMs);
  if (SUCCEEDED(r)) r = operation->GetResults(&supported);
  winrt::Release(operation);
  if (FAILED(r)) {
    Log("KeyCredentialManager.IsSupportedAsync", r);
    *hr = r;
    return -1;
  }
  return supported ? 1 : 0;
}

int Open(const wchar_t* name, size_t length, std::vector<uint8_t>* publicKey, HRESULT* hr) {
  publicKey->clear();
  IKeyCredential* credential = nullptr;
  int status = OpenCredential(name, length, &credential, hr);
  if (status != kSuccess) return status;
  HRESULT r = PublicKeyOf(credential, publicKey);
  winrt::Release(credential);
  if (FAILED(r)) {
    Log("KeyCredential.RetrievePublicKey", r);
    *hr = r;
    return kFailed;
  }
  return kSuccess;
}

int Delete(const wchar_t* name, size_t length, HRESULT* hr) {
  *hr = S_OK;
  // DeleteAsync is an IAsyncAction: it has no status. A missing key is NotFound
  // from OpenAsync first.
  IKeyCredential* credential = nullptr;
  int status = OpenCredential(name, length, &credential, hr);
  winrt::Release(credential);
  if (status != kSuccess) return status;
  IKeyCredentialManagerStatics* statics = nullptr;
  HRESULT r = Manager(&statics);
  HSTRING text = nullptr;
  if (SUCCEEDED(r)) r = Name(name, length, &text);
  IUnknown* action = nullptr;
  if (SUCCEEDED(r)) r = statics->DeleteAsync(text, &action);
  winrt::DeleteString(text);
  winrt::Release(statics);
  if (SUCCEEDED(r)) r = winrt::Wait(action, kQuietTimeoutMs);
  winrt::Release(action);
  if (FAILED(r)) {
    Log("KeyCredentialManager.DeleteAsync", r);
    *hr = r;
    return kFailed;
  }
  return kSuccess;
}

Operation* StartCreate(DWORD pid, const wchar_t* name, size_t length) {
  HWND owner = hello::ForegroundWindowOf(pid);
  if (owner == nullptr) return Immediate(kNotForeground);
  IKeyCredentialManagerStatics* statics = nullptr;
  HRESULT r = Manager(&statics);
  if (FAILED(r)) {
    Log("GetActivationFactory(KeyCredentialManager)", r);
    return Immediate(kFailed);
  }
  HSTRING text = nullptr;
  r = Name(name, length, &text);
  IAsyncOperation<IKeyCredentialRetrievalResult*>* operation = nullptr;
  if (SUCCEEDED(r)) {
    // Owned by the app's window where this Windows has the window variant.
    IKeyCredentialManagerCreateWithWindowStatics* windowed = nullptr;
    if (SUCCEEDED(statics->QueryInterface(IID_IKeyCredentialManagerCreateWithWindowStatics,
                                          reinterpret_cast<void**>(&windowed)))) {
      WindowId id = {static_cast<UINT64>(reinterpret_cast<uintptr_t>(owner))};
      r = windowed->RequestCreateForWindowAsync(id, text, kFailIfExists, &operation);
      winrt::Release(windowed);
      if (FAILED(r)) Log("RequestCreateForWindowAsync (falling back)", r);
    } else {
      r = E_NOINTERFACE;
    }
    if (FAILED(r)) r = statics->RequestCreateAsync(text, kFailIfExists, &operation);
  }
  winrt::DeleteString(text);
  winrt::Release(statics);
  return Started(false, operation, r, "KeyCredentialManager.RequestCreateAsync");
}

Operation* StartSign(DWORD pid, const wchar_t* name, size_t length, const uint8_t* data,
                     size_t size) {
  HWND owner = hello::ForegroundWindowOf(pid);
  if (owner == nullptr) return Immediate(kNotForeground);
  HRESULT r = S_OK;
  IKeyCredential* credential = nullptr;
  int status = OpenCredential(name, length, &credential, &r);
  if (status != kSuccess) return Immediate(status);
  IBuffer* buffer = nullptr;
  r = BufferOf(data, size, &buffer);
  IAsyncOperation<IKeyCredentialOperationResult*>* operation = nullptr;
  if (SUCCEEDED(r)) {
    IKeyCredentialWithWindow* windowed = nullptr;
    if (SUCCEEDED(credential->QueryInterface(IID_IKeyCredentialWithWindow,
                                             reinterpret_cast<void**>(&windowed)))) {
      WindowId id = {static_cast<UINT64>(reinterpret_cast<uintptr_t>(owner))};
      r = windowed->RequestSignForWindowAsync(id, buffer, &operation);
      winrt::Release(windowed);
      if (FAILED(r)) Log("RequestSignForWindowAsync (falling back)", r);
    } else {
      r = E_NOINTERFACE;
    }
    if (FAILED(r)) r = credential->RequestSignAsync(buffer, &operation);
  }
  winrt::Release(buffer);
  winrt::Release(credential);
  return Started(true, operation, r, "KeyCredential.RequestSignAsync");
}

int Await(Operation* o, std::vector<uint8_t>* bytes, DWORD cancel_grace_ms) {
  bytes->clear();
  if (o->info == nullptr) return o->immediate;
  // The awaiting thread may be one that made no WinRT call yet (WindowsHelloKey's
  // own daemon thread for a signature), so it joins the apartment itself.
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r)) Log("RoInitialize (await)", r);
  // A user may take minutes: no time-out while nobody canceled, 1 -> 20 ms
  // back-off.
  int status = kStarted;
  DWORD delay = 1;
  while (SUCCEEDED(r = o->info->get_Status(&status)) && status == kStarted) {
    ULONGLONG requested = o->cancel_tick.load();
    if (requested != 0 && GetTickCount64() - requested >= cancel_grace_ms) {
      // Windows refused the cancel or ignores it. The request is over for the
      // caller, so it is abandoned: Release drops the references without
      // reading a result, and a signature or key the user's later answer to
      // Windows' prompt produces reaches nobody.
      fprintf(stderr,
              "gesher hello key: Windows did not end a canceled request within %lu ms; "
              "abandoning it\n",
              static_cast<unsigned long>(cancel_grace_ms));
      o->abandoned.store(true);
      return kUserCanceled;
    }
    Sleep(delay);
    if (delay < 20) delay *= 2;
  }
  if (FAILED(r)) {
    Log("IAsyncInfo::get_Status", r);
    return kFailed;
  }
  if (status == kCanceled) return kUserCanceled;
  if (status != kCompleted) {
    HRESULT code = E_FAIL;
    o->info->get_ErrorCode(&code);
    Log(o->sign ? "RequestSignAsync completion" : "RequestCreateAsync completion", code);
    return kFailed;
  }
  IInspectable* result = nullptr;
  r = o->operation->GetResults(&result);
  if (FAILED(r) || result == nullptr) {
    Log("GetResults", r);
    return kFailed;
  }
  int answer = kFailed;
  IBuffer* buffer = nullptr;
  if (o->sign) {
    IKeyCredentialOperationResult* signed_ = static_cast<IKeyCredentialOperationResult*>(result);
    r = signed_->get_Status(&answer);
    if (SUCCEEDED(r) && answer == kSuccess) r = signed_->get_Result(&buffer);
  } else {
    IKeyCredentialRetrievalResult* created = static_cast<IKeyCredentialRetrievalResult*>(result);
    IKeyCredential* credential = nullptr;
    r = created->get_Status(&answer);
    if (SUCCEEDED(r) && answer == kSuccess) r = created->get_Credential(&credential);
    if (SUCCEEDED(r) && answer == kSuccess) {
      r = credential != nullptr ? credential->RetrievePublicKeyWithDefaultBlobType(&buffer)
                                : E_POINTER;
    }
    winrt::Release(credential);
  }
  if (SUCCEEDED(r) && answer == kSuccess) {
    r = buffer != nullptr ? BytesOf(buffer, bytes) : E_POINTER;
  }
  winrt::Release(buffer);
  winrt::Release(result);
  if (FAILED(r)) {
    Log(o->sign ? "KeyCredentialOperationResult" : "KeyCredentialRetrievalResult", r);
    bytes->clear();
    return kFailed;
  }
  return answer;
}

void Cancel(Operation* o) {
  // Called from a thread other than the awaiting one.
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r)) Log("RoInitialize (cancel)", r);
  std::lock_guard<std::mutex> guard(o->lock);
  if (o->released || o->info == nullptr || o->cancel_tick.load() != 0) return;
  // The grace period runs from the request, also when the call below fails or
  // takes long.
  ULONGLONG now = GetTickCount64();
  o->cancel_tick.store(now != 0 ? now : 1);
  r = o->info->Cancel();
  if (FAILED(r)) Log("IAsyncInfo::Cancel", r);
}

void Release(Operation* o) {
  {
    std::lock_guard<std::mutex> guard(o->lock);
    o->released = true;
    // IAsyncInfo::Close is only legal on a finished operation; an abandoned one
    // still runs inside Windows and ends there.
    if (o->info != nullptr && !o->abandoned.load()) o->info->Close();
    winrt::Release(o->info);
    winrt::Release(o->operation);
  }
  delete o;
}

}  // namespace gesher::devices::hello_key

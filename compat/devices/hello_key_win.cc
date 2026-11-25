// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows Hello keys through their ABI (ADR 0095).
// Interfaces, IIDs and vtable orders are those of the Windows SDK's
// windows.security.credentials.idl, windows.storage.streams.idl and
// windows.security.cryptography.idl (as in microsoft/win32metadata and
// microsoft/windows-rs).

#include "hello_key_win.h"

#include <stdio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
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

struct IAsyncAction : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
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
constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2, kError = 3;  // AsyncStatus
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
  if (SUCCEEDED(hr)) hr = buffer != nullptr ? BytesOf(buffer, out) : E_POINTER;
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

// A create can outlive its caller after Windows ignores cancellation. Keep one
// bounded owner for it rather than forgetting a key Windows might create later.
// Capacity is reserved BEFORE the prompt starts. The work item owns both COM
// references until terminal completion, and the exact new name until accepted
// delivery or successful cleanup. A stuck Windows operation therefore prevents
// another enrollment; it cannot accumulate workers, handles or orphaned names.
struct CreateState {
  std::wstring name;
  std::function<int(const wchar_t*, size_t, HRESULT*)> delete_created;
  IAsyncOperation<IInspectable*>* operation = nullptr;
  IAsyncInfo* info = nullptr;
  std::atomic<ULONGLONG> cancel_tick{0};
  std::mutex lock;
  std::condition_variable changed;
  bool ready = false;
  bool disposed = false;
  bool accepted = false;
  int result = kFailed;
  std::vector<uint8_t> bytes;
};

std::atomic<bool> g_create_pending{false};
// GetResults is a one-shot handoff. If it fails before creation status is known,
// there is no safe result retry or permission to delete the named credential.
// Retain that one operation/name for this process, without retaining a worker.
// Admission stays closed; process exit is not a claim that the key was deleted.
std::shared_ptr<CreateState> g_unobserved_create;

void FailCreate(const std::shared_ptr<CreateState>& state, const char* call, HRESULT hr) {
  std::lock_guard<std::mutex> guard(state->lock);
  if (!state->ready) {
    Log(call, hr);
    state->result = kFailed;
    state->ready = true;
    state->changed.notify_all();
  }
}

void RetainUnobservedCreate(const std::shared_ptr<CreateState>& state,
                            const char* call, HRESULT hr, PTP_WORK work) {
  FailCreate(state, call, hr);
  fprintf(stderr, "gesher hello key: creation outcome cannot be observed safely; "
                  "retaining its operation and enrollment slot until process exit\n");
  g_unobserved_create = state;
  CloseThreadpoolWork(work);
}

// This deletion already owns an observed newly created key. Unlike the public
// quiet Delete call, it must not abandon a running DeleteAsync and launch more
// deletion operations behind it. One work item retains the action to terminal
// completion; a hung cleanup keeps the single enrollment slot occupied.
int DeleteCreated(const wchar_t* name, size_t length, HRESULT* hr) {
  return internal::DeleteCreated(name, length, hr,
      [](const wchar_t* exact_name, size_t exact_length, IUnknown** action) {
        IKeyCredentialManagerStatics* statics = nullptr;
        HRESULT r = Manager(&statics);
        HSTRING text = nullptr;
        if (SUCCEEDED(r)) r = Name(exact_name, exact_length, &text);
        if (SUCCEEDED(r)) r = statics->DeleteAsync(text, action);
        winrt::DeleteString(text);
        winrt::Release(statics);
        return r;
      });
}

void CALLBACK FinishCreate(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK work) {
  std::unique_ptr<std::shared_ptr<CreateState>> owner(
      static_cast<std::shared_ptr<CreateState>*>(context));
  std::shared_ptr<CreateState> state = *owner;
  owner.reset();
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r) && r != RPC_E_CHANGED_MODE) {
    RetainUnobservedCreate(state, "RoInitialize (create completion)", r, work);
    return;
  }

  while (state->info == nullptr) {
    r = state->operation->QueryInterface(winrt::IID_IAsyncInfo,
                                         reinterpret_cast<void**>(&state->info));
    if (SUCCEEDED(r) && state->info != nullptr) break;
    winrt::Release(state->info);
    FailCreate(state, "QueryInterface(IAsyncInfo) (create)", FAILED(r) ? r : E_POINTER);
    Sleep(1000);
  }

  bool cancel_sent = false;
  int async_status = kStarted;
  DWORD delay = 1;
  while (true) {
    if (!cancel_sent && state->cancel_tick.load() != 0) {
      cancel_sent = true;
      // No mutex needed by Await or Release is held across an external COM call.
      HRESULT canceled = state->info->Cancel();
      if (FAILED(canceled)) Log("IAsyncInfo::Cancel (create)", canceled);
    }
    r = state->info->get_Status(&async_status);
    if (FAILED(r) || async_status < kStarted || async_status > kError) {
      // A failed status query does not prove that Windows stopped creating the
      // key. Fail the caller, but keep ownership until a terminal status can be
      // observed; Release will request cancellation of the rejected delivery.
      FailCreate(state, "IAsyncInfo::get_Status (create)", FAILED(r) ? r : E_UNEXPECTED);
      Sleep(1000);
      continue;
    }
    if (async_status != kStarted) break;
    Sleep(delay);
    if (delay < 20) delay *= 2;
  }

  bool created = false;
  int answer = kFailed;
  std::vector<uint8_t> bytes;
  if (SUCCEEDED(r) && async_status == kCanceled) {
    answer = kUserCanceled;
  } else if (SUCCEEDED(r) && async_status == kCompleted) {
    IInspectable* raw = nullptr;
    r = state->operation->GetResults(&raw);
    if (SUCCEEDED(r) && raw == nullptr) r = E_POINTER;
    if (FAILED(r)) {
      winrt::Release(raw);
      RetainUnobservedCreate(state, "RequestCreateAsync.GetResults", r, work);
      return;
    }
    auto* result = static_cast<IKeyCredentialRetrievalResult*>(raw);
    while (true) {
      r = result->get_Status(&answer);
      if (SUCCEEDED(r)) break;
      // Keep the SAME successfully retrieved object. Calling GetResults again
      // is not permitted: only its first call has a valid result guarantee.
      FailCreate(state, "KeyCredentialRetrievalResult::get_Status", r);
      Sleep(1000);
    }
    // Record actual creation before extracting its public key. A failure in
    // extraction or the later JNI handoff must not leave the created key.
    created = answer == kSuccess;
    if (created) {
      IKeyCredential* credential = nullptr;
      r = result->get_Credential(&credential);
      if (SUCCEEDED(r)) {
        r = credential != nullptr ? PublicKeyOf(credential, &bytes) : E_POINTER;
      }
      winrt::Release(credential);
    }
    winrt::Release(raw);
  } else if (SUCCEEDED(r)) {
    state->info->get_ErrorCode(&r);
    if (SUCCEEDED(r)) r = E_FAIL;
  }
  if (FAILED(r)) {
    Log("RequestCreateAsync completion", r);
    answer = kFailed;
    bytes.clear();
  }
  if (answer == kSuccess && bytes.empty()) answer = kFailed;
  {
    std::unique_lock<std::mutex> guard(state->lock);
    if (!state->ready) {
      state->result = answer;
      state->bytes = std::move(bytes);
    }
    state->ready = true;
    state->changed.notify_all();
    state->changed.wait(guard, [&] { return state->disposed; });
  }

  // Both async completion and the creation result have now been observed.
  // Unknown outcomes above retain ownership and never reach this Close.
  state->info->Close();
  winrt::Release(state->info);
  winrt::Release(state->operation);

  if (created && (!state->accepted || state->cancel_tick.load() != 0
                  || answer != kSuccess)) {
    bool warned = false;
    while (true) {
      HRESULT hr = S_OK;
      int removed = state->delete_created(state->name.data(), state->name.size(), &hr);
      if (removed == kSuccess || removed == 2 /* NotFound */) break;
      if (!warned) {
        fprintf(stderr, "gesher hello key: canceled enrollment cleanup failed "
                        "(status %d, HRESULT 0x%08lx); retaining its slot and retrying\n",
                removed, static_cast<unsigned long>(hr));
        warned = true;
      }
      Sleep(1000);
    }
  }
  g_create_pending.store(false);
  CloseThreadpoolWork(work);
}

}  // namespace

struct Operation {
  std::shared_ptr<CreateState> create;
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

int DeleteCreated(
    const wchar_t* name, size_t length, HRESULT* hr,
    const std::function<HRESULT(const wchar_t*, size_t, IUnknown**)>& start_delete) {
  IUnknown* action = nullptr;
  HRESULT r = start_delete(name, length, &action);
  if (action == nullptr) {
    *hr = FAILED(r) ? r : E_POINTER;
    return kFailed;
  }
  // Even an inconsistent failed start with an action cannot justify dropping a
  // possibly running deletion. Keep this exact action until it is terminal.
  IAsyncInfo* info = nullptr;
  bool warned = false;
  while (info == nullptr) {
    r = action->QueryInterface(winrt::IID_IAsyncInfo, reinterpret_cast<void**>(&info));
    if (SUCCEEDED(r) && info != nullptr) break;
    winrt::Release(info);
    if (!warned) {
      Log("QueryInterface(IAsyncInfo) (delete)", FAILED(r) ? r : E_POINTER);
      warned = true;
    }
    Sleep(1000);
  }
  int status = kStarted;
  while (true) {
    r = info->get_Status(&status);
    if (SUCCEEDED(r) && status >= kCompleted && status <= kError) break;
    if ((FAILED(r) || status != kStarted) && !warned) {
      Log("IAsyncInfo::get_Status (delete)", FAILED(r) ? r : E_UNEXPECTED);
      warned = true;
    }
    Sleep(100);
  }
  if (status == kCompleted) {
    // One-shot handoff, including its HRESULT. A failure here may be retried as
    // a new action only because the prior action is already known to be terminal.
    r = reinterpret_cast<IAsyncAction*>(action)->GetResults();
  } else {
    HRESULT error = E_FAIL;
    r = info->get_ErrorCode(&error);
    if (SUCCEEDED(r)) r = FAILED(error) ? error : E_FAIL;
  }
  info->Close();
  winrt::Release(info);
  winrt::Release(action);
  *hr = r;
  return SUCCEEDED(r) ? kSuccess : kFailed;
}

Operation* StartCreate(
    const wchar_t* name, size_t length,
    const std::function<HRESULT(IUnknown**)>& start,
    const std::function<int(const wchar_t*, size_t, HRESULT*)>& delete_created) {
  // Never truncate a name whose ownership determines what cleanup may delete.
  if (name == nullptr || length == 0 || length > kMaxName) return Immediate(kFailed);
  auto state = std::make_shared<CreateState>();
  state->name.assign(name, length);
  state->delete_created = delete_created;
  auto owner = std::make_unique<std::shared_ptr<CreateState>>(state);
  auto result = std::make_unique<Operation>();
  bool empty = false;
  if (!g_create_pending.compare_exchange_strong(empty, true)) return Immediate(kFailed);
  PTP_WORK work = CreateThreadpoolWork(FinishCreate, owner.get(), nullptr);
  if (work == nullptr) {
    g_create_pending.store(false);
    return Immediate(kFailed);
  }
  IUnknown* async = nullptr;
  HRESULT r = start(&async);
  if (async == nullptr) {
    Log("RequestCreateAsync start", FAILED(r) ? r : E_POINTER);
    CloseThreadpoolWork(work);
    g_create_pending.store(false);
    return Immediate(kFailed);
  }
  if (FAILED(r)) FailCreate(state, "RequestCreateAsync start", r);
  // Once an operation exists it owns the reservation, even if acquiring its
  // IAsyncInfo interface fails. The worker reports failure and retains it while
  // retrying observation; no second create is admitted behind an unknown one.
  state->operation = reinterpret_cast<IAsyncOperation<IInspectable*>*>(async);
  result->create = state;
  owner.release();  // the single submitted work item owns this reference
  SubmitThreadpoolWork(work);
  return result.release();
}

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
  return internal::StartCreate(name, length, [&](IUnknown** async) {
    IKeyCredentialManagerStatics* statics = nullptr;
    HRESULT r = Manager(&statics);
    if (FAILED(r)) return r;
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
      if (FAILED(r) && operation == nullptr)
        r = statics->RequestCreateAsync(text, kFailIfExists, &operation);
    }
    winrt::DeleteString(text);
    winrt::Release(statics);
    *async = operation;
    return r;
  }, DeleteCreated);
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
  if (o->create) {
    auto state = o->create;
    std::unique_lock<std::mutex> guard(state->lock);
    while (!state->ready) {
      ULONGLONG requested = state->cancel_tick.load();
      if (requested != 0 && GetTickCount64() - requested >= cancel_grace_ms) {
        return kUserCanceled;  // the one native owner still observes and cleans up
      }
      state->changed.wait_for(guard, std::chrono::milliseconds(20));
    }
    if (state->cancel_tick.load() != 0) return kUserCanceled;
    if (state->result == kSuccess) *bytes = state->bytes;
    return state->result;
  }
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
  if (o->create) {
    // Do not call COM on the Java owner-watch thread while it holds Request's
    // release lock. The single native owner sends Cancel outside any such lock.
    ULONGLONG expected = 0;
    ULONGLONG now = GetTickCount64();
    o->create->cancel_tick.compare_exchange_strong(expected, now != 0 ? now : 1);
    o->create->changed.notify_all();
    return;
  }
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

void Release(Operation* o, bool accept_result) {
  if (o->create) {
    auto state = o->create;
    {
      std::lock_guard<std::mutex> guard(state->lock);
      state->accepted = accept_result;
      state->disposed = true;
    }
    if (!accept_result) Cancel(o);
    state->changed.notify_all();
    delete o;
    return;
  }
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

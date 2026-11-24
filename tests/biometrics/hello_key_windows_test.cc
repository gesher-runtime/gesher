// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/hello_key_win.cc (ADR 0095), run against
// the real KeyCredentialManager without showing any UI:
//  - IsSupportedAsync answers (a runner usually has no Windows Hello);
//  - a key that does not exist is NotFound, for OpenAsync and for deleting it;
//  - creating a key or signing for a process that owns no foreground window never
//    starts: kNotForeground, no prompt;
// and against scripted IAsyncOperation objects handed to internal::Wrap: a signature
// is exactly the bytes of Windows' result, a status other than Success carries none, a
// cancel Windows honors late, a cancel Windows refuses or ignores (the wait is bounded,
// the abandoned request's signature or key is never read and its references are
// released), and the WinRT apartment of the awaiting thread.
// Creating and signing need a person: tests/biometrics/hello_key_probe.cc.

#include <windows.h>
#include <objbase.h>
#include <stdio.h>

#include <atomic>
#include <initializer_list>
#include <vector>

#include "hello_key_win.h"
#include "winrt.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  fflush(stdout);
}

namespace {

const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// Windows.Storage.Streams.IBufferByteAccess (robuffer.h).
const IID kIidBufferByteAccess = {
    0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2;  // AsyncStatus
constexpr int kUserCanceled = 3;                            // KeyCredentialStatus
const HRESULT kIllegalStateChange = static_cast<HRESULT>(0x8000000DL);  // E_ILLEGAL_STATE_CHANGE

// The ABI of Windows.Storage.Streams.IBuffer and IBufferByteAccess.
struct IBufferAbi : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 value) = 0;
};
struct IBufferByteAccessAbi : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) = 0;
};

class ScriptedBuffer final : public IBufferAbi, public IBufferByteAccessAbi {
 public:
  std::vector<uint8_t> bytes;
  std::atomic<ULONG> refs{1};  // the test's own reference

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (IsEqualIID(iid, kIidUnknown)) {
      *out = static_cast<IBufferAbi*>(this);
    } else if (IsEqualIID(iid, kIidBufferByteAccess)) {
      *out = static_cast<IBufferByteAccessAbi*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(winrt::HSTRING*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* value) override {
    *value = static_cast<UINT32>(bytes.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Length(UINT32* value) override {
    *value = static_cast<UINT32>(bytes.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Length(UINT32) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) override {
    *value = bytes.data();
    return S_OK;
  }
};

// The ABI of KeyCredentialOperationResult's default interface.
struct IKeyCredentialOperationResultAbi : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Result(IBufferAbi** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int* value) = 0;
};

class ScriptedSignResult final : public IKeyCredentialOperationResultAbi {
 public:
  ScriptedBuffer* buffer = nullptr;
  int status = 0;
  std::atomic<int> results_read{0};
  std::atomic<ULONG> refs{1};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(winrt::HSTRING*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Result(IBufferAbi** value) override {
    results_read++;
    *value = buffer;
    if (buffer != nullptr) buffer->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Status(int* value) override {
    *value = status;
    return S_OK;
  }
};

// An IAsyncOperation<KeyCredentialOperationResult> (or, for a creation that never
// completes, <KeyCredentialRetrievalResult>) whose course the test scripts.
class ScriptedOperation final : public winrt::IAsyncOperation<winrt::IInspectable*>,
                                public winrt::IAsyncInfo {
 public:
  enum class OnCancel { kComplete, kFail, kIgnore };
  OnCancel on_cancel = OnCancel::kComplete;
  DWORD cancel_delay_ms = 0;  // kComplete: the status changes this long after Cancel
  ScriptedSignResult* result = nullptr;
  std::atomic<int> status{kStarted};
  std::atomic<int> cancels{0}, closes{0}, results_read{0};
  std::atomic<ULONG> refs{1};  // the test's own reference

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (IsEqualIID(iid, kIidUnknown)) {
      *out = static_cast<winrt::IAsyncOperation<winrt::IInspectable*>*>(this);
    } else if (IsEqualIID(iid, winrt::IID_IAsyncInfo)) {
      *out = static_cast<winrt::IAsyncInfo*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(winrt::HSTRING*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetResults(winrt::IInspectable** value) override {
    results_read++;
    *value = result;
    if (result != nullptr) result->AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Id(UINT32*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Status(int* value) override {
    if (cancel_at_ != 0 && GetTickCount64() >= cancel_at_) status = kCanceled;
    *value = status;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) override {
    *code = S_OK;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Cancel() override {
    cancels++;
    if (on_cancel == OnCancel::kFail) return E_FAIL;
    if (on_cancel == OnCancel::kComplete) cancel_at_ = GetTickCount64() + cancel_delay_ms;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Close() override {
    closes++;
    return status == kStarted ? kIllegalStateChange : S_OK;
  }

 private:
  std::atomic<ULONGLONG> cancel_at_{0};
};

struct Waiter {
  hello_key::Operation* o = nullptr;
  DWORD grace = hello_key::kCancelGraceMs;
  int result = -100;
  std::vector<uint8_t> bytes;
  HRESULT apartment_hr = E_FAIL;
  APTTYPE type = APTTYPE_CURRENT;
  APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
  HANDLE thread = nullptr;
  ULONGLONG started = 0;
};

DWORD WINAPI AwaitThread(void* argument) {
  Waiter* w = static_cast<Waiter*>(argument);
  w->result = hello_key::Await(w->o, &w->bytes, w->grace);
  w->apartment_hr = CoGetApartmentType(&w->type, &w->qualifier);
  return 0;
}

void StartWaiter(Waiter* w) {
  w->started = GetTickCount64();
  w->thread = CreateThread(nullptr, 0, AwaitThread, w, 0, nullptr);
}

// True when Await returned within `limit_ms`; a waiter still blocked stays blocked (the
// process ends with ExitProcess).
bool Returned(Waiter* w, DWORD limit_ms, ULONGLONG* elapsed) {
  bool done = WaitForSingleObject(w->thread, limit_ms) == WAIT_OBJECT_0;
  *elapsed = GetTickCount64() - w->started;
  return done;
}

// Hands Wrap its own reference, as Windows hands one to the caller of the request.
hello_key::Operation* Wrapped(bool sign, ScriptedOperation* op) {
  op->AddRef();
  return hello_key::internal::Wrap(
      sign, static_cast<winrt::IAsyncOperation<winrt::IInspectable*>*>(op));
}

void ScriptedContracts() {
  ULONGLONG elapsed = 0;
  const std::vector<uint8_t> signature = {0x30, 0x00, 0xff, 0x7f, 0x80, 0x01};

  {  // A signature Windows returned.
    ScriptedBuffer buffer;
    buffer.bytes = signature;
    ScriptedSignResult result;
    result.buffer = &buffer;
    ScriptedOperation op;
    op.result = &result;
    Waiter w;
    w.o = Wrapped(true, &op);
    StartWaiter(&w);
    Sleep(100);
    op.status = kCompleted;
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == hello_key::kSuccess && w.bytes == signature,
          "Await returns Success and exactly the bytes of Windows' result");
    Check(done && w.apartment_hr == S_OK && w.type == APTTYPE_MTA &&
              w.qualifier != APTTYPEQUALIFIER_IMPLICIT_MTA,
          "the awaiting thread joined the WinRT apartment itself");
    if (done) {
      hello_key::Release(w.o);
      Check(op.refs == 1 && op.closes == 1 && result.refs == 1 && buffer.refs == 1,
            "Release closes a finished request and every reference is dropped");
    }
  }

  {  // Windows' own answer that the user canceled: a status, no bytes.
    ScriptedBuffer buffer;
    buffer.bytes = signature;
    ScriptedSignResult result;
    result.buffer = &buffer;
    result.status = kUserCanceled;
    ScriptedOperation op;
    op.result = &result;
    op.status = kCompleted;
    Waiter w;
    w.o = Wrapped(true, &op);
    StartWaiter(&w);
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == kUserCanceled && w.bytes.empty() && result.results_read == 0,
          "a result whose status is not Success gives that status and no bytes");
    if (done) hello_key::Release(w.o);
  }

  {  // Windows honors the cancel, a little later.
    ScriptedOperation op;
    op.cancel_delay_ms = 150;
    Waiter w;
    w.o = Wrapped(true, &op);
    StartWaiter(&w);
    Sleep(50);
    hello_key::Cancel(w.o);
    hello_key::Cancel(w.o);
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == kUserCanceled && w.bytes.empty(),
          "a cancel Windows honors late ends UserCanceled");
    Check(done && elapsed < 2000, "and well inside the grace period");
    Check(op.cancels == 1, "Cancel reaches Windows once however often it is called");
    if (done) hello_key::Release(w.o);
  }

  struct Case {
    bool sign;
    ScriptedOperation::OnCancel mode;
    const char* ends;
  };
  for (const Case& c : {
           Case{true, ScriptedOperation::OnCancel::kFail,
                "a signature whose cancel Windows refuses still ends the wait, UserCanceled"},
           Case{true, ScriptedOperation::OnCancel::kIgnore,
                "a signature whose cancel Windows ignores still ends the wait, UserCanceled"},
           Case{false, ScriptedOperation::OnCancel::kIgnore,
                "a creation whose cancel Windows ignores still ends the wait, UserCanceled"},
       }) {
    ScriptedBuffer buffer;
    buffer.bytes = signature;
    ScriptedSignResult result;
    result.buffer = &buffer;
    ScriptedOperation op;
    op.on_cancel = c.mode;
    Waiter w;
    w.o = Wrapped(c.sign, &op);
    w.grace = 300;
    StartWaiter(&w);
    Sleep(50);
    hello_key::Cancel(w.o);
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == kUserCanceled && w.bytes.empty(), c.ends);
    Check(done && elapsed >= 300, "only after the grace period");
    if (!done) continue;  // the waiter owns the request forever: the defect under test
    // The user answers the abandoned prompt later: nobody may read that signature.
    if (c.sign) op.result = &result;
    op.status = kCompleted;
    hello_key::Release(w.o);
    Check(op.results_read == 0 && result.results_read == 0,
          "an abandoned request's late result is never read");
    Check(op.refs == 1 && op.closes == 0, "and its references are released, without Close");
  }
}

}  // namespace

int main() {
  HRESULT hr = E_FAIL;
  int supported = hello_key::Supported(&hr);
  printf("supported=%d hr=0x%08lx\n", supported, (unsigned long)hr);
  Check((supported == 0 || supported == 1) && SUCCEEDED(hr), "IsSupportedAsync answers yes or no");
  Check(hello_key::Supported(&hr) == supported, "and the same when asked again");

  const wchar_t name[] = L"Gesher test: a key nobody created 7c1d6f0e";
  const size_t length = sizeof(name) / sizeof(name[0]) - 1;
  std::vector<uint8_t> bytes(3, 0x55);
  hr = E_FAIL;
  int status = hello_key::Open(name, length, &bytes, &hr);
  printf("open status=%d hr=0x%08lx\n", status, (unsigned long)hr);
  Check(status == 2 && bytes.empty(), "opening a key that does not exist: NotFound, no public key");
  status = hello_key::Delete(name, length, &hr);
  printf("delete status=%d hr=0x%08lx\n", status, (unsigned long)hr);
  Check(status == 2, "deleting a key that does not exist: NotFound");

  // This console test owns no window, so it cannot be the foreground owner.
  const uint8_t data[] = {1, 2, 3};
  hello_key::Operation* o = hello_key::StartCreate(GetCurrentProcessId(), name, length);
  bytes.assign(3, 0x55);
  status = hello_key::Await(o, &bytes);
  hello_key::Cancel(o);  // harmless on an operation that never started
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground && bytes.empty(),
        "creating a key without the foreground window: kNotForeground, no prompt");
  o = hello_key::StartSign(GetCurrentProcessId(), name, length, data, sizeof(data));
  status = hello_key::Await(o, &bytes);
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground && bytes.empty(),
        "signing without the foreground window: kNotForeground, no prompt");
  o = hello_key::StartSign(0xFFFFFFF0u, name, length, data, sizeof(data));
  status = hello_key::Await(o, &bytes);
  hello_key::Release(o);
  Check(status == hello_key::kNotForeground, "an unknown pid is never an owner");
  Check(hello_key::Open(name, length, &bytes, &hr) == 2, "and no key was created by any of it");

  ScriptedContracts();

  printf("%d checks, %d failures\n", checks, failures);
  fflush(stdout);
  // A waiter the defect left blocked must not keep the process alive.
  ExitProcess(failures == 0 ? 0 : 1);
}

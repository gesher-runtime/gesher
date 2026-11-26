// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/hello_win.cc (ADR 0091, phase 2), run
// without showing any UI:
//  - against the real UserConsentVerifier: availability is one of Windows' five values
//    (a runner usually has no Hello), and a verification for a process that owns no
//    foreground window never starts: kNotForeground, no prompt, no operation;
//  - against scripted IAsyncOperation objects handed to internal::Wrap: completion,
//    a cancel Windows honors late, a cancel Windows refuses or ignores (the wait is
//    bounded, the abandoned operation's result is never read and its references are
//    released), and the WinRT apartment of the awaiting thread.
// tests/biometrics/hello_probe.cc covers the real prompt by hand.

#include <windows.h>
#include <objbase.h>
#include <stdio.h>

#include <atomic>

#include "hello_win.h"

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
constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2;
// The pinned llvm-mingw headers do not define this WinRT HRESULT.
constexpr HRESULT kIllegalStateChange = static_cast<HRESULT>(0x8000000Du);

// An IAsyncOperation<UserConsentVerificationResult> whose course the test scripts.
class ScriptedOperation final : public winrt::IAsyncOperation<int>, public winrt::IAsyncInfo {
 public:
  enum class OnCancel { kComplete, kFail, kIgnore };
  OnCancel on_cancel = OnCancel::kComplete;
  DWORD cancel_delay_ms = 0;  // kComplete: the status changes this long after Cancel
  std::atomic<int> status{kStarted};
  std::atomic<int> result{-1};
  std::atomic<int> cancels{0}, closes{0}, results_read{0};
  std::atomic<ULONG> refs{1};  // the test's own reference

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (IsEqualIID(iid, kIidUnknown)) {
      *out = static_cast<winrt::IAsyncOperation<int>*>(this);
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
  HRESULT STDMETHODCALLTYPE GetResults(int* value) override {
    results_read++;
    *value = result;
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
  hello::Verification* v = nullptr;
  DWORD grace = hello::kCancelGraceMs;
  int result = -100;
  HRESULT apartment_hr = E_FAIL;
  APTTYPE type = APTTYPE_CURRENT;
  APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
  HANDLE thread = nullptr;
  ULONGLONG started = 0;
};

DWORD WINAPI AwaitThread(void* argument) {
  Waiter* w = static_cast<Waiter*>(argument);
  w->result = hello::Await(w->v, w->grace);
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

// Hands Wrap its own reference, as Windows hands one to the caller of the interop.
hello::Verification* Wrapped(ScriptedOperation* op, HRESULT* hr) {
  op->AddRef();
  return hello::internal::Wrap(op, hr);
}

void ScriptedContracts() {
  HRESULT hr = E_FAIL;
  ULONGLONG elapsed = 0;

  {  // Completion without any cancel.
    ScriptedOperation op;
    hello::Verification* v = Wrapped(&op, &hr);
    Check(v != nullptr && SUCCEEDED(hr), "a started operation is wrapped");
    Waiter w;
    w.v = v;
    StartWaiter(&w);
    Sleep(100);
    op.result = 0;  // Verified
    op.status = kCompleted;
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == 0, "Await returns Windows' result when the operation completes");
    Check(done && w.apartment_hr == S_OK && w.type == APTTYPE_MTA &&
              w.qualifier != APTTYPEQUALIFIER_IMPLICIT_MTA,
          "the awaiting thread joined the WinRT apartment itself");
    if (done) {
      hello::Release(v);
      Check(op.refs == 1 && op.closes == 1, "Release closes a finished operation and drops its references");
    }
  }

  {  // Windows honors the cancel, a little later.
    ScriptedOperation op;
    op.cancel_delay_ms = 150;
    hello::Verification* v = Wrapped(&op, &hr);
    Waiter w;
    w.v = v;
    StartWaiter(&w);
    Sleep(50);
    hello::Cancel(v);
    hello::Cancel(v);
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == 6, "a cancel Windows honors late ends Canceled");
    Check(done && elapsed < 2000, "and well inside the grace period");
    Check(op.cancels == 1, "Cancel reaches Windows once however often it is called");
    if (done) hello::Release(v);
  }

  for (ScriptedOperation::OnCancel mode :
       {ScriptedOperation::OnCancel::kFail, ScriptedOperation::OnCancel::kIgnore}) {
    bool fails = mode == ScriptedOperation::OnCancel::kFail;
    ScriptedOperation op;
    op.on_cancel = mode;
    hello::Verification* v = Wrapped(&op, &hr);
    Waiter w;
    w.v = v;
    w.grace = 300;
    StartWaiter(&w);
    Sleep(50);
    hello::Cancel(v);
    bool done = Returned(&w, 3000, &elapsed);
    Check(done && w.result == 6,
          fails ? "a cancel Windows refuses still ends the wait, Canceled"
                : "a cancel Windows ignores still ends the wait, Canceled");
    Check(done && elapsed >= 300, "only after the grace period");
    if (!done) continue;  // the waiter owns v forever: the defect under test
    // Windows finishes the abandoned prompt later, as verified: nobody may read that.
    op.result = 0;
    op.status = kCompleted;
    hello::Release(v);
    Check(op.results_read == 0, "an abandoned operation's late result is never read");
    Check(op.refs == 1, "and its references are released");
  }
}

}  // namespace

int main() {
  HRESULT hr = E_FAIL;
  int availability = hello::Availability(&hr);
  printf("availability=%d hr=0x%08lx\n", availability, (unsigned long)hr);
  Check(availability >= 0 && availability <= 4 && SUCCEEDED(hr),
        "availability is a UserConsentVerifierAvailability value");
  Check(hello::Availability(&hr) == availability, "availability is stable across queries");

  const wchar_t message[] = L"must not be shown";
  // This console test owns no window, so it cannot be the foreground owner.
  hello::StartFailure failure = hello::StartFailure::kNone;
  hr = E_FAIL;
  hello::Verification* v = hello::Start(GetCurrentProcessId(), message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground && hr == S_OK,
        "a process without the foreground window gets kNotForeground and no prompt");
  if (v != nullptr) {
    hello::Cancel(v);
    hello::Await(v);
    hello::Release(v);
  }

  // PID 0 (System Idle) and a PID no process has: never an owner.
  failure = hello::StartFailure::kNone;
  v = hello::Start(0, message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground, "pid 0 is never an owner");
  failure = hello::StartFailure::kNone;
  v = hello::Start(0xFFFFFFF0u, message, 17, &failure, &hr);
  Check(v == nullptr && failure == hello::StartFailure::kNotForeground,
        "an unknown pid is never an owner");

  ScriptedContracts();

  printf("%d checks, %d failures\n", checks, failures);
  fflush(stdout);
  // A waiter the defect left blocked must not keep the process alive.
  ExitProcess(failures == 0 ? 0 : 1);
}

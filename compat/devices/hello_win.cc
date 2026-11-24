// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows Hello user-consent verification through its
// ABI (docs/research/windows-hello-native-spec.md, ADR 0091).

#include "hello_win.h"

#include <stdio.h>

#include <atomic>
#include <mutex>

#include "winrt.h"

namespace gesher::devices::hello {

using winrt::IAsyncInfo;
using winrt::IAsyncOperation;
using winrt::IInspectable;
using winrt::HSTRING;

namespace {

constexpr wchar_t kVerifier[] = L"Windows.Security.Credentials.UI.UserConsentVerifier";

// windows.security.credentials.ui.idl
struct IUserConsentVerifierStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CheckAvailabilityAsync(IAsyncOperation<int>** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestVerificationAsync(HSTRING message,
                                                             IAsyncOperation<int>** result) = 0;
};
const IID IID_IUserConsentVerifierStatics = {
    0xAF4F3F91, 0x564C, 0x4DDC, {0xB8, 0xB5, 0x97, 0x34, 0x47, 0x62, 0x7C, 0x65}};

// UserConsentVerifierInterop.h (desktop partition).
struct IUserConsentVerifierInterop : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RequestVerificationForWindowAsync(HWND appWindow,
                                                                      HSTRING message,
                                                                      REFIID riid,
                                                                      void** asyncOperation) = 0;
};
const IID IID_IUserConsentVerifierInterop = {
    0x39E050C3, 0x4E74, 0x441A, {0x8D, 0xC0, 0xB8, 0x11, 0x04, 0xDF, 0x94, 0x9C}};

// IAsyncOperation<UserConsentVerificationResult>.
const IID IID_IAsyncOperation_VerificationResult = {
    0xfd596ffd, 0x2318, 0x558f, {0x9d, 0xbe, 0xd2, 0x1d, 0xf4, 0x37, 0x64, 0xa5}};

constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2;
constexpr int kResultCanceled = 6;
constexpr size_t kMaxMessage = 1024;

void Log(const char* call, HRESULT hr) {
  fprintf(stderr, "gesher hello: %s failed: 0x%08lx\n", call, static_cast<unsigned long>(hr));
}

// The foreground top-level window of `pid`, checked as ForegroundProcess
// (clipboard_access.cc) does: owner alive and unchanged across the query.
HWND ForegroundWindowOf(DWORD pid) {
  HWND window = GetForegroundWindow();
  if (window == nullptr) return nullptr;
  DWORD owner = 0;
  DWORD thread = GetWindowThreadProcessId(window, &owner);
  if (thread == 0 || owner != pid) return nullptr;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner);
  if (process == nullptr) return nullptr;
  DWORD current_owner = 0;
  bool valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
               GetWindowThreadProcessId(window, &current_owner) == thread &&
               current_owner == owner && GetForegroundWindow() == window;
  CloseHandle(process);
  if (!valid) return nullptr;
  HWND root = GetAncestor(window, GA_ROOTOWNER);
  return root != nullptr ? root : window;
}

}  // namespace

struct Verification {
  IAsyncOperation<int>* operation = nullptr;
  IAsyncInfo* info = nullptr;
  std::mutex lock;
  // GetTickCount64 at the first Cancel (never 0 once requested): the start of
  // Await's grace period.
  std::atomic<ULONGLONG> cancel_tick{0};
  // Await gave up on an operation Windows did not end after Cancel.
  std::atomic<bool> abandoned{false};
  bool released = false;
};

int Availability(HRESULT* hr) {
  *hr = S_OK;
  IUserConsentVerifierStatics* statics = nullptr;
  HRESULT r = winrt::GetActivationFactory(kVerifier, IID_IUserConsentVerifierStatics,
                                          reinterpret_cast<void**>(&statics));
  if (FAILED(r)) {
    Log("GetActivationFactory(UserConsentVerifier)", r);
    *hr = r;
    return -1;
  }
  IAsyncOperation<int>* operation = nullptr;
  r = statics->CheckAvailabilityAsync(&operation);
  winrt::Release(statics);
  if (FAILED(r)) {
    Log("CheckAvailabilityAsync", r);
    *hr = r;
    return -1;
  }
  r = winrt::Wait(operation, 2000);
  int availability = -1;
  if (SUCCEEDED(r)) r = operation->GetResults(&availability);
  winrt::Release(operation);
  if (FAILED(r)) {
    Log("CheckAvailabilityAsync result", r);
    *hr = r;
    return -1;
  }
  return availability;
}

Verification* Start(DWORD pid, const wchar_t* message, size_t length, StartFailure* failure,
                    HRESULT* hr) {
  *failure = StartFailure::kNone;
  *hr = S_OK;
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r)) {
    Log("RoInitialize", r);
    *failure = StartFailure::kWindows;
    *hr = r;
    return nullptr;
  }
  HWND owner = ForegroundWindowOf(pid);
  if (owner == nullptr) {
    *failure = StartFailure::kNotForeground;
    return nullptr;
  }
  IUserConsentVerifierInterop* interop = nullptr;
  r = winrt::GetActivationFactory(kVerifier, IID_IUserConsentVerifierInterop,
                                  reinterpret_cast<void**>(&interop));
  if (FAILED(r)) {
    Log("GetActivationFactory(IUserConsentVerifierInterop)", r);
    *failure = StartFailure::kWindows;
    *hr = r;
    return nullptr;
  }
  HSTRING text = nullptr;
  r = winrt::CreateString(message, static_cast<UINT32>(length < kMaxMessage ? length : kMaxMessage),
                          &text);
  if (FAILED(r)) {
    Log("WindowsCreateString", r);
    winrt::Release(interop);
    *failure = StartFailure::kWindows;
    *hr = r;
    return nullptr;
  }
  IAsyncOperation<int>* operation = nullptr;
  r = interop->RequestVerificationForWindowAsync(owner, text, IID_IAsyncOperation_VerificationResult,
                                                 reinterpret_cast<void**>(&operation));
  winrt::DeleteString(text);
  winrt::Release(interop);
  if (FAILED(r)) {
    Log("RequestVerificationForWindowAsync", r);
    *failure = StartFailure::kWindows;
    *hr = r;
    return nullptr;
  }
  Verification* v = internal::Wrap(operation, &r);
  if (v == nullptr) {
    Log("IAsyncOperation::QueryInterface(IAsyncInfo)", r);
    *failure = StartFailure::kWindows;
    *hr = r;
  }
  return v;
}

namespace internal {

Verification* Wrap(IAsyncOperation<int>* operation, HRESULT* hr) {
  IAsyncInfo* info = nullptr;
  *hr = operation->QueryInterface(winrt::IID_IAsyncInfo, reinterpret_cast<void**>(&info));
  if (FAILED(*hr)) {
    winrt::Release(operation);
    return nullptr;
  }
  Verification* v = new Verification;
  v->operation = operation;
  v->info = info;
  return v;
}

}  // namespace internal

int Await(Verification* v, DWORD cancel_grace_ms) {
  // The awaiting thread is WindowsHello's own daemon thread: its first WinRT
  // call is here, so it joins the apartment itself (spec section 5).
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r)) Log("RoInitialize (await)", r);
  // A user may take minutes: no time-out while nobody canceled, 1 -> 20 ms
  // back-off.
  int status = kStarted;
  DWORD delay = 1;
  while (SUCCEEDED(r = v->info->get_Status(&status)) && status == kStarted) {
    ULONGLONG requested = v->cancel_tick.load();
    if (requested != 0 && GetTickCount64() - requested >= cancel_grace_ms) {
      // Windows refused the cancel or ignores it. The session is over for the
      // app (AuthService reported the cancel when it was requested), so the
      // operation is abandoned: Release drops the references without reading
      // a result, and whatever Windows' prompt reports later reaches nobody.
      fprintf(stderr,
              "gesher hello: Windows did not end a canceled verification within %lu ms; "
              "abandoning it\n",
              static_cast<unsigned long>(cancel_grace_ms));
      v->abandoned.store(true);
      return kResultCanceled;
    }
    Sleep(delay);
    if (delay < 20) delay *= 2;
  }
  if (FAILED(r)) {
    Log("IAsyncInfo::get_Status", r);
    return -1;
  }
  if (status == kCanceled) return kResultCanceled;
  if (status != kCompleted) {
    HRESULT code = E_FAIL;
    v->info->get_ErrorCode(&code);
    Log("RequestVerificationForWindowAsync completion", code);
    return -1;
  }
  int result = -1;
  r = v->operation->GetResults(&result);
  if (FAILED(r)) {
    Log("GetResults(UserConsentVerificationResult)", r);
    return -1;
  }
  return result;
}

void Cancel(Verification* v) {
  // Called from AuthService's thread or a binder thread.
  HRESULT r = winrt::EnsureApartment();
  if (FAILED(r)) Log("RoInitialize (cancel)", r);
  std::lock_guard<std::mutex> guard(v->lock);
  if (v->released || v->cancel_tick.load() != 0) return;
  // The grace period runs from the request, also when the call below fails or
  // takes long.
  ULONGLONG now = GetTickCount64();
  v->cancel_tick.store(now != 0 ? now : 1);
  r = v->info->Cancel();
  if (FAILED(r)) Log("IAsyncInfo::Cancel", r);
}

void Release(Verification* v) {
  {
    std::lock_guard<std::mutex> guard(v->lock);
    v->released = true;
    // IAsyncInfo::Close is only legal on a finished operation; an abandoned one
    // still runs inside Windows and ends there.
    if (!v->abandoned.load()) v->info->Close();
    winrt::Release(v->info);
    winrt::Release(v->operation);
  }
  delete v;
}

}  // namespace gesher::devices::hello

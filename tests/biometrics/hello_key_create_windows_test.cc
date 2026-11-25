// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// The real hello_key_win.cc create lifecycle, with scripted WinRT boundaries.
// No Windows Hello ceremony or key store is fabricated: only the async result
// and deletion boundary are supplied by this isolated test. The test proves
// ownership, cancellation and reclamation, not interactive Hello availability.

#include <windows.h>
#include <stdio.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hello_key_win.h"
#include "winrt.h"

namespace hk = gesher::devices::hello_key;
namespace wr = gesher::devices::winrt;

namespace {

int checks = 0, failures = 0;
constexpr int kStarted = 0, kCompleted = 1, kCanceled = 2, kError = 3;
constexpr int kNotFound = 2, kUserCanceled = 3, kAlreadyExists = 5;
const IID kUnknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const IID kBufferBytes = {
    0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

void Check(bool ok, const char* description) {
  ++checks;
  if (!ok) ++failures;
  printf("%s %s\n", ok ? "PASS" : "FAIL", description);
  fflush(stdout);
}

[[noreturn]] void Fatal(const char* description) {
  Check(false, description);
  // A failed lifetime assertion must not return and destroy objects still used
  // by the production worker. This is failure, never abandonment as success.
  ExitProcess(2);
}

template <class Predicate>
bool Until(Predicate predicate, DWORD limit_ms = 5000) {
  ULONGLONG start = GetTickCount64();
  do {
    if (predicate()) return true;
    Sleep(5);
  } while (GetTickCount64() - start < limit_ms);
  return predicate();
}

struct Watchdog {
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = nullptr;
  Watchdog() {
    if (done == nullptr) Fatal("create test watchdog event");
    thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
    if (thread == nullptr) Fatal("create test watchdog thread");
  }
  ~Watchdog() {
    SetEvent(done);
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
      Fatal("join test watchdog");
    CloseHandle(thread);
    CloseHandle(done);
  }
  static DWORD WINAPI Run(void* value) {
    auto* self = static_cast<Watchdog*>(value);
    if (WaitForSingleObject(self->done, 45000) != WAIT_OBJECT_0) {
      fprintf(stderr, "FAIL create lifecycle exceeded the 45-second process deadline\n");
      fflush(stderr);
      ExitProcess(2);
    }
    return 0;
  }
};

// These are the SDK vtable layouts used by the production implementation.
struct BufferAbi : wr::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32) = 0;
};
struct BufferBytesAbi : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE**) = 0;
};
struct CredentialAbi : wr::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Name(wr::HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithDefaultBlobType(BufferAbi**) = 0;
  virtual HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithBlobType(int, BufferAbi**) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestSignAsync(BufferAbi*, IUnknown**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAttestationAsync(IUnknown**) = 0;
};
struct RetrievalAbi : wr::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Credential(CredentialAbi**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
};
struct ActionAbi : wr::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};

#define SCRIPTED_INSPECTABLE_METHODS                                            \
  std::atomic<ULONG> refs{1};                                                   \
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }                  \
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }                 \
  HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; } \
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(wr::HSTRING*) override {         \
    return E_NOTIMPL;                                                          \
  }                                                                            \
  HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) override { return E_NOTIMPL; }

struct ScriptedBuffer final : BufferAbi, BufferBytesAbi {
  SCRIPTED_INSPECTABLE_METHODS
  const std::vector<uint8_t> bytes{0x30, 0x82, 0, 0xff, 0x7f, 0x80, 0x01};
  HRESULT bytes_hr = S_OK;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    *out = nullptr;
    if (IsEqualIID(iid, kUnknown)) *out = static_cast<BufferAbi*>(this);
    else if (IsEqualIID(iid, kBufferBytes)) *out = static_cast<BufferBytesAbi*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* value) override {
    *value = static_cast<UINT32>(bytes.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Length(UINT32* value) override { return get_Capacity(value); }
  HRESULT STDMETHODCALLTYPE put_Length(UINT32) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) override {
    *value = SUCCEEDED(bytes_hr) ? const_cast<BYTE*>(bytes.data()) : nullptr;
    return bytes_hr;
  }
};

struct ScriptedCredential final : CredentialAbi {
  SCRIPTED_INSPECTABLE_METHODS
  ScriptedBuffer* buffer = nullptr;
  HRESULT public_hr = S_OK;
  std::atomic<int> public_reads{0};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE get_Name(wr::HSTRING*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithDefaultBlobType(BufferAbi** value) override {
    ++public_reads;
    *value = SUCCEEDED(public_hr) ? buffer : nullptr;
    if (*value != nullptr) (*value)->AddRef();
    return public_hr;
  }
  HRESULT STDMETHODCALLTYPE RetrievePublicKeyWithBlobType(int, BufferAbi**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE RequestSignAsync(BufferAbi*, IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAttestationAsync(IUnknown**) override { return E_NOTIMPL; }
};

struct ScriptedResult final : RetrievalAbi {
  SCRIPTED_INSPECTABLE_METHODS
  ScriptedCredential* credential = nullptr;
  int status = hk::kSuccess;
  std::atomic<HRESULT> status_hr{S_OK};
  HRESULT credential_hr = S_OK;
  std::atomic<int> status_reads{0}, credential_reads{0};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE get_Credential(CredentialAbi** value) override {
    ++credential_reads;
    *value = SUCCEEDED(credential_hr) ? credential : nullptr;
    if (*value != nullptr) (*value)->AddRef();
    return credential_hr;
  }
  HRESULT STDMETHODCALLTYPE get_Status(int* value) override {
    ++status_reads;
    *value = status;
    return status_hr;
  }
};

struct ScriptedAsync final : wr::IAsyncOperation<wr::IInspectable*>, wr::IAsyncInfo {
  SCRIPTED_INSPECTABLE_METHODS
  ScriptedResult* result = nullptr;
  std::atomic<HRESULT> info_hr{S_OK}, results_hr{S_OK};
  HRESULT cancel_hr = S_OK;
  HANDLE cancel_gate = nullptr;
  std::atomic<int> status{kStarted}, results_read{0}, cancels{0}, closes{0};
  std::atomic<bool> cancel_returned{false}, fail_status_observation{false};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    *out = nullptr;
    if (IsEqualIID(iid, kUnknown))
      *out = static_cast<wr::IAsyncOperation<wr::IInspectable*>*>(this);
    else if (IsEqualIID(iid, wr::IID_IAsyncInfo)) {
      HRESULT hr = info_hr.load();
      if (FAILED(hr)) return hr;
      *out = static_cast<wr::IAsyncInfo*>(this);
    }
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetResults(wr::IInspectable** value) override {
    ++results_read;
    HRESULT hr = results_hr.load();
    *value = SUCCEEDED(hr) ? result : nullptr;
    if (*value != nullptr) (*value)->AddRef();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE get_Id(UINT32*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Status(int* value) override {
    *value = status;
    return fail_status_observation ? E_FAIL : S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* value) override { *value = E_FAIL; return S_OK; }
  HRESULT STDMETHODCALLTYPE Cancel() override {
    ++cancels;
    if (cancel_gate != nullptr && WaitForSingleObject(cancel_gate, 10000) != WAIT_OBJECT_0)
      return E_FAIL;
    cancel_returned = true;
    return cancel_hr;
  }
  HRESULT STDMETHODCALLTYPE Close() override {
    ++closes;
    return status == kStarted ? static_cast<HRESULT>(0x8000000DL) : S_OK;
  }
};

struct ScriptedDeleteAction final : ActionAbi, wr::IAsyncInfo {
  SCRIPTED_INSPECTABLE_METHODS
  std::atomic<HRESULT> info_hr{E_FAIL}, status_hr{E_FAIL};
  HRESULT results_hr = S_OK;
  std::atomic<int> status{kStarted}, info_queries{0}, status_reads{0}, results_read{0}, closes{0};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    *out = nullptr;
    if (IsEqualIID(iid, kUnknown)) *out = static_cast<ActionAbi*>(this);
    else if (IsEqualIID(iid, wr::IID_IAsyncInfo)) {
      ++info_queries;
      HRESULT hr = info_hr.load();
      if (FAILED(hr)) return hr;
      *out = static_cast<wr::IAsyncInfo*>(this);
    } else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetResults() override { ++results_read; return results_hr; }
  HRESULT STDMETHODCALLTYPE get_Id(UINT32*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Status(int* value) override {
    ++status_reads;
    *value = status;
    return status_hr;
  }
  HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* value) override { *value = E_FAIL; return S_OK; }
  HRESULT STDMETHODCALLTYPE Cancel() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Close() override {
    ++closes;
    return status == kStarted ? static_cast<HRESULT>(0x8000000DL) : S_OK;
  }
};

#undef SCRIPTED_INSPECTABLE_METHODS

struct Fixture {
  ScriptedBuffer buffer;
  ScriptedCredential credential;
  ScriptedResult result;
  ScriptedAsync async;
  std::atomic<int> starts{0}, successful_deletions{0};
  std::atomic<bool> reject_deletion{false};
  int deletion_success = hk::kSuccess;
  std::mutex deletion_lock;
  std::vector<std::wstring> deleted_names;
  Fixture() {
    credential.buffer = &buffer;
    result.credential = &credential;
    async.result = &result;
  }
  int DeleteCreated(const wchar_t* name, size_t length, HRESULT* hr) {
    std::lock_guard<std::mutex> guard(deletion_lock);
    deleted_names.emplace_back(name, length);
    if (reject_deletion) {
      *hr = E_FAIL;
      return hk::kFailed;
    }
    *hr = S_OK;
    ++successful_deletions;
    return deletion_success;
  }
  size_t Deletions() {
    std::lock_guard<std::mutex> guard(deletion_lock);
    return deleted_names.size();
  }
  bool ExactNames(const std::wstring& name) {
    std::lock_guard<std::mutex> guard(deletion_lock);
    for (const auto& actual : deleted_names) if (actual != name) return false;
    return true;
  }
  bool ReferencesReturned() {
    return async.refs == 1 && result.refs == 1 && credential.refs == 1 && buffer.refs == 1;
  }
  hk::Operation* Start(const wchar_t* name, size_t length) {
    return hk::internal::StartCreate(name, length,
        [this](IUnknown** value) -> HRESULT {
          ++starts;
          async.AddRef();
          *value = static_cast<wr::IAsyncOperation<wr::IInspectable*>*>(&async);
          return S_OK;
        }, [this](const wchar_t* key, size_t size, HRESULT* hr) {
          return DeleteCreated(key, size, hr);
        });
  }
  hk::Operation* StartWhenFree(const std::wstring& name) {
    hk::Operation* operation = nullptr;
    bool admitted = Until([&] {
      operation = Start(name.data(), name.size());
      if (starts != 0) return true;
      std::vector<uint8_t> bytes{0x55};
      int status = hk::Await(operation, &bytes, 50);
      if (status != hk::kFailed || !bytes.empty()) Fatal("busy admission fails without output");
      hk::Release(operation, false);
      return false;
    });
    if (!admitted) Fatal("previous create slot becomes available");
    return operation;
  }
  void Reclaimed(size_t expected_deletions, const std::wstring& name) {
    if (!Until([&] { return ReferencesReturned() && Deletions() >= expected_deletions; }))
      Fatal("terminal create releases every scripted COM reference");
    Check(ReferencesReturned(), "async, result, credential and buffer references return to their owner");
    Check(async.closes == 1, "terminal async request is closed exactly once");
    Check(Deletions() == expected_deletions && ExactNames(name),
          "cleanup deletes only the exact owned name, the expected number of times");
  }
};

const std::wstring kName = L"Gesher isolated create lifecycle \u05d0 \u6c34";

void StartupFailures() {
  for (HRESULT outcome : {E_FAIL, S_OK}) {
    std::atomic<int> starts{0}, deletions{0};
    hk::Operation* operation = nullptr;
    if (!Until([&] {
      operation = hk::internal::StartCreate(kName.data(), kName.size(),
          [&](IUnknown** async) -> HRESULT {
            ++starts;
            *async = nullptr;
            return outcome;
          }, [&](const wchar_t*, size_t, HRESULT* hr) {
            ++deletions;
            *hr = S_OK;
            return hk::kSuccess;
          });
      if (starts != 0) return true;
      std::vector<uint8_t> unused;
      (void)hk::Await(operation, &unused, 50);
      hk::Release(operation, false);
      return false;
    })) Fatal("startup failure fixture obtains the reservation");
    std::vector<uint8_t> bytes{0x55};
    Check(hk::Await(operation, &bytes, 50) == hk::kFailed && bytes.empty(),
          "a failed start or null async cannot report successful enrollment");
    hk::Release(operation, false);
    Check(starts == 1 && deletions == 0,
          "failed startup invokes Windows once and never deletes an unknown credential");
  }
  // The next normal fixture must acquire the slot after both startup failures.
}

void BusyMustNotStart(Fixture* contender) {
  hk::Operation* rejected = contender->Start(kName.data(), kName.size());
  std::vector<uint8_t> bytes{0x55};
  Check(hk::Await(rejected, &bytes, 50) == hk::kFailed && bytes.empty(),
        "an occupied create slot reports failure without bytes");
  Check(contender->starts == 0 && contender->ReferencesReturned(),
        "capacity is reserved before calling Windows or acquiring its async reference");
  hk::Release(rejected, false);
}

void AcceptedAndRejected() {
  for (bool accept : {true, false}) {
    Fixture fixture, contender;
    fixture.async.status = kCompleted;
    hk::Operation* operation = fixture.StartWhenFree(kName);
    std::vector<uint8_t> bytes{0x55};
    Check(hk::Await(operation, &bytes) == hk::kSuccess && bytes == fixture.buffer.bytes,
          "completed creation returns precisely the real result's public-key bytes");
    BusyMustNotStart(&contender);
    hk::Release(operation, accept);
    fixture.Reclaimed(accept ? 0 : 1, kName);
    Check(fixture.starts == 1 && fixture.async.results_read == 1,
          "an admitted creation starts and consumes its result once");
  }
}

void LateSuccessAndCapacity() {
  Fixture fixture, contender;
  std::wstring caller_name = kName + L" ignored suffix";
  hk::Operation* operation = nullptr;
  if (!Until([&] {
    operation = fixture.Start(caller_name.data(), kName.size());
    if (fixture.starts != 0) return true;
    std::vector<uint8_t> unused;
    (void)hk::Await(operation, &unused, 50);
    hk::Release(operation, false);
    return false;
  })) Fatal("late-success fixture is admitted");
  caller_name.assign(caller_name.size(), L'x');
  hk::Cancel(operation);
  hk::Cancel(operation);
  std::vector<uint8_t> bytes{0x55};
  ULONGLONG start = GetTickCount64();
  Check(hk::Await(operation, &bytes, 60) == kUserCanceled && bytes.empty(),
        "cancellation grace returns UserCanceled and no bytes before Windows completes");
  Check(GetTickCount64() - start < 2000, "the caller is not held by an unanswered prompt");
  hk::Release(operation, false);
  Check(fixture.async.closes == 0 && fixture.async.refs > 1 && fixture.Deletions() == 0,
        "unresolved creation remains owned, without premature Close or blind deletion");
  for (int i = 0; i < 12; ++i) BusyMustNotStart(&contender);
  fixture.async.status = kCompleted;
  fixture.Reclaimed(1, kName);
  Check(fixture.async.cancels == 1, "repeated caller cancellation reaches Windows at most once");
  Check(fixture.result.status_reads == 1,
        "late creation success is observed before deleting the created key");
}

void CancellationAfterSuccess() {
  Fixture fixture;
  fixture.async.status = kCompleted;
  hk::Operation* operation = fixture.StartWhenFree(kName);
  std::vector<uint8_t> bytes;
  Check(hk::Await(operation, &bytes) == hk::kSuccess,
        "success can precede a concurrent caller cancellation");
  hk::Cancel(operation);
  hk::Release(operation, true);
  fixture.Reclaimed(1, kName);
  Check(fixture.async.results_read == 1, "canceling a consumed success does not consume it twice");
}

void FailedExtraction() {
  for (int boundary = 0; boundary < 3; ++boundary) {
    Fixture fixture;
    if (boundary == 0) fixture.result.credential_hr = E_FAIL;
    if (boundary == 1) fixture.credential.public_hr = E_FAIL;
    if (boundary == 2) fixture.buffer.bytes_hr = E_FAIL;
    fixture.async.status = kCompleted;
    hk::Operation* operation = fixture.StartWhenFree(kName);
    std::vector<uint8_t> bytes{0x55};
    Check(hk::Await(operation, &bytes) == hk::kFailed && bytes.empty(),
          "failed public-key extraction cannot report a successful enrollment");
    hk::Release(operation, false);
    fixture.Reclaimed(1, kName);
  }
}

void UnownedResultsAreNeverDeleted() {
  for (int boundary = 0; boundary < 5; ++boundary) {
    Fixture fixture;
    fixture.async.status = kCompleted;
    if (boundary == 0) fixture.result.status = kAlreadyExists;
    if (boundary == 1) fixture.result.status = kUserCanceled;
    if (boundary == 2) fixture.result.status = 1;  // KeyCredentialStatus.UnknownError
    if (boundary == 3) fixture.async.status = kError;
    if (boundary == 4) fixture.async.status = kCanceled;
    hk::Operation* operation = fixture.StartWhenFree(kName);
    std::vector<uint8_t> bytes{0x55};
    int expected = boundary == 0 ? kAlreadyExists
        : (boundary == 1 || boundary == 4) ? kUserCanceled : boundary == 2 ? 1 : hk::kFailed;
    Check(hk::Await(operation, &bytes) == expected && bytes.empty(),
          "AlreadyExists, cancellation or unknown failure is returned honestly without bytes");
    hk::Release(operation, false);
    fixture.Reclaimed(0, kName);
    Check(fixture.result.credential_reads == 0 && fixture.credential.public_reads == 0,
          "an unowned or unknown result never exposes or deletes an existing credential");
  }
}

void FailedResultObservationRetainsOwnership() {
  for (int boundary = 0; boundary < 2; ++boundary) {
    for (int final_status : {hk::kSuccess, kAlreadyExists}) {
      Fixture fixture, contender;
      fixture.async.status = kCompleted;
      fixture.result.status = final_status;
      if (boundary == 0) fixture.async.info_hr = E_FAIL;
      if (boundary == 1) fixture.result.status_hr = E_FAIL;
      hk::Operation* operation = fixture.StartWhenFree(kName);
      std::vector<uint8_t> bytes{0x55};
      Check(hk::Await(operation, &bytes, 60) == hk::kFailed && bytes.empty(),
            "QI or result-status observation failure is reported honestly");
      hk::Release(operation, false);
      Check(fixture.async.refs > 1 && fixture.async.closes == 0 && fixture.Deletions() == 0,
            "unknown creation disposition retains the original operation and name");
      BusyMustNotStart(&contender);
      fixture.async.info_hr = S_OK;
      fixture.result.status_hr = S_OK;
      fixture.Reclaimed(final_status == hk::kSuccess ? 1 : 0, kName);
      Check(fixture.starts == 1 && fixture.async.results_read == 1,
            "recovering an observation retains the operation/result and consumes GetResults once");
      if (final_status == kAlreadyExists)
        Check(fixture.result.credential_reads == 0 && fixture.credential.public_reads == 0,
              "late AlreadyExists recovery never reads or deletes somebody else's key");
    }
  }
}

void FailedFirstGetResultsIsQuarantined() {
  // GetResults is consumable only once (Microsoft's WinRT contract). When its
  // first call fails, creation is unknown: this one owner must remain retained.
  // This last fixture therefore lives until process exit, just like the one
  // bounded production quarantine; returning cannot destroy its COM objects.
  auto* fixture = new Fixture;
  Fixture contender;
  fixture->async.status = kCompleted;
  fixture->async.results_hr = E_FAIL;
  hk::Operation* operation = fixture->StartWhenFree(kName);
  std::vector<uint8_t> bytes{0x55};
  Check(hk::Await(operation, &bytes, 60) == hk::kFailed && bytes.empty(),
        "failed first GetResults reports failure without public-key bytes");
  hk::Release(operation, false);
  fixture->async.results_hr = S_OK;
  Sleep(1200);  // longer than a retry interval, if an invalid retry were attempted
  Check(fixture->async.results_read == 1 && fixture->async.refs > 1 &&
            fixture->async.closes == 0 && fixture->Deletions() == 0,
        "unknown consumed result remains owned without a second GetResults or blind deletion");
  for (int i = 0; i < 12; ++i) BusyMustNotStart(&contender);
  Check(fixture->starts == 1 && contender.starts == 0,
        "permanent result uncertainty is one bounded unavailable slot, not more requests");
}

void DeleteActionIsRetainedUntilTerminal() {
  for (HRESULT result_hr : {S_OK, E_FAIL}) {
    ScriptedDeleteAction action;
    action.results_hr = result_hr;
    std::atomic<int> starts{0};
    std::atomic<bool> returned{false};
    int result = -99;
    HRESULT observed_hr = E_UNEXPECTED;
    std::wstring started_name;
    std::thread waiter([&] {
      (void)wr::EnsureApartment();
      result = hk::internal::DeleteCreated(kName.data(), kName.size(), &observed_hr,
          [&](const wchar_t* name, size_t length, IUnknown** async) -> HRESULT {
            started_name.assign(name, length);
            ++starts;
            action.AddRef();
            *async = static_cast<ActionAbi*>(&action);
            return S_OK;
          });
      returned = true;
    });
    if (!Until([&] { return action.info_queries > 0; })) Fatal("cleanup observes started action");
    Check(!returned && starts == 1 && action.refs > 1 && action.closes == 0,
          "DeleteAsync QI failure retains the same live action instead of starting another");
    action.info_hr = S_OK;
    if (!Until([&] { return action.status_reads > 0; })) Fatal("cleanup reaches action status");
    Check(!returned && starts == 1 && action.refs > 1 && action.closes == 0,
          "DeleteAsync status-query failure retains ownership without premature Close");
    action.status_hr = S_OK;
    Check(!returned && action.results_read == 0,
          "an unfinished deletion has no consumed result and cannot permit an outer retry");
    action.status = kCompleted;
    if (!Until([&] { return returned.load(); })) Fatal("terminal DeleteAsync ends cleanup wait");
    waiter.join();
    Check(result == (SUCCEEDED(result_hr) ? hk::kSuccess : hk::kFailed) && observed_hr == result_hr,
          "the sole terminal deletion result preserves its exact success or HRESULT failure");
    Check(starts == 1 && started_name == kName && action.results_read == 1 &&
              action.closes == 1 && action.refs == 1,
          "cleanup consumes the action once and returns only after terminal Close and release");
  }
}

void FailedDeletionRetainsCapacity() {
  for (int terminal : {hk::kSuccess, kNotFound}) {
    Fixture fixture, contender;
    fixture.async.status = kCompleted;
    fixture.reject_deletion = true;
    fixture.deletion_success = terminal;
    hk::Operation* operation = fixture.StartWhenFree(kName);
    std::vector<uint8_t> bytes;
    Check(hk::Await(operation, &bytes) == hk::kSuccess, "create completes before handoff refusal");
    hk::Release(operation, false);
    if (!Until([&] { return fixture.Deletions() >= 1; }))
      Fatal("rejected creation attempts cleanup promptly");
    BusyMustNotStart(&contender);
    fixture.reject_deletion = false;
    if (!Until([&] { return fixture.ReferencesReturned() && fixture.successful_deletions == 1; }))
      Fatal("successful cleanup reclaims the pending creation");
    fixture.Reclaimed(fixture.Deletions(), kName);
    Check(fixture.Deletions() >= 2 && fixture.successful_deletions == 1,
          "failed deletion is retained and retried until Success or NotFound");
  }
}

void FailedStatusObservationRetainsOwnership() {
  Fixture fixture, contender;
  fixture.async.fail_status_observation = true;
  hk::Operation* operation = fixture.StartWhenFree(kName);
  std::vector<uint8_t> bytes{0x55};
  Check(hk::Await(operation, &bytes, 60) == hk::kFailed && bytes.empty(),
        "a failed status observation returns failure without public-key bytes");
  hk::Release(operation, false);
  Check(fixture.async.refs > 1 && fixture.async.closes == 0 && fixture.Deletions() == 0,
        "unknown async status retains ownership without blind deletion or Close");
  BusyMustNotStart(&contender);
  fixture.async.status = kCompleted;
  fixture.async.fail_status_observation = false;
  fixture.Reclaimed(1, kName);
  Check(fixture.async.results_read == 1 && fixture.result.status_reads == 1,
        "a later observable success is consumed and deleted after status-query failure");
}

void BlockedWindowsCancelDoesNotHoldCaller() {
  Fixture fixture, contender;
  fixture.async.cancel_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (fixture.async.cancel_gate == nullptr) Fatal("create blocked-Cancel test event");
  hk::Operation* operation = fixture.StartWhenFree(kName);
  hk::Cancel(operation);
  if (!Until([&] { return fixture.async.cancels == 1; })) Fatal("worker reaches Windows Cancel");
  std::vector<uint8_t> bytes{0x55};
  ULONGLONG start = GetTickCount64();
  Check(hk::Await(operation, &bytes, 60) == kUserCanceled && bytes.empty(),
        "grace returns while Windows Cancel itself is blocked");
  hk::Release(operation, false);
  Check(GetTickCount64() - start < 2000 && !fixture.async.cancel_returned,
        "Await and Release do not share a lock held across Windows Cancel");
  BusyMustNotStart(&contender);
  fixture.async.status = kCompleted;
  SetEvent(fixture.async.cancel_gate);
  fixture.Reclaimed(1, kName);
  CloseHandle(fixture.async.cancel_gate);
  fixture.async.cancel_gate = nullptr;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Watchdog watchdog;
  StartupFailures();
  AcceptedAndRejected();
  LateSuccessAndCapacity();
  CancellationAfterSuccess();
  FailedExtraction();
  UnownedResultsAreNeverDeleted();
  FailedResultObservationRetainsOwnership();
  FailedDeletionRetainsCapacity();
  FailedStatusObservationRetainsOwnership();
  BlockedWindowsCancelDoesNotHoldCaller();
  // A final accepted creation proves all rejected/canceled slots were reclaimed.
  AcceptedAndRejected();
  DeleteActionIsRetainedUntilTerminal();
  FailedFirstGetResultsIsQuarantined();
  printf("%d create-lifecycle checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

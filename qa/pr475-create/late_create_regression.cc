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


}  // namespace

int main() {
  static_assert(kCanceled == 2 && kError == 3 && kNotFound == 2 && kAlreadyExists == 5);
  setvbuf(stdout, nullptr, _IONBF, 0);
  Watchdog watchdog;
  Fixture fixture;
  hk::Operation* operation = fixture.StartWhenFree(kName);
  hk::Cancel(operation);
  std::vector<uint8_t> bytes{0x55};
  Check(hk::Await(operation, &bytes, 60) == kUserCanceled && bytes.empty(),
        "canceled create returns no successful result before Windows completes");
  hk::Release(operation, false);
  Check(fixture.async.refs > 1 && fixture.async.closes == 0,
        "canceled create remains owned until its terminal result is observed");
  fixture.async.status = kCompleted;
  bool cleaned = Until([&] {
    return fixture.ReferencesReturned() && fixture.Deletions() == 1;
  });
  Check(cleaned, "late successful canceled create is deleted before its ownership is released");
  if (!cleaned) ExitProcess(1);  // never destroy objects a broken worker could still use
  fixture.Reclaimed(1, kName);
  printf("%d focused late-create checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

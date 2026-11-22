// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "native_fence_internal.h"
#include "binder_wire.h"
#include "event_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"

#include <bcrypt.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <winternl.h>
#include <io.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <new>

namespace gesher {
using Microsoft::WRL::ComPtr;
static_assert(kTransferredCompletedFence == binder_wire::kCompletedNativeFenceMetadata,
              "Binder and SCM_RIGHTS validate the completed-fence transport type");

class NativeFence {
 public:
  ~NativeFence() {
    if (event != nullptr) CloseHandle(event);
  }
  ComPtr<ID3D11Device> device;
  HANDLE event = nullptr;
  uint64_t metadata = 0;
  std::atomic<FenceStatus> terminal{FenceStatus::kPending};
};

namespace {
constexpr wchar_t kEventPrefix[] = L"Gesher.NativeFence.v1.";
constexpr DWORD kReadOnlyEvent = SYNCHRONIZE | 0x0001;  // EVENT_QUERY_STATE
constexpr uint64_t kForever = UINT64_MAX;

// UCRT's hard low-I/O ceiling (also used by record_lock_duplicate.cc). Fixed
// slots make dup/dup2 metadata publication infallible after CRT duplication;
// a map insertion there could fail and publish an ordinary NUL fd instead.
constexpr size_t kDescriptorCapacity = 8192;
struct Registry {
  std::mutex mutex;
  std::array<std::shared_ptr<NativeFence>, kDescriptorCapacity> slots{};
};
std::array<std::atomic<uint64_t>, kDescriptorCapacity / 64> members{};
Registry& Fences() {
  // No heap allocation, and no static destructor racing completion callbacks
  // during process exit. Handles are normally retired by close().
  alignas(Registry) static unsigned char storage[sizeof(Registry)];
  static Registry* registry = new (storage) Registry;
  return *registry;
}

std::shared_ptr<NativeFence> MakeFence() noexcept {
  try { return std::make_shared<NativeFence>(); }
  catch (const std::bad_alloc&) { errno = ENOMEM; return nullptr; }
}

bool MaybeFence(int fd) {
  if (fd < 0 || static_cast<size_t>(fd) >= kDescriptorCapacity) return false;
  size_t word = static_cast<size_t>(fd) / 64;
  return (members[word].load(std::memory_order_acquire) & (uint64_t{1} << (fd % 64))) != 0;
}
void Mark(int fd, bool present) {
  size_t word = static_cast<size_t>(fd) / 64;
  if (present) members[word].fetch_or(uint64_t{1} << (fd % 64), std::memory_order_release);
  else members[word].fetch_and(~(uint64_t{1} << (fd % 64)), std::memory_order_release);
}

// A wait registration owns a completion until it is signaled. Closing the EGL
// sync or its last fd must not close an event still registered with DXGI/D3D.
// One thread-pool wait per pending fence; no polling thread and no GL context.
struct CompletionWatch {
  std::mutex mutex;
  HANDLE wait = nullptr;
  std::shared_ptr<NativeFence> fence;
};

void CALLBACK Completed(void* argument, BOOLEAN) {
  auto* watch = static_cast<CompletionWatch*>(argument);
  HANDLE wait;
  {
    // RegisterWait can run us before it returns its handle to the creator.
    std::lock_guard<std::mutex> lock(watch->mutex);
    wait = watch->wait;
  }
  // Never wait for this callback itself. WT_EXECUTEONLYONCE prevents another.
  UnregisterWaitEx(wait, nullptr);
  // Exported native descriptors are already complete. Do not wake unrelated
  // socket/pipe pollers for every private EGL fence completion.
  delete watch;
}

void RetainUntilComplete(const std::shared_ptr<NativeFence>& fence) {
  if (WaitForSingleObject(fence->event, 0) == WAIT_OBJECT_0) return;
  auto* watch = new (std::nothrow) CompletionWatch;
  if (!watch) { WaitForSingleObject(fence->event, INFINITE); return; }
  watch->fence = fence;
  bool registered;
  {
    std::lock_guard<std::mutex> lock(watch->mutex);
    registered = RegisterWaitForSingleObject(&watch->wait, fence->event, Completed, watch,
                                             INFINITE, WT_EXECUTEONLYONCE) != FALSE;
  }
  if (!registered) {
    // Resource pressure must not strand a handle in a pending GPU operation.
    // Factories call this AFTER releasing ANGLE's mutex. The rare allocation
    // fallback sleeps here rather than leaking the event or signaling early.
    WaitForSingleObject(fence->event, INFINITE);
    delete watch;
  }
}

int Register(const std::shared_ptr<NativeFence>& fence) {
  record_locks::DescriptorGuard guard;
  int fd = ReserveFd();
  if (fd < 0) return -1;
  if (static_cast<size_t>(fd) >= kDescriptorCapacity) { _close(fd); return errno = EMFILE, -1; }
  std::lock_guard<std::mutex> lock(Fences().mutex);
  Fences().slots[fd] = fence;
  Mark(fd, true);
  return fd;
}

// Only our immutable completed-event descriptor type can use this fallback.
// An arbitrary pipe, file, mutex, unnamed event, or mutable event is rejected.
bool CompletedEvent(HANDLE handle) {
  struct BasicObject { ULONG attributes, access, handles, pointers; ULONG reserved[10]; };
  struct BasicEvent { ULONG type; LONG state; };
  using QueryObject = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  using QueryEvent = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  static auto query_object = reinterpret_cast<QueryObject>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  static auto query_event = reinterpret_cast<QueryEvent>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEvent"));
  if (!query_object || !query_event) return false;
  BasicObject object{};
  BasicEvent event{};
  alignas(UNICODE_STRING) std::array<unsigned char, 2048> buffer{};
  if (query_object(handle, 0, &object, sizeof(object), nullptr) < 0 || object.access != kReadOnlyEvent ||
      query_event(handle, 0, &event, sizeof(event), nullptr) < 0 || event.type != 0 || event.state == 0 ||
      query_object(handle, 1, buffer.data(), static_cast<ULONG>(buffer.size()), nullptr) < 0) return false;
  const auto* name = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
  if (name->Buffer == nullptr || name->Length % sizeof(wchar_t) != 0) return false;
  const size_t length = name->Length / sizeof(wchar_t);
  size_t start = length;
  while (start && name->Buffer[start - 1] != L'\\') --start;
  constexpr size_t prefix = std::size(kEventPrefix) - 1;
  return length - start == prefix + 32 && std::wmemcmp(name->Buffer + start, kEventPrefix, prefix) == 0;
}

}  // namespace

HANDLE CreateNativeFenceEvent() {
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return nullptr;
  wchar_t name[96] = L"Local\\";
  std::wcscat(name, kEventPrefix);
  size_t offset = std::wcslen(name);
  constexpr wchar_t hex[] = L"0123456789abcdef";
  for (unsigned char byte : random) { name[offset++] = hex[byte >> 4]; name[offset++] = hex[byte & 15]; }
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name);
  if (event && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(event); return nullptr; }
  return event;
}

std::shared_ptr<NativeFence> NativeFenceFromEvent(ID3D11Device* device, HANDLE owned_event) {
  auto result = MakeFence();
  if (!result) {
    if (owned_event) { WaitForSingleObject(owned_event, INFINITE); CloseHandle(owned_event); }
    return nullptr;
  }
  result->device = device;
  result->event = owned_event;
  if (!owned_event) return nullptr;
  RetainUntilComplete(result);
  return result;
}

FenceStatus NativeFenceStatus(const std::shared_ptr<NativeFence>& fence) {
  if (!fence) return FenceStatus::kFailed;
  FenceStatus status = fence->terminal.load(std::memory_order_acquire);
  if (status != FenceStatus::kPending) return status;
  DWORD result = WaitForSingleObject(fence->event, 0);
  status = result == WAIT_OBJECT_0 ? FenceStatus::kSignaled
         : result == WAIT_TIMEOUT ? FenceStatus::kPending : FenceStatus::kFailed;
  // DXGI also signals on removal; that is not successful GPU completion.
  if (fence->device && FAILED(fence->device->GetDeviceRemovedReason())) status = FenceStatus::kFailed;
  if (status != FenceStatus::kPending) {
    FenceStatus pending = FenceStatus::kPending;
    fence->terminal.compare_exchange_strong(pending, status, std::memory_order_acq_rel);
  }
  return fence->terminal.load(std::memory_order_acquire);
}

FenceStatus WaitNativeFence(const std::shared_ptr<NativeFence>& fence, uint64_t timeout_ns) {
  FenceStatus status = NativeFenceStatus(fence);
  if (status != FenceStatus::kPending || timeout_ns == 0) return status;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    uint64_t remaining = timeout_ns;
    if (timeout_ns != kForever) {
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
      if (static_cast<uint64_t>(elapsed) >= timeout_ns) return NativeFenceStatus(fence);
      remaining -= static_cast<uint64_t>(elapsed);
    }
    DWORD millis = remaining == kForever ? INFINITE
        : static_cast<DWORD>(std::min<uint64_t>(remaining / 1'000'000 + (remaining % 1'000'000 != 0), INFINITE - 1));
    DWORD result = WaitForSingleObject(fence->event, millis);
    if (result == WAIT_FAILED) return FenceStatus::kFailed;
    status = NativeFenceStatus(fence);
    if (status != FenceStatus::kPending) return status;
  }
}

HRESULT ServerWaitNativeFence(const std::shared_ptr<NativeFence>& fence, ID3D11Device* device) {
  if (!fence) return E_INVALIDARG;
  FenceStatus status = NativeFenceStatus(fence);
  if (status == FenceStatus::kFailed) return DXGI_ERROR_DEVICE_REMOVED;
  if (status == FenceStatus::kSignaled) return S_OK;
  // All local fences were inserted into this one ANGLE immediate queue.
  // Its later commands already follow them, including from other EGL contexts.
  // Never enqueue an external pending dependency on this shared queue: doing
  // so would block contexts other than the one that requested eglWaitSync.
  // Export/import expose only immutable COMPLETED native events, so a pending
  // foreign-device fence cannot arise through an application EGL/native fd.
  return fence->device.Get() == device ? S_OK : E_INVALIDARG;
}

int ExportNativeFence(const std::shared_ptr<NativeFence>& fence) {
  if (!fence) return errno = EINVAL, -1;
  // Native export remains synchronous until ANGLE has independent per-context
  // queues. This sleep is outside ANGLE's mutex and happens only when an app
  // actually requests an fd; creating/retiring an EGL fence stays asynchronous.
  if (WaitNativeFence(fence, kForever) != FenceStatus::kSignaled) return errno = EIO, -1;
  auto completed = MakeFence();
  if (!completed) return -1;
  if (!DuplicateHandle(GetCurrentProcess(), fence->event, GetCurrentProcess(), &completed->event,
                       kReadOnlyEvent, FALSE, 0)) return FailWithLastError();
  completed->metadata = kTransferredCompletedFence;
  completed->terminal.store(FenceStatus::kSignaled, std::memory_order_relaxed);
  return Register(completed);
}

std::shared_ptr<NativeFence> LookupNativeFence(int fd) {
  if (!MaybeFence(fd)) return nullptr;
  std::lock_guard<std::mutex> lock(Fences().mutex);
  return Fences().slots[fd];
}

void ForgetNativeFence(int fd) {
  if (!MaybeFence(fd)) return;
  std::shared_ptr<NativeFence> retired;
  {
    std::lock_guard<std::mutex> lock(Fences().mutex);
    retired = std::move(Fences().slots[fd]);
    Mark(fd, false);
  }
  WakePollers();  // a blocked poll revalidates this exact descriptor identity
}

void DupNativeFence(int from, int to) {
  auto fence = LookupNativeFence(from);
  if (!fence) return;
  std::lock_guard<std::mutex> lock(Fences().mutex);
  Fences().slots[to] = std::move(fence);  // to is a successful UCRT duplicate, hence below its hard limit
  Mark(to, true);
}

bool TransferNativeFence(int fd, uintptr_t* handle, uint64_t* metadata,
                         std::shared_ptr<NativeFence>* retained) {
  auto fence = LookupNativeFence(fd);
  if (!fence) return false;
  *handle = reinterpret_cast<uintptr_t>(fence->event);
  *metadata = fence->metadata;
  if (retained) *retained = std::move(fence);
  return true;
}

int AdoptNativeFence(HANDLE owned, uint64_t metadata) {
  auto result = MakeFence();
  if (!result) { CloseHandle(owned); return -1; }
  if (metadata == kTransferredCompletedFence) {
    result->event = owned;
    if (!CompletedEvent(owned)) return errno = EINVAL, -1;
    result->terminal.store(FenceStatus::kSignaled, std::memory_order_relaxed);
  } else {
    CloseHandle(owned);
    return errno = EINVAL, -1;
  }
  result->metadata = metadata;
  return Register(result);
}

short PollNativeFence(const std::shared_ptr<NativeFence>& fence, short events, HANDLE* wake) {
  if (wake) *wake = nullptr;
  switch (NativeFenceStatus(fence)) {
    case FenceStatus::kSignaled: return events & (POLLIN | POLLRDNORM);
    case FenceStatus::kFailed: return POLLERR;
    case FenceStatus::kPending:
      if (wake && (events & (POLLIN | POLLRDNORM))) *wake = fence->event;
      return 0;
  }
  return POLLERR;
}

}  // namespace gesher

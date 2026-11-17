// A runtime image's process-lifetime power callback, with quiesced consumers.
// PowerUnregisterSuspendResumeNotification does not document a callback join;
// no destructible consumer is exposed as its raw context. The dispatcher stays
// alive like the runtime's process-lifetime signal state.
#include <gesher/resume.h>

#include <windows.h>
#include <powrprof.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <vector>

namespace gesher {

struct ResumeObserver {
  std::mutex mutex;
  void (*callback)(void*) = nullptr;
  void* context = nullptr;
};

namespace {

// LLVM-MinGW's pinned powrprof.h declares the functions but omits this SDK
// payload and flag. Match Microsoft's powrprof.h callback ABI locally.
#ifndef DEVICE_NOTIFY_CALLBACK
constexpr DWORD kDeviceNotifyCallback = 2;
struct ResumeNotifyParameters {
  ULONG (CALLBACK *Callback)(PVOID, ULONG, PVOID);
  PVOID Context;
};
#else
constexpr DWORD kDeviceNotifyCallback = DEVICE_NOTIFY_CALLBACK;
using ResumeNotifyParameters = DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
#endif

struct ResumeDispatcher {
  std::mutex registration;
  std::mutex mutex;
  HPOWERNOTIFY handle = nullptr;
  ResumeNotifyParameters parameters{};
  std::vector<std::shared_ptr<ResumeObserver>> observers;

  static ULONG CALLBACK Notify(void* context, ULONG type, void*) {
    if (type != PBT_APMRESUMEAUTOMATIC && type != PBT_APMRESUMESUSPEND) {
      return ERROR_SUCCESS;
    }
    auto& dispatcher = *static_cast<ResumeDispatcher*>(context);
    std::vector<std::shared_ptr<ResumeObserver>> snapshot;
    {
      std::lock_guard<std::mutex> lock(dispatcher.mutex);
      snapshot = dispatcher.observers;
    }
    // Never acquire a consumer lock under the registry lock. Reset may remove
    // an observer from the registry while an old snapshot still references it.
    for (const auto& observer : snapshot) {
      std::lock_guard<std::mutex> lock(observer->mutex);
      if (observer->callback != nullptr) observer->callback(observer->context);
    }
    return ERROR_SUCCESS;
  }

  int EnsureRegistered() {
    std::lock_guard<std::mutex> lock(registration);
    if (handle != nullptr) return 0;
    // gesher_posix is static and can also be linked into a host DLL. Pin the
    // module containing Notify before giving Windows a process-lifetime code
    // pointer; consumer subscriptions themselves remain removable.
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&Notify), &module)) return EIO;
    parameters = {Notify, this};
    // This call is outside the observer registry lock: a notification may run
    // concurrently with registration. A new consumer checks its clock after
    // subscribing, so it cannot lose a resume that preceded its registration.
    const DWORD error = PowerRegisterSuspendResumeNotification(
        kDeviceNotifyCallback, &parameters, &handle);
    if (error == ERROR_SUCCESS) return 0;
    handle = nullptr;
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ? ENOMEM : EIO;
  }
};

ResumeDispatcher& Dispatcher() {
  static auto* dispatcher = new ResumeDispatcher;
  return *dispatcher;
}

}  // namespace

ResumeSubscription::~ResumeSubscription() { Reset(); }

int ResumeSubscription::Start(void (*callback)(void*), void* context) {
  Reset();
  if (callback == nullptr) return EINVAL;
  auto& dispatcher = Dispatcher();
  const int error = dispatcher.EnsureRegistered();
  if (error != 0) return error;
  auto observer = std::make_shared<ResumeObserver>();
  observer->callback = callback;
  observer->context = context;
  {
    std::lock_guard<std::mutex> lock(dispatcher.mutex);
    dispatcher.observers.push_back(observer);
  }
  observer_ = std::move(observer);
  return 0;
}

void ResumeSubscription::Reset() {
  auto observer = std::move(observer_);
  if (observer == nullptr) return;
  {
    // Acquiring this lock joins an admitted consumer callback. No registry or
    // consumer lock may be held here; a callback can need its consumer mutex.
    std::lock_guard<std::mutex> lock(observer->mutex);
    observer->callback = nullptr;
    observer->context = nullptr;
  }
  auto& dispatcher = Dispatcher();
  std::lock_guard<std::mutex> lock(dispatcher.mutex);
  std::erase(dispatcher.observers, observer);
}

}  // namespace gesher

// Only the Windows power API boundary is controlled. The dispatcher and its
// per-consumer callback quiescence are compiled from production resume.cc.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

using ULONG = uint32_t;
using HPOWERNOTIFY = void*;
using HMODULE = void*;
using LPCWSTR = const wchar_t*;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_OUTOFMEMORY = 14;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4, GET_MODULE_HANDLE_EX_FLAG_PIN = 1;
constexpr ULONG PBT_APMSUSPEND = 4, PBT_APMRESUMEAUTOMATIC = 18, PBT_APMRESUMESUSPEND = 7;
#define DEVICE_NOTIFY_CALLBACK 2
struct DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
  ULONG (*Callback)(void*, ULONG, void*);
  void* Context;
};

namespace power_transport {
inline std::mutex mutex;
inline DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS recipient{};
inline std::atomic<DWORD> error{0};
inline std::atomic<bool> pin_failure{false};
inline std::atomic<unsigned> registrations{0};
inline void (*on_registration)() = nullptr;
inline bool Notify(ULONG type = PBT_APMRESUMEAUTOMATIC) {
  DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS callback;
  { std::lock_guard<std::mutex> lock(mutex); callback = recipient; }
  if (callback.Callback == nullptr) return false;
  return callback.Callback(callback.Context, type, nullptr) == ERROR_SUCCESS;
}
}

static bool GetModuleHandleExW(DWORD flags, LPCWSTR address, HMODULE* module) {
  if (power_transport::pin_failure || flags != 5 || address == nullptr) return false;
  *module = reinterpret_cast<HMODULE>(1);
  return true;
}
static DWORD PowerRegisterSuspendResumeNotification(DWORD flags, void* parameters,
                                                    HPOWERNOTIFY* registration) {
  ++power_transport::registrations;
  if (power_transport::error != 0) return power_transport::error;
  if (flags != DEVICE_NOTIFY_CALLBACK) return 87;
  {
    std::lock_guard<std::mutex> lock(power_transport::mutex);
    power_transport::recipient = *static_cast<DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS*>(parameters);
  }
  if (power_transport::on_registration != nullptr) power_transport::on_registration();
  *registration = reinterpret_cast<HPOWERNOTIFY>(1);
  return ERROR_SUCCESS;
}

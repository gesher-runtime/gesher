// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// libandroid's sensor inventory and normal event queues share the Java sensor hub.

#include <android/sensor.h>
#include <gesher/bionic_shim.h>
#include <sys/eventfd.h>
#include <utils/Looper.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "../arm64/include/gesher/arm64_runtime.h"
#include "../bionic/abi_types.h"
#include "../elf/include/gesher/elf_loader.h"
#include "sensors.h"

namespace gesher::bionic {
namespace {

namespace hub = gesher::devices::sensors;
using android::Looper;
using android::sp;

struct Manager {};
Manager g_manager;

const hub::Sensor* FindSensor(int handle) {
  for (const auto& sensor : hub::List()) {
    if (sensor.handle == handle) return &sensor;
  }
  return nullptr;
}

const char* StringType(int type) {
  switch (type) {
    case hub::kAccelerometer: return "android.sensor.accelerometer";
    case hub::kMagneticField: return "android.sensor.magnetic_field";
    case hub::kGyroscope: return "android.sensor.gyroscope";
    case hub::kLight: return "android.sensor.light";
    case hub::kRotationVector: return "android.sensor.rotation_vector";
    default: return nullptr;
  }
}

Manager* BIONIC_SYSV GetManager() { return &g_manager; }
Manager* BIONIC_SYSV GetManagerForPackage(const char*) { return &g_manager; }

int BIONIC_SYSV GetSensorList(Manager* manager, const hub::Sensor* const** list) {
  if (manager != &g_manager || list == nullptr) return -EINVAL;
  static const std::vector<const hub::Sensor*> sensors = [] {
    std::vector<const hub::Sensor*> result;
    result.reserve(hub::List().size());
    for (const auto& sensor : hub::List()) result.push_back(&sensor);
    return result;
  }();
  *list = sensors.data();
  return static_cast<int>(sensors.size());
}

ssize_t BIONIC_SYSV GetDynamicSensorList(Manager* manager, const hub::Sensor* const** list) {
  if (manager != &g_manager || list == nullptr) return -EINVAL;
  *list = nullptr;
  return 0;
}

const hub::Sensor* BIONIC_SYSV GetDefaultSensor(Manager* manager, int type) {
  if (manager != &g_manager) return nullptr;
  for (const auto& sensor : hub::List()) {
    if (sensor.type == type) return &sensor;
  }
  return nullptr;
}

const hub::Sensor* BIONIC_SYSV GetDefaultSensorEx(Manager* manager, int type, bool wake_up) {
  // The polled Windows sources cannot wake the PC to deliver a sample.
  return wake_up ? nullptr : GetDefaultSensor(manager, type);
}

bool IsRateLimitedMotion(const hub::Sensor& sensor) {
  return sensor.type == hub::kAccelerometer || sensor.type == hub::kMagneticField ||
      sensor.type == hub::kGyroscope;
}

int SamplingPeriod(const hub::Sensor& sensor, int requested_us) {
  // This bridge lacks a permission-verified elevated-rate path.
  constexpr int kUnprivilegedMinimumUs = 5000;
  return IsRateLimitedMotion(sensor) ? std::max(requested_us, kUnprivilegedMinimumUs) : requested_us;
}

using GuestCallback = int (BIONIC_SYSV*)(int, int, void*);

class Callback : public android::LooperCallback {
 public:
  explicit Callback(GuestCallback callback)
      : callback_(callback), owner_(elf::RetainForAddress(reinterpret_cast<void*>(callback))) {
    void* address = reinterpret_cast<void*>(callback);
    if (arm64::IsGuestCode(address)) {
      callback_ = reinterpret_cast<GuestCallback>(arm64::HostEntry(address));
    }
  }

  ~Callback() override { elf::ReleaseLifetimePin(owner_); }
  bool valid() const { return callback_ != nullptr; }

  int handleEvent(int fd, int events, void* data) override {
    elf::SetupThreadTls();
    return callback_(fd, events, data);
  }

 private:
  GuestCallback callback_;
  elf::Library* owner_;
};

class Queue : public hub::Sink {
 public:
  explicit Queue(Looper* looper) : looper_(looper), fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

  bool Start(int ident, GuestCallback guest, void* data) {
    if (fd_ < 0) return false;
    sp<Callback> callback;
    if (guest != nullptr) {
      callback = sp<Callback>::make(guest);
      if (!callback->valid()) return false;
    }
    watching_ = looper_->addFd(fd_, ident, Looper::EVENT_INPUT, callback, data) == 1;
    return watching_;
  }

  ~Queue() override {
    if (watching_) {
      hub::DisableAll(this);
      looper_->removeFd(fd_);
    }
    if (fd_ >= 0) close(fd_);
  }

  void OnEvent(int handle, const hub::Reading& reading, int64_t timestamp_ns) override {
    const auto* sensor = FindSensor(handle);
    if (sensor == nullptr) return;
    ASensorEvent event{};
    event.version = sizeof(event);
    event.sensor = handle;
    event.type = sensor->type;
    event.timestamp = timestamp_ns;
    std::memcpy(event.data, reading.values, sizeof(event.data));
    if (IsRateLimitedMotion(*sensor)) {
      event.vector.status = static_cast<int8_t>(reading.accuracy);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool wake = pending_.empty();
    if (pending_.size() == kCapacity) pending_.pop_front();
    pending_.push_back(event);
    if (wake) {
      const uint64_t one = 1;
      while (write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {}
    }
  }

  int OnFlush(const std::vector<int>&) override { return -ENOTSUP; }

  bool HasEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_.empty();
  }

  ssize_t Read(ASensorEvent* events, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    count = std::min(count, pending_.size());
    for (size_t i = 0; i < count; ++i) {
      events[i] = pending_.front();
      pending_.pop_front();
    }
    if (count != 0 && pending_.empty()) {
      uint64_t value;
      while (read(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {}
    }
    return static_cast<ssize_t>(count);
  }

 private:
  static constexpr size_t kCapacity = 256;
  sp<Looper> looper_;
  int fd_;
  bool watching_ = false;
  std::mutex mutex_;
  std::deque<ASensorEvent> pending_;
};

Queue* BIONIC_SYSV CreateQueue(Manager* manager, Looper* looper, int ident, GuestCallback callback,
                              void* data) {
  if (manager != &g_manager || looper == nullptr) return nullptr;
  auto queue = std::unique_ptr<Queue>(new (std::nothrow) Queue(looper));
  return queue && queue->Start(ident, callback, data) ? queue.release() : nullptr;
}

int BIONIC_SYSV DestroyQueue(Manager* manager, Queue* queue) {
  if (manager != &g_manager || queue == nullptr) return -EINVAL;
  delete queue;
  return 0;
}

int BIONIC_SYSV RegisterSensor(Queue* queue, const hub::Sensor* sensor, int32_t period_us,
                             int64_t latency_us) {
  if (queue == nullptr || sensor == nullptr || period_us < 0 || latency_us < 0) return -EINVAL;
  // No batching FIFO. Samples arrive immediately, which meets the maximum latency.
  return hub::Enable(queue, sensor->handle, SamplingPeriod(*sensor, period_us));
}

int BIONIC_SYSV EnableSensor(Queue* queue, const hub::Sensor* sensor) {
  return RegisterSensor(queue, sensor, 200000, 0);
}

int BIONIC_SYSV DisableSensor(Queue* queue, const hub::Sensor* sensor) {
  return queue && sensor ? hub::Disable(queue, sensor->handle) : -EINVAL;
}

int BIONIC_SYSV SetEventRate(Queue* queue, const hub::Sensor* sensor, int32_t period_us) {
  if (queue == nullptr || sensor == nullptr || period_us < sensor->minDelayUs) return -EINVAL;
  return hub::SetRate(queue, sensor->handle, SamplingPeriod(*sensor, period_us));
}

int BIONIC_SYSV HasEvents(Queue* queue) { return queue ? queue->HasEvents() : -EINVAL; }

ssize_t BIONIC_SYSV ReadEvents(Queue* queue, ASensorEvent* events, size_t count) {
  if (queue == nullptr || (events == nullptr && count != 0)) return -EINVAL;
  return queue->Read(events, count);
}

int BIONIC_SYSV AdditionalInfo(Queue* queue, bool enable) {
  if (queue == nullptr) return -EINVAL;
  return enable ? -ENOTSUP : 0;
}

int BIONIC_SYSV CreateSharedChannel(Manager*, int, size_t) { return -EINVAL; }
int BIONIC_SYSV CreateHardwareChannel(Manager*, void*, size_t) { return -EINVAL; }
void BIONIC_SYSV DestroyChannel(Manager*, int) {}
int BIONIC_SYSV ConfigureChannel(Manager*, const hub::Sensor*, int, int) { return -EINVAL; }

const char* BIONIC_SYSV GetName(const hub::Sensor* sensor) {
  return sensor ? sensor->name.c_str() : nullptr;
}
const char* BIONIC_SYSV GetVendor(const hub::Sensor* sensor) {
  return sensor ? sensor->vendor.c_str() : nullptr;
}
const char* BIONIC_SYSV GetStringType(const hub::Sensor* sensor) {
  return sensor ? StringType(sensor->type) : nullptr;
}
int BIONIC_SYSV GetType(const hub::Sensor* sensor) {
  return sensor ? sensor->type : ASENSOR_TYPE_INVALID;
}
int BIONIC_SYSV GetMinDelay(const hub::Sensor* sensor) {
  return sensor ? sensor->minDelayUs : ASENSOR_DELAY_INVALID;
}
int BIONIC_SYSV GetFifoCount(const hub::Sensor* sensor) {
  return sensor ? 0 : ASENSOR_FIFO_COUNT_INVALID;
}
int BIONIC_SYSV GetReportingMode(const hub::Sensor* sensor) {
  return sensor ? sensor->flags >> 1 : AREPORTING_MODE_INVALID;
}
int BIONIC_SYSV GetDirectRate(const hub::Sensor*) { return ASENSOR_DIRECT_RATE_STOP; }
int BIONIC_SYSV GetHandle(const hub::Sensor* sensor) { return sensor ? sensor->handle : -1; }
float BIONIC_SYSV GetResolution(const hub::Sensor* sensor) {
  return sensor ? sensor->resolution : ASENSOR_RESOLUTION_INVALID;
}
bool BIONIC_SYSV IsWakeUp(const hub::Sensor*) { return false; }
bool BIONIC_SYSV IsDirectChannelSupported(const hub::Sensor*, int) { return false; }

static_assert(sizeof(ASensorEvent) == 104);

}  // namespace

void RegisterNdkSensors() {
#define SENSOR_API(name, fn) AddBionicSymbol(name, reinterpret_cast<void*>(&fn))
  SENSOR_API("ASensorManager_getInstance", GetManager);
  SENSOR_API("ASensorManager_getInstanceForPackage", GetManagerForPackage);
  SENSOR_API("ASensorManager_getSensorList", GetSensorList);
  SENSOR_API("ASensorManager_getDynamicSensorList", GetDynamicSensorList);
  SENSOR_API("ASensorManager_getDefaultSensor", GetDefaultSensor);
  SENSOR_API("ASensorManager_getDefaultSensorEx", GetDefaultSensorEx);
  SENSOR_API("ASensorManager_createEventQueue", CreateQueue);
  SENSOR_API("ASensorManager_destroyEventQueue", DestroyQueue);
  SENSOR_API("ASensorManager_createSharedMemoryDirectChannel", CreateSharedChannel);
  SENSOR_API("ASensorManager_createHardwareBufferDirectChannel", CreateHardwareChannel);
  SENSOR_API("ASensorManager_destroyDirectChannel", DestroyChannel);
  SENSOR_API("ASensorManager_configureDirectReport", ConfigureChannel);
  SENSOR_API("ASensorEventQueue_registerSensor", RegisterSensor);
  SENSOR_API("ASensorEventQueue_enableSensor", EnableSensor);
  SENSOR_API("ASensorEventQueue_disableSensor", DisableSensor);
  SENSOR_API("ASensorEventQueue_setEventRate", SetEventRate);
  SENSOR_API("ASensorEventQueue_hasEvents", HasEvents);
  SENSOR_API("ASensorEventQueue_getEvents", ReadEvents);
  SENSOR_API("ASensorEventQueue_requestAdditionalInfoEvents", AdditionalInfo);
  SENSOR_API("ASensor_getName", GetName);
  SENSOR_API("ASensor_getVendor", GetVendor);
  SENSOR_API("ASensor_getStringType", GetStringType);
  SENSOR_API("ASensor_getType", GetType);
  SENSOR_API("ASensor_getMinDelay", GetMinDelay);
  SENSOR_API("ASensor_getFifoMaxEventCount", GetFifoCount);
  SENSOR_API("ASensor_getFifoReservedEventCount", GetFifoCount);
  SENSOR_API("ASensor_getReportingMode", GetReportingMode);
  SENSOR_API("ASensor_getHighestDirectReportRateLevel", GetDirectRate);
  SENSOR_API("ASensor_getHandle", GetHandle);
  SENSOR_API("ASensor_getResolution", GetResolution);
  SENSOR_API("ASensor_isWakeUpSensor", IsWakeUp);
  SENSOR_API("ASensor_isDirectChannelTypeSupported", IsDirectChannelSupported);
#undef SENSOR_API
}

}  // namespace gesher::bionic

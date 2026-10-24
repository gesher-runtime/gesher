// Gesher (devices track): the sensors of sensors.h, Windows.Devices.Sensors
// through its ABI and the GESHER_SENSOR_TEST sensors, sampled by one thread.

#include "sensors.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "winrt.h"

namespace gesher::devices::sensors {

using winrt::IInspectable;

namespace {

// --- Windows.Devices.Sensors ----------------------------------------------------------
// Each class: a statics interface with GetDefault (null when the PC has no
// such sensor), the sensor's GetCurrentReading, and the reading's values.

struct DateTime {
  int64_t UniversalTime;
};

struct IStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetDefault(IUnknown** sensor) = 0;
};

// IAccelerometer, IGyrometer, IMagnetometer, ILightSensor, IOrientationSensor
// all begin with GetCurrentReading, then MinimumReportInterval.
struct ISensorBase : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(IUnknown** reading) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MinimumReportInterval(UINT32* ms) = 0;
};

struct IAccelerometerReading : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AccelerationX(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AccelerationY(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AccelerationZ(double* value) = 0;
};

struct IGyrometerReading : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AngularVelocityX(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AngularVelocityY(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AngularVelocityZ(double* value) = 0;
};

struct IMagnetometerReading : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MagneticFieldX(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MagneticFieldY(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MagneticFieldZ(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DirectionalAccuracy(int* value) = 0;
};

struct ILightSensorReading : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IlluminanceInLux(float* value) = 0;
};

struct ISensorQuaternion : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_W(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_X(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Y(float* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Z(float* value) = 0;
};

struct IOrientationSensorReading : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RotationMatrix(IUnknown** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Quaternion(ISensorQuaternion** value) = 0;
};

constexpr float kGravity = 9.80665f;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;

// One Windows sensor: its object and how a reading becomes Android values.
class WindowsSource : public Source {
 public:
  WindowsSource(ISensorBase* sensor, int type) : sensor_(sensor), type_(type) {}
  ~WindowsSource() override { winrt::Release(sensor_); }

  bool Read(Reading* r) override {
    if (FAILED(winrt::EnsureApartment())) return false;
    IUnknown* reading = nullptr;
    if (FAILED(sensor_->GetCurrentReading(&reading)) || reading == nullptr) return false;
    bool ok = false;
    switch (type_) {
      case kAccelerometer: {
        // Windows: g, -1 on Z lying face up; Android: m/s^2, +g on Z.
        auto* a = static_cast<IAccelerometerReading*>(reading);
        double x = 0, y = 0, z = 0;
        ok = SUCCEEDED(a->get_AccelerationX(&x)) && SUCCEEDED(a->get_AccelerationY(&y)) &&
             SUCCEEDED(a->get_AccelerationZ(&z));
        r->values[0] = static_cast<float>(-x) * kGravity;
        r->values[1] = static_cast<float>(-y) * kGravity;
        r->values[2] = static_cast<float>(-z) * kGravity;
        break;
      }
      case kGyroscope: {
        auto* g = static_cast<IGyrometerReading*>(reading);
        double x = 0, y = 0, z = 0;
        ok = SUCCEEDED(g->get_AngularVelocityX(&x)) && SUCCEEDED(g->get_AngularVelocityY(&y)) &&
             SUCCEEDED(g->get_AngularVelocityZ(&z));
        r->values[0] = static_cast<float>(x) * kDegreesToRadians;
        r->values[1] = static_cast<float>(y) * kDegreesToRadians;
        r->values[2] = static_cast<float>(z) * kDegreesToRadians;
        break;
      }
      case kMagneticField: {
        auto* m = static_cast<IMagnetometerReading*>(reading);
        int accuracy = 0;
        ok = SUCCEEDED(m->get_MagneticFieldX(&r->values[0])) &&
             SUCCEEDED(m->get_MagneticFieldY(&r->values[1])) &&
             SUCCEEDED(m->get_MagneticFieldZ(&r->values[2]));
        // MagnetometerAccuracy: Unknown 0, Unreliable 1, Approximate 2, High 3.
        if (SUCCEEDED(m->get_DirectionalAccuracy(&accuracy))) {
          r->accuracy = accuracy == 3 ? 3 : accuracy == 2 ? 2 : accuracy == 1 ? 0 : 1;
        }
        break;
      }
      case kLight: {
        auto* l = static_cast<ILightSensorReading*>(reading);
        ok = SUCCEEDED(l->get_IlluminanceInLux(&r->values[0]));
        break;
      }
      case kRotationVector: {
        auto* o = static_cast<IOrientationSensorReading*>(reading);
        ISensorQuaternion* q = nullptr;
        if (SUCCEEDED(o->get_Quaternion(&q)) && q != nullptr) {
          ok = SUCCEEDED(q->get_X(&r->values[0])) && SUCCEEDED(q->get_Y(&r->values[1])) &&
               SUCCEEDED(q->get_Z(&r->values[2])) && SUCCEEDED(q->get_W(&r->values[3]));
          r->values[4] = -1;  // no heading accuracy
          q->Release();
        }
        break;
      }
      default:
        break;
    }
    reading->Release();
    return ok;
  }

  UINT32 MinimumIntervalMs() {
    UINT32 ms = 0;
    return SUCCEEDED(sensor_->get_MinimumReportInterval(&ms)) ? ms : 0;
  }

 private:
  ISensorBase* sensor_;
  int type_;
};

struct WindowsClass {
  const wchar_t* className;
  IID statics;
  int type;
  const char* name;
  const char* label;
  float maxRange, resolution;
  int flags;
};

const WindowsClass kWindowsClasses[] = {
    {L"Windows.Devices.Sensors.Accelerometer",
     {0xA5E28B74, 0x5A87, 0x4A2D, {0xBE, 0xCC, 0x0F, 0x90, 0x6E, 0xA0, 0x61, 0xDD}},
     kAccelerometer, "Windows Accelerometer", "accelerometer", 8 * kGravity, 0.001f * kGravity,
     kContinuous},
    {L"Windows.Devices.Sensors.Gyrometer",
     {0x83B6E7C9, 0xE49D, 0x4B39, {0x86, 0xE6, 0xCD, 0x55, 0x4B, 0xE4, 0xC5, 0xC1}},
     kGyroscope, "Windows Gyrometer", "gyrometer", 2000 * kDegreesToRadians,
     0.01f * kDegreesToRadians, kContinuous},
    {L"Windows.Devices.Sensors.Magnetometer",
     {0x853C64CC, 0x0698, 0x4DDA, {0xA6, 0xDF, 0x9C, 0xB9, 0xCC, 0x4A, 0xB4, 0x0A}},
     kMagneticField, "Windows Magnetometer", "magnetometer", 2000, 0.1f, kContinuous},
    {L"Windows.Devices.Sensors.LightSensor",
     {0x45DB8C84, 0xC3A8, 0x471E, {0x9A, 0x53, 0x64, 0x57, 0xFA, 0xD8, 0x7C, 0x0E}},
     kLight, "Windows Light Sensor", "light", 100000, 1, kOnChange},
    {L"Windows.Devices.Sensors.OrientationSensor",
     {0x10EF8712, 0xFB4C, 0x428A, {0x89, 0x8B, 0x27, 0x65, 0xE4, 0x09, 0xE6, 0x69}},
     kRotationVector, "Windows Orientation Sensor", "orientation", 1, 0.0001f, kContinuous},
};

// --- GESHER_SENSOR_TEST --------------------------------------------------------------

class TestSource : public Source {
 public:
  explicit TestSource(int type) : type_(type) {}
  bool Read(Reading* r) override {
    if (type_ == kAccelerometer) {
      // Lying face up, still: gravity on Z.
      r->values[0] = 0;
      r->values[1] = 0;
      r->values[2] = kGravity;
    } else {
      r->values[0] = 250;  // lux: an office
    }
    return true;
  }

 private:
  int type_;
};

std::string& DescribeText() {
  static std::string text;
  return text;
}

std::vector<Sensor> Find() {
  std::vector<Sensor> list;
  std::string& describe = DescribeText();
  int handle = 1;
  for (const WindowsClass& c : kWindowsClasses) {
    if (!describe.empty()) describe += ", ";
    describe += c.label;
    describe += ": ";
    IStatics* statics = nullptr;
    HRESULT hr = winrt::GetActivationFactory(c.className, c.statics,
                                             reinterpret_cast<void**>(&statics));
    if (FAILED(hr)) {
      char buf[32];
      snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
      describe += buf;
      continue;
    }
    IUnknown* sensor = nullptr;
    hr = statics->GetDefault(&sensor);
    statics->Release();
    if (FAILED(hr) || sensor == nullptr) {
      describe += "none";
      continue;
    }
    describe += "present";
    auto source = std::make_shared<WindowsSource>(static_cast<ISensorBase*>(sensor), c.type);
    Sensor s;
    s.handle = handle++;
    s.type = c.type;
    s.name = c.name;
    s.vendor = "Microsoft (Windows.Devices.Sensors)";
    s.maxRange = c.maxRange;
    s.resolution = c.resolution;
    s.power = 0.5f;
    UINT32 minMs = source->MinimumIntervalMs();
    s.minDelayUs = c.flags == kOnChange ? 0 : static_cast<int>(std::max<UINT32>(minMs, 1) * 1000);
    s.maxDelayUs = 1000000;
    s.flags = c.flags;
    s.source = source;
    list.push_back(s);
  }
  const char* test = getenv("GESHER_SENSOR_TEST");
  if (test != nullptr && *test != '\0' && *test != '0') {
    Sensor a;
    a.handle = handle++;
    a.type = kAccelerometer;
    a.name = "Gesher Test Accelerometer";
    a.vendor = "Gesher (GESHER_SENSOR_TEST)";
    a.maxRange = 8 * kGravity;
    a.resolution = 0.01f;
    a.power = 0.1f;
    a.minDelayUs = 10000;
    a.maxDelayUs = 1000000;
    a.source = std::make_shared<TestSource>(kAccelerometer);
    list.push_back(a);
    Sensor l;
    l.handle = handle++;
    l.type = kLight;
    l.name = "Gesher Test Light Sensor";
    l.vendor = "Gesher (GESHER_SENSOR_TEST)";
    l.maxRange = 100000;
    l.resolution = 1;
    l.power = 0.1f;
    l.minDelayUs = 0;
    l.maxDelayUs = 1000000;
    l.flags = kOnChange;
    l.source = std::make_shared<TestSource>(kLight);
    list.push_back(l);
    describe += ", test sensors: accelerometer, light";
  }
  return list;
}

// --- sampling --------------------------------------------------------------------------

struct Subscription {
  Sink* sink;
  int handle;
  int64_t periodNs;
  int64_t nextNs;
  bool onChange;
  bool sent;
  Reading last;
};

class Sampler {
 public:
  static Sampler& Get() {
    static Sampler* sampler = new Sampler();  // never destroyed: its thread outlives main
    return *sampler;
  }

  int Enable(Sink* sink, int handle, int periodUs) {
    const Sensor* sensor = Find(handle);
    if (sensor == nullptr) return -EINVAL;
    int64_t period = std::max<int64_t>(periodUs, std::max(sensor->minDelayUs, 1000)) * 1000;
    if (sensor->flags == kOnChange) period = std::max<int64_t>(period, 20000000);  // 50 Hz
    std::lock_guard<std::mutex> lock(mutex_);
    for (Subscription& s : subs_) {
      if (s.sink == sink && s.handle == handle) {
        s.periodNs = period;
        cv_.notify_all();
        return 0;
      }
    }
    subs_.push_back({sink, handle, period, android::elapsedRealtimeNano(),
                     sensor->flags == kOnChange, false, Reading()});
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    cv_.notify_all();
    return 0;
  }

  int Disable(Sink* sink, int handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [&](const Subscription& s) {
                                 return s.sink == sink && s.handle == handle;
                               }),
                subs_.end());
    return 0;
  }

  void DisableAll(Sink* sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [&](const Subscription& s) { return s.sink == sink; }),
                subs_.end());
  }

 private:
  static const Sensor* Find(int handle) {
    for (const Sensor& s : List()) {
      if (s.handle == handle) return &s;
    }
    return nullptr;
  }

  // Events are delivered with the lock held, so that DisableAll ends them.
  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      int64_t now = android::elapsedRealtimeNano();
      int64_t next = INT64_MAX;
      for (Subscription& s : subs_) {
        if (s.nextNs <= now) {
          const Sensor* sensor = Find(s.handle);
          Reading r;
          if (sensor != nullptr && sensor->source->Read(&r)) {
            bool changed = !s.sent || memcmp(r.values, s.last.values, sizeof(r.values)) != 0 ||
                           r.accuracy != s.last.accuracy;
            if (!s.onChange || changed) {
              s.sink->OnEvent(s.handle, r, android::elapsedRealtimeNano());
              s.sent = true;
              s.last = r;
            }
          }
          s.nextNs = std::max(s.nextNs + s.periodNs, now);
        }
        next = std::min(next, s.nextNs);
      }
      if (next == INT64_MAX) {
        cv_.wait(lock);
      } else {
        cv_.wait_for(lock, std::chrono::nanoseconds(std::max<int64_t>(next - now, 0)));
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Subscription> subs_;
  std::thread thread_;
};

}  // namespace

const std::vector<Sensor>& List() {
  static const std::vector<Sensor>* list = new std::vector<Sensor>(Find());
  return *list;
}

int Enable(Sink* sink, int handle, int periodUs) {
  return Sampler::Get().Enable(sink, handle, periodUs);
}

int Disable(Sink* sink, int handle) { return Sampler::Get().Disable(sink, handle); }

void DisableAll(Sink* sink) { Sampler::Get().DisableAll(sink); }

std::string Describe() {
  List();
  return DescribeText();
}

}  // namespace gesher::devices::sensors

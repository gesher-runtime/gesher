// Gesher (devices track): Windows.Devices.Geolocation through its ABI.

#include "location_win.h"

#include <cmath>

#include "winrt.h"

namespace gesher::devices::location {

using winrt::IAsyncOperation;
using winrt::IInspectable;
using winrt::IReference;

namespace {

constexpr wchar_t kGeolocator[] = L"Windows.Devices.Geolocation.Geolocator";

struct DateTime {
  int64_t UniversalTime;
};
struct TimeSpan {
  int64_t Duration;
};

struct IGeocoordinate : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Latitude(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Longitude(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Altitude(IReference<double>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Accuracy(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AltitudeAccuracy(IReference<double>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Heading(IReference<double>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Speed(IReference<double>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(DateTime* value) = 0;
};

struct IGeocoordinateWithPositionData : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_PositionSource(int* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SatelliteData(IInspectable** value) = 0;
};

struct IGeoposition : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Coordinate(IGeocoordinate** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_CivicAddress(IInspectable** value) = 0;
};

struct IGeolocator : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DesiredAccuracy(int* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_DesiredAccuracy(int value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MovementThreshold(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MovementThreshold(double value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ReportInterval(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ReportInterval(UINT32 value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LocationStatus(int* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGeopositionAsync(
      IAsyncOperation<IGeoposition*>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGeopositionAsyncWithAgeAndTimeout(
      TimeSpan maximumAge, TimeSpan timeout, IAsyncOperation<IGeoposition*>** value) = 0;
  // add_/remove_PositionChanged and StatusChanged follow; not used.
};

struct IGeolocatorStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IAsyncOperation<int>** value) = 0;
  // GetGeopositionHistoryAsync and its overload follow; not used.
};

const IID IID_IGeolocator = {0xA9C3BF62, 0x4524, 0x4989,
                             {0x8A, 0xA9, 0xDE, 0x01, 0x9D, 0x2E, 0x55, 0x1F}};
const IID IID_IGeolocatorStatics = {0x9A8E7571, 0x2DF5, 0x4591,
                                    {0x9F, 0x87, 0xEB, 0x5F, 0xD8, 0x94, 0xE9, 0xB7}};
const IID IID_IGeocoordinateWithPositionData = {
    0x95E634BE, 0xDBD6, 0x40AC, {0xB8, 0xF2, 0xA6, 0x5C, 0x03, 0x40, 0xD9, 0xA6}};

HRESULT NewGeolocator(IGeolocator** geolocator) {
  *geolocator = nullptr;
  IInspectable* instance = nullptr;
  HRESULT hr = winrt::ActivateInstance(kGeolocator, &instance);
  if (FAILED(hr)) return hr;
  hr = instance->QueryInterface(IID_IGeolocator, reinterpret_cast<void**>(geolocator));
  instance->Release();
  return hr;
}

// An optional double: IReference<double> or nothing.
bool Optional(IReference<double>* ref, double* value) {
  if (ref == nullptr) return false;
  bool ok = SUCCEEDED(ref->get_Value(value)) && std::isfinite(*value);
  ref->Release();
  return ok;
}

}  // namespace

Access RequestAccess(HRESULT* hr) {
  IGeolocatorStatics* statics = nullptr;
  *hr = winrt::GetActivationFactory(kGeolocator, IID_IGeolocatorStatics,
                                    reinterpret_cast<void**>(&statics));
  if (FAILED(*hr)) return kUnavailable;
  IAsyncOperation<int>* op = nullptr;
  *hr = statics->RequestAccessAsync(&op);
  statics->Release();
  if (FAILED(*hr)) return kUnavailable;
  *hr = winrt::Wait(op, 10000);
  int status = kUnspecified;
  if (SUCCEEDED(*hr)) *hr = op->GetResults(&status);
  op->Release();
  if (FAILED(*hr)) return kUnavailable;
  return status == kAllowed ? kAllowed : status == kDenied ? kDenied : kUnspecified;
}

int Status(HRESULT* hr) {
  IGeolocator* geolocator = nullptr;
  *hr = NewGeolocator(&geolocator);
  if (FAILED(*hr)) return -1;
  int status = -1;
  *hr = geolocator->get_LocationStatus(&status);
  geolocator->Release();
  return SUCCEEDED(*hr) ? status : -1;
}

HRESULT GetFix(bool high, int64_t maxAgeMs, int64_t timeoutMs, Fix* fix) {
  IGeolocator* geolocator = nullptr;
  HRESULT hr = NewGeolocator(&geolocator);
  if (FAILED(hr)) return hr;
  geolocator->put_DesiredAccuracy(high ? 1 : 0);
  IAsyncOperation<IGeoposition*>* op = nullptr;
  hr = geolocator->GetGeopositionAsyncWithAgeAndTimeout(TimeSpan{maxAgeMs * 10000},
                                                        TimeSpan{timeoutMs * 10000}, &op);
  if (SUCCEEDED(hr)) {
    // A little past Windows' own time-out, so that its error is the one seen.
    hr = winrt::Wait(op, timeoutMs + 2000);
    IGeoposition* position = nullptr;
    if (SUCCEEDED(hr)) hr = op->GetResults(&position);
    op->Release();
    IGeocoordinate* c = nullptr;
    if (SUCCEEDED(hr) && position == nullptr) hr = E_POINTER;
    if (SUCCEEDED(hr)) hr = position->get_Coordinate(&c);
    if (position != nullptr) position->Release();
    if (SUCCEEDED(hr) && c == nullptr) hr = E_POINTER;
    if (SUCCEEDED(hr)) {
      hr = c->get_Latitude(&fix->latitude);
      if (SUCCEEDED(hr)) hr = c->get_Longitude(&fix->longitude);
      if (SUCCEEDED(hr)) hr = c->get_Accuracy(&fix->accuracy);
      if (SUCCEEDED(hr)) {
        IReference<double>* ref = nullptr;
        if (SUCCEEDED(c->get_Altitude(&ref))) fix->hasAltitude = Optional(ref, &fix->altitude);
        ref = nullptr;
        if (SUCCEEDED(c->get_AltitudeAccuracy(&ref))) {
          fix->hasAltitudeAccuracy = Optional(ref, &fix->altitudeAccuracy);
        }
        ref = nullptr;
        if (SUCCEEDED(c->get_Heading(&ref))) fix->hasHeading = Optional(ref, &fix->heading);
        ref = nullptr;
        if (SUCCEEDED(c->get_Speed(&ref))) fix->hasSpeed = Optional(ref, &fix->speed);
        DateTime time{0};
        if (SUCCEEDED(c->get_Timestamp(&time)) && time.UniversalTime > winrt::kUnixEpochTicks) {
          fix->timeMillis = (time.UniversalTime - winrt::kUnixEpochTicks) / 10000;
        }
        IGeocoordinateWithPositionData* data = nullptr;
        if (SUCCEEDED(c->QueryInterface(IID_IGeocoordinateWithPositionData,
                                        reinterpret_cast<void**>(&data)))) {
          int source = -1;
          if (SUCCEEDED(data->get_PositionSource(&source))) fix->source = source;
          data->Release();
        }
      }
      c->Release();
    }
  }
  geolocator->Release();
  return hr;
}

}  // namespace gesher::devices::location

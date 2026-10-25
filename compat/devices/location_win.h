// Gesher (devices track): positions from Windows' location service
// (Windows.Devices.Geolocation.Geolocator), for the location service of the
// system server (com.gesher.server.WindowsLocation, docs/devices.md).
#pragma once

#include <windows.h>
#include <stdint.h>

namespace gesher::devices::location {

// Windows' answer to "may this desktop app use the location?" (the privacy
// setting): Geolocator.RequestAccessAsync's GeolocationAccessStatus.
enum Access : int {
  kUnavailable = -1,  // no location service (or no WinRT): *hr says why
  kUnspecified = 0,
  kAllowed = 1,
  kDenied = 2,
};

Access RequestAccess(HRESULT* hr);

// Geolocator.LocationStatus (PositionStatus: 0 ready, 1 initializing, 2 no
// data, 3 disabled, 4 not initialized, 5 not available), or -1 with *hr.
int Status(HRESULT* hr);

struct Fix {
  double latitude = 0, longitude = 0, accuracy = 0;
  bool hasAltitude = false;
  double altitude = 0;
  bool hasAltitudeAccuracy = false;
  double altitudeAccuracy = 0;
  bool hasHeading = false;
  double heading = 0;
  bool hasSpeed = false;
  double speed = 0;
  int64_t timeMillis = 0;  // Unix time of the fix
  int source = -1;         // PositionSource, or -1 when Windows does not say
};

// One position (Geolocator.GetGeopositionAsync with a maximum age and a
// time-out); DesiredAccuracy High when `high`.
HRESULT GetFix(bool high, int64_t maxAgeMs, int64_t timeoutMs, Fix* fix);

}  // namespace gesher::devices::location

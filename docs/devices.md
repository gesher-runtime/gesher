# Devices: location, sensors, cameras (devices track)

Design: [0043](decisions/0043-devices-hardware-apis.md). Tests:
`tests/windows/tracks/58-devices.ps1` with `tests/apps/devices` (`devices.apk`,
built in framework.yml), in the single-process mode and on the Gesher device.

## Pieces

| Piece | Where |
|---|---|
| Location service (`location`) | `app/java/com/gesher/server/LocationManagerService.java` (registered in `Services.create`) |
| Windows' location service | `app/java/com/gesher/server/WindowsLocation.java`, `compat/devices/location_win.cc` (Windows.Devices.Geolocation.Geolocator through its ABI), `compat/devices/winrt.cc` (combase's WinRT functions looked up at run time) |
| framework-location (`LocationManager`, `ILocationManager`...) | not in framework.jar (on a device it is its own boot jar): `app/build.sh` puts the classes the framework build compiled from `frameworks/base/location/java` into gesher-app.jar, replacing app/compat-java's placeholders |
| Sensors | `compat/devices/sensors_jni.cc` (SystemSensorManager and BaseEventQueue natives, replacing device_standins.cc's), `compat/devices/sensors.cc` (Windows.Devices.Sensors; GESHER_SENSOR_TEST's sensors; one sampling thread) |
| Cameras | `compat/devices/camera_mf.cc` (Media Foundation capture devices, NV12 through an IMFSourceReader; GESHER_CAMERA_TEST's camera), `camera_metadata_jni.cc` (CameraMetadataNative over system/media's libcamera_metadata), `camera_frames.cc` (ImageReader, SurfaceUtils, frames into surfaces) |
| Registration | `compat/devices/devices_jni.cc` (`register_gesher_devices`, a row in `compat/framework/android_runtime.cc` after the device stand-ins); `compat/devices/devices.cmake` (included from CMakeLists.txt) |

All natives are in dalvikvm.exe (libandroid_runtime's builtin library): the
installer ships nothing more. Windows' APIs are found at run time, so a
Windows without them (no location service, no Media Foundation) gets clean
"none" answers instead of a runtime that does not start.

## Location

- Providers `passive`, `network`, `fused`, `gps`. gps, network and fused are
  Windows' location service (gps asks for high accuracy); passive gets what
  the others report. `isLocationEnabled()` is Windows' setting for desktop
  apps ("Let desktop apps access your location"), or a test provider being on.
- Permissions: the app's manifest must ask for ACCESS_FINE_LOCATION or
  ACCESS_COARSE_LOCATION (Gesher grants what an app requests; there is no
  runtime prompt yet), else a SecurityException as on Android; coarse-only apps
  get locations snapped to a 2 km grid, without altitude, speed or bearing.
- Requests: listeners and PendingIntents (`KEY_LOCATION_CHANGED`,
  `KEY_LOCATIONS`, `KEY_PROVIDER_ENABLED`, `KEY_FLUSH_COMPLETE`),
  `getCurrentLocation` (a last location under 30 s old, else the next one
  within the request's duration), `getLastKnownLocation` (Windows' cached
  position the first time). Interval, minimum interval and distance, maximum
  updates and duration are honoured. Windows is polled at the shortest
  interval asked for (at least 1 s), only while a request needs it.
- Test providers: `addTestProvider` replaces a real provider of that name (or
  adds one), disabled until `setTestProviderEnabled`; `setTestProviderLocation`
  delivers mock locations (`isMock()`). Any app may add them (Android asks for
  the mock-location app op; Gesher's app ops allow everything).
- `PROVIDERS_CHANGED_ACTION` is broadcast on changes.
- Not there: a geocoder (`Geocoder.isPresent()` is false and its calls throw
  IOException), GNSS status, NMEA and measurements (accepted, never called
  back), geofences (accepted, not monitored).
- `ActivityManagerService.isIntentSenderTargetedToPackage` (as Android's): the
  client checks it before registering a PendingIntent.

## Sensors

- Windows.Devices.Sensors' default accelerometer, gyrometer, magnetometer,
  light sensor and orientation sensor, when the PC has them, as
  TYPE_ACCELEROMETER (m/s^2, Android's sign), TYPE_GYROSCOPE (rad/s),
  TYPE_MAGNETIC_FIELD, TYPE_LIGHT and TYPE_ROTATION_VECTOR (the quaternion).
  Readings are polled at the rate asked for; on-change sensors report changes
  only. The CI runners have none: the list is empty and `registerListener`
  answers false (checked).
- `GESHER_SENSOR_TEST=1` adds a test accelerometer (gravity on Z) and a test
  light sensor (250 lux), below the framework's natives, so the whole path is
  checked where there is no hardware.
- Each event queue is an eventfd watched by the queue's Looper, as Android's
  is a BitTube's fd; events are dispatched on the Looper's thread
  (`dispatchSensorEvent`, `dispatchFlushCompleteEvent`). No direct channels,
  no data injection, no dynamic sensors.

## Cameras

In progress (see Status).

## Status and next steps

### Done and verified
- **CI run 40134370867** (ci, both windows-2022 and windows-2025, every job
  green; commit 2b3d509): location and sensors, single process and on the
  Gesher device:
  - `PASS  devices: location (single process): LocationManager from framework-location, the gps/network/fused/passive providers, no geocoder`
  - `PASS  devices: location (single process): a test provider in gps' place: requestLocationUpdates to a listener and a PendingIntent, getLastKnownLocation, getCurrentLocation, removeUpdates`
  - `PASS  devices: location (single process): provider disabled and enabled reach the listener; nothing after removeUpdates(listener)`
  - `PASS  devices: location (single process): fused as apps without Play services ask (LocationRequest, an Executor), test providers removed, Geocoder throws IOException`
  - `PASS  devices: location (single process): Windows.Devices.Geolocation answers: access denied, status not initialized; network provider: disabled (Windows gives this program no location)`
  - `PASS  devices: sensors (single process): SystemSensorManager over Windows.Devices.Sensors; no sensors here, an empty list, registerListener false (accelerometer: none, gyrometer: none, magnetometer: none, light: none, orientation: none)`
  - `PASS  devices: sensors (single process): GESHER_SENSOR_TEST's accelerometer and light sensor through SystemSensorManager: the list, events on a Handler, flush, unregisterListener`
  - the same five location checks and the test-sensor check on the Gesher device
    (the location service in the system server, the app in its own process,
    listeners and the PendingIntent's broadcast across binder).
- The runners deny desktop apps the location (RequestAccessAsync: Denied):
  the WinRT calls themselves work and fail cleanly there. On a Windows 11 PC
  with location on, the same code gets a position (Wi-Fi/IP, checked by hand
  with a probe of location_win.cc: accuracy 20 km, source IP address).

### Next steps
1. Cameras (in progress): Java side (`CameraService`, `CameraDeviceUser`,
   `WindowsCamera`, `CameraFrames` in com.gesher.server), the in-process
   `media.camera` (single process: `Services`; device: the app process),
   devices.apk's camera steps, 58-devices.ps1 checks.
2. Legacy `android.hardware.Camera`: answer "no cameras" cleanly.
3. ADR 0043, roadmap boxes.

### Open problems, dead ends ruled out
- Windows.Devices.Sensors' classes all activate on the runners (their statics
  interfaces resolve) and `GetDefault` answers null: there are no sensors to
  read values from, so the value conversion of real sensors is checked only by
  reading the SDK's definitions, not on hardware.
- The local llvm-mingw (scratch) syntax-checks compat/devices against AOSP's
  headers without building AOSP; CI builds it.

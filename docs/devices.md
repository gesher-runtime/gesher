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
  IOException; final: Windows.Services.Maps' MapLocationFinder answers
  InvalidCredentials without a Bing Maps key, checked on the runners by
  58-devices.ps1 and locally; hwsvc2, ADR 0051), GNSS status, NMEA and measurements (accepted, never called
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

- `media.camera` (`CameraService`, cameraserver's ICameraService) runs in the
  app's own process: `Services.installInProcess` (single process) and
  `GesherAppProcess` (each app process of the Gesher device) put it into the
  process's ServiceManager cache, where CameraManager looks first. A request's
  Surfaces are that process's objects, which no binder transaction could carry.
- Devices: Media Foundation's video capture devices (`MFEnumDeviceSources`,
  read with an `IMFSourceReader`, NV12 out with video processing), ids "0",
  "1"... in Windows' order. The reader has advanced video processing on
  (`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`), so cameras that only
  offer YUY2 (or MJPG, decoded by Windows) still give NV12.
- The test camera (`GESHER_CAMERA_TEST=1`) comes last. It is a Media Foundation
  media source of Gesher's own (`TestSource` in camera_mf.cc: a live
  IMFMediaSource with one stream offering YUY2 at 640x480 and 320x240, 30
  frames a second; Y = x + y with a moving bright square in rows 0-31, gray
  chroma), read through the same path as a PC's camera: the same
  `IMFSourceReader` with video processing (YUY2 converted to NV12 by Windows),
  `ReadSample`, the same sample copying, the same Camera2 service above. Only
  where Media Foundation's DLLs are missing does it fall back to frames made
  directly as NV12 (logged as "without Media Foundation"; the checks then fail,
  since they ask for the Media Foundation path).
- What a real camera adds that CI does not exercise: `MFEnumDeviceSources`
  finding a device and `IMFActivate::ActivateObject` (the runners have no
  capture device, so the enumeration answers 0), the Frame Server and the
  driver below a real camera's source, MJPG decoding, the camera privacy
  setting (a denied app gets E_ACCESSDENIED from ActivateObject or ReadSample,
  reported to Camera2 as an open or capture error), and real exposure and
  timing. A virtual camera (`MFCreateVirtualCamera`, Windows 11) needs the
  source as a registered COM server and the Frame Server service; not done.
- Characteristics: an EXTERNAL-level camera (as Android's USB cameras): fixed
  focus, auto exposure and white balance, YUV_420_888, PRIVATE and JPEG at the
  device's sizes, LENS_FACING_EXTERNAL (the test camera: BACK), one partial
  result, the available request/result/characteristics keys.
- `CameraDeviceUser` (CameraDeviceClient): streams from OutputConfigurations,
  `submitRequestList` (bursts get their frame numbers at submission; the
  repeating request one per frame), `cancelRequest`, `flush`, `waitUntilIdle`,
  default request templates (preview, still, record, video snapshot), and a
  capture thread: one frame per request, opened at the largest configured
  size, then for each target: an ImageReader gets YUV_420_888 (I420 planes),
  RGBA_8888 or JPEG (encoded with YuvImage, else a Bitmap), scaled to the
  reader's size; another surface with a native window gets RGBA through
  `ANativeWindow_lock`. Then `onCaptureStarted` and the result
  (`CameraMetadataInfo.metadata`, SENSOR_TIMESTAMP and the 3A states).
  `onDeviceIdle` when nothing is left. A second client of the same camera
  evicts the first (onError DISCONNECTED), as cameraserver does.
- Permission: `connectDevice` needs `android.permission.CAMERA` in the app's
  manifest (`IPackageManager.getPackageInfo`), else `ERROR_PERMISSION_DENIED`
  (a SecurityException in the app).
- Natives: `CameraMetadataNative` over system/media's libcamera_metadata
  (`camera_metadata_jni.cc`; no vendor tags; a Parcel carries the packed
  buffer, in-process only); `CameraDeviceImpl`'s result queue natives answer
  "none"; `ImageReader`/`SurfaceImage` (`camera_frames.cc`): images in process
  memory, the reader's Surface a plain Java Surface only these natives know;
  `SurfaceUtils` (size, format, usage of readers and native windows). An empty
  `libmedia_jni.so` builtin is registered when nothing else provides one
  (ImageReader's static initializer loads it).
- Legacy `android.hardware.Camera` (hwsvc2, ADR 0051): the same devices as
  Camera2, in the app's process (`LegacyCamera`, natives in camera_frames.cc that
  call it): ids in Windows' order, a PC's camera FRONT, the test camera BACK;
  parameters of a fixed-focus camera without flash, zoom or face detection; preview
  callbacks (NV21 or YV12, callback buffers) and a preview surface with a native
  window; `setPreviewTexture` accepted without frames; `takePicture` (shutter, a
  null raw image, postview, JPEG at the picture size; the preview stops);
  `autoFocus` succeeds at once; CAMERA in the manifest, else `Fail to connect to
  camera service`. Without a camera: `getNumberOfCameras()` 0, `open()` null.
- `MediaStore.ACTION_IMAGE_CAPTURE`: Gesher Camera (`app/gesher-camera`,
  `gesher-camera.apk` next to dalvikvm.exe; `CameraApp`): the persistent device
  installs it at its start when Camera2 has cameras and removes it when there are
  none (so without a camera nothing resolves it, as on a device without one). A
  Camera2 preview in an ImageView with Take photo and Cancel; the JPEG (largest size)
  goes to EXTRA_OUTPUT (RESULT_OK) or comes back as a 160-pixel-wide thumbnail in
  `data`. `com.gesher.camera.extra.AUTO_CAPTURE_MS` takes it by itself (tests).
- Not there: reprocessing, offline sessions, extensions, torch, high-speed
  sessions, multi-resolution readers, ImageReader surfaces used by other
  producers than the camera (MediaCodec...), preview into SurfaceView or
  TextureView (they get no native window on Gesher today; frames go to any
  surface that has one).

## Status and next steps

- Continuation (2026-10-26): ci 40291437446 at 025fda1 passed windows-2022,
  package and e2e. windows-2025 ended with one locale test failure: the restored
  locale and HandlesActivity callback #2 are present, but Set-Culture's own
  broadcast finishes the test activity before the explicit WM_SETTINGCHANGE
  send returns. Updated 49-locale to accept that completed callback while still
  requiring the restored locale, first day and hour cycle. This test fix is
  not yet CI-verified; all remaining surfaces scope below stays open.
- Running: ci 40297011754 at b8d6752, after merging integration a98dda9.
  framework.yml keeps both surfaceapp and winapp build/copy lines.
- That run passed windows-2025/package/e2e; windows-2022 failed only the known
  badssl timeout. All surfaces checks pass there. Merged integration 8b1b09d
  (native2), and expanded e2e to both Windows runners to verify TIC-80.
  New combined verification is required before merge.

### Done and verified
- **hwsvc2 track, ci run 40214264956 at aaa1f90** (ADR 0051; every job green,
  both runners; see docs/services.md "Status and next steps (hwsvc2)"):
  - `PASS  devices: camera (single process): the legacy android.hardware.Camera over GESHER_CAMERA_TEST's camera: info, parameters, a 320x240 NV21 preview into callback buffers, autoFocus, takePicture (shutter, raw, JPEG), release`
  - the same on the Gesher device
  - `PASS  devices: camera (Gesher device): ACTION_IMAGE_CAPTURE resolves to Gesher Camera (installed by the device as the PC has a camera); without EXTRA_OUTPUT it answers RESULT_OK (thumbnail: none), with it it writes a 640x480 JPEG to the caller's provider's content URI`
  - `PASS  devices: geocoder: Windows.Services.Maps without a map service key answers InvalidCredentials (Geocoder stays absent: isPresent false, IOException)`
  - Still open: preview into SurfaceView/TextureView (no native window for their
    child surfaces); the thumbnail Bitmap across processes (Gesher's hwui has no
    Bitmap parcel natives: graphics track).- **CI run 40142590418** (ci at f62f1aa, merged with origin/phase4/integration
  at 6751717; every job green: test on windows-2022 and windows-2025, package,
  e2e): location, sensors and cameras, single process and on the Gesher device.
  The devices checks, the same on both runners:
  - `PASS  devices: location (single process): LocationManager from framework-location, the gps/network/fused/passive providers, no geocoder`
  - `PASS  devices: location (single process): a test provider in gps' place: requestLocationUpdates to a listener and a PendingIntent, getLastKnownLocation, getCurrentLocation, removeUpdates`
  - `PASS  devices: location (single process): provider disabled and enabled reach the listener; nothing after removeUpdates(listener)`
  - `PASS  devices: location (single process): fused as apps without Play services ask (LocationRequest, an Executor), test providers removed, Geocoder throws IOException`
  - `PASS  devices: location (single process): Windows.Devices.Geolocation answers: access denied, status not initialized; network provider: disabled (Windows gives this program no location)`
  - `PASS  devices: sensors (single process): SystemSensorManager over Windows.Devices.Sensors; no sensors here, an empty list, registerListener false (accelerometer: none, gyrometer: none, magnetometer: none, light: none, orientation: none)`
  - `PASS  devices: camera (single process): CameraManager over Media Foundation; no camera here (Media Foundation: 0 video capture devices): an empty id list, openCamera throws, legacy Camera none, no ACTION_IMAGE_CAPTURE activity`
  - `PASS  devices: sensors (single process): GESHER_SENSOR_TEST's accelerometer and light sensor through SystemSensorManager: the list, events on a Handler, flush, unregisterListener`
  - `PASS  devices: camera (single process): GESHER_CAMERA_TEST's camera through Camera2: characteristics, openCamera, a session, repeating preview into a YUV ImageReader, a JPEG still capture, close`
  - `PASS  devices: camera (single process): the test camera's frames through Media Foundation: test camera (GESHER_CAMERA_TEST) as a Media Foundation media source, 2 YUY2 sizes; opened at 640x480 through Media Foundation (IMFSourceReader over the test media source, YUY2 to NV12)`
  - `PASS  devices: permissions (single process): without ACCESS_*_LOCATION LocationManager throws SecurityException; without CAMERA openCamera throws SecurityException`
  - the five location checks, the test-sensor check and both camera checks
    again on the Gesher device (location service in the system server, the
    app in its own process, `media.camera` in the app's process).
- Earlier: ci run 40134370867 (location and sensors, both runners).
- The runners deny desktop apps the location (RequestAccessAsync: Denied):
  the WinRT calls themselves work and fail cleanly there. On a Windows 11 PC
  with location on, the same code gets a position (Wi-Fi/IP, checked by hand
  with a probe of location_win.cc: accuracy 20 km, source IP address).

### Not verified (no hardware on the runners)
- A real camera: device enumeration and activation, the Frame Server and
  driver, MJPG decoding, the camera privacy setting, real exposure/timing
  (see Cameras above). The source reader, its YUY2-to-NV12 processing, sample
  copying and everything above are what a real camera uses too, and are
  checked.
- Real sensors' value conversion (checked against the SDK's definitions only).
- A real position from Windows in CI (by hand only).

### Next steps (beyond the brief)
1. Preview into SurfaceView/TextureView once the graphics stack gives them a
   native window.
2. (Done by hwsvc2: Gesher Camera, the legacy android.hardware.Camera.) Bitmap
   parcelling in hwui, so Gesher Camera's thumbnail reaches the caller.
3. A virtual camera (MFCreateVirtualCamera) where Frame Server is available.

### Open problems, dead ends ruled out
- Windows.Devices.Sensors' classes all activate on the runners (their statics
  interfaces resolve) and `GetDefault` answers null: there are no sensors to
  read values from.
- The local llvm-mingw (scratch) syntax-checks compat/devices against AOSP's
  headers without building AOSP; CI builds it.
- CameraManager's getRotationOverride walks ActivityManager.getAppTasks():
  Gesher's ActivityTaskManagerService answers an empty list (it answered null).
- android.media.PublicFormatUtils' natives come from libui's PublicFormat.cpp,
  which Gesher does not build: camera_frames.cc maps the formats itself.

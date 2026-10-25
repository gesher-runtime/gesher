# 0043: Device APIs over Windows' location, sensor and camera APIs (devices track)

Status: accepted (devices track; see docs/devices.md)

## Context

Android apps reach a phone's hardware through system services and their
natives: LocationManager talks to system_server's location service,
SensorManager's natives to sensorservice, Camera2's CameraManager to
cameraserver, which fills the app's Surfaces through BufferQueues. On
Gesher none of these existed: framework-location (LocationManager and its
AIDL) was not even on the boot class path, SystemSensorManager's natives were
empty stand-ins, and CameraManager found no `media.camera` (CameraMetadataNative,
ImageReader and SurfaceUtils had no natives at all).

Windows offers the same hardware through Windows.Devices.Geolocation (WinRT),
Windows.Devices.Sensors (WinRT) and Media Foundation (COM). The CI runners have
no camera, no sensors and deny desktop apps the location.

## Decision

1. **Location lives in the system server**, as on Android: a
   `LocationManagerService` (ILocationManager) with gps/network/fused backed by
   Windows' Geolocator and passive fed by the others, test providers replacing
   or adding providers. Windows' privacy setting is the master switch; an app's
   manifest permission (fine or coarse) is its runtime permission, since Gesher
   grants what an app requests. framework-location's classes, which the
   framework build compiles but framework.jar does not contain, go into
   gesher-app.jar (already on every boot class path) rather than a new boot jar,
   which would have touched the launcher, the installer, the boot image and
   every test's class path.
2. **Sensors live in the app process**, below SystemSensorManager's natives:
   Gesher's natives list Windows' default sensors and sample them on one
   thread into each event queue's eventfd, watched by the queue's Looper (the
   framework's BitTube design). No sensorservice: one process reading a sensor
   needs no broker on Windows.
3. **Cameras live in the app process too**: `media.camera` is put into the app
   process's ServiceManager cache (device mode and single-process mode). A
   capture request's targets are Surfaces of that process (Gesher's ImageReader
   natives, or native windows); no binder transaction could carry them, and
   Windows arbitrates the device itself. ImageReader gets Gesher's own natives:
   a queue of images in process memory whose Surface only these natives (and
   SurfaceUtils') recognize. CameraMetadataNative is built on system/media's
   libcamera_metadata, as Android's is, without vendor tags.
4. **Windows' APIs are looked up at run time** (combase's Ro* functions, the
   Media Foundation DLLs) and declared by their ABI in Gesher's sources: the
   runtime starts on any Windows, and a Windows without them has none of the
   devices, cleanly.
5. **CI verifies with test sources injected below the framework APIs**
   (`GESHER_SENSOR_TEST`, `GESHER_CAMERA_TEST`, test location providers), plus
   the real Windows calls' answers on the runners (no device, access denied).
   The test camera sits as low as a process can put it without registering
   anything with Windows: it is an IMFMediaSource of Gesher's own (YUY2, as
   webcams offer), read by the same Media Foundation source reader, video
   processing and sample copying as a real camera. A virtual camera
   (`MFCreateVirtualCamera`) was not used: it needs the source as a
   registered COM server run by the Frame Server service, which CI runners
   may not have; it would add device enumeration and activation to what CI
   covers, nothing above.

## Consequences

- Real apps get LocationManager (with or without Play services' fused API
  shape), sensors when the PC has them, and Camera2 with YUV/JPEG/PRIVATE
  outputs. Not covered: geocoding, GNSS details, geofences, sensor direct
  channels, reprocessing, offline sessions, extensions, torch, the legacy
  android.hardware.Camera (it reports no cameras), ACTION_IMAGE_CAPTURE (no
  activity resolves it).
- Preview into SurfaceView/TextureView works only where the graphics stack
  gives those surfaces a native window (ANativeWindow_lock); today it does not
  (SurfaceControl stand-ins, no SurfaceTexture), so preview goes to an
  ImageReader in the tests.
- Two apps cannot share a camera at once across processes; Windows decides
  who gets it.
- If another track provides `libmedia_jni.so`, ImageReader's natives must stay
  this track's or be merged (this track registers an empty one only when none
  exists).

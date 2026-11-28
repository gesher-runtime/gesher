# System features

`PackageManager.hasSystemFeature` and `getSystemAvailableFeatures` tell apps what
the device has. Google Play also filters on them: at check-in (microG's too) the
device uploads the feature names, `ConfigurationInfo.reqGlEsVersion` and the GL
extensions, and Play hides an app that `<uses-feature>`s, or whose permissions
imply, a feature the device lacks. The framework checks some of them as well.

Gesher reports a feature only when real Windows hardware or a Windows runtime
backs it **and** the Android API behind it is implemented (CONTRIBUTING.md,
"Report successful ... capabilities that the backend does not implement" is out
of scope). A missing feature is Android's normal "this device has no X" state.
An API that exists without its feature returns Android's documented
unavailable result.

## Mechanism

- `PackageManagerService.FEATURES` (`app/java/com/gesher/server/PackageManagerService.java`)
  is built once, when the system server starts, from what Windows has then. It
  is served by `hasSystemFeature` and `getSystemAvailableFeatures`, and goes to
  every app process through `ApplicationSharedMemory`'s `SystemFeaturesCache`
  (`ActivityManagerService`). Hardware attached later (for example a USB
  webcam, a headset on a PC with no other output, or the WebView2 Runtime
  installed afterward) appears after Gesher restarts, the same as Android's
  boot-time feature list.
- `getSystemAvailableFeatures` ends with an unnamed `FeatureInfo` carrying
  `reqGlEsVersion`, as AOSP's does. `ActivityTaskManagerService.getDeviceConfigurationInfo`
  gives the same value. Both read `ro.opengles.version`, which the system server
  sets at boot from the device Android's default EGL display runs on ("OpenGL ES
  version" below).
- The Windows probes are `WindowsHost.hardwareSummary()`
  (`compat/devsvc/devsvc_hardware.cc`), `WebView2Host.runtimeVersion()`,
  `WindowsCamera.devices()`, `WindowsBluetooth.adapter()`, the audio engine's
  input devices, `ActivityManager.isLowRamDeviceStatic()`,
  `WindowsSensors.types()` (the sensor list SensorManager serves),
  `WindowsWifi.hardware()` (the WLAN census, docs/wifi.md),
  `WindowsLocation.hasGnssReceiver()` and
  `TelephonyService.bootSnapshot()` (the cellular modem, docs/telephony.md).
  Without the runtime natives (a host JVM) a probe reports nothing.

## OpenGL ES version

The declared version (`ro.opengles.version`, `reqGlEsVersion`) follows the
device Android's default EGL display runs on, by speed: **OpenGL ES 3.0
(0x30000) only when ANGLE runs on a hardware Direct3D 11 device that gives
ES 3.0**. Without a GPU the device is a software one, WARP, and it is capped
so that it gives only ES 2.0, which it declares (0x20000), as an Android device
whose GPU does not support ES 3.0 does. What the device declares
(`PackageManager`, `ConfigurationInfo`) and what EGL gives (configs, contexts,
`GL_VERSION`) are the same (CDD 7.1.4.1), in every process of a boot.

**Who gets ES 2.0.** Machines without a GPU, or whose Direct3D hardware device
is the Microsoft Basic Render Driver: virtual machines without GPU
passthrough or paravirtualization, Remote Desktop / Remote Desktop Services
sessions that have no GPU, Windows Server without a display driver, and the CI
runners. Before this change they got ES 3.0 on WARP; now they get ES 2.0, and
apps that require ES 3.0 (`<uses-feature android:glEsVersion="0x00030000">`,
which Google Play also filters on) are incompatible there, as on an Android
device without ES 3.0. A GPU below feature level 10_1 keeps ES 2.0 on the GPU.

All of Android's EGL paths (an app's `.so`, `android.opengl.EGL14`/`GLSurfaceView`,
hwui) use one default display, `gesher_egl_default_display()`
(`compat/bionic/egl_gles.cc`): ANGLE on Direct3D 11. ANGLE at the pinned
commit (ab6a82e0, `renderer11_utils.cpp` `GetMaximumClientVersion`) gives
ES 3.0 from feature level 10_1, ES 3.1 from 11_0, and ES 2.0 below 10_1. One
policy (`compat/bionic/egl_gles_version.h`) chooses the display and the
version.

**At boot**, the system server probes with nothing declared yet:

| ANGLE's device | Default display | Declared |
| --- | --- | --- |
| hardware device on a GPU with ES 3.0 configs (feature level 10_1+) | that device | 0x30000 |
| hardware device on a GPU without them (Direct3D 10.0 hardware, such as GeForce 8/9, GMA X4500) | that device, ES 2.0 on the GPU | 0x20000 |
| hardware device on the Microsoft Basic Render Driver (no display driver; the CI runners), or not identified | WARP at feature level 10_0 | 0x20000 |
| the hardware device failed or had no ES configs, or `GESHER_EGL_DEVICE=warp` | WARP at feature level 10_0 | 0x20000 |
| none (no ANGLE DLLs, or no Direct3D 11 device at all) | `EGL_NO_DISPLAY` | none (`GL_ES_VERSION_UNDEFINED`) |

**Every process after that** reads the boot's `ro.opengles.version` and
chooses the display that gives exactly that version, whatever the adapter has
become meanwhile: the console's GPU at boot and then a Remote Desktop
reconnect onto the Basic Render Driver, a driver update or a device reset
falling back to the Basic Render Driver, or `GESHER_EGL_DEVICE=warp` for one
process. Declared 3.0: the GPU if it still gives ES 3.0, else WARP uncapped,
which gives ES 3.0 in software (slower, but what the device declared).
Declared 2.0: the GPU or WARP, both capped at feature level 10_0, so a GPU that
appeared later still gives ES 2.0 only. A display that would give another
version is not kept (no default display rather than a disagreement). The
declaration changes only with the next boot (Gesher's restart), as on Android.

WARP and a device declared 2.0 are opened with ANGLE's
`EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE` 10 and `..._MINOR_ANGLE` 0
(`Renderer11` then asks Direct3D for feature level 10_0 at most; this works on
the hardware device type too), ANGLE's own cap: no config carries
`EGL_OPENGL_ES3_BIT_KHR` in `EGL_RENDERABLE_TYPE` or `EGL_CONFORMANT`, so
`eglChooseConfig` with that bit finds nothing; `eglCreateContext` with
`EGL_CONTEXT_CLIENT_VERSION` 3 fails with `EGL_BAD_MATCH` (ANGLE's
`ValidateCreateContext`, as `EGL_KHR_create_context` specifies for a version
the config does not support); an ES 2 context's `GL_VERSION` is "OpenGL ES
2.0". hwui's SkiaGL and Skia's Ganesh use ES 2 contexts and work on it
(`GESHER_HWUI_RENDERER=skiagl` with `GESHER_EGL_DEVICE=warp`).

**ES 3.1 and 3.2 are not declared, and not given.** On a GPU at feature level
11_0 ANGLE would create ES 3.1 contexts, but its Direct3D 11 backend reports
3.0 as its highest conformant version (`Renderer11::getMaxConformantESVersion`,
"3.1 support is in progress"), and declaring 0x30001 would claim what Gesher
has not verified (Android declares ES 3.1 only on drivers that pass it; AEP,
`android.hardware.opengles.aep`, needs 3.1 plus more and is not reported
either). Capping the GPU at feature level 10_1 would also cap ES at 3.0, but it
drops what the hardware gives within ES 3.0 (for example 16384-pixel textures,
and extensions only feature level 11 has). So the GPU keeps its feature level
and the EGL entry points refuse a context above the version the default
display gives (an ES 3.1 or 3.2 context on an ES 3.0 display) with
`EGL_BAD_MATCH`, as a 3.0 driver refuses it: `eglCreateContext` of an app's
`.so` (`compat/bionic/egl_gles.cc`) and of the Java bindings and hwui
(`compat/hwui/gl/angle_gl.cc`, through `gesher_egl_create_context`). The error
is ANGLE's own (it is asked for ES 3.9, which its validation refuses with
`EGL_BAD_MATCH`), so `eglGetError` reports it as for any refused context.
Contexts on other displays (an app's own ANGLE display through
`eglGetPlatformDisplay`'s pass-through) are ANGLE's.

"Hardware" is decided from the display's own Direct3D 11 device
(`EGL_EXT_device_query`, `EGL_D3D11_DEVICE_ANGLE`, then the DXGI adapter's
`GetDesc1`): an adapter flagged `DXGI_ADAPTER_FLAG_SOFTWARE`, the Basic Render
Driver (vendor 0x1414, device 0x8c), a device opened as WARP, or an adapter the
query cannot identify is not hardware. ANGLE's "hardware" device type alone is
not evidence: without a display driver it runs on the Basic Render Driver.

The system server asks once at boot (`PackageManagerService.glEsVersion`,
`WindowsHost.glEsVersion`, `gesher_egl_gles_version`): it makes the choice with
nothing declared, sets `ro.opengles.version` for every process of that boot,
and terminates the display again, so it keeps no Direct3D device. That costs
one ANGLE initialization (one or two Direct3D 11 devices) at boot, and ANGLE's
DLLs and the Direct3D/driver DLLs it loaded stay loaded in the system server
(about 3.7 MB, as measured in review). The probe runs on a thread of its own
and the system server waits at most 5 s (`GL_PROBE_TIMEOUT_MS`): a display
driver that hangs device creation does not stop the start. Until the probe
answers, no version is declared and the property stays unset (apps started
meanwhile see none declared and choose their display as the probe would);
an answer that comes later is declared then (`lateGlEsVersion`: the property,
`getSystemAvailableFeatures`, `getDeviceConfigurationInfo`), with a log line. A
display that was already initialized in the process (an app's own through
`eglGetPlatformDisplay`'s pass-through with the same attributes) is not
terminated; the system server loads no app code. The probe runs while
PackageManagerService's class is being initialized, so the probe thread's code
is in classes of its own that never touch PackageManagerService (a call into
it from that thread would wait for the initialization, which waits for the
probe); normally it answers in well under a second. A value already in the
property (an explicit `build.prop` line) is kept, as on a device, if it is one
Gesher serves (0x30000 or 0x20000). Any other value in a `build.prop` is not
loaded by any of its three loaders (the property area's initial values,
`compat/posix/src/system_properties.cc`; dalvikvm's, `compat/art/jni_invocation_static.cc`,
with a log line; `BuildProps.java`, with a log line), which share one rule
(`gesher/gles_version_property.h`: decimal digits without a leading zero or
`0x`/`0X` and hexadecimal digits, nothing else; app processes read the boot's
declaration with the same parser, egl_gles.cc), so the property stays unset
and the probe declares; a value set some
other way that Gesher does not serve is not a declaration either, for
PackageManagerService (which declares the probed version) and for the EGL
display choice alike.

Without the ANGLE DLLs, or without any Direct3D 11 device, there is no GLES and
no version is declared.

Not reported:

- **ES 3.1 / 3.2** (above).
- **`android.hardware.opengles.aep`.** AEP needs ES 3.1 plus geometry and
  tessellation shaders and the other AEP extensions, which ANGLE's Direct3D 11
  backend does not provide.

## Reported

| Feature | When | What backs it |
| --- | --- | --- |
| `android.hardware.touchscreen`, `android.hardware.faketouch` | always | phone-sized window, mouse as touch (DeviceProfile); earlier decision |
| `android.hardware.screen.portrait`, `.landscape` | always | windows rotate (docs/windows3.md) |
| `android.hardware.touchscreen.multitouch`, `.multitouch.distinct` | Windows touch digitizer with ≥ 2 contacts (`SM_DIGITIZER` ready with integrated/external touch, `SM_MAXIMUMTOUCHES`) | WM_POINTER contacts become separate MotionEvent pointer ids (`compat/input/translate.cc`, up to 16) |
| `android.hardware.touchscreen.multitouch.jazzhand` | the same, ≥ 5 contacts | as above |
| `android.hardware.audio.output` | an audio render endpoint that is active or an unplugged jack (MMDevice `eRender`, `DEVICE_STATE_ACTIVE \| DEVICE_STATE_UNPLUGGED`): CDD 7.8.2 requires the feature for a speaker or an audio output port, and forbids it without one | AudioTrack/AAudio/OpenSL to the WASAPI sink (docs/media.md). While there is no default endpoint (a jack with nothing in it) the engine plays to a null sink and retries every 5 s, so output starts when something is plugged in. Without any render endpoint (disabled or none) no output is claimed, and the audio APIs keep working as no-ops (CDD 7.8.2 C-2-2) |
| `android.hardware.microphone` | an audio capture device | AudioRecord |
| `android.hardware.ethernet` | a hardware wired Ethernet interface (`GetIfTable2`: `IF_TYPE_ETHERNET_CSMACD`, `NdisPhysicalMedium802_3`, `HardwareInterface`), connected or not | ConnectivityManager's `TRANSPORT_ETHERNET` networks (docs/network.md) |
| `android.hardware.location`, `.location.network` | always | LocationManagerService, Windows location with IP fallback |
| `android.hardware.location.gps` | Windows has a GNSS receiver (`WindowsLocation.hasGnssReceiver`) | the gps provider gets its fixes |
| `android.hardware.bluetooth`, `.bluetooth_le` | Windows radio / LE support | docs/bluetooth.md |
| `android.hardware.camera.any` | a Windows camera, or the selected-image camera while the Control Center's "Allow choosing an image from the PC instead of taking a photo" setting is on at the device's start (CONTRIBUTING.md, PR #487) | CameraService (docs/devices.md) |
| `android.hardware.camera` | a camera explicitly configured as rear-mounted (`GESHER_CAMERA_REAR_ID`), or the selected-image camera ("Gesher Image Picker", a rear camera without autofocus) while that setting is on | as above; Camera2 and the legacy API report it `LENS_FACING_BACK` |
| `android.hardware.camera.front` | a camera not configured as rear: a PC's camera faces the user (maintainer-approved desktop default, CONTRIBUTING.md) | as above; Camera2 and the legacy API report it `LENS_FACING_FRONT` (`CAMERA_FACING_FRONT`) |
| `android.hardware.camera.autofocus` | the rear (`android.hardware.camera`) camera's driver really focuses automatically (`WindowsCamera.Device.autofocus`); a front camera's autofocus does not add it | as above |
| `android.hardware.ram.normal` / `.ram.low` | whichever `ActivityManager.isLowRamDevice()` says (`ro.config.low_ram` is unset: normal) | as AOSP's SystemConfig |
| `android.software.webview` | the WebView2 Runtime is installed | `android.webkit.WebView` on WebView2 (docs/webview.md); without the runtime WebViews stay empty, so it is not claimed |
| `android.hardware.sensor.accelerometer`, `.gyroscope`, `.compass`, `.light` | the matching sensor in the list SensorManager gives apps (Windows.Devices.Sensors' default sensors), except Gesher's stationary accelerometer: the gravity-only desktop default SensorManager lists on a PC without motion sensors (`GESHER_SENSOR_FALLBACK`) is not hardware and declares no feature | SensorManager over Windows sensors (docs/devices.md) |
| `android.hardware.wifi` | a WLAN interface in a `WlanEnumInterfaces` census at boot (waited for at most 5 s; a hung WLAN service means no feature) | WifiManager over wlanapi (docs/wifi.md); no Wi-Fi Direct, Aware, RTT or Passpoint |
| `android.hardware.telephony`, `.telephony.subscription`, `.telephony.radio_access`, `.telephony.data`, `.telephony.gsm` (3GPP only) | a cellular modem the Mobile Broadband API reports at boot | read-only, data-only telephony services (docs/telephony.md) |

## Not reported

Partial (b): some of the API or hardware exists, but not what the feature
promises. Absent (c): nothing implements it.

| Feature | Status | What would be needed |
| --- | --- | --- |
| `android.software.app_widgets` | b | Discover providers, allocate and bind widget IDs, and deliver widget updates to a host. The AppWidgetManager Binder answers empty queries today. |
| `android.hardware.audio.low_latency` | b | Proof of the CDD's latency limits (≤ 45 ms cold output, continuous output ≤ 45 ms, round trip) with the WASAPI shared-mode mixer; measurement on real hardware |
| `android.hardware.audio.pro` | c | low_latency, plus ≤ 20 ms round trip, USB host audio and MIDI |
| `android.hardware.audio.spatial.headtracking.low_latency` | c | a spatializer with head tracking |
| `android.hardware.camera.external` | b | cameras are reported with a facing (FRONT, or BACK when configured as rear), never `LENS_FACING_EXTERNAL`, so the feature that describes such cameras does not apply |
| `android.hardware.camera.flash`, `.capability.*`, `.level.full`, `.concurrent`, `.ar` | c | EXTERNAL level, no flash; needs Media Foundation torch controls and the matching Camera2 metadata |
| `android.software.input_methods` | c | only Windows' own text input is an IME (InputMethodManagerService); hosting installed `InputMethodService` apps |
| `android.software.home_screen` | c | Windows' Start menu is the launcher; hosting a HOME activity |
| `android.software.live_wallpaper` | c | no wallpaper service |
| `android.software.print` | c | a PrintManager service over Windows printing (print spooler / XPS) |
| `android.software.backup` | c | a BackupManager service and transport |
| `android.software.companion_device_setup` | c | CompanionDeviceManager over Windows Bluetooth pairing |
| `android.software.device_admin`, `.managed_users` | c | DevicePolicyManager is the unmanaged stand-in; no profiles or users |
| `android.software.picture_in_picture`, `.expanded_picture_in_picture` | c | enterPictureInPictureMode and PiP windowing (could map to a small topmost window) |
| `android.software.freeform_window_management`, `.activities_on_secondary_displays` | c | tasks are fullscreen on one display (`WINDOWING_MODE_FULLSCREEN`) |
| `android.software.secure_lock_screen` | c | KeyguardManager reports not secure (TrustService); needs Windows sign-in / Hello as the device credential, including auth-bound Keystore keys |
| `android.software.autofill`, `.credentials` | c | AutofillManager / CredentialManager services |
| `android.software.midi` | c | a MidiManager service over Windows MIDI |
| `android.software.connectionservice`, `.telecom`, `.sip`, `.sip.voip` | c | no Telecom service; SIP is deprecated in Android |
| `android.software.controls`, `.window_magnification`, `.wallet_location_based_suggestions`, `.device_lock`, `.cant_save_state` | c | no service |
| `android.software.ipsec_tunnels`, `.ipsec_tunnel_migration` | c | IpSecManager tunnels |
| `android.software.verified_boot`, `android.hardware.security.model.compatible`, `.securely_removes_users` | c | no verified boot chain or Android security model attestation |
| `android.hardware.hardware_keystore`, `.strongbox_keystore`, `.keystore.*`, `.identity_credential*` | c | the AndroidKeyStore is software; a TPM-backed KeyMint would be needed |
| `android.hardware.fingerprint`, `.biometrics.face`, `.biometrics.iris` | c | BiometricPrompt over Windows Hello |
| `android.hardware.usb.host`, `.usb.accessory` | c | UsbService has no host or device manager (docs/devices.md) |
| `android.hardware.nfc*`, `.se.omapi.*` | c | no NFC service; Windows proximity / smart-card APIs could back parts |
| `android.hardware.consumerir`, `.uwb`, `.thread_network`, `.bluetooth_le.channel_sounding` | c | no service |
| `android.hardware.gamepad` | c | no game controller input (XInput / GameInput to `SOURCE_GAMEPAD`) |
| `android.hardware.faketouch.multitouch.*` | c | precision touchpad contacts are not delivered as separate pointers |
| `android.hardware.vulkan.*`, `android.software.vulkan.deqp.level` | c | Vulkan is not built |
| `android.software.opengles.deqp.level` | c | dEQP is not run against Gesher's ANGLE |
| `android.hardware.opengles.aep` | c | see "OpenGL ES version" |
| `android.hardware.type.pc` | open question | Gesher presents a phone (DeviceProfile, `touchscreen`, Pixel build properties). `type.pc` makes apps and Play treat it as a ChromeOS-style PC (keyboard and mouse first, large-screen app catalog). It is a product decision about the presented form factor, not a hardware fact. Not claimed. |
| `android.hardware.type.automotive`, `.television`, `.watch`, `.embedded`, `android.software.leanback`, `.leanback_only`, `.live_tv` | c | other form factors |
| `android.software.vr.mode`, `android.hardware.vr.*` | c | no VR |
| `android.hardware.sensor.hifi_sensors`, `.sensor.barometer`, `.proximity`, `.stepcounter`, `.stepdetector`, `.heartrate*`, `.ambient_temperature`, `.relative_humidity`, `.hinge_angle` | c | SensorManager does not list them (Windows.Devices.Sensors has a barometer and proximity sensor that are not wired); hifi_sensors also needs CDD 7.3.9's rate and latency limits |
| `android.hardware.wifi.direct`, `.wifi.aware`, `.wifi.rtt`, `.wifi.passpoint` | c | Wi-Fi Direct over `Windows.Devices.WiFiDirect`; the others have no Windows equivalent |
| `android.hardware.telephony.calling`, `.messaging`, `.ims`, `.euicc*`, `.cdma`, `.mbms`, `.carrierlock`, `.satellite` | c | no voice path or SMS through the modem yet (docs/telephony.md follow-ups) |

## Validation

- `tests/dex-services/gesher/services/PackageServiceQueriesTest.java`: the
  production list omits App Widgets and matches this process's probes (WebView2
  Runtime, hardware summary, low-RAM), has exactly one OpenGL ES entry carrying
  `ro.opengles.version`, which is the default display's version
  (`WindowsHost.glEsVersion`; 2.0 under `GESHER_EGL_DEVICE=warp`), and builds a
  `SystemFeaturesCache`. It also runs the bounded probe with an answer, with a
  probe that hangs (-2, no version, after the timeout, then its late answer
  reaching the late handler) and with a LinkageError, and `lateGlEsVersion`
  (declared after a timeout, equal to the property where the boot already
  declared one; a known version kept). Where the boot probe ran, it must have
  answered within its bound (fields `sGlProbeTimedOut`, `sGlProbeMillis`). The helpers claim
  nothing for unknown or absent hardware, and the unimplemented features above
  are absent.
- `tests/native/egl_gles_version_test.cc` (in `tools/test_hwui_renderer_policy.py`):
  the probe (hardware ES 3.0 → 0x30000; capped WARP → 0x20000; a software or
  unidentified display offering ES 3.0 refused), the processes of a boot that
  declared 3.0 (the GPU, else uncapped WARP) or 2.0 (capped GPU or WARP), the
  context versions allowed (no ES 3.1 on a 3.0 display), and over all 240
  combinations that a kept display gives exactly the declaration. A
  negative-control build (commit 7d0bd340d's policy, which ignores the
  declaration) must fail. `tests/native/gpu_keepalive_test.cc` (CI track 40)
  checks real ANGLE: the version equals what the configs give; on a software
  adapter no config with the ES 3 bit, an ES 3 context refused with
  `EGL_BAD_MATCH`, `GL_VERSION` "OpenGL ES 2.0"; with ES 3 configs an ES 3.0
  context and an ES 3.1 one refused; and, with `--declared` (a property session
  of its own), a boot that declared 3.0 under `GESHER_EGL_DEVICE=warp` gets
  uncapped WARP with ES 3.0, one that declared 2.0 on a GPU gets the GPU capped
  with ES 3 refused. Where the default display has no ES 3 (the CI runners),
  `libglapp.so` (`gl_window_test`), `surface_control_test`,
  `gl_extension_procs_test` and `ImageReaderEglTest` check the refusal and run
  their ES 3 paths on ANGLE's own uncapped WARP display, reached through
  `eglGetPlatformDisplay(EXT)`'s pass-through (test-only); track 40 decides
  which from its own DXGI enumeration of the adapter ANGLE's hardware device
  takes, not from the tested output. The ES 3.1 refusal is checked through
  each app-facing `eglCreateContext`: an NDK `.so`'s (`libglapp.so` on an
  ES 3.0 default display: a GPU, or `gl_window_test --declared 0x30000` under
  `GESHER_EGL_DEVICE=warp`, which track 40 runs on the runners) and the Java
  bindings' (`ImageReaderEglTest` with EGL14 where the default display gives
  ES 3.0, and in track 31 with `declared-es3`: a `build.prop` with
  `ro.opengles.version=196608` under `GESHER_EGL_DEVICE=warp`, so the runners
  check it on WARP uncapped). The `--declared` runs of `gl_window_test` and
  `gpu_keepalive_test` run the test again as a child process in a property
  session and data directory of their own (`tests/native/declared_boot.h`),
  which the parent removes; `gl_window_test --declared` fails unless the
  default display gives exactly the declared version.
- `tools/test_camera_selection_and_orientation.py`: `camera.front` for a
  camera not configured as rear, `camera` for a configured rear one, both for
  a rear and a front camera together; never `camera.external`.
- `PackageServiceQueriesTest` compares the sensor features with
  `WindowsSensors.types()`, the plain run without natives (nothing listed,
  nothing claimed). Through `ServiceFixtureRunner` (the runtime natives) the
  framework-fixes track runs it three times: with `GESHER_SENSOR_TEST=1` the test
  accelerometer and light sensor are `hasSystemFeature` and
  `getSystemAvailableFeatures` answers; with the defaults on the runners (no
  Windows motion sensor) SensorManager's native list holds the stationary
  accelerometer and `android.hardware.sensor.accelerometer` is not declared; with
  `GESHER_SENSOR_FALLBACK=0` the list has no stationary accelerometer and no
  sensor feature is declared.
- CI runs `PackageServiceQueriesTest` without `libandroid_runtime`. Without
  the property natives, the RAM features and `ro.opengles.version` are absent
  (`GL_ES_VERSION_UNDEFINED`), and the test expects exactly that. Through
  `ServiceFixtureRunner`, which has the natives, it expects the real values.
  Locally (ART, installed boot jars, this branch's gesher-app.jar and
  dalvikvm), it ran with GLES 0x30000 on an Intel UHD Graphics 770 (8086:4680)
  and 0x20000 with `GESHER_EGL_DEVICE=warp`, RAM normal, and webview 158.0.4370.33 when the WebView2
  bridge sat next to gesher-app.jar. `devsvc_hardware.cc`'s probes on the
  development machine report no touch digitizer, no audio render endpoint and
  one wired Ethernet interface (the physical I219-LM). Hyper-V virtual, WAN
  miniport and kernel-debug adapters are excluded.
- Limits: the development machine has no touch digitizer, camera, audio
  endpoint, Wi-Fi adapter, cellular modem or sensors. Multitouch on a real touchscreen, a real webcam's
  `camera.front`, an unplugged audio jack counting as an output, and a
  hardware GPU below feature level 10_1 declaring ES 2.0 on the GPU were not observed.

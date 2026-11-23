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
  `WindowsWifi.hardware()` (the WLAN census, docs/wifi.md) and
  `TelephonyService.bootSnapshot()` (the cellular modem, docs/telephony.md).
  Without the runtime natives (a host JVM) a probe reports nothing.

## OpenGL ES version

The declared version (`ro.opengles.version`, `reqGlEsVersion`) follows the
device Android's default EGL display runs on, by speed: **OpenGL ES 3.0
(0x30000) only when ANGLE runs on a hardware Direct3D 11 device that gives
ES 3.0**. On WARP, Direct3D's software rasterizer, the device declares ES 2.0
(0x20000), as an Android device whose GPU does not support ES 3.0 does.

All of Android's EGL paths (an app's `.so`, `android.opengl.EGL14`/`GLSurfaceView`,
hwui) use one default display, `gesher_egl_default_display()`
(`compat/bionic/egl_gles.cc`): ANGLE on Direct3D 11. ANGLE at the pinned
commit (ab6a82e0, `renderer11_utils.cpp` `GetMaximumClientVersion`) gives
ES 3.0 from feature level 10_1, ES 3.1 from 11_0, and ES 2.0 below 10_1. The
display and the version are chosen together (`compat/bionic/egl_gles_version.h`):

| ANGLE's device | Default display | Declared |
| --- | --- | --- |
| hardware device on a GPU with ES 3.0 configs (feature level 10_1+) | that device | 0x30000 |
| hardware device on a GPU without them (Direct3D 10.0 hardware, such as GeForce 8/9, GMA X4500) | that device, ES 2.0 on the GPU | 0x20000 |
| hardware device on the Microsoft Basic Render Driver (no display driver; the CI runners) | that device, which is WARP | 0x20000 |
| WARP (the hardware device failed or had no ES configs, or `GESHER_EGL_DEVICE=warp`) | WARP | 0x20000 |
| none (no ANGLE DLLs, or no Direct3D 11 device at all) | `EGL_NO_DISPLAY` | none (`GL_ES_VERSION_UNDEFINED`) |

"Hardware" is decided from the display's own Direct3D 11 device
(`EGL_EXT_device_query`, `EGL_D3D11_DEVICE_ANGLE`, then the DXGI adapter's
`GetDesc1`): an adapter flagged `DXGI_ADAPTER_FLAG_SOFTWARE`, the Basic Render
Driver (vendor 0x1414, device 0x8c), a device opened as WARP, or an adapter the
query cannot identify is not hardware. ANGLE's "hardware" device type alone is
not evidence: without a display driver it runs on the Basic Render Driver.

The system server asks once at boot (`PackageManagerService.glEsVersion`,
`WindowsHost.glEsVersion`, `gesher_egl_gles_version`): it makes the same choice
an app process makes, sets `ro.opengles.version` for every process of that
boot, and terminates the display again, so it keeps no Direct3D device. That
costs one ANGLE initialization (one or two Direct3D 11 devices) at boot. A
value already in the property (an explicit `build.prop` line) is kept, as on
a device.

WARP itself still creates ES 3.0 contexts for an app that asks: the declared
version is what hardware acceleration gives, not everything the software
rasterizer can do. A GPU below feature level 10_1 now runs apps' GL as ES 2.0
on the GPU, where before it ran on WARP as ES 3.0. Without the ANGLE DLLs, or
without any Direct3D 11 device, there is no GLES and no version is declared.

Not reported:

- **ES 3.1 / 3.2.** ANGLE's `Renderer11::getMaxConformantESVersion` caps at 3.0
  ("3.1 support is in progress"), and Gesher has not verified ES 3.1 on a real
  GPU (docs/angle.md).
- **`android.hardware.opengles.aep`.** AEP needs ES 3.1 plus geometry and
  tessellation shaders and the other AEP extensions, which ANGLE's Direct3D 11
  backend does not provide.

## Reported

| Feature | When | What backs it |
| --- | --- | --- |
| `android.software.app_widgets` | always | AppWidgetManager service |
| `android.hardware.touchscreen`, `android.hardware.faketouch` | always | phone-sized window, mouse as touch (DeviceProfile); earlier decision |
| `android.hardware.screen.portrait`, `.landscape` | always | windows rotate (docs/windows3.md) |
| `android.hardware.touchscreen.multitouch`, `.multitouch.distinct` | Windows touch digitizer with ≥ 2 contacts (`SM_DIGITIZER` ready with integrated/external touch, `SM_MAXIMUMTOUCHES`) | WM_POINTER contacts become separate MotionEvent pointer ids (`compat/input/translate.cc`, up to 16) |
| `android.hardware.touchscreen.multitouch.jazzhand` | the same, ≥ 5 contacts | as above |
| `android.hardware.audio.output` | an audio render endpoint that is active or an unplugged jack (MMDevice `eRender`, `DEVICE_STATE_ACTIVE \| DEVICE_STATE_UNPLUGGED`): CDD 7.8.2 requires the feature for a speaker or an audio output port, and forbids it without one | AudioTrack/AAudio/OpenSL to the WASAPI sink (docs/media.md). While there is no default endpoint (a jack with nothing in it) the engine plays to a null sink and retries every 5 s, so output starts when something is plugged in. Without any render endpoint (disabled or none) no output is claimed, and the audio APIs keep working as no-ops (CDD 7.8.2 C-2-2) |
| `android.hardware.microphone` | an audio capture device | AudioRecord |
| `android.hardware.ethernet` | a hardware wired Ethernet interface (`GetIfTable2`: `IF_TYPE_ETHERNET_CSMACD`, `NdisPhysicalMedium802_3`, `HardwareInterface`), connected or not | ConnectivityManager's `TRANSPORT_ETHERNET` networks (docs/network.md) |
| `android.hardware.location`, `.location.network` | always | LocationManagerService, Windows location with IP fallback |
| `android.hardware.bluetooth`, `.bluetooth_le` | Windows radio / LE support | docs/bluetooth.md |
| `android.hardware.camera.any` | a Windows camera | CameraService (docs/devices.md) |
| `android.hardware.camera` | a camera explicitly configured as rear-mounted | as above |
| `android.hardware.camera.external` | a camera without a configured mounting | Camera2 reports it `LENS_FACING_EXTERNAL`, hardware level EXTERNAL, as Android's USB cameras, which Android declares with this feature |
| `android.hardware.ram.normal` / `.ram.low` | whichever `ActivityManager.isLowRamDevice()` says (`ro.config.low_ram` is unset: normal) | as AOSP's SystemConfig |
| `android.software.webview` | the WebView2 Runtime is installed | `android.webkit.WebView` on WebView2 (docs/webview.md); without the runtime WebViews stay empty, so it is not claimed |
| `android.hardware.sensor.accelerometer`, `.gyroscope`, `.compass`, `.light` | the matching sensor in the list SensorManager gives apps (Windows.Devices.Sensors' default sensors) | SensorManager over Windows sensors (docs/devices.md) |
| `android.hardware.wifi` | a WLAN interface in a `WlanEnumInterfaces` census at boot (waited for at most 5 s; a hung WLAN service means no feature) | WifiManager over wlanapi (docs/wifi.md); no Wi-Fi Direct, Aware, RTT or Passpoint |
| `android.hardware.telephony`, `.telephony.subscription`, `.telephony.radio_access`, `.telephony.data`, `.telephony.gsm` (3GPP only) | a cellular modem the Mobile Broadband API reports at boot | read-only, data-only telephony services (docs/telephony.md) |

## Not reported

Partial (b): some of the API or hardware exists, but not what the feature
promises. Absent (c): nothing implements it.

| Feature | Status | What would be needed |
| --- | --- | --- |
| `android.hardware.audio.low_latency` | b | Proof of the CDD's latency limits (≤ 45 ms cold output, continuous output ≤ 45 ms, round trip) with the WASAPI shared-mode mixer; measurement on real hardware |
| `android.hardware.audio.pro` | c | low_latency, plus ≤ 20 ms round trip, USB host audio and MIDI |
| `android.hardware.audio.spatial.headtracking.low_latency` | c | a spatializer with head tracking |
| `android.hardware.location.gps` | b | GPS_PROVIDER exists over Windows location, which can use a GNSS receiver, but there is no GnssStatus, NMEA, measurements or `GnssCapabilities`, and no detection of a GNSS device. Needs a GNSS device check (GUID_DEVINTERFACE_GNSS / Sensor API) and the GNSS status callbacks |
| `android.hardware.camera.front` | b | a truthful front-mounting configuration (CameraSelection only offers rear), with sensor orientation and facing reported to match |
| `android.hardware.camera.autofocus`, `.flash`, `.capability.*`, `.level.full`, `.concurrent`, `.ar` | c | the camera is fixed-focus, EXTERNAL level, no flash; needs Media Foundation focus/torch controls and the matching Camera2 metadata |
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
  production list matches this process's probes (WebView2 Runtime, hardware
  summary, low-RAM), has exactly one OpenGL ES entry carrying
  `ro.opengles.version`, which is the default display's version
  (`WindowsHost.glEsVersion`; 2.0 under `GESHER_EGL_DEVICE=warp`), and builds a
  `SystemFeaturesCache`. The helpers claim
  nothing for unknown or absent hardware, and the unimplemented features above
  are absent.
- `tests/native/egl_gles_version_test.cc` (in `tools/test_hwui_renderer_policy.py`):
  the version policy, hardware ES 3.0 → 0x30000, WARP, the Basic Render Driver,
  software-flagged or unidentified adapters → 0x20000, `EGL_NO_DISPLAY` → none,
  and which device the default display keeps; a negative-control build (ES 3.0
  whenever ES 3.0 configs exist) must fail. `tests/native/gpu_keepalive_test.cc`
  checks the real ANGLE answer against the display's adapter (CI: Basic Render
  Driver, 0x20000).
- `tools/test_camera_selection_and_orientation.py`: `camera.external` for an
  unconfigured camera, not for a configured rear one.
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
  `camera.external`, an unplugged audio jack counting as an output, and a
  hardware GPU below feature level 10_1 declaring ES 2.0 on the GPU were not observed.

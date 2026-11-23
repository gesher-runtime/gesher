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
  gives the same value. Both read `ro.opengles.version` (`app/build.prop`).
- The Windows probes are `WindowsHost.hardwareSummary()`
  (`compat/devsvc/devsvc_hardware.cc`), `WebView2Host.runtimeVersion()`,
  `WindowsCamera.devices()`, `WindowsBluetooth.adapter()`, the audio engine's
  input devices and `ActivityManager.isLowRamDeviceStatic()`. Without the
  runtime natives (a host JVM) a probe reports nothing.

## OpenGL ES version

`ro.opengles.version=196608` (0x30000, OpenGL ES 3.0). Before this change the
property was unset, and `getDeviceConfigurationInfo` reported 0x20000. All of
Android's EGL paths (an app's `.so`, `android.opengl.EGL14`/`GLSurfaceView`,
hwui) use one default display, `gesher_egl_default_display()`
(`compat/bionic/egl_gles.cc`). That display is ANGLE on Direct3D 11. ANGLE at
the pinned commit (ab6a82e0, `renderer11_utils.cpp` `GetMaximumClientVersion`)
gives ES 3.0 from feature level 10_1, ES 3.1 from 11_0, and ES 2.0 below 10_1.
The default display now uses the hardware adapter only when it has an
`EGL_OPENGL_ES3_BIT_KHR` config. Otherwise it uses WARP, which gives ES 3.0.
So wherever ANGLE runs, ES 3.0 contexts can be created. CI's ANGLE probes
create them on hardware and WARP (docs/angle.md).

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
| `android.hardware.audio.output` | an active audio render endpoint (MMDevice `eRender`, `DEVICE_STATE_ACTIVE`) | AudioTrack/AAudio/OpenSL to the WASAPI sink (docs/media.md); without an endpoint the engine has a null sink, so no output is claimed |
| `android.hardware.microphone` | an audio capture device | AudioRecord |
| `android.hardware.ethernet` | a hardware wired Ethernet interface (`GetIfTable2`: `IF_TYPE_ETHERNET_CSMACD`, `NdisPhysicalMedium802_3`, `HardwareInterface`), connected or not | ConnectivityManager's `TRANSPORT_ETHERNET` networks (docs/network.md) |
| `android.hardware.location`, `.location.network` | always | LocationManagerService, Windows location with IP fallback |
| `android.hardware.bluetooth`, `.bluetooth_le` | Windows radio / LE support | docs/bluetooth.md |
| `android.hardware.camera.any` | a Windows camera | CameraService (docs/devices.md) |
| `android.hardware.camera` | a camera explicitly configured as rear-mounted | as above |
| `android.hardware.camera.external` | a camera without a configured mounting | Camera2 reports it `LENS_FACING_EXTERNAL`, hardware level EXTERNAL, as Android's USB cameras, which Android declares with this feature |
| `android.hardware.ram.normal` / `.ram.low` | whichever `ActivityManager.isLowRamDevice()` says (`ro.config.low_ram` is unset: normal) | as AOSP's SystemConfig |
| `android.software.webview` | the WebView2 Runtime is installed | `android.webkit.WebView` on WebView2 (docs/webview.md); without the runtime WebViews stay empty, so it is not claimed |
| sensors (`android.hardware.sensor.*`) | the sensors change (branch feat/hardware-features) | SensorManager over Windows sensors |
| Wi-Fi, telephony | their own changes | |

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
| `android.hardware.sensor.hifi_sensors` | sensors change | |
| `android.hardware.wifi*`, `android.hardware.telephony*` | other changes | |

## Validation

- `tests/dex-services/gesher/services/PackageServiceQueriesTest.java`: the
  production list matches this process's probes (WebView2 Runtime, hardware
  summary, low-RAM), has exactly one OpenGL ES entry carrying
  `ro.opengles.version`, and builds a `SystemFeaturesCache`. The helpers claim
  nothing for unknown or absent hardware, and the unimplemented features above
  are absent.
- `tools/test_camera_selection_and_orientation.py`: `camera.external` for an
  unconfigured camera, not for a configured rear one.
- CI runs `PackageServiceQueriesTest` without `libandroid_runtime`. Without
  the property natives, the RAM features and `ro.opengles.version` are absent
  (`GL_ES_VERSION_UNDEFINED`), and the test expects exactly that. Through
  `ServiceFixtureRunner`, which has the natives, it expects the real values.
  Locally (ART, installed boot jars, this branch's gesher-app.jar), it ran
  with GLES 0x30000, RAM normal, and webview 158.0.4370.33 when the WebView2
  bridge sat next to gesher-app.jar. `devsvc_hardware.cc`'s probes on the
  development machine report no touch digitizer, no audio render endpoint and
  one wired Ethernet interface (the physical I219-LM). Hyper-V virtual, WAN
  miniport and kernel-debug adapters are excluded.
- Limits: the development machine has no touch digitizer, camera, audio
  endpoint, Wi-Fi or sensors. Multitouch on a real touchscreen, a real webcam's
  `camera.external` and a hardware GPU below feature level 10_1 falling back to
  WARP were not observed.

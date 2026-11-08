# Audio capability queries must return valid empty values

Baseline: `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63`. `Services.create`
wraps `AudioService` directly with the generated `IAudioServiceStub`; it does
not use `StandIn`'s empty-list fallback. Unimplemented collection methods
therefore returned the AIDL Default's `null`.

Two failures were reproduced against the actual compiled Android framework
classes, before changing the service:

- `AudioManager.getAvailableCommunicationDevices()` throws
  `NullPointerException` at `AudioManager.java:9293` when it iterates the
  result of `getAvailableCommunicationDeviceIds()`.
- `Spatializer.getSupportedHeadTrackingModes()` throws
  `NullPointerException` at `Spatializer.java:728` when it iterates the
  result of `getSupportedHeadTrackingModes()`.

The same service also returned null from public non-null list queries for
recording configurations, attribute routes and spatializer capabilities.
The real wrapper's `getAudioDevicesForAttributes()` iterates its service
result outside its exception fallback, so null is unsafe there too.
An absent feature does not excuse these invalid return shapes.

## Fix and contract references

The service now returns empty communication-device IDs, routing and recording
configuration lists, supported head-tracking modes, spatializer devices and
channel masks. Actual and desired tracking modes report
`HEAD_TRACKING_MODE_UNSUPPORTED`, rather than Default's zero (the valid but
inapplicable `HEAD_TRACKING_MODE_OTHER`). No devices or supported operations
are invented. Windows playback/capture behavior is unchanged.

References:

- [AudioManager: communication-device queries](https://developer.android.com/reference/android/media/AudioManager#getAvailableCommunicationDevices())
  specifies a non-null result; `getCommunicationDevice()` may be null on
  platforms without telephony.
- [AudioManager: recording queries](https://developer.android.com/reference/android/media/AudioManager#getActiveRecordingConfigurations())
  specifies a non-null list.
- [Pinned Android 16 r4 Spatializer source](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/Spatializer.java)
  defines the unsupported mode and empty-list behavior for unsupported
  head tracking. The inspected source and framework bytecode agree on
  the unconditional array iteration.

## Verification boundaries

`ManagerConstructionBatchTest` now exercises the actual managers created by
`ContextImpl`, with services supplied by `Services.create`. It queries the
communication-device list, attribute routes, recording configurations and
spatializer values and checks consistent unavailable-capability answers.
The existing Windows services test track already runs this fixture.

The changed service and fixture compile against the verified Android 16
framework/core artifact used for the [NSD check](nsd-contracts-2026-11-08.md).
A local host-JVM diagnostic reproduced the two baseline wrapper NPEs and
null recording list using those real classes. After the fix, real Spatializer
and AudioManager recording queries pass, and the communication array is
non-null and empty at the service boundary. That diagnostic bypasses
Windows initialization using object allocation without constructors; it
does not replace the Windows fixture. The successful full communication
manager path logs through Android JNI, so its end-to-end check belongs to
the Windows runtime test.

Android communication-route selection, product-strategy routing,
spatialization and a recording-session inventory remain unimplemented.
An empty recording list currently describes the absent inventory, not proof
that Windows has no active microphone stream. Routing setters, hardware
behavior and broader audio callbacks require separate work.

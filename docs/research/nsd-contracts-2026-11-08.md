# DNS-SD API and callback compatibility, 2026-11-08

Source baseline: `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63`.

`NsdManager` was present, but three overloads declared by the pinned Android
Connectivity module were missing:

- `discoverServices(DiscoveryRequest, Executor, DiscoveryListener)`
- `discoverServices(String, int, NetworkRequest, Executor, DiscoveryListener)`
- `registerService(AdvertisingRequest, Executor, RegistrationListener)`

An APK using one of those descriptors could fail with `NoSuchMethodError`
before reaching Gesher's existing unsupported-backend behavior. The value
classes alone did not supply the manager methods. The baseline class fails
the new regression's first descriptor lookup with `NoSuchMethodException`.

The source reference is AOSP
[`NsdManager.java`, Android 16 r4](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework-t/src/android/net/nsd/NsdManager.java).
The downloaded source's SHA-256 is
`b5dc6fbd3a009bdbe15ae42a83a605dbeace3211b7c7ed39b34c6b798f64264b`.
Its `ServiceHandler` dispatches discovery start and stop through the registered
executor. Its listener map compares listener identities. The local implementation
previously forgot the executor on stop and used listener `equals`/`hashCode`.

## Changes

The missing methods now use the same existing backend policy as the older
overloads. Discovery produces an empty session; registration fails through
the supplied listener with `FAILURE_INTERNAL_ERROR`. The NetworkRequest
overload does not start unnecessary Windows network watchers when there is
no mDNS backend to query.

Each discovery retains its executor and is keyed by listener identity. It
remains reserved until the terminal callback is delivered; that callback
releases the listener before calling application code, allowing reentrant
reuse. A previously queued start that runs after its request has completed
is discarded. Rejection of the initial executor submission removes that
request; rejection of a stop submission leaves it available for a retry.
Application callback exceptions are not swallowed.

## Validation and limits

`NsdContractTest` exercises the production manager and value classes with
controlled executors. Its 37 assertions cover the restored descriptors,
caller/default executor selection, listener identity, duplicate and pending
requests, reuse inside a stop callback, reversed executor queues, rejected
submissions, invalid inputs and the unsupported-advertisement result. The
existing `FrameworkFixesTest` now also requires the stop callback on its
explicit direct executor.

The production sources and test compiled with JDK 21 against the unmodified
`framework-classes.zip` and `coremod.zip` from CI run
[42425141954](https://github.com/gesher-runtime/gesher/actions/runs/42425141954),
artifact `13595267948` (archive SHA-256
`e27b851b2cd2ac52481c341ea102e6f959a10dc528373e7cec5a35ad6668ed79`).
All 37 assertions passed on the host JVM using those actual Android class
inputs; no replacement Android test stubs were used. The test is also
registered in `tests/windows/tracks/50-services.ps1` for execution against
the built Gesher boot classes. Host-JVM success is not Windows ART validation.

This change does not implement mDNS, DNS-SD network discovery, advertising,
resolution or service-info monitoring. Existing cleanup tolerance after a
failed registration/resolution remains. It does not claim complete DNS-SD
parameter validation, all future module APIs or Android certification.

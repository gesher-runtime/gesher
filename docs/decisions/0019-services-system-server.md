# 0019 — A system server of Gesher's own, with the framework's parser and AIDL stubs

**Status:** accepted, 2026-10-23 (services track)

## Context
With Binder across processes ([0015](0015-services-binder-ipc.md)), the
system services an app looks up can live in a process of their own, as on
Android. The real ones (services.jar: PackageManagerService,
ActivityManagerService, WindowManagerService...) need most of a device
(SELinux, installd, zygote, surfaceflinger, settings providers...) and are
out of reach. gesher-app's services were in-process stand-ins: `IFoo.Default`
subclasses behind a `java.lang.reflect.Proxy` attached to a plain `Binder`,
which only serves the same process, and the APK's manifest was read by a
small reader of Gesher's own.

## Decision
- **The package is parsed by the framework's own parser**: `PackageParser2`
  (`ParsingPackageUtils`) and `PackageInfoCommonUtils` from framework.jar,
  as PackageManagerService parses at install (`com.gesher.server.ApkPackage`),
  in both modes. No reader of our own, no fallback.
- **A system server of Gesher's own**, `com.gesher.server.SystemServer`, in
  gesher-app.jar on the boot class path: it parses the APKs it is given and
  registers `package`, `activity`, `activity_task`, `window`, `display` and
  `user` with servicemanager. The services stay stand-ins (Ravenwood's
  approach), now in `com.gesher.server`, shared with gesher-app's
  single-process mode.
- **Concrete AIDL stubs generated at build time** (`app/gen/StubGen.java`):
  for each interface, `IFooStub extends IFoo.Stub` delegating every abstract
  method to an `IFoo`, written by reflection over framework.jar's classes on
  the JDK. The stubs' AIDL-generated `onTransact` does the unmarshalling, so
  the stand-ins need no Parcel code and always match the framework they are
  built against; javac still checks each override against the interface.
- **The client side is the framework's**: `gesher-app --system-server` uses
  `ServiceManager` → servicemanager → the server, with no service cache of
  its own. One exception, `DisplayManagerGlobal` without
  `getOverlaySupport` (OverlayProperties' natives are missing); removed in
  0018, which gives those natives stand-ins.
- **Opt-in multi-process mode**: the single-process mode stays the default
  of `gesher-app.cmd` (and the `.apk` association); `--system-server` makes
  the launcher start binderd, servicemanager and the server for the run.

## Consequences
- An app's lifecycle was still driven by gesher-app (`GesherAppDriver`), not
  by the server; attaching the app (`attachApplication`, `bindApplication`,
  `ClientTransaction`s) followed in [0024](0024-services-app-process.md).
- Every service call of the app is a binder round trip through binderd in
  multi-process mode; lifecycle.apk launches in about the same time.
- Building gesher-app needs JDK 21 (framework.jar's class files) for the
  generator, as javac against framework.jar already did.

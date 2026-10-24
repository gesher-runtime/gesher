# 0046 — Gesher's own system server is the system services; services.jar stays out

**Status:** accepted, 2026-10-25 (coordinator)

## Context
The Phase 4 roadmap box "The framework's own system services (PackageManager
parses the APK, ActivityManager starts the Activity) in place of gesher-app's
stand-ins" was written before [0019](0019-services-system-server.md). Read
literally it asks for services.jar (PackageManagerService,
ActivityManagerService, WindowManagerService) to run on Windows. 0019 decided
against that: those services need most of a device (SELinux, installd,
zygote, surfaceflinger, the settings providers, vold, netd...), and porting
them would mean emulating that device rather than bridging to Windows. Every
sub-box under it (the framework's PackageParser2, a system server process on
servicemanager, the app attached and driven through `ClientTransaction`s, the
app model through the server, the persistent Gesher device) is done and
CI-verified, so the box stayed open only because of its wording.

## Decision
- The box is **closed by decision, not by porting services.jar**. Gesher's
  own server (`com.gesher.server`, 0019, 0024-0026) is the system services:
  it implements the framework's AIDL interfaces through stubs generated from
  framework.jar, the APK is parsed by the framework's own parser, and apps
  reach it only through the framework's client side (`ServiceManager`,
  `ActivityThread`, `ClientTransaction`).
- The roadmap box is reworded to say exactly that, and ticked on the sub-boxes'
  CI runs.
- services.jar is **not** a goal. A service an app needs is added to Gesher's
  server against the framework's interface (as the devsvc, net, media, ime and
  devices tracks do), reusing framework.jar's non-service helpers
  (PackageParser2, `ParsingPackageUtils`, `android.content.pm` classes) where
  they run as is.

## Consequences
- Behavior that lives only inside services.jar (e.g. its exact activity
  stack rules, permission policy, ANR handling) is reimplemented where apps
  observe it, and gaps are tracked as ordinary leftovers (e.g. pause vs stop
  under a translucent activity).
- framework.jar updates keep the server honest: the generated stubs are
  checked by javac against each interface, so an interface change fails the
  build instead of drifting.

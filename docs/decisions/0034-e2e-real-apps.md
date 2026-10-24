# 0034 — End-to-end tests with real apps, and a device without hardware

**Status:** accepted, 2026-10-24

## Context
Gesher's own test apps (lifecycle, components, uiapp, nativeapp, ...) exercise
what each track built, but they only use what their authors knew was there.
Real apps call whatever a phone has: system services the stand-ins did not
register, framework natives nobody had needed, mainline-module classes that
framework.jar compiles against but does not contain, PendingIntents, SQLite,
app-compat changes gated on the target SDK.

## Decision
- **Real, unmodified apps in CI.** `tests/e2e/apps.json` pins a handful of
  open-source apps from F-Droid's official repository by version code and
  SHA-256; the `e2e` job of `ci.yml` downloads them (only in CI), installs
  them with the installer the `package` job built, and drives each one in an
  off-desktop window (`tests/e2e/run-e2e.ps1`): first frame, not blank,
  input changes the window, closing ends the process. Apps that do not work
  yet carry a `knownFailure` reason and print `KNOWN-FAIL`; they do not turn
  the job red, and they are listed in [e2e.md](../e2e.md), never skipped.
- **A device that has the service but not the hardware.** Where
  `SystemServiceRegistry` fetches a service with `getServiceOrThrow`, a
  missing service makes `getSystemService` answer null, which apps do not
  expect for services every phone has. Gesher registers stand-ins that answer
  like a device without the hardware or the data behind them: no network
  (ConnectivityManager), no sensors (SensorManager's list is empty), no
  widgets on the launcher (AppWidgetManager), alarms accepted but not
  delivered, every app-op allowed, the screen on. Services that only some
  devices have stay absent (`getSystemService` answers null, as on such a
  device).
- **Mainline-module classes as placeholders** (as `TrafficStats` and
  `DeviceConfig` before): the public API of `ConnectivityManager` and its
  types, and `SdkExtensions` (every extension at 0), on the boot class path
  after framework.jar, so a real module jar wins when there is one.
- **App-compat changes by target SDK.** The system server computes
  bindApplication's `disabledCompatChanges` from the framework's `@ChangeId`
  fields (read from its sources at build time), as PlatformCompat does; until
  then every change was enabled for every app.

## Consequences
Each gap a real app hits becomes a small fix with a check (dbapp.apk for
SQLite, the e2e lines for the rest). The pinned apps make runs reproducible;
updating one is a manifest change with a new hash. Stand-ins answer "absent"
rather than throwing, so apps take their no-hardware paths; features that
need real behavior (alarms firing, network state, the Windows clipboard) are
listed as remaining gaps in [e2e.md](../e2e.md).

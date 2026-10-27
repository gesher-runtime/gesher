# 0066. Locale2: persisted supported-locale overrides and selection reconciliation

Status: proposed; source and CI fixtures prepared on `wip/locale2`, based on
accepted integration `7b49a718`. Compilation and runtime acceptance are pending.
Test 81 is reserved to this slice. No local compilation or ART/APK execution.

## Pinned contract

The baseline is `android-16.0.0_r4`, particularly
[`LocaleManagerService.java`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/locales/LocaleManagerService.java).
The inspected source SHA256 is
`2aa666ec5d650fef42258fac347cdf46cd69b2920c0836790d59deefd57342b6`.
The companion source hashes are recorded in the
[source audit](../handoff/tracks/locale2-source-audit.json).

- Lines 569–599: override SET checks incoming user and package ownership or
  `SET_APP_SPECIFIC_LOCALECONFIG`. Selected-locale SET at 231–265 instead
  permits ownership or `CHANGE_CONFIGURATION`; `fromDelegate` is not a grant.
- Lines 614–678: clearing an override reconciles against the original APK
  resource configuration. A non-null configuration with a null supported
  list is stored as an **explicit empty** override. A failed write returns
  before reconciliation. A reordered equivalent override is ignored before
  reconciliation, preserving the previously stored order.
- Lines 699–730: unsupported selected locales are removed using the actual
  `LocaleConfig.containsLocale` language/script comparison, preserving the
  order of retained entries. This is not complete-tag equality.
- Lines 751–788: override GET validates the incoming user and reads public
  supported configuration. It has no package-owner/READ permission gate;
  absence and an explicitly empty stored list have different results.
- Selected-locale GET at 405–438 is private: owner, installing package,
  foreground current IME, or `READ_APP_SPECIFIC_LOCALES` is required.

## Implementation boundary

The locale service keeps existing selected-locale Properties in
`system/app_locales` and adds supported overrides in
`system/app_locale_configs`. Absence of a package key means no override;
an empty value means a successful, empty override. Format choice is internal.
Writes use a locale-owned `.new` file, sync/close and existing `Os.rename`
replacement; a failure never deletes the last committed target. Memory is
updated only after successful replacement. This does not modify the broader
`Device.write` helper, whose delete-on-failed-rename fallback remains outside
this slice. Override and selected-locale files are separate commits, as the
pinned service commits supported configuration before updating selected
locales; no multi-file transaction or power-loss durability is claimed.

The original resource configuration is read through real framework
`AssetManager`, `Resources` and `LocaleConfig(ApplicationInfo, Resources)`.
It does not consult the override being cleared. The dynamic-locales property
gate uses the pinned property name and default.

Ordinary app identity is derived from Binder PID to the registered ATMS
process/package, or the exact PID/package of a single-process launch. User 0
is the supported user. `CURRENT_OR_SELF` can fall back to it; `CURRENT`, ALL
and another user need authority not supplied by this bounded implementation.
No system-PID or shared-host-UID privileged bypass is invented.

This does **not** implement Android's complete privileged permission model.
Concrete Services3 prerequisites remain: `ApkPackage.installed` assigns every
package `Process.myUid()` (current lines 415); PM `getPackagesForUid` returns
all packages regardless of its argument (258–259); AMS `checkPermission` and
`checkPermissionForDevice` grant every permission (368–374). Those answers
cannot authorize private locale reads or cross-package writes. Privileged
cross-package grants, installer identity and current-IME authority remain
open. The ordinary owner/negative controls are not evidence for those grants.

## Required evidence (pending)

Test 81 uses two actual APKs, the real `LocaleManager` and `ILocaleManager`
boundary, resources and configuration/default-locale results in single-process
and device modes on both Windows runners. Planned controls include:

- set/replacement/reordered equivalence, language/script matching and retained
  order; app and device restarts; actual French/Hebrew resources and callbacks;
- clear against original resources, explicit empty and non-null/null-list
  configurations, including persisted empty versus absent after restart;
- actual staging failure and a writable staging file held without DELETE
  sharing, so the real rename source cannot be opened for replacement;
  committed bytes, memory and selected locales must remain unchanged;
- null/unknown-package/user boundaries and a distinct registered package
  reading public supported config while private reads/delegated writes/clear
  are denied. This Java API slice makes no NDK ABI claim.

The fixture's `GESHER_LOCALES=en-US` is a controlled source, not evidence for
Windows display-language changes or sign-in behavior. No PASS is claimed yet.

## Still open

Permission-gated/installer `ACTION_APPLICATION_LOCALE_CHANGED` recipients,
install-source persistence, uninstall/retain-data state cleanup, real NLS
display-language/event behavior, regional formatting and native locale/time-zone
limits remain separate Locale2 work. `sendBroadcastFor`'s third argument is a
resolved MIME type, not a permission gate. No generic permission, broadcast,
installer, NLS or custom-format contract is closed here. The existing windows3
single-process relaunch implementation is not reimplemented or newly claimed.

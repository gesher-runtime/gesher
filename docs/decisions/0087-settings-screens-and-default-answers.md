# 0087: Android Settings screens, and stand-in answers that were wrong

Status: accepted (see docs/services.md "Settings screens")

## Context

Gesher has no Settings APK. Apps open Settings screens with implicit
`android.settings.*` intents, and on a phone something always handles them, so many apps
call `startActivity` without resolving first. File Manager+ (com.alphainventor.filemanager)
opens `APPLICATION_DETAILS_SETTINGS` (`package:<app>`) when an app in its list is tapped,
catches `ActivityNotFoundException`, and shows "Application not found" (Hebrew: "האפליקציה
לא נמצאה") for every app. The PackageManager lookups themselves worked. Only the two
notification screens had a Windows counterpart (ms-settings:notifications).

`tools/compatibility_audit.py` over the runtime logs also listed stand-in methods
answered by `IFoo.Default` (null, 0, false, nothing done) and one missing service:

- `storagestats`: getSystemService answered null ("No service published for:
  storagestats") for file managers and cleaners.
- `setComponentEnabledSetting` did nothing while `getComponentEnabledSetting` always
  answered DEFAULT, so an app that disabled a component read it back as enabled, and the
  component still resolved.
- `getMemoryInfo` left every field 0 (totalMem 0, threshold 0); `getMyMemoryState`
  left importance 0, which no RunningAppProcessInfo constant names.
- `getSimStateForSlotIndex` answered 0, SIM_STATE_UNKNOWN ("still loading"), not
  ABSENT; `getProfileType` answered null where UserManagerService answers "".
- `queryIntentContentProviders` answered nothing even for an explicit component.

## Decision

1. **Per-app Settings screens open Gesher's Control Center on that app.**
   `SettingsIntents` maps an implicit intent (no component; no package or
   `com.android.settings`) for App info and the per-app permission screens (all-files
   access, unknown sources, battery optimization, overlay, write settings, exact alarms,
   app locale, open by default...) whose `package:` data or APP_PACKAGE extra names an
   installed package to `ControlCenter.open(package)`. The system server writes the
   package and optional visit token to one immutable record in
   `data/system/control-panel-requests` and opens the Control Center through a
   shortcut it keeps (`data/system/gesher-control-panel.lnk` to `gesher-launch.exe
   control-panel`): `WindowsHost.openUrl` (ShellExecute) takes no arguments, and a
   shortcut needs no new native code. The Control Center shows the app's details dialog
   (install and update times, uninstall; Close is the default button, since another app
   raised it). There is one Control Center: a second start (an App info, or the Start menu
   again) signals the open one through a named event and exits; the open one comes to the
   front and takes the request on its live-refresh timer. The named mutex is never owned
   and is released as the window closes; "Reopen after update" passes
   GESHER_PANEL_REOPEN to start the new version while the old one is still open.
2. **List screens open the Control Center itself** (app lists, sync and accounts,
   Settings' main page); **device screens open a fixed `ms-settings:` page** (Wi-Fi,
   Bluetooth, location, date, language, display, sound, storage, battery...). The intent's
   data and extras never reach Windows; only a package name that PackageParser would
   accept, and that is installed, reaches the request file.
3. **Anything else stays unresolved**, as before: an app that is not installed (Settings'
   own "not found"), screens with no Windows counterpart (NFC), explicit components.
4. **A `startActivityForResult` on these screens gets RESULT_CANCELED at once**: Windows
   reports nothing back, and Settings' screens answer CANCELED on a phone too; apps
   re-check the permission state themselves.
5. **The wrong stand-in answers are implemented**: StorageStatsService (sizes measured on
   the host files, one budget of 200,000 entries per query; no quotas; an app may ask about
   itself, anything else needs PACKAGE_USAGE_STATS, which on Gesher means the app declares
   it, since there is no "Usage access" screen to grant the app op), component enabled
   states (kept for the device's lifetime, dropped with the package, applied to queries and
   to get*Info unless MATCH_DISABLED_COMPONENTS; DISABLED_USER and DISABLED_UNTIL_USED are
   ignored for components, and an unknown class reads as DEFAULT, as on Android), memory info from
   Gesher's /proc/meminfo, the calling process's importance, SIM_STATE_ABSENT, "" for the
   profile type, provider intent queries. Methods whose Default answer is already right
   (listener registrations with nothing to report, no mainline modules, no IME tracker)
   are implemented explicitly so the stand-in log lists only real gaps.

## Consequences

- Any app's "App info" and per-app permission links work; File Manager+'s app list opens
  the Control Center on the tapped app.
- An app can open the Control Center and bring it to the front; it cannot pass anything
  but an installed package name to it.
- `queryIntentActivities` still finds no Settings activity, so an app that resolves before
  starting (`resolveActivity() != null`) still shows its own fallback. Faking a Settings
  ResolveInfo would affect every chooser; left out.
- Component states are not persisted across device restarts; apps that toggle components
  normally set them again as they start. No PACKAGE_CHANGED broadcast is sent, and the
  Start menu shortcut of a disabled launcher activity stays.
- `IInputManager.setPointerIcon` (cursor shapes) and
  `IActivityManager.getHistoricalProcessExitReasons` still answer Default and remain in
  the stand-in log: they need Windows cursor mapping and an exit-history store.

## Addendum: special app access switches (issues #310, #312)

The per-app permission screens reached the app's details, but the details had no switch,
so apps rechecked on resume, found their requested access denied and asked again.
The details now show the implemented Android Settings special accesses the app's manifest
requests: Display over other apps, Install unknown apps, Modify system settings and Usage
access (`SpecialAccess`; docs/services.md "Special app access"). They write the
app-op modes AOSP Settings writes, through `AppOpsService`, only on the user's click, over
a `gesher` service transaction restricted to the host shell. Every switch starts off.
`canRequestPackageInstalls` now follows `OP_REQUEST_INSTALL_PACKAGES` as AOSP does (it
answered true for every requesting app before). An eligible app's validated install session
returns a pending Android user action. Launching it takes the installer through its
Install unknown apps choice if required and asks for consent to the selected APK on
Windows. The server rechecks the source at package publication. An INSTALL_PACKAGES
holder is allowed to request installation, and its switch is shown fixed on.

All files access and Media management remain unavailable. The initial implementation
offered those switches, but the raw shared-storage and media consent operation paths did
not enforce their decisions. The review therefore removed both from the displayed and
writable allowlists: a grant must change the operation it advertises. A production
shared-storage authorization backend and MediaProvider are prerequisites, including
ordinary-app tests for grant and revocation. The MediaStore facade's SDK-call tests use a
controlled provider; they do not supply that backend. This change does not resolve the
File Manager X all-files request in #310 or establish which permission blocks #312.

Item 4 changes for per-app details. The system creates an opaque visit token for a
request from an activity and pauses the covered activity when the Control Center launch
succeeds. On close, it restores the eligible activity or its replacement process. This
lets an ordinary `startActivity` caller recheck special access in `onResume`. A result
requested with `startActivityForResult` is delivered before resume to its original live
recipient. Failed starts leave the requesting activity in its existing lifecycle state.

The host acknowledges a visible visit with its process ID and Windows start time. It
reports the tokens it is displaying with each switch turn and serializes the close
notification after outstanding changes finish. The Install unknown apps screen answers
RESULT_OK only after the user turned its switch on during that visit. A later turn off,
closing without a turn, or another screen answers RESULT_CANCELED. The result is independent
of whether the same package has another visit waiting to be displayed. If the Control
Center exits, the device cancels that exact host's pending visits. Unshown visits time out
after 15 minutes. New requests do not replace or cancel earlier visits. At capacity,
new requests are refused so a
visible visit can continue until the user closes it. The Windows settings pages and
Control Center's main view still answer RESULT_CANCELED at once.

Each request atomically publishes its package and optional token in the same immutable
file. Ordered, unique filenames preserve queued requests for other packages. The panel
claims a record by renaming that exact file, so a concurrent publication cannot change
the selected request or transfer another caller's result to the current dialog. Requests
for the displayed package can join that visit; other packages wait until it closes.
The queue holds at most 32 waiting records and the panel claims at most 32 per dispatcher
tick, including when a producer continually refills it. Failed host launches remove only
their own record. Timeout and device-start cleanup retire unclaimed requests.

The updated panel also imports the older server's single request file and separate
result files. A new server requires the updated panel from the same installation;
after an update, use the normal panel reopening and device restart. The old shown-package
marker serves only the legacy reader and is not an ownership decision for new requests.

Item 5 changes too: StorageStatsService decides usage access as AOSP's checkStatsPermission
does, from `OP_GET_USAGE_STATS` (MODE_ALLOWED allows, MODE_DEFAULT falls back to holding
PACKAGE_USAGE_STATS, other modes deny), so the Usage access switch governs storage stats as
it governs usage stats; a declared permission alone no longer gives access. Hosting the
service in an ordinary standalone app's process does not bypass this decision.

Package UIDs survive replacement and reinstallation, so special-access decisions are bound
to the caller's installation. StorageStatsService usage access and SettingsProvider writes
(the Modify system settings op and the privileged WRITE_SECURE_SETTINGS path) answer only an
attached live process whose recorded installation is still current, and check that again
after reading the decision; the usage check repeats after a scan. A process retained from an
earlier installation therefore never uses a grant made to its replacement. Own-UID storage
queries, current shared-UID peers and system or shell callers are unchanged.

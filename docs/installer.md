# The Windows installer

For rebuilding from a defined source, see [Build and package](build-and-package.md).

`tools/package/gesher.iss` (Inno Setup 6) is built and tested by the
`package` job in `.github/workflows/ci.yml`. It installs per user, without
administrator rights, into `%LOCALAPPDATA%\Programs\Gesher`, puts `gesher`
and `gesher-app` on the user's `PATH`, adds Start menu entries (including the
Gesher Control Center), offers optional 1-click Google Play services (microG)
and Aurora Store installation, and registers Explorer associations for `.apk`,
`.apks`, `.xapk`, `.apkm` and `.apk+`. If another program owns an extension,
Gesher is available in **Open with**. Apps installed with gesher-app get their
own [Start menu shortcuts](#installed-apps-in-the-start-menu).

Gesher's icon, `tools/package/gesher.ico` (a bridge, drawn by
`tools/icons/make-icons.py`), is Setup's (`SetupIconFile`), Add/Remove
Programs' (`UninstallDisplayIcon`), the Start menu entries' and that of an
`.apk` file Gesher has not installed.

## Portable package

Each release also includes `gesher-X.Y.Z-portable.zip`, its `.sha256` file and
`SHA256SUMS.txt`, which covers both the ZIP and the installer. CI builds both
packages from the same verified runtime suppliers. The release publisher checks
the exact versioned filenames and both checksums before uploading the assets.

Extract the complete `GesherPortable` folder and open `Gesher.exe`. `App` contains
the runtime, while `Data` contains the Android profile, installed apps, settings
and logs. A `portable.marker` file declares the mode; an unrelated folder named
`Data` does not. `GESHER_PROFILE` remains an explicit profile override. Stop the
Gesher device and close its apps before copying the folder or removing a drive.

Setup also offers a portable mode (`/PORTABLE /DIR=<runtime-directory>`), with a
flat runtime layout. Updating an extracted ZIP targets its existing `App`
directory, refreshes the root `Gesher.exe`, and preserves the adjacent `Data`.
The Control Center updater supplies the same mode and directory automatically.
Setup prevents switching between standard and portable modes in an existing
runtime directory. Portable Setup creates no uninstaller or uninstall entry and
does not delete or replace another installation's registration.

Portable mode suppresses automatic Windows Start menu shortcuts, desktop
shortcuts, file associations and app URL-protocol registration. An APK or bundle
dropped on `Gesher.exe` asks before installing, as one opened from Explorer does
([below](#opening-an-apk-from-explorer)), but its confirmation has no "Create a
desktop shortcut" check box, and `gesher-app install --desktop-shortcut` says why
it makes none. Android app labels, icons,
dynamic-shortcut data and internal intents remain in the profile. External-browser
sign-in callbacks that need a custom host URL protocol are not newly registered
by portable mode. Notifications can still create per-user Windows AppUserModelID
metadata; this mode does not promise that Windows or apps leave no host records.
WebView2 and other host prerequisites must already be available. Android apps
retain the Windows user's access, as described in the bundled security notice.

CI separately tests the actual packaged Windows/ART runtime: Hello, APK
installation and launch, copying a populated profile while the original remains,
an APK dropped on the copy's `Gesher.exe` (the confirmation without the desktop
shortcut box, Cancel, Install and Done, then the installed version opened with no
dialog), flat and `App`-layout Setup updates, and preservation of an existing
standard installation's uninstall entry, PATH, associations, Start menu shortcuts
and desktop `.lnk` files (`tests/windows/shell/PortableRuntimeChecks.ps1`). Host fixtures
also cover profile selection, add-on routing and release payload rejection.
The package check uses the same installer for update-layout validation; it does
not exercise network update discovery or physical USB removal.

## microG on update

Setup recognizes microG that `gesher-app-microg.ps1` staged in the user's
profile (`%LOCALAPPDATA%\Gesher\microg.enabled` and GmsCore's
`system\priv-app\com.google.android.gms\base.apk`). Without it, Setup offers
the microG installation unchecked. With it, Setup compares
`%LOCALAPPDATA%\Gesher\microg.release` with the release it downloads
(`MicrogRelease` in `gesher.iss`): on a match it offers nothing; otherwise,
including microG staged from APKs the user supplied (no recorded release), it
offers the update, checked (a silent run keeps `/TASKS`, `/MERGETASKS` and the
previous choice, and goes on without the optional downloads if they fail).
`gesher-app-microg.ps1` records the release right after staging: the `-Release`
Setup passes with its own downloads, or the one `-AutoDownload` fetches when no
APK is passed; staging other APKs or `-Remove` clears it. The update replaces only the verified APK slots, keeping
microG's data and accounts.

## Optional downloads that fail

Setup downloads each optional component (WebView2, the three microG APKs as
one component, Aurora Store) on its own when **Install** is clicked. If one
cannot be downloaded (no connection, or a filtering proxy that refuses the
host, for example with HTTP 418), Setup lists the components it could not get
with the first error, skips only those, and installs everything else. Running
Setup again offers them again; microG and Aurora Store can also be installed
from the Gesher Control Center. Post-install passes the microG APKs only when
all three are present, otherwise `-AutoDownload`.
Cancelling the download still returns to the Ready page. A silent run shows
nothing and keeps the earlier behavior: each post-install step retries or
skips its own download.

Setup's finish page does not offer to open the README; the Start menu entry
does.

## The `.apk` association

Everything is per user, under `HKCU\Software\Classes`:

| Key | Value |
|---|---|
| `.apk` (default) | `Gesher.Apk`, only when `.apk` is unclaimed (see below) |
| `.apk\OpenWithProgids` | `Gesher.Apk` (empty string) |
| `Gesher.Apk` (default) | `Android app (Gesher)` |
| `Gesher.Apk` `AppUserModelID` | `Gesher.Installer`: opened APKs are its recent items, not those of every app's jump list |
| `Gesher.Apk\DefaultIcon` (default) | `%1` (the icon handler's) |
| `Gesher.Apk\shellex\IconHandler` (default) | `{6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50}` |
| `CLSID\{6B0C3C9E-...}\InprocServer32` | `<app>\gesher_shellext.dll`, `ThreadingModel` `Apartment` |
| `Gesher.Apk\shell\open` (default) | `Run with Gesher` |
| `Gesher.Apk\shell\open\command` (default) | `"<app>\gesher-launch.exe" "%1"` |

`ChangesAssociations=yes` makes Setup and the uninstaller notify the shell
(`SHChangeNotify`), so Explorer picks up the change without a sign-out.

`gesher-launch.exe` (`compat/shell/gesher_launch.cc`) runs `gesher-app.ps1`
with the same arguments. It is a Windows-subsystem program: Explorer starting
`powershell.exe` (a console program) directly flashed a console window before
`-WindowStyle Hidden` hid it, while gesher-launch starts it with
`CREATE_NO_WINDOW`. It also calls `AllowSetForegroundWindow(ASFW_ANY)`: the
app's window belongs to a process the device's system server starts, which
Windows would otherwise keep behind Explorer.

With standard handles to pass on (a redirected output, as CI reads it) it
passes them on and, as on a console, the launch lasts as long as the app's
process: `com.gesher.app.Launcher` streams the app's log and returns its exit
code. Without them (a Start menu shortcut, an APK opened from Explorer: nobody
reads the output) it sets `GESHER_LAUNCH_DETACH=1` (unless the caller set it):
the launcher returns once the activity has started (`startActivityAndWait`:
its first frame drawn, or idle), so gesher-launch, `powershell.exe` (and its
console) and the launcher's `dalvikvm.exe` end instead of staying, about
185 MB of working set and 800 MB of commit, for each open app. The app's process
is the device's (its system server started it); the device's idle timeout
counts app processes only. `gesher-app ... --detach` asks for the same from a
console, `--wait` for the waiting launch whatever the variable says (gesher-launch's
splash follows these arguments too).

### The splash window

A launch can take several seconds before the app's window is up (the first
one starts the device, installs and compiles the APK; every one starts the
app's process). So that something is on screen at once, gesher-launch shows a
splash window (class `GesherSplash`) right after it starts `gesher-app.ps1`:

- **When**: for `gesher-launch launch <package>` (a Start menu shortcut, and an
  APK opened from Explorer once it is installed: [Open](#opening-an-apk-from-explorer),
  or the same version already installed), `gesher-launch launch <file.apk>` and
  `gesher-launch <file.apk> <options>` (gesher-app's command line) whose app's
  window is shown; not for `install`, `uninstall`, `view`, `list`, `status`,
  `start`, `shutdown`, `toast-activated`, `apk-icon` or `apk-info`, nor with `--hidden-window`,
  `--headless`, `--single-process`, `--system-server` or
  `GESHER_APP_WINDOW=hidden|headless` (`--window` shows it again).
  `GESHER_SPLASH=0` turns it off, and also the reading and installing progress
  windows of an opened APK (its confirmation dialogs still show). When the app already has its window (it
  runs), none is shown.
- **What**: the size of the app's window (`GESHER_DISPLAY`'s, else 400x800
  pixels, within the work area), centered on the monitor of the pointer, with
  the app's icon (`gesher-shell\icon.ico` of the package, else the APK's
  `apk-icons.list` entry, else `gesher.ico`), its label (`label` of
  `gesher-shell\shortcut.properties`, else the package name or the APK's file
  name) as title and text, and "Starting…". With the package known (a
  package name, or an APK `apk-icons.list` names), it carries the app's
  AppUserModelID `Gesher.App.<package>` (`PKEY_AppUserModel_ID`), so its
  taskbar button is the app's.
- **Until**: a visible top-level window (not a tool window, not a splash) with
  that AppUserModelID is up, the app's own (the app process's windows carry it);
  for an APK whose package is not known, a new window with any `Gesher.App.*`
  ID. It looks every 200 ms. It also goes when `gesher-app.ps1` ends (the
  launch failed, or the app ended without a window; a detached launch's
  `gesher-app.ps1` ends once the activity has started with exit code 0, and the
  splash then waits for the window up to 60 s more) and after 2 minutes. If
  it still has the foreground when the app's window is up, it hands it on to
  that window; it never takes it from the app. Closing it (its X) cancels the
  launch: gesher-launch terminates the starting processes and exits, so the app
  does not open (after a detached launcher has ended, the device stays and the
  app's visible windows, never a splash, are closed as they come for 10 s).

CI: `tests/windows/tracks/55-shell.ps1` runs a copy of gesher-launch with a
stand-in `gesher-app.ps1` and a stand-in `%LOCALAPPDATA%` (a shortcut record
with an escaped label, an icon, an `apk-icons.list` line): `launch <package>`,
an APK given with options and an APK opened from Explorer whose version is
installed show the splash with the label, icon and AppUserModelID, it stays while
no app window is up and goes once a window with the app's AppUserModelID is shown;
closing it cancels the launch; `install` and `--headless` show none. The same
stand-in answers `apk-info` and records each run's arguments, and
`tests/windows/shell/InstallConfirmDialog.ps1` answers the confirmation and
installed dialogs ([below](#opening-an-apk-from-explorer)). The package job
runs the installed `gesher-launch install uiapp.apk` (no splash) and
`gesher-launch launch com.gesher.uiapp` with its window shown: the splash,
with `Gesher UI` and `Gesher.App.com.gesher.uiapp`, is gone within 5 s of the
app's `GesherWindow` being visible.

Each APK or split APK bundle's icon comes from `gesher_shellext.dll`
(`compat/shell/gesher_shellext.cc`), loaded by Explorer: its app's icon. It
parses no APK: it looks the file (its size and last write time) up in
`%LOCALAPPDATA%\Gesher\data\system\apk-icons.list`, which `gesher-app install`
and launches fill with the shortcut's `.ico`. An APK not listed shows Gesher's
icon at first, while the handler runs `gesher-launch.exe apk-icon "<file>"` in
the background (no window; once per file, at most two at once, not for an empty
file or one still being written). That command draws the app's icon without
installing it into `data\system\apk-icons\<size>-<time>.ico`, records it, and
Explorer shows it. Bundles are read in a temporary directory without installing
the app: the base APK supplies package metadata and the resource splits supply
the density-specific images. The cache key remains that of the original bundle.
Temporary icon files have unique names, and complete icon lists are published by
replacement so concurrent requests do not expose partially written cache data.

### Opening an APK from Explorer

Opening an APK or split APK bundle (`.apk`, `.xapk`, `.apks`, `.apkm`, `.apk+`)
from Explorer runs `gesher-launch.exe "<file>"`: the bare form, one argument
naming an existing file (portable `Gesher.exe` given a dropped file runs the same).
Nothing is installed without the user's consent (issue 519,
[ADR 0028's amendment](decisions/0028-shell-start-menu-apps.md)):

1. A small progress window (the splash's painting: the file's name, "Reading…")
   while `gesher-app.ps1 apk-info "<file>" "<temp file>"` reads the APK or bundle
   without installing it or starting the device: its package, versionCode,
   versionName, label and icon (recorded for Explorer as `apk-icon` records it),
   the installed versionCode and versionName when the package is installed, and
   whether its recorded desktop shortcut is there. A file that cannot be read
   (not an app, an encrypted `.apkm`) ends here with the error dialog.
2. The same versionCode is installed: no dialog; gesher-launch runs
   `launch <package>`, as the app's Start menu shortcut does, with its splash.
3. Otherwise the confirmation dialog, "Install Android app": the app's icon,
   label and version, "Do you want to install this app?" (or "update", when another version
   is installed), the buttons **Install** (**Update** when the package is
   installed) and **Cancel**, and the check box **Create a desktop shortcut**,
   unchecked. Cancel is the default button and has the initial focus, so Enter
   alone does not install. Cancel, Esc and the X install nothing; gesher-launch
   exits with 1223 (`ERROR_CANCELLED`).
4. Install: a progress window ("Installing <label>…") while `gesher-app.ps1
   install "<file>"` runs, with `--desktop-shortcut` when the box was checked.
   The APK is installed (or updated) on the user's Gesher device as `gesher-app
   install` installs it; its Start menu shortcut is made as usual.
5. Only when the install succeeded, a second dialog, "<label> was installed."
   (or updated): **Open** runs `launch <package>` with the splash; **Done** (the
   default), Esc and the X end it. The app is never started without Open. When
   the box was checked but the record names no desktop shortcut that exists
   (install printed why), the dialog adds "The desktop shortcut was not created."

A failing step (an unreadable file, a failed install) shows Gesher's error
dialog with its message and ends with that step's non-zero exit code; nothing
claims success. The check box is not shown in portable Gesher, with
`GESHER_SHORTCUTS=0`, and when the package's recorded desktop shortcut is still
there. The dialogs are right-to-left for Hebrew, scale with the monitor's DPI and
follow high contrast (`compat/windows/host_dialog_theme.h`). The command lines
`gesher-app my.apk`, `gesher-app install my.apk`, `gesher-launch launch
<file|package>`, `gesher-launch install my.apk` and an APK given to gesher-launch
with options are unchanged: they install (and run) without asking, as before.

For UI automation (CI, `tests/windows/shell/InstallConfirmDialog.ps1`), both
dialogs are top-level `#32770` dialogs of the gesher-launch process with stable
control IDs:

| Dialog | Title (`tools/package/ui-messages.json`) | Controls |
|---|---|---|
| Confirmation | `install_confirm_title` | Install/Update `IDOK` (1), Cancel `IDCANCEL` (2), "Create a desktop shortcut" check box 1001 (absent when not offered) |
| Installed | `install_done_title` | Open 1002, Done `IDCANCEL` (2) |

The helper finds them by these IDs (and their titles), presses buttons with
`BM_CLICK` and sets the check box as a click would.

A launch (Open, or the same version opened) runs on the user's Gesher device:
its launcher Activity is started through the device's system server, in a window
of the app's process (ADR 0029; see `app/README.md` and
[0026](decisions/0026-services-persistent-device.md)). The first launch starts
the device (binderd, servicemanager, the system server) in the background; it
outlives gesher-launch and serves later launches until `gesher-app shutdown` or
10 minutes without an app. The APK must be on the same drive as Gesher.

## Installed apps in the Start menu

Each installed package with a launcher (MAIN/LAUNCHER) activity gets a Start
menu shortcut, as a launcher lists it
([0028](decisions/0028-shell-start-menu-apps.md)):

| | |
|---|---|
| Shortcut | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Gesher Apps\<label>.lnk` (FOLDERID_Programs; `<label> (<package>).lnk` if another app has that label) |
| Command | `<app>\gesher-launch.exe launch <package>`, in `<app>` (an older Gesher without it: `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "<app>\gesher-app.ps1" launch <package>`) |
| Name | the app's label in the default locale (`android:label`, else the package name) |
| Icon | `%LOCALAPPDATA%\Gesher\data\data\<package>\gesher-shell\icon.ico` |
| AppUserModelID | `Gesher.App.<package>` (`System.AppUserModel.ID` in the link) |

- **When**: `gesher-app install APK` (also Install in the
  [confirmation](#opening-an-apk-from-explorer) of an APK opened from Explorer)
  and every launch (`gesher-app my.apk`, `launch <package>`) make it, or make it again when the package was
  updated (new label or icon) or the shortcut is missing; it says so in the
  console (`gesher-app: Start menu shortcut ...`). `gesher-app uninstall
  PACKAGE` removes it and its icon, and the `Gesher Apps` folder once it is
  empty. An install committed through Android's `PackageInstaller` session API
  (an app store such as Aurora Store) makes the shortcut and URL protocols too,
  from the system server, once the install is published; a removal committed
  through `PackageInstaller` removes them (ADR 0028's amendment). A failure never
  fails the install, the removal or the launch. `GESHER_SHORTCUTS=0` turns
  shortcuts off; runs from a build folder without `gesher-app.cmd` make none.
- **Desktop shortcut**: only when the user asks for one, with the check box of
  the [confirmation](#opening-an-apk-from-explorer) (`gesher-app install APK
  --desktop-shortcut`): the same link (target, arguments, icon, description,
  AppUserModelID) as `<Desktop>\<label>.lnk` (FOLDERID_Desktop; `<label>
  (<package>).lnk` when another file has that name), recorded as `desktop_link`
  in `shortcut.properties`. Later launches, `sync-shortcuts` and updates keep the
  record but never make the link again: a desktop shortcut the user deleted stays
  deleted. Uninstalling the package (`gesher-app uninstall`, a `PackageInstaller`
  removal, the Control Center) removes the recorded link, and only that path.
  Never in portable Gesher or with `GESHER_SHORTCUTS=0` (the output says why); a
  failure to make it does not fail the install and is printed.
- **Window icon without a shortcut**: the app's windows take the shortcut's
  `icon.ico`. A package with no launcher activity, and a staged microG addon
  (its verified `system\priv-app\<package>` slot, which `gesher-app-microg.ps1`
  manages rather than `gesher-app install`), gets only that `icon.ico`, from the
  application icon, as Android shows a task's application icon; no shortcut.
  It is written again when the APK is newer; `gesher-app uninstall`, or
  `gesher-app-microg.ps1 -Remove` for an addon, removes it. A package with no
  launcher activity also gets `gesher-shell\no-launcher-activity`, so that the
  Control Center lists it without "Launch", as a launcher would not show it;
  the camera app (`com.gesher.camera`, installed by the system server) is
  recorded the same way. The system server updates the record on package
  install, at each device start, and when the application or a launcher
  activity or alias changes its enabled state, including with `GESHER_SHORTCUTS=0`.
- **`gesher-app launch PACKAGE`** starts an installed package by name (the
  shortcut's command; the device starts if it is not running). `gesher-app
  launch APK` is `gesher-app APK`.
- **The label and icon** are read and drawn with the framework, as a
  launcher does (`com.gesher.app.shell.AppIcon`): the APK's resources for the
  default locale at xxxhdpi, so the best density of a PNG or WebP mipmap is
  picked; the application's icon, else the launcher activity's, else the
  framework's default (`sym_def_app_icon`); an adaptive icon is drawn by
  `AdaptiveIconDrawable`, its foreground over its background in the system's
  icon mask (if the framework cannot, Gesher composes the two layers in a
  circle). The `.ico` holds ten sizes, 16 to 256 pixels: 32-bit DIBs up to 48,
  PNGs above. `shortcut.properties` next to it records the shortcut.
- **The taskbar**: the system server starts the package's processes with
  `GESHER_APP_USER_MODEL_ID=Gesher.App.<package>`, and the app process
  (`android.app.GesherAppProcess`) calls `SetCurrentProcessExplicitAppUserModelID`
  with it before any window exists (`gesher-app: Windows AppUserModelID ...` in
  the app's output), so its windows group with the shortcut and get its icon.
  Window code that needs it per window reads the same variable and sets
  `PKEY_AppUserModel_ID` on the window (`SHGetPropertyStoreForWindow`).
  `--single-process` and `--system-server` runs set none.
- **Uninstalling Gesher** removes `{userprograms}\Gesher Apps` with every
  app's shortcut (`[UninstallDelete]`) and the desktop shortcuts the apps'
  records name (`desktop_link`, `[Code]`); the icons go with `%LOCALAPPDATA%\Gesher`.
  Nothing else of the Start menu or the desktop is touched.

The natives (`com.gesher.app.shell.Shell`: `IShellLinkW`, `IPropertyStore`,
`SHGetKnownFolderPath`, `SetCurrentProcessExplicitAppUserModelID`, the URL
protocols' registry keys) are in `compat/shell`, part of libandroid_runtime's
builtin library.

## Installed apps' URL protocols (OAuth redirects)

An app that signs in with OAuth (AppAuth, as Thunderbird does) opens the sign-in
page in a browser and waits for the browser to open its redirect URL, a custom
scheme one of its activities declares (`<data android:scheme=...>` in a VIEW
filter with BROWSABLE, e.g. `net.thunderbird.android:/oauth2redirect` or
`msauth://net.thunderbird.android/...`). On Gesher the browser is Windows':

- **The browser**: apps that look for one (`queryIntentActivities` of VIEW
  `http:` BROWSABLE) find `com.gesher.browser`, a synthetic package with a full
  browser's filter (`com.gesher.server.BrowserPackageSupport`); starting it opens
  the URL with Windows' default browser (`WindowsHost.openUrl`), as any http(s)
  VIEW nothing else handles.
- **Mail, phone, messaging and map links**: `com.gesher.urihandlers`
  (`com.gesher.server.WindowsUriHandlers`) has one activity per Windows protocol
  handler: `mailto:` (VIEW, SENDTO), `tel:` (VIEW, DIAL), `sms:`/`smsto:` (VIEW,
  SENDTO) and `geo:` (VIEW), the filters of AOSP's own apps for them. An activity
  exists only while Windows has a handler registered for its scheme
  (`AssocQueryStringW`, `ASSOCF_IS_PROTOCOL`: the current user's default); without
  one, `resolveActivity` is null and `startActivity` throws
  `ActivityNotFoundException`, as on a device without such an app. Starting one
  hands Windows the URI with the intent's own fields: for mail the URI's
  recipients, subject and body plus `EXTRA_EMAIL`/`EXTRA_CC`/`EXTRA_BCC` and, when
  the URI has none, `EXTRA_SUBJECT`/`EXTRA_TEXT`; for SMS the recipients and the
  body AOSP Messaging reads (`sms_body`, `body=`, `EXTRA_TEXT` of `text/plain`).
  `ACTION_DIAL` first shows **Gesher - Confirm dialing**, with the complete
  number and requesting package, and Cancel selected by default. It explains
  that the Windows phone app may place the call immediately; only an explicit
  OK hands that same URI to Windows. This review runs off the activity-launch
  thread and preserves pause/resume and result ownership. Caller/package
  retirement or removal of the handler cancels the pending dialog; the handler
  is checked again after approval. Detached requests belong to Gesher rather
  than a possibly recycled source process. The queue is bounded to 16 requests
  and one per requesting package. Closing this review yields `RESULT_CANCELED`
  to a result caller, never a fabricated call-completion result. `ACTION_VIEW`
  keeps its direct Windows delegation.
  Not handled: `ACTION_CALL` (no confirmation step is promised by a Windows
  handler), `mms:`, and `ACTION_SEND` of `message/rfc822` (attachments). These
  schemes are among those Gesher never registers for an Android app (below), so
  the handler is never Gesher itself.
- **The way back**: `gesher-app install` (and launches that install or update)
  registers each custom scheme of the package's BROWSABLE VIEW filters as a URL
  protocol, per user:

  | Key (`HKCU\Software\Classes`) | Value |
  |---|---|
  | `<scheme>` (default) | `URL:<scheme> (<package>, an Android app on Gesher)` |
  | `<scheme>` `URL Protocol` | empty |
  | `<scheme>` `GesherPackage` | `<package>`: the key is Gesher's, for that package |
  | `<scheme>\DefaultIcon` (default) | the app's `.ico` (the shortcut's) |
  | `<scheme>\shell\open\command` (default) | `"<app>\gesher-launch.exe" view <package> "%1"` |

  When the browser opens such a URL (asking the user first), `gesher-app view
  <package> <url>` starts ACTION_VIEW of it (BROWSABLE) in the package through
  the device, in the app's task when it runs, where AppAuth's
  RedirectUriReceiverActivity hands it to the activity waiting below it
  (`FLAG_ACTIVITY_CLEAR_TOP | FLAG_ACTIVITY_SINGLE_TOP`: `onNewIntent`).
- **Not taking over another's scheme**: the web's, mail's, files' and Android's
  own schemes (`http`, `https`, `mailto`, `file`, `content`, `tel`, ...) are
  never registered, nor a scheme with a key of another program (in HKCU without
  `GesherPackage`, or in HKLM) or another package's; the install says so
  (`gesher-app: URL protocol <scheme>: not registered ...`).
- **Removal**: `url-protocols.properties` in the package's `gesher-shell`
  folder records its schemes; an update removes those it no longer declares,
  `gesher-app uninstall PACKAGE` removes them all (only keys whose
  `GesherPackage` still names it), and uninstalling Gesher removes every key of
  `HKCU\Software\Classes` with a `GesherPackage` value (`RemoveUrlProtocols`).
  `GESHER_SHORTCUTS=0` turns them off with the shortcuts.

CI: `tests/windows/tracks/95-browser.ps1` checks the browser package and the
delegation (`gesher.services.BrowserIntentTest`, with no browser opened); the
package job installs components.apk (CrossActivity declares
`gesher-components://callback`), reads its URL protocol, uninstalls and
reinstalls it around a key of another program's, runs a protocol command line
through `gesher-launch.exe` and, after uninstalling Gesher, checks the key is
gone.

## The Gesher device and (un)installing

Open an APK or bundle from Explorer or use the Control Center's install
dialog. Bundle formats supported here are ZIP archives containing one base
APK, its companion split APKs and optional dex metadata, and bundletool APK
sets (`bundletool build-apks` output, `.apks` with a `toc.pb`). From an APK set
the installer takes what `bundletool install-apks` would install on Gesher's
device (`com.gesher.app.ApkSetToc`, after bundletool's `ApkMatcher`): the one
variant matching its SDK level and ABIs (x86_64, then arm64-v8a), so a
standalone or `universal.apk` only when no split variant fits; the install-time
modules with their dependencies; their splits for the preferred ABI; and the
install-time asset packs. Device tier, country set and device group targeting
take the set's own defaults (tier 0, the fallback country split and no group when
it names none), as `install-apks` does for a device spec without them. It keeps
every density and language split of those modules: Gesher's density is set per launch and its languages follow Windows
and per-app locales, and no store delivers a missing split later
([ADR 0091](decisions/0091-apk-bundle-selection.md)). An XAPK
(APKPure's format) whose `manifest.json` lists `split_apks` installs those
files; any other archive without a `toc.pb` (`.apkm`, `.apk+`, an older
`.xapk`) installs every `.apk` in it. Installed files are named from their
manifests (`base.apk`, `split_<split name>.apk`), as Android's package
installer names them, whatever the archive calls them. A loose APK's
`split_*.apk` and `config.*.apk` neighbours are its companions only when their
manifests name its package; APKs whose manifests identify a different package
are left out and logged. An unreadable or malformed companion cannot establish
that it belongs to another package, so it rejects the installation. The previous
installation and its data remain in place, including when the base APK itself
has not changed.

Not installed:

- Expansion files (`.obb`, an XAPK's `expansions`). Android keeps them in shared
  storage, `Android/obb/<package>/`, where Google Play or an XAPK installer
  copies them; a package install, `adb install-multiple` included, does not.
  Gesher installs the APKs and logs the expansion files it left in the archive
  (`install <package>: ... also holds expansion files`); an app that needs them
  finds its OBB folder as it is.
- Encrypted `.apkm` files. APKMirror distributed some `.apkm` files encrypted
  for its own installer. Gesher has no key for them: such a file, which is not
  a ZIP, is refused with `INSTALL_FAILED_INVALID_APK: ... an encrypted
  APKMirror bundle`. Unencrypted `.apkm` files (a ZIP of `base.apk`,
  `split_config.*.apk` and `info.json`) install.

Apps using Android's system file or folder picker can select Windows files and
folders through the [Storage Access Framework](services.md#storage-access-framework-documents).
Apps browsing Android shared storage directly see Gesher's shared volume.

The Control Center's [Updates card](updates.md) discovers stable GitHub releases
and starts the verified installer for the current installation directory. Setup
writes `gesher-version.ini` from its own `AppVersion`; the updater also verifies
the downloaded installer's Windows product version. Updates are in-place
installations, so installed apps and their data are retained. Running the
uninstaller first is not part of an update.

The packaged `gesher-runtime-files.ps1` helper invalidates the profile's cached
runtime copies once per installed version, even for equal-size resources. App
and dex launchers record separate completed generations; a failed copy is retried
and neither path changes app data.

### Non-ASCII profile and install paths

When the Windows user name (and so `%LOCALAPPDATA%\Gesher`) or the install
folder contains non-ASCII characters, the launchers (`gesher.ps1`,
`gesher-app.ps1`, `gesher-app-microg.ps1`) pass Gesher's own roots to the
runtime through `ConvertTo-GesherAndroidPath`, which rewrites the non-ASCII
ancestors to their NTFS 8.3 short names: `ANDROID_ROOT`, `ANDROID_ART_ROOT`,
`ANDROID_DATA`, `ANDROID_I18N_ROOT`, `ANDROID_TZDATA_ROOT`, the boot class path
and the boot image. The leaf name is never replaced by its 8.3 alias, so file
names and extensions stay as they are (a non-ASCII leaf leaves the whole path in
long form).

User-supplied paths are not rewritten: the APK given to `gesher-app` (`install`,
`launch`, a double-clicked APK or split APK bundle) and the dex given to `gesher`
are passed in long form (`UserPath`), so the installer still sees the real file
name and finds a `.dm` file beside the APK. Whether a file is an APK or a
bundle is decided by its content, not its suffix: an APK has
`AndroidManifest.xml` at its root, a bundle is a ZIP of `.apk` files. A single
APK saved as `.xapk` installs as that APK.

Limitations:

- 8.3 names can be disabled per volume (`fsutil 8dot3name`), and folders created
  while they were disabled have none. Then no ASCII short name exists and the
  long, non-ASCII path is used unchanged.
- Only the path strings handed to the runtime change. Canonical paths
  (`GetFinalPathNameByHandleW`, and so `realpath`/`File.getCanonicalPath`) still
  expand to the long form, so an app's `sourceDir`, `nativeLibraryDir` and other
  canonicalized paths remain non-ASCII.
- For existing users with a non-ASCII profile, visible paths (environment
  variables, logs, and paths derived from `ANDROID_DATA` without
  canonicalization) change form from the long name to the `XXXX~N` short name
  after the update; both name the same files.

The device runs `dalvikvm.exe`, `binderd.exe` and `servicemanager.exe` from the
install folder and keeps files of `%LOCALAPPDATA%\Gesher` open, so:

- the uninstaller runs `gesher-app shutdown` first (`[UninstallRun]`), then
  removes `%LOCALAPPDATA%\Gesher` with the installed packages, their data and the
  logs;
- Setup runs `gesher-app shutdown` before it replaces the files of an earlier
  version (`PrepareToInstall`).
- Explorer, and any program whose file dialog showed an APK, keeps
  `gesher_shellext.dll` loaded. Setup does not close them for it
  (`CloseApplicationsFilter=*.exe,*.chm`: closing Explorer took the desktop away
  mid-update). `PrepareToInstall` renames the loaded copy to
  `gesher_shellext.<time>.old` (a loaded DLL can be renamed, not replaced) and the
  new one is written in its place; those processes keep the old copy until they
  end. The next update deletes the `.old` copies nothing has loaded any more, and
  uninstalling deletes them too. CI loads the DLL into a process, runs Setup again
  and checks that the process is still running.

`gesher-app shutdown` needs no runtime: `gesher-app.ps1` reads the device's
state file (`%LOCALAPPDATA%\Gesher\data\system\gesher-device`), sets the system
server's shutdown event, waits for it to exit and ends whatever is left.

### Not taking over another app's association

- Gesher always joins `.apk`'s "Open with" list (`OpenWithProgids`).
- It becomes `.apk`'s default only when `HKCR\.apk` (the merged per-user and
  per-machine view) has no default ProgID, or already names `Gesher.Apk`
  (`ApkUnclaimed` in the script). An emulator or another tool that registered
  `.apk` first, for this user or machine-wide, keeps it.
- It never writes Explorer's `UserChoice`
  (`...\Explorer\FileExts\.apk\UserChoice`): a default the user picked in
  Windows' "Open with" dialog or Settings always wins, as Windows requires.
- On uninstall, `Gesher.Apk` and Gesher's `OpenWithProgids` entry are
  removed; `.apk`'s default is removed only while it still names `Gesher.Apk`
  (another app may have claimed it since), and the `.apk` key only if it is
  then empty.

APKs show their icons from Gesher's icon handler while `.apk` opens with Gesher.

### How CI checks it

After installing, the `package` job checks that `.apk`'s default is
`Gesher.Apk` and that `OpenWithProgids` lists it, reads the open command from
the registry, replaces `%1` with the installed `lifecycle.apk` and runs that
command line as Explorer would, answering gesher-launch's dialogs with
`tests/windows/shell/InstallConfirmDialog.ps1`:

- with the same version installed: no dialog comes up, and the installed package
  is launched (`launching the installed package com.gesher.lifecycle`,
  `gesher-app: done`; the device started);
- after `gesher-app uninstall`: the confirmation (its title `install_confirm_title`,
  Install and Cancel, the desktop shortcut box shown and unchecked); Cancel exits
  with 1223 and nothing is installed (`gesher-app list`, no shortcut);
- Install with the box checked, then Done: installed (`installed
  com.gesher.lifecycle`), not launched (no `gesher-app: done`, no
  `LifecycleActivity`), the Start menu shortcut and `<Desktop>\Gesher
  Lifecycle.lnk` (the same target, arguments, icon and AppUserModelID), recorded
  as `desktop_link`; `gesher-app uninstall` removes it;
- Install with the box unchecked, then Open: installed and launched
  (`LifecycleActivity onResume`, `gesher-app: done`), no desktop shortcut;
- an `.apkm` bundle of a package not installed asks (Install, Open); an `.apk+`
  bundle of the installed version opens it directly.

`tests/windows/tracks/55-shell.ps1` covers the rest with a stand-in
`gesher-app.ps1`: an update whose recorded desktop shortcut is there and
`GESHER_SHORTCUTS=0` hide the box, a `portable.marker` beside gesher-launch hides
it, and a failing `apk-info` or install ends with its exit code and no installed
dialog. The e2e job opens a real app's APK of its installed version through the
association (no dialog). After uninstalling, the package job checks that the
`Gesher.Apk` key, `.apk`'s default and the `OpenWithProgids` entry are gone,
and that uninstalling Gesher removes the apps' recorded desktop shortcuts and
keeps the user's own.

With the Gesher device (ci run 40019524773), the package job also runs the
installed `gesher-app lifecycle.apk` (the device starts, the APK is installed
and run; `gesher-app list` shows it), `--single-process`, `gesher-app
shutdown` (the device's three processes gone), the `.apk` open command (the
same version installed: it launches the package and starts the device again) and checks that the device still runs after that
command's console is gone; after uninstalling, that its processes are gone and
`%LOCALAPPDATA%\Gesher` removed.

The Start menu shortcuts (ci run 40024847287, package job on windows-2022):
the first `gesher-app lifecycle.apk` makes `Gesher Apps\Gesher Lifecycle.lnk`
and the app process reports its AppUserModelID; `gesher-app uninstall
com.gesher.lifecycle` removes the shortcut and the icon; `gesher-app install
lifecycle.apk` makes it again, and the job reads it with `WScript.Shell`
(target `gesher-app.cmd`, arguments `launch com.gesher.lifecycle`, working
folder, icon) and its `System.AppUserModel.ID` through the link's
`IPropertyStore` (`tests/windows/shell/ShellChecks.ps1`), checks the `.ico`
(ten sizes, DIBs and PNGs, System.Drawing reads them, the xxxhdpi picture's
red at 256 pixels and transparent corners), runs the shortcut's command line
(`gesher-app: done`), installs components.apk (an adaptive icon: white
foreground, blue background, masked corners) and checks `Gesher.Apk`'s
`DefaultIcon` and the Gesher Control Center entry's icon. After uninstalling Gesher,
`Gesher Apps` is gone and a file of the job's in the Start menu is still
there. `tests/windows/tracks/55-shell.ps1` checks the label and the icon of
lifecycle.apk, components.apk and peer.apk (the framework's default icon) on
both test runners.

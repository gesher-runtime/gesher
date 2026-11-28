# 0028 — Installed apps as Windows apps: Start menu shortcuts, icons, AppUserModelIDs

**Status:** accepted, 2026-10-24 (shell track); amended 2026-11-22 (installs and
removals committed through PackageInstaller sessions, below); amended 2026-11-25
(confirmation before installing an APK opened from Explorer, optional desktop
shortcut, below)

## Context
With the Gesher device ([0026](0026-services-persistent-device.md)) packages stay
installed (`gesher-app install`, `%LOCALAPPDATA%\Gesher\data\app`), but nothing on
Windows showed them: an installed app could only be started again from a console,
with its APK's path. On Android the launcher lists every package with a
MAIN/LAUNCHER activity, with the app's label and icon, and starts it by package. On
Windows that is the Start menu: a shortcut with a name, an icon, and an
AppUserModelID that ties the app's taskbar buttons to it.

## Decision
- **One shortcut per launchable package**,
  `<FOLDERID_Programs>\Gesher Apps\<label>.lnk` (per user, as Gesher is installed),
  whose command is `gesher-app.cmd launch <package>` in Gesher's folder.
  `gesher-app launch PACKAGE` starts an installed package without its APK (the
  launcher's `startActivity` by package); `launch APK` is `gesher-app APK`.
  Packages without a MAIN/LAUNCHER activity get none, as on a launcher.
- **Made by the launcher process** (`com.gesher.app.Launcher`, what gesher-app
  runs) after `install` and each launch's install step, not by the system server:
  it runs in the user's console with Gesher's folder known (dalvikvm.exe's), and
  logs what it did there. A package that is installed and unchanged keeps its
  shortcut unless it is missing (then it is made again: the folder is the app
  list, not a set of user-made shortcuts). It is never fatal: an install or launch
  goes on without a shortcut and says why. Gesher's own test runs (no
  `gesher-app.cmd` next to dalvikvm.exe) make none; `GESHER_SHORTCUTS=0` turns them
  off.

  **Amended 2026-11-22:** apps also install and remove packages through Android's
  `PackageInstaller` session API (an app store such as Aurora Store commits a
  session; `PackageInstaller.uninstall` removes a package), which never runs
  gesher-app. Those packages got no shortcut and no URL protocols, and a removed
  one kept them. Such installs and removals are now reflected from the system
  server: `PackageInstallerService`, after a session's install is committed and
  published, registers the package's shortcut and URL protocols (the same
  `AppShell` steps, for the published code generation, `changed` unless the code
  was unchanged); before a session removal it reads the shortcut and URL protocol
  records under the publication lock, and after the removal is committed it
  unregisters them. This is best effort, as for the launcher: a failure is logged
  and never changes the install's or removal's result, and a denied, blocked or
  failed commit changes nothing. The system server is dalvikvm.exe in Gesher's
  folder as well, so the rules above hold: Gesher's own test runs make none and
  `GESHER_SHORTCUTS=0` turns them off. `gesher-app install`, `uninstall` and each
  launch keep doing it from the launcher process.
- **The label and the icon come from the framework**, as a launcher gets them:
  the APK's resources through `AssetManager`/`Resources` with the default locale
  and xxxhdpi (so the best density of a mipmap is chosen by the resource
  resolver), the application's icon (else the launcher activity's, else the
  framework's `sym_def_app_icon`), inflated and drawn with android.graphics:
  hwui's Skia decodes PNG, WebP and JPEG and draws vector drawables, and an
  adaptive icon is drawn by `AdaptiveIconDrawable` (foreground over background,
  in `config_icon_mask`). If the framework cannot inflate or draw it, Gesher
  composes the layers itself (decoded with BitmapFactory if need be, in a
  circle). The pictures (16 to 256 pixels, ten sizes) go into a `.ico`: 32-bit
  DIBs up to 48 pixels, PNGs above, in the package's data directory
  (`data\data\<package>\gesher-shell\icon.ico`, with `shortcut.properties`
  recording the shortcut, so that an uninstall or a new label finds the old one).
  Reading files out of the APK by hand (aapt2's paths, densities, the adaptive
  icon's XML) would duplicate what the framework already does correctly.
- **Shortcuts through COM**, `IShellLinkW` + `IPersistFile` with
  `System.AppUserModel.ID` set through the link's `IPropertyStore`, in
  `compat/shell` (natives of `com.gesher.app.shell.Shell`, in libandroid_runtime's
  builtin library; shell32's functions looked up when used). Writing the `.lnk`
  format by hand or from PowerShell (which cannot set the property store without
  compiling C#) were the alternatives.
- **AppUserModelID `Gesher.App.<package>`** (at most 128 characters: a longer one
  ends with a hash), in the shortcut and in the app's processes: the system server
  puts `GESHER_APP_USER_MODEL_ID` in the environment of the processes it starts for
  a package, and `android.app.GesherAppProcess` calls
  `SetCurrentProcessExplicitAppUserModelID` with it before any window exists, so
  every window of the process groups under the shortcut. A window can also carry
  it itself (`SHGetPropertyStoreForWindow`, `PKEY_AppUserModel_ID`) — the hook for
  the window code if an app process ever hosts another package's windows.
- **Uninstalling**: `gesher-app uninstall` removes the package's shortcut and icon
  (and the folder when it is empty), as does a removal committed through
  `PackageInstaller` (amendment above); Gesher's uninstaller removes
  `{userprograms}\Gesher Apps` as a whole (`[UninstallDelete]`), the icons going
  with `%LOCALAPPDATA%\Gesher`.
- **Amended 2026-11-25 (issue 519): an APK opened from Explorer asks first.** A
  double-clicked APK or split bundle (`gesher-launch "<file>"`, the bare form; also
  a file dropped on portable `Gesher.exe`) used to be installed and launched at
  once, as `gesher-app my.apk` is. Opening a file is not consent to install an app
  on the PC, and an installed app that starts by itself is not what Windows users
  expect from an installer. Now:
  - **Confirmation before install.** gesher-launch reads the file first
    (`gesher-app apk-info`: package, version, label, icon, the installed version;
    no install, no device) and shows a dialog with the app's icon, name and
    version and Install (Update when the package is installed) / Cancel, Cancel
    being the default. Nothing is installed until the user clicks Install; Cancel,
    Esc and the X do nothing (exit 1223, `ERROR_CANCELLED`).
  - **No automatic launch.** After a successful install a second dialog says the
    app was installed and offers Open (`launch <package>`, as the Start menu
    shortcut does) and Done. A failed step shows the error and never claims
    success.
  - **Optional desktop shortcut.** The confirmation has a "Create a desktop
    shortcut" check box, unchecked by default (`gesher-app install
    --desktop-shortcut`): the Start menu link's copy as `<Desktop>\<label>.lnk`,
    recorded as `desktop_link` in `shortcut.properties`. Later launches keep the
    record and never make the link again (a link the user deleted stays deleted);
    uninstalling the package removes the recorded path, and so does Gesher's
    uninstaller. The box is hidden with `GESHER_SHORTCUTS=0` and when the recorded
    desktop link exists.
  - **The same version opens directly.** When the installed versionCode equals the
    file's, there is nothing to install: no dialog, the installed package is
    launched with the normal splash.
  - **Portable: none.** Portable Gesher still asks before installing but shows no
    check box and makes no desktop shortcut, as it makes no Start menu shortcut.
  - The command lines (`gesher-app my.apk`, `gesher-app install`, `gesher-launch
    launch`, `gesher-launch install`, an APK given with options) are unchanged:
    a user typing them has already asked for the install. The dialogs' control IDs
    are stable for UI automation (`docs/installer.md`, "Opening an APK from
    Explorer"); CI drives them through the registered association command.
- **Gesher's own icon**, `tools/package/gesher.ico` (drawn by
  `tools/icons/make-icons.py`): Setup's, Add/Remove Programs', the Start menu
  entries', the `.apk` type's (`Gesher.Apk\DefaultIcon`), and a shortcut's when an
  app's icon cannot be drawn.

## Consequences
- An installed app is a Start menu entry with its own name and icon, searchable
  and pinnable; it opens a console (gesher-app's) while it runs.
- Opening an APK from Explorer takes one more click (Install), and another (Open)
  to start the app; reopening the same file of an installed app opens it at once.
  Automation of the Explorer path has to answer the dialogs (CI uses
  `tests/windows/shell/InstallConfirmDialog.ps1`).
- The app's windows and its shortcut share an ID; the console window of
  gesher-app.cmd does not (it belongs to the console host).
- `--single-process` and `--system-server` runs (the app in the launcher's
  process) set no AppUserModelID.
- The shortcut keeps working across Gesher updates (same folder) and app updates
  (made again with the new label and icon).

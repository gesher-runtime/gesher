# 0028 — Installed apps as Windows apps: Start menu shortcuts, icons, AppUserModelIDs

**Status:** accepted, 2026-10-24 (shell track); amended 2026-11-22 (installs and
removals committed through PackageInstaller sessions, below)

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
- **Gesher's own icon**, `tools/package/gesher.ico` (drawn by
  `tools/icons/make-icons.py`): Setup's, Add/Remove Programs', the Start menu
  entries', the `.apk` type's (`Gesher.Apk\DefaultIcon`), and a shortcut's when an
  app's icon cannot be drawn.

## Consequences
- An installed app is a Start menu entry with its own name and icon, searchable
  and pinnable; it opens a console (gesher-app's) while it runs.
- The app's windows and its shortcut share an ID; the console window of
  gesher-app.cmd does not (it belongs to the console host).
- `--single-process` and `--system-server` runs (the app in the launcher's
  process) set no AppUserModelID.
- The shortcut keeps working across Gesher updates (same folder) and app updates
  (made again with the new label and icon).

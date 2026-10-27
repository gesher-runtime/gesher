; Gesher's Windows installer (Inno Setup 6), built in CI by the package job:
;   iscc /DBuildDir=<dir with the build output> /DAppVersion=<x.y.z> gesher.iss
; Installs per user (no administrator rights) into
; %LOCALAPPDATA%\Programs\Gesher, puts `gesher` and `gesher-app` on the user's
; PATH, adds a Start menu entry that runs the bundled Hello and opens `.apk`
; files with gesher-app (docs/installer.md). Apps installed with gesher-app get
; Start menu shortcuts in {userprograms}\Gesher Apps (com.gesher.app.shell),
; which the uninstaller removes.

#ifndef BuildDir
  #error Pass /DBuildDir=<build output directory>
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6A1F3C2E-5B7D-4E8A-9C0F-1D2E3F4A5B6C}
AppName=Gesher
AppVersion={#AppVersion}
; Keep both the numeric PE resource and displayed metadata tied to the tested version.
VersionInfoVersion={#AppVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
AppPublisher=Gesher
DefaultDirName={localappdata}\Programs\Gesher
DisableDirPage=yes
DefaultGroupName=Gesher
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesEnvironment=yes
ChangesAssociations=yes
OutputBaseFilename=gesher-setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Gesher {#AppVersion}
; Gesher's icon (tools/icons/make-icons.py): Setup, Add/Remove Programs, the
; Start menu entries and the .apk file type.
SetupIconFile=gesher.ico
UninstallDisplayIcon={app}\gesher.ico

[Files]
Source: "{#BuildDir}\dalvikvm.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\core-oj.jar"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\core-libart.jar"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\core-icu4j.jar"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\icudt*l.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\hello.dex"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\framework.jar"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\framework-res.apk"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\build.prop"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\gesher-app.jar"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\binderd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\servicemanager.exe"; DestDir: "{app}"; Flags: ignoreversion
; gesher-exec (docs/exec.md): runs an app's ELF executables (ProcessBuilder) next to dalvikvm.
Source: "{#BuildDir}\gesher-exec.exe"; DestDir: "{app}"; Flags: ignoreversion
; dex2oat and profman (docs/compiled-code.md, ADR 0054): the device compiles apps at
; install and, with their JIT profiles, in the background.
Source: "{#BuildDir}\dex2oat.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\profman.exe"; DestDir: "{app}"; Flags: ignoreversion
; oatdump independently inspects this runtime's oat/vdex/app images (ADR 0065).
Source: "{#BuildDir}\oatdump.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\lifecycle.apk"; DestDir: "{app}"; Flags: ignoreversion
; Gesher Camera (ADR 0051): ACTION_IMAGE_CAPTURE's app, installed by the Gesher device when the PC has a camera.
Source: "{#BuildDir}\gesher-camera.apk"; DestDir: "{app}"; Flags: ignoreversion
; The boot image (docs/boot-image.md): the boot class path's classes, verified
; and linked by dex2oat, which dalvikvm maps instead of loading them one by one.
Source: "{#BuildDir}\framework\x86_64\*"; DestDir: "{app}\framework\x86_64"; Flags: ignoreversion
; ANGLE (docs/angle.md): OpenGL ES and EGL over Direct3D 11, BSD-licensed;
; its license and those of the code linked into it are in ANGLE-NOTICES.txt.
Source: "{#BuildDir}\libEGL.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libGLESv2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ANGLE-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; The canonical OpenSL interface identifiers retain their source notice.
Source: "OPENSL-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; The arm64 translator (docs/arm64.md, ADR 0047): dalvikvm LoadLibrary()s it to
; run arm64-v8a apps' native code. dynarmic is 0BSD (DYNARMIC-NOTICES.txt).
Source: "{#BuildDir}\gesher_arm64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DYNARMIC-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; The system fonts and fonts.xml (docs/fonts.md): Roboto and DroidSansMono
; (Apache 2.0), Noto Sans Hebrew and Noto Serif (OFL 1.1); gesher-app.ps1
; installs them as $ANDROID_ROOT/fonts and $ANDROID_ROOT/etc/fonts.xml.
Source: "{#BuildDir}\fonts\*"; DestDir: "{app}\fonts"; Flags: ignoreversion
Source: "{#BuildDir}\FONTS-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; Time zone data (docs/posix.md): the com.android.tzdata module's files,
; $ANDROID_TZDATA_ROOT for both launchers.
Source: "{#BuildDir}\tzdata\*"; DestDir: "{app}\tzdata"; Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\TZDATA-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; Networking (docs/network.md): Android's CA certificates, which dalvikvm joins
; with the roots Windows trusts as the system CAs ($ANDROID_ROOT/etc/security/cacerts),
; and the licenses of BoringSSL, Conscrypt, OkHttp and BouncyCastle.
Source: "{#BuildDir}\cacerts\*"; DestDir: "{app}\cacerts"; Flags: ignoreversion
Source: "{#BuildDir}\NET-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; Gesher's default ringtone, notification and alarm sounds (docs/media.md):
; gesher-app.ps1 installs them as $ANDROID_ROOT/media/audio.
Source: "{#BuildDir}\media\audio\*"; DestDir: "{app}\media\audio"; Flags: ignoreversion recursesubdirs
Source: "gesher.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher-app.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher-app.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "gesher.ico"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
; android.webkit.WebView runs on Microsoft Edge WebView2 (docs/webview.md):
; Windows 11 has its runtime; elsewhere Setup offers to install it (Microsoft's
; Evergreen bootstrapper, downloaded; a per-user install without elevation).
Name: "webview2"; Description: "Install the Microsoft Edge WebView2 Runtime (Android apps' web views need it)"; Check: not WebView2Installed

[Icons]
Name: "{autoprograms}\Gesher Hello"; Filename: "{app}\gesher.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\gesher.ico"
Name: "{autoprograms}\Gesher README"; Filename: "{app}\README.txt"

[Registry]
; The `.apk` association, per user (HKCU\Software\Classes). Gesher.Apk opens
; an APK with gesher-app. Gesher always joins `.apk`'s "Open with" list, but
; becomes the default only when no other ProgID owns `.apk` (ApkUnclaimed), and
; never touches Explorer's UserChoice. The uninstaller removes `.apk`'s default
; only while it still names Gesher.Apk (CurUninstallStepChanged).
Root: HKCU; Subkey: "Software\Classes\.apk"; ValueType: string; ValueName: ""; ValueData: "Gesher.Apk"; Check: ApkUnclaimed
Root: HKCU; Subkey: "Software\Classes\.apk\OpenWithProgids"; ValueType: string; ValueName: "Gesher.Apk"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\Gesher.Apk"; ValueType: string; ValueName: ""; ValueData: "Android app (Gesher)"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Gesher.Apk\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\gesher.ico,0"
Root: HKCU; Subkey: "Software\Classes\Gesher.Apk\shell\open"; ValueType: string; ValueName: ""; ValueData: "Run with Gesher"
Root: HKCU; Subkey: "Software\Classes\Gesher.Apk\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gesher-app.cmd"" ""%1"""
; Gesher's toast activator (ADR 0051): the CustomActivator of every app's
; AppUserModelID. A click on an app's toast when no Gesher device runs makes COM
; start `gesher-app toast-activated`, which starts the device and hands it the
; click (the notification's intent is sent).
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C18D9445-A7F0-472E-985F-AC354EBEB660}"; ValueType: string; ValueName: ""; ValueData: "Gesher toast activator"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C18D9445-A7F0-472E-985F-AC354EBEB660}\LocalServer32"; ValueType: string; ValueName: ""; ValueData: """{sys}\WindowsPowerShell\v1.0\powershell.exe"" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\gesher-app.ps1"" toast-activated"

[UninstallRun]
; The Gesher device (binderd, servicemanager, the system server and app
; processes, docs/services.md) runs from {app} and keeps files of
; {localappdata}\Gesher open: it is stopped first.
Filename: "{cmd}"; Parameters: "/d /c """"{app}\gesher-app.cmd"" shutdown"""; Flags: runhidden waituntilterminated; RunOnceId: "GesherDeviceShutdown"

[UninstallDelete]
; The runtime's directories (gesher.ps1), the installed packages and the logs.
Type: filesandordirs; Name: "{localappdata}\Gesher"
; The installed apps' Start menu shortcuts (gesher-app install; their icons are
; in the packages' data directories, above).
Type: filesandordirs; Name: "{userprograms}\Gesher Apps"

[Code]
const
  EnvKey = 'Environment';
  ApkKey = 'Software\Classes\.apk';
  ApkProgId = 'Gesher.Apk';

// True when `.apk` has no default ProgID, or Gesher's: HKCR is the merged
// per-user and per-machine view, so another app's machine-wide association
// counts too.
function ApkUnclaimed(): Boolean;
var
  ProgId: String;
begin
  if not RegQueryStringValue(HKEY_CLASSES_ROOT, '.apk', '', ProgId) then
    ProgId := '';
  Result := (ProgId = '') or (CompareText(ProgId, ApkProgId) = 0);
end;

// The Microsoft Edge WebView2 Runtime's registration (per machine or per user).
function WebView2Installed(): Boolean;
var
  Key, Version: String;
begin
  Key := 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  Result := False;
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) or
     RegQueryStringValue(HKEY_LOCAL_MACHINE, Key, 'pv', Version) or
     RegQueryStringValue(HKEY_CURRENT_USER, Key, 'pv', Version) then
    Result := (Version <> '') and (Version <> '0.0.0.0');
end;

// The WebView2 task: Microsoft's bootstrapper installs the Evergreen runtime.
procedure InstallWebView2();
var
  Code: Integer;
begin
  try
    DownloadTemporaryFile('https://go.microsoft.com/fwlink/p/?LinkId=2124703',
      'MicrosoftEdgeWebview2Setup.exe', '', nil);
    if not Exec(ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe'), '/silent /install', '',
                SW_HIDE, ewWaitUntilTerminated, Code) then
      Log('WebView2 bootstrapper did not start');
  except
    Log('WebView2 Runtime download failed: ' + GetExceptionMessage);
  end;
end;

// An update replaces files a running Gesher device uses: it is stopped first.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Launcher: String;
  Code: Integer;
begin
  Launcher := ExpandConstant('{app}\gesher-app.cmd');
  if FileExists(Launcher) then
    Exec(ExpandConstant('{cmd}'), '/d /c ""' + Launcher + '" shutdown"', '', SW_HIDE,
      ewWaitUntilTerminated, Code);
  Result := '';
end;

function PathEntries(): String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Result) then
    Result := '';
end;

function HasEntry(Path, Dir: String): Boolean;
begin
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';') > 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Path, Dir: String;
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('webview2') then
      InstallWebView2();
    Path := PathEntries();
    Dir := ExpandConstant('{app}');
    if not HasEntry(Path, Dir) then
    begin
      if (Path <> '') and (Copy(Path, Length(Path), 1) <> ';') then
        Path := Path + ';';
      RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Path + Dir);
    end;
  end;
end;

// The apps' AppUserModelIDs (Gesher.App.<package>, registered for their toasts by
// the system server, ADR 0038/0051) go with Gesher.
procedure RemoveAppIds();
var
  Names: TArrayOfString;
  I: Integer;
begin
  if RegGetSubkeyNames(HKEY_CURRENT_USER, 'Software\Classes\AppUserModelId', Names) then
    for I := 0 to GetArrayLength(Names) - 1 do
      if Pos('Gesher.App.', Names[I]) = 1 then
        RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\AppUserModelId\' + Names[I]);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, Dir: String;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveAppIds();
    if RegQueryStringValue(HKEY_CURRENT_USER, ApkKey, '', Path) and
       (CompareText(Path, ApkProgId) = 0) then
      RegDeleteValue(HKEY_CURRENT_USER, ApkKey, '');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, ApkKey);
    Path := PathEntries();
    Dir := ExpandConstant('{app}');
    if HasEntry(Path, Dir) then
    begin
      Path := ';' + Path + ';';
      P := Pos(';' + Uppercase(Dir) + ';', Uppercase(Path));
      Delete(Path, P, Length(Dir) + 1);
      Path := Copy(Path, 2, Length(Path) - 2);
      RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Path);
    end;
  end;
end;

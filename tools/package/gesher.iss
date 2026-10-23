; Gesher's Windows installer (Inno Setup 6), built in CI by the package job:
;   iscc /DBuildDir=<dir with the build output> /DAppVersion=<x.y.z> gesher.iss
; Installs per user (no administrator rights) into
; %LOCALAPPDATA%\Programs\Gesher, puts `gesher` and `gesher-app` on the user's
; PATH and adds a Start menu entry that runs the bundled Hello.

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
OutputBaseFilename=gesher-setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Gesher {#AppVersion}

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
Source: "{#BuildDir}\lifecycle.apk"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher-app.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "gesher-app.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\Gesher Hello"; Filename: "{app}\gesher.cmd"; WorkingDir: "{app}"
Name: "{autoprograms}\Gesher README"; Filename: "{app}\README.txt"

[UninstallDelete]
; The runtime's directories (gesher.ps1).
Type: filesandordirs; Name: "{localappdata}\Gesher"

[Code]
const
  EnvKey = 'Environment';

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

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, Dir: String;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
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

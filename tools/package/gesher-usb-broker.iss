; The optional machine-wide USB authority. Build with Inno Setup 6.7:
;   ISCC /DBuildDir=<verified build outputs> /DAppVersion=<version> gesher-usb-broker.iss

#ifndef BuildDir
  #error Pass /DBuildDir=<verified build output directory>
#endif
#ifndef FilesystemToolsDir
  #error Pass /DFilesystemToolsDir=<verified optional filesystem-tools directory>
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define BrokerDir "{commonpf}\Gesher Raw USB Broker"
#define RuntimeDir BrokerDir + "\runtime"
#define BrokerExe "gesher-usb-authority.exe"
#define BrokerSource AddBackslash(BuildDir) + BrokerExe
#define BrokerSha256 GetSHA256OfFile(BrokerSource)

[Setup]
AppId={{D1A18A54-DB1F-44C8-BD7E-1402AF2E9531}
AppName=Gesher Raw USB Broker
AppVersion={#AppVersion}
AppPublisher=Gesher
DefaultDirName={#BrokerDir}
UninstallFilesDir={#BrokerDir}
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
RedirectionGuard=yes
OutputBaseFilename=gesher-usb-broker-setup-{#AppVersion}
WizardStyle=modern
SetupIconFile=gesher.ico

[Files]
Source: "{#BrokerSource}"; DestDir: "{#BrokerDir}"; Flags: ignoreversion
#define EmitRuntimeFile(str Source, str Subdir) \
  'Source: "' + Source + '"; DestDir: "' + RuntimeDir + Subdir + '"; Flags: ignoreversion'
#include "usb-broker-runtime-files.iss"
#undef EmitRuntimeFile

[Dirs]
Name: "{#RuntimeDir}\art"

[UninstallDelete]
Type: dirifempty; Name: "{#BrokerDir}"

[Code]
const
  BrokerBinary = '{#BrokerExe}';
  BrokerDigest = '{#BrokerSha256}';
  InvalidAttributes = $FFFFFFFF;
  DirectoryAttribute = $10;
  ReparseAttribute = $400;
  // Owner, DACL, and service identity checks must succeed even on custom ACLs.
  MutableRights = $500D0150;  // Generic write/all, delete, WRITE_DAC/OWNER, delete-child, write attributes.
  CreateRights = $6;         // Write data/add file and append data/add subdirectory.

type
  TAclSize = record
    AceCount: Cardinal;
    BytesInUse: Cardinal;
    BytesFree: Cardinal;
  end;
  TAce = record
    Kind: Byte;
    Flags: Byte;
    Size: Word;
    Mask: Cardinal;
  end;
  TTokenOwner = record
    Sid: Cardinal;
  end;

// Inno Setup 6's installer and script engine are 32-bit, including in 64-bit install mode.
function GetFileAttributesW(Path: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function GetDriveTypeW(Path: String): Cardinal;
  external 'GetDriveTypeW@kernel32.dll stdcall';
function GetCurrentProcess(): Cardinal;
  external 'GetCurrentProcess@kernel32.dll stdcall';
function CloseHandle(Handle: Cardinal): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function OpenProcessToken(Process, Access: Cardinal; var Token: Cardinal): Boolean;
  external 'OpenProcessToken@advapi32.dll stdcall';
function SetTokenInformation(Token, InfoClass: Cardinal; var Info: TTokenOwner; Size: Cardinal): Boolean;
  external 'SetTokenInformation@advapi32.dll stdcall';
function GetNamedSecurityInfoW(Path: String; ObjectType, Requested: Cardinal;
  var Owner: Cardinal; Group: Cardinal; var Dacl: Cardinal;
  Sacl: Cardinal; var Descriptor: Cardinal): Cardinal;
  external 'GetNamedSecurityInfoW@advapi32.dll stdcall';
function GetAclInformation(Acl: Cardinal; var Info: TAclSize; Size, InfoClass: Cardinal): Boolean;
  external 'GetAclInformation@advapi32.dll stdcall';
function GetAce(Acl, Index: Cardinal; var Entry: Cardinal): Boolean;
  external 'GetAce@advapi32.dll stdcall';
procedure ReadAce(var Ace: TAce; Entry, Size: Cardinal);
  external 'RtlMoveMemory@kernel32.dll stdcall';
function ConvertStringSidToSidW(Text: String; var Sid: Cardinal): Boolean;
  external 'ConvertStringSidToSidW@advapi32.dll stdcall';
function EqualSid(First, Second: Cardinal): Boolean;
  external 'EqualSid@advapi32.dll stdcall';
function LocalFree(Memory: Cardinal): Cardinal;
  external 'LocalFree@kernel32.dll stdcall';

var
  RegistrationFailed: Boolean;

function BrokerDirectory(): String;
begin
  Result := ExpandConstant('{#BrokerDir}');
end;

function SameSid(Sid: Cardinal; Text: String): Boolean;
var
  Expected: Cardinal;
begin
  Result := False;
  if (Sid = 0) or not ConvertStringSidToSidW(Text, Expected) then
    Exit;
  try
    Result := EqualSid(Sid, Expected);
  finally
    LocalFree(Expected);
  end;
end;

function TrustedSid(Sid: Cardinal): Boolean;
begin
  Result := SameSid(Sid, 'S-1-5-18') or SameSid(Sid, 'S-1-5-32-544') or
    SameSid(Sid, 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464');
end;

function ProtectOutputOwnership(): Boolean;
var
  Token: Cardinal;
  Owner: TTokenOwner;
begin
  Result := False;
  // An elevated individual SID as NTFS owner could change the ACL again at medium
  // integrity. Use Administrators as the default owner for all Setup outputs.
  if not ConvertStringSidToSidW('S-1-5-32-544', Owner.Sid) then
    Exit;
  try
    if not OpenProcessToken(GetCurrentProcess(), $80, Token) then  // TOKEN_ADJUST_DEFAULT
      Exit;
    try
      Result := SetTokenInformation(Token, 4, Owner, SizeOf(Owner));  // TokenOwner
    finally
      CloseHandle(Token);
    end;
  finally
    LocalFree(Owner.Sid);
  end;
end;

function ProtectedNode(Path: String; Directory, ProtectCreation: Boolean): Boolean;
var
  Attributes, Owner, Dacl, Descriptor, Entry, Rights: Cardinal;
  Acl: TAclSize;
  Ace: TAce;
  Index: Integer;
begin
  Result := False;
  Attributes := GetFileAttributesW(Path);
  if (Attributes = InvalidAttributes) or ((Attributes and ReparseAttribute) <> 0) or
      (((Attributes and DirectoryAttribute) <> 0) <> Directory) then
    Exit;
  // SE_FILE_OBJECT; OWNER_SECURITY_INFORMATION or DACL_SECURITY_INFORMATION.
  if GetNamedSecurityInfoW(Path, 1, 5, Owner, 0, Dacl, 0, Descriptor) <> 0 then
    Exit;
  try
    if not TrustedSid(Owner) or (Dacl = 0) or not GetAclInformation(Dacl, Acl, SizeOf(Acl), 2) then
      Exit;
    Rights := MutableRights;
    if ProtectCreation then
      Rights := Rights or CreateRights;
    for Index := 0 to Integer(Acl.AceCount) - 1 do
    begin
      if not GetAce(Dacl, Index, Entry) then
        Exit;
      ReadAce(Ace, Entry, SizeOf(Ace));
      if (Ace.Flags and $8) <> 0 then  // INHERIT_ONLY_ACE
        Continue;
      if Ace.Kind = 1 then  // ACCESS_DENIED_ACE_TYPE
        Continue;
      // Allow only ordinary allow/deny ACLs. A standard allow ACE's SID follows the mask.
      if (Ace.Kind <> 0) or (Ace.Size < 16) then
        Exit;
      if ((Ace.Mask and Rights) <> 0) and not TrustedSid(Entry + SizeOf(Ace)) then
        Exit;
    end;
    Result := True;
  finally
    LocalFree(Descriptor);
  end;
end;

function ProtectedProgramFiles(): Boolean;
var
  Drive, Parent, Child: String;
begin
  Result := False;
  Child := ExpandConstant('{commonpf}');
  Drive := AddBackslash(ExtractFileDrive(Child));
  // Local fixed drive. The ancestors may let users create unrelated siblings,
  // but must prevent deletion/replacement of this existing protected path.
  if (Length(Drive) <> 3) or (GetDriveTypeW(Drive) <> 3) or
      not ProtectedNode(Child, True, True) then
    Exit;
  while CompareText(Child, Drive) <> 0 do
  begin
    Parent := ExtractFileDir(Child);
    if (Parent = '') or (CompareText(Parent, Child) = 0) or
        not ProtectedNode(Parent, True, False) then
      Exit;
    Child := Parent;
  end;
  Result := True;
end;

function FixedDestination(): Boolean;
begin
  // The compiler's fixed DestDir also keeps [Files] independent of /DIR.
  Result := IsAdminInstallMode and (CompareText(ExpandConstant('{app}'), BrokerDirectory()) = 0) and
    ProtectedProgramFiles();
end;

function EmptyDirectory(Path: String): Boolean;
var
  Item: TFindRec;
begin
  Result := False;
  if not FindFirst(Path + '\*', Item) then
    Exit;
  try
    repeat
      if (Item.Name <> '.') and (Item.Name <> '..') then
        Exit;
    until not FindNext(Item);
    Result := True;
  finally
    FindClose(Item);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Path: String;
begin
  Result := 'Setup requires its fixed, protected Program Files destination.';
  if not FixedDestination() or not ProtectOutputOwnership() then
    Exit;
  Path := BrokerDirectory();
  if GetFileAttributesW(Path) = InvalidAttributes then
  begin
    if (DLLGetLastError() <> 2) or not CreateDir(Path) then
      Exit;
  end;
  if not ProtectedNode(Path, True, True) then
    Exit;
  if not EmptyDirectory(Path) then
  begin
    Result := 'Uninstall the previous Gesher Raw USB Broker before installing this package.';
    Exit;
  end;
  Result := '';
end;

function RunBroker(Verb: String): Boolean;
var
  Dir, Exe: String;
  Code: Integer;
begin
  Result := False;
  if not FixedDestination() then
    Exit;
  Dir := BrokerDirectory();
  Exe := Dir + '\' + BrokerBinary;
  if not ProtectedNode(Dir, True, True) or not ProtectedNode(Exe, False, True) or
      (CompareText(GetSHA256OfFile(Exe), BrokerDigest) <> 0) then
    Exit;
  Code := -1;
  Result := Exec(Exe, Verb, Dir, SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  Log('USB authority ' + Verb + ': exit code ' + IntToStr(Code));
end;

function ProtectedRuntimeFile(Root, Relative, Digest: String): Boolean;
var
  FilePath, Parent: String;
begin
  Result := False;
  FilePath := Root + Relative;
  Parent := ExtractFileDir(FilePath);
  while CompareText(Parent, Root) <> 0 do
  begin
    if not ProtectedNode(Parent, True, True) then
    begin
      Log('Protected runtime directory failed verification: ' + Parent);
      Exit;
    end;
    Parent := ExtractFileDir(Parent);
  end;
  if ProtectedNode(FilePath, False, True) then
    Result := CompareText(GetSHA256OfFile(FilePath), Digest) = 0;
  if not Result then
    Log('Protected runtime file failed verification: ' + Relative);
end;

function VerifyRuntime(): Boolean;
var
  Root: String;
begin
  Result := False;
  Root := BrokerDirectory() + '\runtime';
  if not FixedDestination() or not ProtectedNode(BrokerDirectory(), True, True) or
     not ProtectedNode(Root, True, True) or not ProtectedNode(Root + '\art', True, True) then
    Exit;
#define EmitRuntimeFile(str Source, str Subdir) \
  "  if not ProtectedRuntimeFile(Root, '" + Subdir + "\" + ExtractFileName(Source) + \
  "', '" + GetSHA256OfFile(Source) + "') then Exit;"
#include "usb-broker-runtime-files.iss"
#undef EmitRuntimeFile
  Result := True;
end;

procedure CurStepChanged(Step: TSetupStep);
begin
  if Step = ssPostInstall then
  begin
    RegistrationFailed := True;
    if VerifyRuntime() then
      RegistrationFailed := not RunBroker('install-service');
    if RegistrationFailed then
      SuppressibleMsgBox('The protected runtime could not be verified or the USB authority service could not be registered. The protected files can be removed through Windows Installed apps.',
        mbError, MB_OK, IDOK);
  end;
end;

function GetCustomSetupExitCode(): Integer;
begin
  Result := 0;
  if RegistrationFailed then
    Result := 20;
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
begin
  if (Step = usUninstall) and not RunBroker('uninstall-service') then
  begin
    SuppressibleMsgBox('The USB authority service could not be safely removed. No broker files will be deleted.',
      mbError, MB_OK, IDOK);
    Abort();
  end;
end;

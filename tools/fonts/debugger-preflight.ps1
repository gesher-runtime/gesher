# CI-only harmless debugger prerequisite. Never an ownership/host/full gate.
# Flags: https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/cdb-command-line-options
# Extension path: https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-extpath--set-extension-path-
param([Parameter(Mandatory)][string]$ProbeDirectory)
$ErrorActionPreference='Stop'

function Assert-OrdinaryWindowsPath([string]$Path) {
 # Deliberately lexical: these functions are also tested on Linux pwsh.
 if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Length -gt 240 -or
     $Path -cnotmatch '^[A-Za-z]:\\[^\\]' -or $Path -match '[\x00-\x1f"<>|?*;/]' -or
     $Path.Substring(2).Contains(':')) { throw 'UNAVAILABLE: nonordinary Windows absolute path' }
 foreach($part in $Path.Substring(3).Split('\')) {
  if (!$part -or $part -in @('.','..') -or $part -match '[. ]$' -or
      $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') { throw 'UNAVAILABLE: ambiguous path component' }
 }
 $Path
}
function Join-WindowsLiteral([string]$Root,[string]$Leaf) {
 $null=Assert-OrdinaryWindowsPath $Root
 Assert-OrdinaryWindowsPath ($Root+'\'+$Leaf)
}
function Select-UniqueDebugger([string[]]$Candidates) {
 $unique=@($Candidates|ForEach-Object{Assert-OrdinaryWindowsPath $_}|Sort-Object -Unique)
 if($unique.Count -ne 1){throw 'UNAVAILABLE: missing or ambiguous registered x64 CDB'}
 $unique[0]
}
function New-PreflightCommands([string]$ExtensionDirectory) {
 $null=Assert-OrdinaryWindowsPath $ExtensionDirectory
 if($ExtensionDirectory.IndexOfAny([char[]]'$%`') -ge 0){throw 'UNAVAILABLE: command path expansion metacharacter'}
 # No extension is invoked. The initial absolute -a also confines default DLL selection.
 @(('.extpath '+$ExtensionDirectory),
   '.symopt- 0x00000040','.symopt+ 0x00000400','.symopt', '.echo FONT_DEBUGGER_MODULES',
   'lmv m ntdll','lmv m ucrtbase',
   '.reload /f ntdll.dll','.reload /f ucrtbase.dll',
   '.echo FONT_DEBUGGER_PUBLIC_OBSERVATION',
   'x ntdll!RtlEnterCriticalSection','x ucrtbase!*putenv_s*',
   '.echo FONT_DEBUGGER_PRIVATE_OBSERVATION_PENDING',
   'x ucrtbase!__acrt_lock_table','qd') -join "`n"
}
function New-PreflightArguments([int]$OwnedProcessId,[string]$CommandFile,[string]$ImageDirectory,[string]$CacheDirectory,[string]$ExtensionDirectory) {
 if($OwnedProcessId -le 0){throw 'UNAVAILABLE: invalid owned PID'}
 foreach($path in @($CommandFile,$ImageDirectory,$CacheDirectory,$ExtensionDirectory)){$null=Assert-OrdinaryWindowsPath $path}
 @('-pv','-p',$OwnedProcessId.ToString([Globalization.CultureInfo]::InvariantCulture),
   '-noshell','-sins','-nosqm','-ses','-sflags','0x00081600',
   '-i',$ImageDirectory,'-y',('srv*'+$CacheDirectory+'*https://msdl.microsoft.com/download/symbols'),
   ('-a'+$ExtensionDirectory+'\ext'),'-cf',$CommandFile)
}
function New-PreflightStartInfo([string]$Executable,[string]$WorkingDirectory,[string]$WindowsDirectory,[string]$TempDirectory,[string[]]$Arguments,[bool]$Probe) {
 foreach($path in @($Executable,$WorkingDirectory,$WindowsDirectory,$TempDirectory)){$null=Assert-OrdinaryWindowsPath $path}
 $psi=[Diagnostics.ProcessStartInfo]::new($Executable)
 $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.WorkingDirectory=$WorkingDirectory
 $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true;$psi.RedirectStandardInput=$true
 $psi.Environment.Clear()
 foreach($entry in @{SystemRoot=$WindowsDirectory;WINDIR=$WindowsDirectory;TEMP=$TempDirectory;TMP=$TempDirectory;PATH=($WindowsDirectory+'\System32');GITHUB_ACTIONS='true'}.GetEnumerator()){$psi.Environment[$entry.Key]=$entry.Value}
 if($Probe){$psi.Environment['FONT_DEBUGGER_PREFLIGHT_CANARY']='probe-v1'}
 foreach($argument in $Arguments){$psi.ArgumentList.Add($argument)}
 $psi
}
function Assert-PreflightRequest($Info,[string]$Executable,[string]$WorkingDirectory,[int]$OwnedProcessId,[string]$CommandFile,[string]$ImageDirectory,[string]$CacheDirectory,[string]$ExtensionDirectory,[string]$CommandText) {
 $expected=@(New-PreflightArguments $OwnedProcessId $CommandFile $ImageDirectory $CacheDirectory $ExtensionDirectory)
 if($Info.FileName -cne $Executable -or $Info.WorkingDirectory -cne $WorkingDirectory -or $Info.UseShellExecute -or !$Info.CreateNoWindow -or
    !$Info.RedirectStandardInput -or !$Info.RedirectStandardOutput -or !$Info.RedirectStandardError -or $Info.Arguments -cne '' -or $Info.ArgumentList.Count -ne $expected.Count){throw 'UNAVAILABLE: launch metadata changed'}
 for($i=0;$i -lt $expected.Count;$i++){if($Info.ArgumentList[$i] -cne $expected[$i]){throw 'UNAVAILABLE: fixed debugger arguments changed'}}
 if($CommandText -cne ((New-PreflightCommands $ExtensionDirectory)+"`n")){throw 'UNAVAILABLE: fixed command allowlist changed'}
}
function Assert-PreflightRawMarkers([string]$Text) {
 # Only exact .echo handshakes, never a symbol/PDB/owner parser.
 foreach($marker in @('FONT_DEBUGGER_MODULES','FONT_DEBUGGER_PUBLIC_OBSERVATION','FONT_DEBUGGER_PRIVATE_OBSERVATION_PENDING')){
  if(@($Text -split "`r?`n"|Where-Object{$_ -ceq $marker}).Count -ne 1){throw 'UNAVAILABLE: missing/duplicate command-file handshake'}
 }
}
function Emit-PreflightRecord([string]$Stage,$Value) {
 $payload=ConvertTo-Json -InputObject $Value -Depth 12 -Compress -EscapeHandling EscapeNonAscii
 if($payload.Length -gt 4000000){throw 'Record bound exceeded'}
 $digest=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($payload))).ToLowerInvariant()
 $count=[Math]::Max(1,[int][Math]::Ceiling($payload.Length/320.0))
 for($i=0;$i -lt $count;$i++){$offset=$i*320;Write-Host ($Stage+' '+(@{record_sha256=$digest;part=$i+1;parts=$count;payload=$payload.Substring($offset,[Math]::Min(320,$payload.Length-$offset))}|ConvertTo-Json -Compress -EscapeHandling EscapeNonAscii))}
}
function Resolve-PreflightLiteral([string]$Path,[bool]$Directory=$false) {
 if($env:GITHUB_ACTIONS -cne 'true' -or !$IsWindows){throw 'CI Windows only'}
 $null=Assert-OrdinaryWindowsPath $Path
 $cursor=$Path
 while($cursor){
  $item=Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
  if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'UNAVAILABLE: reparse path'}
  if($cursor.Length -le 3){break}
  $cursor=[IO.Path]::GetDirectoryName($cursor)
 }
 $item=Get-Item -LiteralPath $Path -Force -ErrorAction Stop
 if($item.PSIsContainer -ne $Directory -or $item.FullName -ine $Path){throw 'UNAVAILABLE: literal file/directory mismatch'}
 $item.FullName
}
function Get-PreflightFileProfile([string]$Path,[bool]$Microsoft=$true) {
 $path=Resolve-PreflightLiteral $Path
 $item=Get-Item -LiteralPath $path
 if([string]::IsNullOrWhiteSpace($item.VersionInfo.FileVersion) -or [string]::IsNullOrWhiteSpace($item.VersionInfo.ProductVersion)){throw 'UNAVAILABLE: no exact disk file/product version'}
 $record=@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();file_version=$item.VersionInfo.FileVersion;product_version=$item.VersionInfo.ProductVersion;scope='DISK_AT_LOADED_OR_APPROVED_PATH_NOT_MEMORY_ATTESTATION'}
 if($Microsoft){
  $sig=Get-AuthenticodeSignature -LiteralPath $path
  if($sig.Status -ne 'Valid' -or !$sig.SignerCertificate -or $sig.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:,|$)'){throw 'UNAVAILABLE: valid Microsoft signature required'}
  $record.signature_status=$sig.Status.ToString();$record.signer=$sig.SignerCertificate.Subject;$record.signer_thumbprint=$sig.SignerCertificate.Thumbprint
 }
 $record
}
function Get-RegisteredDebugger {
 $candidates=[Collections.Generic.List[string]]::new()
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
  try{
   $key=$base.OpenSubKey('SOFTWARE\Microsoft\Windows Kits\Installed Roots')
   if($key){try{foreach($name in @('KitsRoot10','KitsRoot81')){
    $root=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if($null -ne $root){
     if($root -isnot [string]){throw 'UNAVAILABLE: invalid registry SDK root'}
     $root=$root.TrimEnd('\');$root=Resolve-PreflightLiteral $root $true
     $candidate=Join-WindowsLiteral $root 'Debuggers\x64\cdb.exe'
     if(Test-Path -LiteralPath $candidate){$candidates.Add((Resolve-PreflightLiteral $candidate))}
    }
   }}finally{$key.Dispose()}}
  }finally{$base.Dispose()}
 }
 Select-UniqueDebugger $candidates.ToArray()
}
function New-BoundedPipe($Reader) {
 $state=@{Reader=$Reader;Buffer=[char[]]::new(2048);Text=[Text.StringBuilder]::new();Task=$null;Closed=$false}
 $state.Task=$Reader.ReadAsync($state.Buffer,0,$state.Buffer.Length)
 $state
}
function Update-BoundedPipe($State) {
 if(!$State.Closed -and $State.Task.IsCompleted){
  $n=$State.Task.GetAwaiter().GetResult()
  if($n -eq 0){$State.Closed=$true;return}
  if($State.Text.Length+$n -gt 262144){throw 'UNAVAILABLE: pipe output overflow'}
  $null=$State.Text.Append($State.Buffer,0,$n)
  $State.Task=$State.Reader.ReadAsync($State.Buffer,0,$State.Buffer.Length)
 }
}
function Assert-OwnedAlive($Process,[int]$OwnedProcessId,$Handle) {
 if($Process.Id -ne $OwnedProcessId -or $Handle.IsClosed -or $Handle.IsInvalid -or $Process.WaitForExit(0)){throw 'UNAVAILABLE: exact owned child no longer alive'}
}
function Stop-ExactOwnedProcess($Process) {
 if($null -ne $Process){
  try{if(!$Process.WaitForExit(0)){$Process.Kill();if(!$Process.WaitForExit(2000)){throw 'Owned cleanup did not complete'}}}finally{
   foreach($stream in @('StandardInput','StandardOutput','StandardError')){try{$Process.$stream.Dispose()}catch{}}
   $Process.Dispose()
  }
 }
}

# No process, registry lookup, fixture or verifier starts before this gate.
if($env:GITHUB_ACTIONS -cne 'true' -or !$IsWindows){
 Emit-PreflightRecord 'FONT_DEBUGGER_PREFLIGHT_SUMMARY' @{tool_request_and_detach_completed=$false;font_owner_qualified=$false;host_WCT_Windows2025_qualified=$false;private_CRT_owner_symbols_qualified=$false;full_qualification=$false;reason='Windows CI only'}
 throw 'Debugger prerequisite execution requires Windows CI'
}
$clock=[Diagnostics.Stopwatch]::StartNew()
$child=$null;$debugger=$null;$childHandle=$null;$debuggerHandle=$null;$work=$null;$ok=$false
try {
 Emit-PreflightRecord 'FONT_DEBUGGER_HOST_PROFILE' @{os_version=[Environment]::OSVersion.VersionString;framework=[Runtime.InteropServices.RuntimeInformation]::FrameworkDescription;powershell=$PSVersionTable.PSVersion.ToString();runner_os=$env:RUNNER_OS;runner_arch=$env:RUNNER_ARCH;image_os=$env:ImageOS;image_version=$env:ImageVersion;source=$env:GITHUB_SHA;run=$env:GITHUB_RUN_ID;attempt=$env:GITHUB_RUN_ATTEMPT;diagnostic_only=$true}
 $probeRoot=Resolve-PreflightLiteral $ProbeDirectory $true
 python tools/fonts/debugger-probe.py verify --root $probeRoot
 if($LASTEXITCODE -ne 0){throw 'UNAVAILABLE: current-source/current-attempt probe provenance'}
 $probe=Resolve-PreflightLiteral ($probeRoot+'\probe.exe')
 $tool=Get-RegisteredDebugger
 $sdk=[IO.Path]::GetDirectoryName($tool);$extension=Resolve-PreflightLiteral ($sdk+'\winext') $true
 $checkout=Resolve-PreflightLiteral $env:GITHUB_WORKSPACE $true
 foreach($dir in @($sdk,$extension)){
  if($dir.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase) -or $dir -ieq $checkout){throw 'UNAVAILABLE: SDK inside checkout'}
  $files=@(Get-ChildItem -LiteralPath $dir -File -Force|Where-Object{$_.Extension -in @('.dll','.exe')})
  if($files.Count -eq 0 -or $files.Count -gt 128){throw 'UNAVAILABLE: SDK dependency inventory bound'}
  foreach($file in $files){Emit-PreflightRecord 'FONT_DEBUGGER_TOOL_DISK_PROFILE' (Get-PreflightFileProfile $file.FullName)}
 }
 foreach($required in @('cdb.exe','dbgeng.dll','dbghelp.dll','symsrv.dll')){$null=Resolve-PreflightLiteral ($sdk+'\'+$required)}
 $null=Resolve-PreflightLiteral ($extension+'\ext.dll')
 $windows=Resolve-PreflightLiteral ([Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)) $true
 $system=Resolve-PreflightLiteral ($windows+'\System32') $true
 $temp=Resolve-PreflightLiteral ([IO.Path]::GetTempPath().TrimEnd('\')) $true
 if($temp -ieq $checkout -or $temp.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'UNAVAILABLE: temporary root inside checkout'}
 $work=$temp+'\font-debugger-'+[guid]::NewGuid().ToString('N')
 $null=New-Item -ItemType Directory -Path $work -ErrorAction Stop
 $work=Resolve-PreflightLiteral $work $true
 foreach($leaf in @('child','debugger','cache','commands','temp')){
  $null=New-Item -ItemType Directory -Path ($work+'\'+$leaf) -ErrorAction Stop
  $null=Resolve-PreflightLiteral ($work+'\'+$leaf) $true
  if(@(Get-ChildItem -LiteralPath ($work+'\'+$leaf) -Force).Count){throw 'UNAVAILABLE: nonempty owned directory'}
 }
 $commandFile=$work+'\commands\preflight.txt'
 $commands=(New-PreflightCommands $extension)+"`n"
 $commandBytes=[Text.Encoding]::ASCII.GetBytes($commands)
 # SDK paths with BMP characters cannot be losslessly represented by this ASCII command file.
 if([Text.Encoding]::ASCII.GetString($commandBytes) -cne $commands){throw 'UNAVAILABLE: command path not ASCII'}
 # Finish the writer first; a retained read-only/ShareRead handle then permits
 # hash/CDB readers while denying writers/deletion. Retaining ReadWrite here
 # conflicts with those readers' reciprocal Windows sharing mode.
 $commandWriter=[IO.File]::Open($commandFile,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
 try{$commandWriter.Write($commandBytes,0,$commandBytes.Length);$commandWriter.Flush($true)}finally{$commandWriter.Dispose()}
 $commandStream=[IO.File]::Open($commandFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try {
  $commandHash=(Get-FileHash -LiteralPath $commandFile -Algorithm SHA256).Hash.ToLowerInvariant()
  Emit-PreflightRecord 'FONT_DEBUGGER_COMMAND_PLAN' @{sha256=$commandHash;commands=$commands;symbols='OBSERVATION_PENDING_RAW_ONLY';suspension_is_real_wait=$false}
  $psi=New-PreflightStartInfo $probe ($work+'\child') $windows ($work+'\temp') @() $true
  if($clock.ElapsedMilliseconds -ge 90000){throw 'UNAVAILABLE: prerequisite setup exceeded start budget'}
  $child=[Diagnostics.Process]::new();$child.StartInfo=$psi
  if(!$child.Start()){throw 'UNAVAILABLE: probe start failed'}
  $childId=$child.Id;$childHandle=$child.SafeHandle;$null=$childHandle.DangerousGetHandle()
  $out=New-BoundedPipe $child.StandardOutput;$err=New-BoundedPipe $child.StandardError
  $deadline=[Math]::Min(240000,$clock.ElapsedMilliseconds+10000)
  while($out.Text.ToString() -cnotmatch '^FONT_DEBUGGER_PREFLIGHT_READY\r?\n$'){
   Update-BoundedPipe $out;Update-BoundedPipe $err
   Assert-OwnedAlive $child $childId $childHandle
   if($clock.ElapsedMilliseconds -ge $deadline -or $err.Text.Length -or $out.Text.Length -gt 40){throw 'UNAVAILABLE: probe READY failure/timeout'}
   Start-Sleep -Milliseconds 10
  }
  Assert-OwnedAlive $child $childId $childHandle
  $child.Refresh();$modules=@($child.Modules)
  if($modules.Count -gt 128){throw 'UNAVAILABLE: child module bound'}
  $providers=@{}
  foreach($module in $modules){
   $path=Resolve-PreflightLiteral $module.FileName
   if($module.ModuleName -in @('ntdll.dll','ucrtbase.dll')){
    if($providers.ContainsKey($module.ModuleName)){throw 'UNAVAILABLE: duplicate child provider'}
    $providers[$module.ModuleName]=$path
    Emit-PreflightRecord 'FONT_DEBUGGER_CHILD_LOADED_MODULE_DISK_PROFILE' (Get-PreflightFileProfile $path)
   }
  }
  if(!$providers.ContainsKey('ucrtbase.dll') -or !$providers.ContainsKey('ntdll.dll')){throw 'UNAVAILABLE: actual child CRT/ntdll not loaded'}
  # These paths came from the owned child's actual module list, never a guessed provider.
  $images=@($providers.Values|ForEach-Object{[IO.Path]::GetDirectoryName($_)}|Sort-Object -Unique)
  if($images.Count -ne 1 -or $images[0] -ine $system){throw 'UNAVAILABLE: actual provider image directory outside approved System32'}
  $arguments=New-PreflightArguments $childId $commandFile $images[0] ($work+'\cache') $extension
  Assert-OwnedAlive $child $childId $childHandle
  $dpsi=New-PreflightStartInfo $tool ($work+'\debugger') $windows ($work+'\temp') $arguments $false
  Assert-PreflightRequest $dpsi $tool ($work+'\debugger') $childId $commandFile $images[0] ($work+'\cache') $extension $commands
  Emit-PreflightRecord 'FONT_DEBUGGER_REQUEST' @{owned_pid=$childId;executable=$tool;arguments=$arguments;environment_keys=@($dpsi.Environment.Keys|Sort-Object);working_directory=$dpsi.WorkingDirectory}
  $debugger=[Diagnostics.Process]::new();$debugger.StartInfo=$dpsi
  if(!$debugger.Start()){throw 'UNAVAILABLE: CDB start failed'}
  $debuggerId=$debugger.Id;$debuggerHandle=$debugger.SafeHandle;$null=$debuggerHandle.DangerousGetHandle()
  $dout=New-BoundedPipe $debugger.StandardOutput;$derr=New-BoundedPipe $debugger.StandardError
  $debugger.StandardInput.Close()
  $deadline=[Math]::Min(240000,$clock.ElapsedMilliseconds+120000);$loadedProfiled=$false
  while(!$debugger.WaitForExit(0) -or !$dout.Closed -or !$derr.Closed){
   Update-BoundedPipe $dout;Update-BoundedPipe $derr;Update-BoundedPipe $err
   if(!$loadedProfiled -and !$debugger.WaitForExit(0)){
    $loadedPaths=@()
    try{
     $debugger.Refresh();$loadedPaths=@($debugger.Modules|ForEach-Object{$_.FileName})
    }catch{
     if(!$debugger.WaitForExit(0)){throw}
     Emit-PreflightRecord 'FONT_DEBUGGER_MODULE_PROFILE_UNAVAILABLE' @{reason='process exited during module snapshot';diagnostic_only=$true}
    }
    if($loadedPaths.Count -gt 128){throw 'UNAVAILABLE: debugger module bound'}
    foreach($loadedPath in $loadedPaths){
     $path=Resolve-PreflightLiteral $loadedPath
     $dir=[IO.Path]::GetDirectoryName($path)
     if($dir -ine $sdk -and $dir -ine $extension -and $dir -ine $system){throw 'UNAVAILABLE: loaded debugger dependency outside approved directories'}
     Emit-PreflightRecord 'FONT_DEBUGGER_LOADED_MODULE_DISK_PROFILE' (Get-PreflightFileProfile $path)
    }
    $loadedProfiled=$true
   }
   if($debugger.Id -ne $debuggerId -or $debuggerHandle.IsClosed -or $debuggerHandle.IsInvalid){throw 'UNAVAILABLE: owned debugger identity changed'}
   if($clock.ElapsedMilliseconds -ge $deadline){throw 'UNAVAILABLE: CDB timeout/pipe drain'}
   Start-Sleep -Milliseconds 10
  }
  if(!$loadedProfiled){Emit-PreflightRecord 'FONT_DEBUGGER_MODULE_PROFILE_UNAVAILABLE' @{reason='process exited before snapshot';diagnostic_only=$true}}
  Emit-PreflightRecord 'FONT_DEBUGGER_RAW_OBSERVATION' @{stdout=$dout.Text.ToString();stderr=$derr.Text.ToString();symbol_availability='PENDING_RAW_REVIEW';private_symbols_prove_owner=$false}
  if($debugger.ExitCode -ne 0){throw 'UNAVAILABLE: CDB nonzero exit'}
  Assert-PreflightRawMarkers $dout.Text.ToString()
  if((Get-FileHash -LiteralPath $commandFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $commandHash){throw 'UNAVAILABLE: command bytes changed'}
  Assert-OwnedAlive $child $childId $childHandle
  $child.StandardInput.Write("`n");$child.StandardInput.Flush();$child.StandardInput.Close()
  $deadline=[Math]::Min(250000,$clock.ElapsedMilliseconds+10000)
  while(!$child.WaitForExit(0) -or !$out.Closed -or !$err.Closed){
   Update-BoundedPipe $out;Update-BoundedPipe $err
   if($clock.ElapsedMilliseconds -ge $deadline){throw 'UNAVAILABLE: same child completion timeout'}
   Start-Sleep -Milliseconds 10
  }
  if($child.Id -ne $childId -or $childHandle.IsClosed -or !$child.WaitForExit(0) -or $child.ExitCode -ne 0 -or
     $out.Text.ToString() -cnotmatch '^FONT_DEBUGGER_PREFLIGHT_READY\r?\nFONT_DEBUGGER_PREFLIGHT_DONE\r?\n$' -or $err.Text.Length){throw 'UNAVAILABLE: same child DONE/kernel/exit proof failed'}
  $ok=$true
 }finally{$commandStream.Dispose()}
} catch {
 Emit-PreflightRecord 'FONT_DEBUGGER_UNAVAILABLE' @{error=$_.Exception.Message;diagnostic_only=$true}
 throw
} finally {
 try{Stop-ExactOwnedProcess $debugger}finally{
  try{Stop-ExactOwnedProcess $child}finally{
   foreach($pipe in @($out,$err,$dout,$derr)){
    if($null -ne $pipe -and $null -ne $pipe.Task){try{$null=$pipe.Task.Wait(100)}catch{}}
   }
   $closed=($null -eq $childHandle -or $childHandle.IsClosed) -and ($null -eq $debuggerHandle -or $debuggerHandle.IsClosed)
   Emit-PreflightRecord 'FONT_DEBUGGER_PREFLIGHT_SUMMARY' @{tool_request_and_detach_completed=($ok -and $closed);owned_handles_closed=$closed;font_owner_qualified=$false;host_WCT_Windows2025_qualified=$false;private_CRT_owner_symbols_qualified=$false;full_qualification=$false;symbol_availability='OBSERVATION_PENDING';elapsed_ms=$clock.ElapsedMilliseconds}
   if(!$closed){throw 'UNAVAILABLE: owned process handle closure failed'}
  }
 }
}

# CI-only host-owner contracts. Does not execute ART, guest ELF/JNI or font provider.
param([Parameter(Mandatory)][string]$FixturesDirectory)
$ErrorActionPreference='Stop'
function New-OwnerHostStartInfo([string]$Executable,[string]$Mode,[string]$WorkingDirectory){
 $psi=[Diagnostics.ProcessStartInfo]::new($Executable)
 $psi.ArgumentList.Add($Mode);$psi.UseShellExecute=$false;$psi.WorkingDirectory=$WorkingDirectory
 $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
 foreach($key in @('ANDROID_ROOT','GESHER_WINDOWS_FONTS')){
  if($Mode -ceq 'missing'){$null=$psi.Environment.Remove($key)}
  else{
   $value=switch -CaseSensitive ($Mode){
    'wide' {'initial-אב space'}
    'empty' {''}
    'invalid-wide' {[string][char]0xd800}
    'supplementary' {'initial-'+[char]::ConvertFromUtf32(0x1f600)+' space'}
    default {throw 'Unknown host-owner initial mode'}
   }
   $psi.Environment[$key]=$value
  }
 }
 $psi
}
function Emit-OwnerHostRecord([string]$Stage,$Value){
 $payload=$Value|ConvertTo-Json -Depth 30 -Compress -EscapeHandling EscapeNonAscii
 $digest=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($payload))).ToLowerInvariant()
 $count=[Math]::Max(1,[int][Math]::Ceiling($payload.Length/320.0))
 for($i=0;$i -lt $count;$i++){$offset=$i*320;$chunk=$payload.Substring($offset,[Math]::Min(320,$payload.Length-$offset));Write-Host ($Stage+' '+(@{record_sha256=$digest;part=$i+1;parts=$count;payload=$chunk}|ConvertTo-Json -Compress))}
}
function Convert-OwnerModulePath([string]$Units){
 if([string]::IsNullOrEmpty($Units) -or $Units -cnotmatch '^[0-9a-f]{4}(,[0-9a-f]{4})*$'){throw 'Missing/malformed native module UTF16 path'}
 $chars=[char[]]@($Units.Split(',')|ForEach-Object{[char][Convert]::ToUInt16($_,16)})
 $path=[string]::new($chars)
 # Guard metadata tests run under Linux pwsh too: use Windows lexical rules.
 # Ordinary drive-absolute and UNC file paths only; device/extended namespaces
 # are diagnostic errors rather than guessed or rewritten local paths.
 $driveAbsolute=$path -cmatch '^[A-Za-z]:[\\/]'
 $uncAbsolute=$false
 if($path -cmatch '^\\\\([^\\/:?]+)\\([^\\/:?]+)\\.+$'){
  $uncAbsolute=$Matches[1] -cnotin @('.','..') -and $Matches[2] -cnotin @('.','..')
 }
 if($path.Length -eq 0 -or $path.Contains([char]0) -or -not ($driveAbsolute -or $uncAbsolute)){throw 'Native module path is not an ordinary Windows absolute file path or contains NUL'}
 $path
}
function Emit-OwnerModuleProfiles([string]$Text,[string]$Mode){
 foreach($line in ($Text -split "`r?`n"|Where-Object{$_ -cmatch '^FONT_ENV_MODULE '})){
  try{
   if($line -cnotmatch ' label=([a-z_]+) .*path_valid=1 .*units=([0-9]+) utf16=([0-9a-f,]+)$'){throw 'Native module discovery reported diagnostic error'}
   $label=$Matches[1];$length=[int]$Matches[2];$path=Convert-OwnerModulePath $Matches[3]
   if($path.Length -ne $length){throw 'Native module units length mismatch'}
   $item=Get-Item -LiteralPath $path -ErrorAction Stop
   if($item.PSIsContainer){throw 'Native module path is a directory'}
   $version=$item.VersionInfo
   if([string]::IsNullOrWhiteSpace($version.FileVersion) -or [string]::IsNullOrWhiteSpace($version.ProductVersion)){throw 'Exact-path disk version unavailable'}
   Emit-OwnerHostRecord 'FONT_OWNER_HOST_MODULE_DISK_PROFILE' @{mode=$Mode;label=$label;path=$path;file_version=$version.FileVersion;product_version=$version.ProductVersion;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant();scope='DISK_FILE_AT_REPORTED_LOADED_PATH_NOT_MEMORY_ATTESTATION';diagnostic_only=$true}
  }catch{Emit-OwnerHostRecord 'FONT_OWNER_HOST_MODULE_DIAGNOSTIC_ERROR' @{mode=$Mode;native_record=$line;error=$_.Exception.Message;diagnostic_only=$true}}
 }
}
if($env:GITHUB_ACTIONS -cne 'true'){throw 'Native host-owner fixture execution is CI only'}
$fixtures=(Resolve-Path -LiteralPath $FixturesDirectory).Path
python tools/fonts/owner-host-manifest.py verify --root $fixtures
if($LASTEXITCODE -ne 0){throw 'Current source/run/attempt host-owner manifest failed'}
$assembly=[Diagnostics.Process].Assembly.Location
Emit-OwnerHostRecord 'FONT_OWNER_HOST_MANAGED_PROFILE' @{powershell=$PSVersionTable.PSVersion.ToString();framework=[Runtime.InteropServices.RuntimeInformation]::FrameworkDescription;runtime=[Environment]::Version.ToString();process_assembly=$assembly;process_assembly_version=[Diagnostics.Process].Assembly.GetName().Version.ToString();process_assembly_sha256=(Get-FileHash -LiteralPath $assembly -Algorithm SHA256).Hash.ToLowerInvariant();invalid_UTF16_transport='ACTUAL_RECEIVED_UNITS_REQUIRED'}
try{
 $os=Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
 Emit-OwnerHostRecord 'FONT_OWNER_HOST_OS_PROFILE' @{caption=$os.Caption;version=$os.Version;build=$os.BuildNumber;architecture=$os.OSArchitecture;runner_os=$env:RUNNER_OS;runner_arch=$env:RUNNER_ARCH;image_os=$env:ImageOS;image_version=$env:ImageVersion;diagnostic_only=$true}
}catch{Emit-OwnerHostRecord 'FONT_OWNER_HOST_OS_DIAGNOSTIC_ERROR' @{error=$_.Exception.Message;diagnostic_only=$true}}
$work=Join-Path $env:RUNNER_TEMP ('font-owner-host-'+[guid]::NewGuid());$null=New-Item -ItemType Directory -Path $work
$completed=0;$failedModes=[Collections.Generic.List[string]]::new()
foreach($mode in @('wide','missing','empty','invalid-wide','supplementary')){
 $psi=New-OwnerHostStartInfo (Join-Path $fixtures 'font_environment_test.exe') $mode $work
 if($psi.ArgumentList.Count -ne 1 -or $psi.ArgumentList[0] -cne $mode){throw 'Actual host fixture ArgumentList differs'}
 foreach($key in @('ANDROID_ROOT','GESHER_WINDOWS_FONTS')){
  $present=$psi.Environment.ContainsKey($key)
  $units=if($present){@($psi.Environment[$key].ToCharArray()|ForEach-Object{([int]$_).ToString('x4')})}else{@()}
  Emit-OwnerHostRecord 'FONT_OWNER_HOST_LAUNCH' @{mode=$mode;key=$key;present=$present;utf16_units=$units;source=$env:GITHUB_SHA;run=$env:GITHUB_RUN_ID;attempt=$env:GITHUB_RUN_ATTEMPT;role='CURRENT_SOURCE_HOST_FONT_OWNER_ONLY';runtime_shim_libcore_owner_qualified=$false;font_provider_OEM_qualified=$false}
 }
 $child=$null;$handle=$null;$childId=$null;$timeout=$false;$pipes=$false
 try{
  $child=[Diagnostics.Process]::Start($psi);$handle=$child.SafeHandle;$childId=$child.Id
  $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
  $timeout=-not $child.WaitForExit(90000)
  if($timeout){try{$child.Kill($true)}catch{};$null=$child.WaitForExit(5000)}
  $pipes=$stdout.Wait(10000) -and $stderr.Wait(10000)
  if($stdout.IsCompletedSuccessfully){Write-Host $stdout.Result}
  if($stderr.IsCompletedSuccessfully){Write-Host $stderr.Result}
  if($timeout -or -not $pipes){throw 'Bounded host-owner child or pipe failure; no qualification'}
  $text=$stdout.Result+"`n"+$stderr.Result
  Emit-OwnerModuleProfiles $text $mode
  $pass=[regex]::Matches($text,'(?m)^FONT_ENV_HOST_PASS mode='+[regex]::Escape($mode)+' [^\r\n]*')
  $wide=[regex]::Matches($text,'(?m)^FONT_ENV_HOST_INITIAL_WIDE key=(ANDROID_ROOT|GESHER_WINDOWS_FONTS) [^\r\n]*')
  if($child.ExitCode -ne 0 -or $pass.Count -ne 1 -or $wide.Count -ne 2 -or -not $text.Contains('FONT_ENV_WCT_PASS own_process=1 specific_writer_wait=CRITICAL_SECTION held_owner=INVALID_HANDLER concurrent_read_callback=1 unset_writer=1 true_kernel_completion=1 privilege_restored=1 session_token_handles_closed=1 arbitrary_writer_reentry_qualified=0') -or -not $text.Contains('FONT_ENV_WCT_PRIVILEGE_RESTORE success=1 error=0') -or $text.Contains('FONT_ENV_HOST_FAIL')){throw "Host-owner contract failed: mode=$mode exit=$($child.ExitCode)"}
  $completed++
  Emit-OwnerHostRecord 'FONT_OWNER_HOST_CHILD_RESULT' @{mode=$mode;exit=$child.ExitCode;host_owner_unit_pass=$true;current_source=$env:GITHUB_SHA;current_run=$env:GITHUB_RUN_ID;current_attempt=$env:GITHUB_RUN_ATTEMPT;runtime_shim_libcore_owner_qualified=$false;font_provider_OEM_qualified=$false;generic_getenv_qualified=$false;font_fallback_qualified=$false}
 }catch{
  $failedModes.Add($mode)
  Emit-OwnerHostRecord 'FONT_OWNER_HOST_CHILD_RESULT' @{mode=$mode;started=($null -ne $child);pid=$childId;timeout=$timeout;pipes_completed=$pipes;exit=$(if($child -and $child.HasExited){$child.ExitCode}else{$null});host_owner_unit_pass=$false;error=$_.Exception.Message;current_source=$env:GITHUB_SHA;current_run=$env:GITHUB_RUN_ID;current_attempt=$env:GITHUB_RUN_ATTEMPT;diagnostic_failure_preserved=$true}
 }finally{
  if($child -and -not $child.HasExited){try{$child.Kill($true)}catch{};$null=$child.WaitForExit(5000)}
  if($child){$child.Dispose()}
  Write-Host "FONT_OWNER_HOST_CLEANUP mode=$mode pid=$childId handle_closed=$($null -ne $handle -and $handle.IsClosed)"
 }
}
python tools/fonts/owner-host-manifest.py verify --root $fixtures
if($LASTEXITCODE -ne 0){throw 'Host fixture inputs mutated during execution'}
Emit-OwnerHostRecord 'FONT_OWNER_HOST_COMPLETE' @{cases=$completed;failed_modes=$failedModes.ToArray();all_five_strict_pass=($completed -eq 5 -and $failedModes.Count -eq 0);role='CURRENT_SOURCE_HOST_FONT_OWNER_ONLY';runtime_shim_libcore_owner_qualified=$false;font_provider_OEM_qualified=$false;generic_getenv_qualified=$false;font_fallback_qualified=$false;preparation_oom_qualified=$false;arbitrary_writer_reentry_qualified=$false}
if($completed -ne 5 -or $failedModes.Count -ne 0){throw ('Host-owner all-five strict contract failed: '+($failedModes -join ','))}

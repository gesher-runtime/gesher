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
 # No extension is invoked. Absolute -a selects only the initial default extension;
 # it does not confine the OS loader or prove complete dependency observation.
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
function Get-StageLimits {
 @{file_bytes=134217728L;aggregate_bytes=536870912L;members=128;handles=128;inventory=256;manifest_bytes=262144;setup_ms=90000}
}
function Assert-StageBudget([long]$ElapsedMs) {
 if($ElapsedMs -lt 0 -or $ElapsedMs -ge (Get-StageLimits).setup_ms){throw 'UNAVAILABLE: monotonic staging setup budget'}
}
function Assert-StageRelative([string]$Relative) {
 $null=Assert-OrdinaryWindowsPath ('C:\stage\'+$Relative)
 if($Relative -cnotmatch '^(?:winext\\)?[^\\]+\.(?i:dll|exe)$'){throw 'UNAVAILABLE: unexpected stage relative layout'}
 $Relative
}
function Assert-StageTrustRecord($Record) {
 if($null -eq $Record -or $Record -isnot [System.Collections.IDictionary]){throw 'UNAVAILABLE: null/malformed trust record'}
 foreach($key in @('relative','size','sha256','file_version','product_version','signature_status','signature_type','signature_message','signer','signer_thumbprint','query_error')){
  if(!$Record.Contains($key)){throw 'UNAVAILABLE: missing trust field'}
 }
 $null=Assert-StageRelative $Record.relative
 if($Record.query_error -cne '' -or $Record.size -isnot [long] -or $Record.size -le 0 -or
    $Record.sha256 -isnot [string] -or $Record.sha256 -cnotmatch '^[0-9a-f]{64}$'){throw 'UNAVAILABLE: source identity/query failure'}
 foreach($key in @('file_version','product_version','signature_status','signature_type','signature_message','signer','signer_thumbprint','query_error')){
  if($Record[$key] -isnot [string] -or $Record[$key].Length -gt 4096){throw 'UNAVAILABLE: malformed trust field'}
 }
 if($Record.signature_status -cnotin @('Valid','UnknownError','NotSigned','HashMismatch','NotTrusted','NotSupportedFileFormat','Incompatible') -or
    $Record.signature_type -cnotin @('None','Authenticode','Catalog')){throw 'UNAVAILABLE: malformed signature response'}
 if($Record.signature_status -ceq 'Valid' -and ($Record.signature_type -ceq 'None' -or !$Record.signer -or $Record.signer_thumbprint -cnotmatch '^[A-Fa-f0-9]{40,128}$')){throw 'UNAVAILABLE: malformed valid signer'}
}
function Test-StageEligible($Record) {
 Assert-StageTrustRecord $Record
 $Record.signature_status -ceq 'Valid' -and $Record.signer -match '(?:^|,\s*)O=Microsoft Corporation(?:,|$)' -and
 ![string]::IsNullOrWhiteSpace($Record.file_version) -and ![string]::IsNullOrWhiteSpace($Record.product_version)
}
function New-StageAdmission([object[]]$Records,[long]$ElapsedMs) {
 Assert-StageBudget $ElapsedMs
 $limits=Get-StageLimits
 if($Records.Count -gt $limits.inventory){throw 'UNAVAILABLE: source inventory count cap'}
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 $admitted=[Collections.Generic.List[object]]::new();$excluded=[Collections.Generic.List[object]]::new();$total=0L
 foreach($record in $Records){
  Assert-StageTrustRecord $record
  if(!$seen.Add($record.relative)){throw 'UNAVAILABLE: duplicate/case collision source member'}
  if($record.size -gt $limits.file_bytes){throw 'UNAVAILABLE: source per-file byte cap'}
  if(Test-StageEligible $record){$admitted.Add($record);$total+=$record.size}else{$excluded.Add($record)}
 }
 if($total -gt $limits.aggregate_bytes -or $admitted.Count -gt $limits.members -or $admitted.Count -gt $limits.handles){throw 'UNAVAILABLE: stage aggregate/count/handle cap'}
 foreach($required in @('cdb.exe','dbgeng.dll','dbghelp.dll','symsrv.dll','winext\ext.dll')){
  if(@($admitted|Where-Object{$_.relative -ieq $required}).Count -ne 1){throw 'UNAVAILABLE: required signed stage member missing/excluded'}
 }
 @{admitted=$admitted.ToArray();excluded=$excluded.ToArray();total_bytes=$total}
}
function Assert-StageIdentity($Source,$Destination) {
 Assert-StageTrustRecord $Source;Assert-StageTrustRecord $Destination
 if(!(Test-StageEligible $Source) -or !(Test-StageEligible $Destination)){throw 'UNAVAILABLE: unsigned identity replay'}
 foreach($key in @('relative','size','sha256','file_version','product_version','signature_status','signature_type','signer','signer_thumbprint')){
  if($Source[$key] -cne $Destination[$key]){throw 'UNAVAILABLE: source/destination identity mismatch'}
 }
}
function Assert-StageMembership([string[]]$Expected,[string[]]$Actual,[bool]$AllowPartial=$false) {
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 foreach($name in $Actual){
  $null=Assert-StageRelative $name
  if(!$seen.Add($name) -or $Expected -cnotcontains $name){throw 'UNAVAILABLE: extra/case-collision stage member'}
 }
 if(!$AllowPartial -and $Actual.Count -ne $Expected.Count){throw 'UNAVAILABLE: missing stage member'}
}
function Assert-OwnedStageCleanupPath([string]$Root,[string]$OwnedWork) {
 foreach($path in @($Root,$OwnedWork)){$null=Assert-OrdinaryWindowsPath $path}
 if($Root -cne ($OwnedWork+'\stage') -or !$Root.StartsWith($OwnedWork+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'UNAVAILABLE: cleanup outside exact owned stage'}
}
function Get-StageManifestText([object[]]$Records) {
 $lines=[Collections.Generic.List[string]]::new()
 $names=[string[]]@($Records|ForEach-Object{$_.relative});[Array]::Sort($names,[StringComparer]::Ordinal)
 foreach($name in $names){
  $r=@($Records|Where-Object{$_.relative -ceq $name})
  if($r.Count -ne 1){throw 'UNAVAILABLE: ambiguous manifest member'}
  Assert-StageTrustRecord $r[0]
  $v=$r[0]
  $lines.Add((ConvertTo-Json -InputObject ([ordered]@{relative=$v.relative;size=$v.size;sha256=$v.sha256;file_version=$v.file_version;product_version=$v.product_version;signature_status=$v.signature_status;signature_type=$v.signature_type;signer=$v.signer;signer_thumbprint=$v.signer_thumbprint}) -Compress -EscapeHandling EscapeNonAscii))
 }
 $text="FONT_DEBUGGER_SIGNED_STAGE_MANIFEST_V1`n"+($lines -join "`n")+"`n"
 if([Text.Encoding]::UTF8.GetByteCount($text) -gt (Get-StageLimits).manifest_bytes){throw 'UNAVAILABLE: deterministic manifest byte cap'}
 $text
}
function Assert-StageModule([string]$Path,[string]$StageRoot,[string]$SystemDirectory,[object[]]$Manifest,$Observed) {
 foreach($p in @($Path,$StageRoot,$SystemDirectory)){$null=Assert-OrdinaryWindowsPath $p}
 if($Path.StartsWith($StageRoot+'\',[StringComparison]::OrdinalIgnoreCase)){
  $relative=$Path.Substring($StageRoot.Length+1).ToLowerInvariant()
  $match=@($Manifest|Where-Object{$_.relative -ceq $relative})
  if($match.Count -ne 1){throw 'UNAVAILABLE: observed stage module absent from manifest'}
  Assert-StageIdentity $match[0] $Observed
 }elseif($Path.Substring(0,$Path.LastIndexOf('\')) -ieq $SystemDirectory){
  if(!(Test-StageEligible $Observed)){throw 'UNAVAILABLE: observed unsigned System32 module'}
 }else{throw 'UNAVAILABLE: observed original SDK/other module path'}
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
function Assert-StageCI {
 if($env:GITHUB_ACTIONS -cne 'true' -or !$IsWindows){throw 'Windows CI staging only'}
}
function Get-StageDiskRecord([string]$Path,[string]$Relative,[bool]$Setup=$true) {
 Assert-StageCI
 # Signature/filesystem calls are synchronous: budget checks bound admission at
 # call boundaries, not cancellation of an individual OS call already in flight.
 if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
 $path=Resolve-PreflightLiteral $Path
 $item=Get-Item -LiteralPath $path -Force -ErrorAction Stop
 if($item.Length -le 0 -or $item.Length -gt (Get-StageLimits).file_bytes){throw 'UNAVAILABLE: disk per-file byte cap'}
 # Query exceptions/null/malformed responses are failures, never unsigned exclusions.
 try{$sig=Get-AuthenticodeSignature -LiteralPath $path -ErrorAction Stop}catch{
  Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_SIGNATURE_QUERY_ERROR' @{path=$path;relative=$Relative;error=$_.Exception.Message;query_succeeded=$false;signature_validated=$false}
  throw
 }
 if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
 if($null -eq $sig -or @($sig).Count -ne 1){
  Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_SIGNATURE_MALFORMED' @{path=$path;relative=$Relative;reason='null/multiple response';query_succeeded=$true;signature_validated=$false}
  throw 'UNAVAILABLE: null/multiple signature response'
 }
 foreach($key in @('Status','StatusMessage','SignatureType','SignerCertificate')){
  if($null -eq $sig.PSObject.Properties[$key]){Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_SIGNATURE_MALFORMED' @{path=$path;relative=$Relative;missing_property=$key;query_succeeded=$true;signature_validated=$false};throw 'UNAVAILABLE: malformed signature query object'}
 }
 if($null -eq $sig.Status -or $null -eq $sig.SignatureType -or $null -eq $sig.StatusMessage){Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_SIGNATURE_MALFORMED' @{path=$path;relative=$Relative;reason='null field';query_succeeded=$true;signature_validated=$false};throw 'UNAVAILABLE: malformed signature query values'}
 $record=@{relative=$Relative;size=[long]$item.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();file_version=[string]$item.VersionInfo.FileVersion;product_version=[string]$item.VersionInfo.ProductVersion;signature_status=$sig.Status.ToString();signature_type=$sig.SignatureType.ToString();signature_message=[string]$sig.StatusMessage;signer=[string]$sig.SignerCertificate.Subject;signer_thumbprint=[string]$sig.SignerCertificate.Thumbprint;query_error=''}
 try{Assert-StageTrustRecord $record}catch{Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_SIGNATURE_MALFORMED' @{path=$path;relative=$Relative;received=$record;error=$_.Exception.Message;query_succeeded=$true;signature_validated=$false};throw}
 if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
 $record
}
function Get-StageInventory([string]$Root,[bool]$Setup=$false) {
 Assert-StageCI
 if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
 $root=Resolve-PreflightLiteral $Root $true
 $names=[Collections.Generic.List[string]]::new()
 foreach($item in @(Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop)){
  if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
  if($item.PSIsContainer){
   if($item.Name -cne 'winext'){throw 'UNAVAILABLE: unexpected stage directory'}
   $null=Resolve-PreflightLiteral $item.FullName $true
   foreach($leaf in @(Get-ChildItem -LiteralPath $item.FullName -Force -ErrorAction Stop)){
    if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
    if($leaf.PSIsContainer){throw 'UNAVAILABLE: nested stage directory'}
    $null=Resolve-PreflightLiteral $leaf.FullName;$names.Add(('winext\'+$leaf.Name))
    if($names.Count -gt (Get-StageLimits).members){throw 'UNAVAILABLE: stage membership count cap'}
   }
  }else{$null=Resolve-PreflightLiteral $item.FullName;$names.Add($item.Name)}
  if($names.Count -gt (Get-StageLimits).members){throw 'UNAVAILABLE: stage membership count cap'}
 }
 if($Setup){Assert-StageBudget $clock.ElapsedMilliseconds}
 $names.ToArray()
}
function Initialize-SignedStage([string]$SDK,[string]$StageRoot,$State) {
 Assert-StageCI;Assert-StageBudget $clock.ElapsedMilliseconds
 $records=[Collections.Generic.List[object]]::new();$sources=@{}
 foreach($relativeDirectory in @('','winext')){
  $directory=if($relativeDirectory){$SDK+'\'+$relativeDirectory}else{$SDK}
  $directory=Resolve-PreflightLiteral $directory $true
  $files=@(Get-ChildItem -LiteralPath $directory -File -Force -ErrorAction Stop|Where-Object{$_.Extension -in @('.exe','.dll')})
  if($files.Count -eq 0 -or $files.Count -gt 128){throw 'UNAVAILABLE: bounded immediate SDK inventory'}
  foreach($file in $files){
   Assert-StageBudget $clock.ElapsedMilliseconds
   $relative=if($relativeDirectory){$relativeDirectory+'\'+$file.Name.ToLowerInvariant()}else{$file.Name.ToLowerInvariant()}
   if($sources.ContainsKey($relative)){throw 'UNAVAILABLE: source case collision'}
   $path=Resolve-PreflightLiteral $file.FullName
   $sourceLock=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
   try{$record=Get-StageDiskRecord $path $relative}finally{$sourceLock.Dispose()}
   Emit-PreflightRecord 'FONT_DEBUGGER_STAGING_SOURCE_INVENTORY' @{path=$path;identity=$record;layout='STAGED_SIGNED_COPY_FROM_REGISTERED_SDK'}
   $records.Add($record);$sources[$relative]=$path
  }
 }
 # All well-formed records are inventoried before selection/required decisions.
 foreach($record in $records){if(!(Test-StageEligible $record)){Emit-PreflightRecord 'FONT_DEBUGGER_STAGING_EXCLUDED_UNTRUSTED' @{source=$sources[$record.relative];identity=$record;copied=$false}}}
 $admission=New-StageAdmission $records.ToArray() $clock.ElapsedMilliseconds
 $null=Get-StageManifestText $admission.admitted
 $State.expected=[string[]]@($admission.admitted|ForEach-Object{$_.relative})
 $null=New-Item -ItemType Directory -Path $StageRoot -ErrorAction Stop
 $State.root=Resolve-PreflightLiteral $StageRoot $true
 $null=New-Item -ItemType Directory -Path ($StageRoot+'\winext') -ErrorAction Stop
 foreach($record in $admission.admitted){
  Assert-StageBudget $clock.ElapsedMilliseconds
  $source=Resolve-PreflightLiteral $sources[$record.relative]
  $destination=Join-WindowsLiteral $StageRoot $record.relative
  $null=Resolve-PreflightLiteral ([IO.Path]::GetDirectoryName($destination)) $true
  $sourceLock=[IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try{
   Assert-StageIdentity $record (Get-StageDiskRecord $source $record.relative)
   $writer=[IO.File]::Open($destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
   try{
    $buffer=[byte[]]::new(65536);$copied=0L
    while(($read=$sourceLock.Read($buffer,0,$buffer.Length)) -gt 0){
     Assert-StageBudget $clock.ElapsedMilliseconds;$copied+=$read
     if($copied -gt $record.size){throw 'UNAVAILABLE: source grew during bounded copy'}
     $writer.Write($buffer,0,$read)
    }
    if($copied -ne $record.size){throw 'UNAVAILABLE: truncated staged copy'}
    $writer.Flush($true)
   }finally{$writer.Dispose()}
   # Read/ShareRead excludes writes/delete, no FileShare.Inheritable bit.
   $held=[IO.File]::Open($destination,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
   $State.handles.Add(@{stream=$held;handle=$held.SafeFileHandle})
   if($State.handles.Count -gt (Get-StageLimits).handles){throw 'UNAVAILABLE: held stage handle cap'}
   $actual=Get-StageDiskRecord $destination $record.relative
   Assert-StageIdentity $record $actual
   Assert-StageIdentity $record (Get-StageDiskRecord $source $record.relative)
   Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_COPY_IDENTITY' @{source=$source;destination=$destination;source_identity=$record;destination_identity=$actual;scope='DISK_IDENTITY_NOT_NTFS_OBJECT_OR_MEMORY_ATTESTATION'}
  }finally{$sourceLock.Dispose()}
 }
 Assert-StageMembership $State.expected @(Get-StageInventory $StageRoot $true)
 $State.manifest=$admission.admitted
 $manifestText=Get-StageManifestText $State.manifest
 $State.manifest_sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($manifestText))).ToLowerInvariant()
 Emit-PreflightRecord 'FONT_DEBUGGER_SIGNED_STAGE_MANIFEST' @{schema='FONT_DEBUGGER_SIGNED_STAGE_MANIFEST_V1';layout='STAGED_SIGNED_COPY_FROM_REGISTERED_SDK';root=$StageRoot;sha256=$State.manifest_sha256;manifest=$manifestText;limits=(Get-StageLimits);loader_closure='UNPROVED_SNAPSHOTS_ONLY';source_sdk_modified=$false}
 Assert-StageBudget $clock.ElapsedMilliseconds
}
function Remove-ExactOwnedStage($State,[string]$OwnedWork,[bool]$ProcessesCompleted) {
 Assert-StageCI
 if(!$State.root){return}
 if(!$ProcessesCompleted){throw 'UNAVAILABLE: stage cleanup requires completed/disposed owned processes'}
 foreach($held in $State.handles){if(!$held.handle.IsClosed){throw 'UNAVAILABLE: stage cleanup before handle closure'}}
 $work=Resolve-PreflightLiteral $OwnedWork $true
 $root=Resolve-PreflightLiteral $State.root $true
 Assert-OwnedStageCleanupPath $root $work
 $actual=@(Get-StageInventory $root)
 Assert-StageMembership $State.expected $actual $true
 # No recursion: every literal member and directory was checked above.
 foreach($name in $actual){Remove-Item -LiteralPath (Resolve-PreflightLiteral ($root+'\'+$name)) -Force -ErrorAction Stop}
 if(Test-Path -LiteralPath ($root+'\winext')){Remove-Item -LiteralPath (Resolve-PreflightLiteral ($root+'\winext') $true) -ErrorAction Stop}
 Remove-Item -LiteralPath $root -ErrorAction Stop
}
function Get-PreflightFileProfile([string]$Path,[bool]$Microsoft=$true) {
 $path=Resolve-PreflightLiteral $Path
 $item=Get-Item -LiteralPath $path
 $record=@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();file_version=$item.VersionInfo.FileVersion;product_version=$item.VersionInfo.ProductVersion;scope='DISK_AT_LOADED_OR_APPROVED_PATH_NOT_MEMORY_ATTESTATION'}
 # Emit both sides before fail-closed gates, including the exact rejected file.
 Emit-PreflightRecord 'FONT_DEBUGGER_FILE_PROFILE_BEFORE_GATE' @{received=$record;expected=@{file_version_present=$true;product_version_present=$true;valid_Microsoft_signature=$Microsoft};diagnostic_only=$true}
 if([string]::IsNullOrWhiteSpace($item.VersionInfo.FileVersion) -or [string]::IsNullOrWhiteSpace($item.VersionInfo.ProductVersion)){throw 'UNAVAILABLE: no exact disk file/product version'}
 if($Microsoft){
  try{$sig=Get-AuthenticodeSignature -LiteralPath $path}catch{
   Emit-PreflightRecord 'FONT_DEBUGGER_SIGNATURE_QUERY_ERROR' @{path=$path;sha256=$record.sha256;error=$_.Exception.Message;diagnostic_only=$true}
   throw
  }
  $subject=if($sig.SignerCertificate){$sig.SignerCertificate.Subject}else{$null}
  $thumbprint=if($sig.SignerCertificate){$sig.SignerCertificate.Thumbprint}else{$null}
  Emit-PreflightRecord 'FONT_DEBUGGER_SIGNATURE_BEFORE_GATE' @{path=$path;sha256=$record.sha256;expected=@{status='Valid';signer_organization='Microsoft Corporation'};received=@{status=$sig.Status.ToString();message=$sig.StatusMessage;signature_type=[string]$sig.SignatureType;signer_subject=$subject;signer_thumbprint=$thumbprint};diagnostic_only=$true}
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
function Stop-ExactOwnedProcess($Process,[hashtable]$Completion,[string]$Role) {
 $Completion[$Role]=$false
 if($null -eq $Process){$Completion[$Role]=$true;return}
 if($null -ne $Process){
  $completed=$false
  try{if(!$Process.WaitForExit(0)){$Process.Kill();if(!$Process.WaitForExit(2000)){throw 'Owned cleanup did not complete'}};$completed=$Process.WaitForExit(0)}finally{
   foreach($stream in @('StandardInput','StandardOutput','StandardError')){try{$Process.$stream.Dispose()}catch{}}
   $Process.Dispose()
   $Completion[$Role]=$completed
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
$stage=@{root=$null;handles=[Collections.Generic.List[object]]::new();expected=@();manifest=@()}
$processCompletion=@{child=$false;debugger=$false}
try {
 Emit-PreflightRecord 'FONT_DEBUGGER_HOST_PROFILE' @{os_version=[Environment]::OSVersion.VersionString;framework=[Runtime.InteropServices.RuntimeInformation]::FrameworkDescription;powershell=$PSVersionTable.PSVersion.ToString();runner_os=$env:RUNNER_OS;runner_arch=$env:RUNNER_ARCH;image_os=$env:ImageOS;image_version=$env:ImageVersion;source=$env:GITHUB_SHA;run=$env:GITHUB_RUN_ID;attempt=$env:GITHUB_RUN_ATTEMPT;diagnostic_only=$true}
 $probeRoot=Resolve-PreflightLiteral $ProbeDirectory $true
 python tools/fonts/debugger-probe.py verify --root $probeRoot
 if($LASTEXITCODE -ne 0){throw 'UNAVAILABLE: current-source/current-attempt probe provenance'}
 $probe=Resolve-PreflightLiteral ($probeRoot+'\probe.exe')
 $sourceTool=Get-RegisteredDebugger
 $sdk=[IO.Path]::GetDirectoryName($sourceTool);$sourceExtension=Resolve-PreflightLiteral ($sdk+'\winext') $true
 $checkout=Resolve-PreflightLiteral $env:GITHUB_WORKSPACE $true
 foreach($dir in @($sdk,$sourceExtension)){
  if($dir.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase) -or $dir -ieq $checkout){throw 'UNAVAILABLE: SDK inside checkout'}
 }
 $windows=Resolve-PreflightLiteral ([Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)) $true
 $system=Resolve-PreflightLiteral ($windows+'\System32') $true
 $temp=Resolve-PreflightLiteral ([IO.Path]::GetTempPath().TrimEnd('\')) $true
 if($temp -ieq $checkout -or $temp.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'UNAVAILABLE: temporary root inside checkout'}
 if($temp -ieq $sdk -or $temp.StartsWith($sdk+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'UNAVAILABLE: temporary root inside read-only source SDK'}
 $work=$temp+'\font-debugger-'+[guid]::NewGuid().ToString('N')
 $null=New-Item -ItemType Directory -Path $work -ErrorAction Stop
 $work=Resolve-PreflightLiteral $work $true
 foreach($leaf in @('child','debugger','cache','commands','temp')){
  $null=New-Item -ItemType Directory -Path ($work+'\'+$leaf) -ErrorAction Stop
  $null=Resolve-PreflightLiteral ($work+'\'+$leaf) $true
  if(@(Get-ChildItem -LiteralPath ($work+'\'+$leaf) -Force).Count){throw 'UNAVAILABLE: nonempty owned directory'}
 }
 Initialize-SignedStage $sdk ($work+'\stage') $stage
 $tool=Resolve-PreflightLiteral ($stage.root+'\cdb.exe')
 $extension=Resolve-PreflightLiteral ($stage.root+'\winext') $true
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
  Assert-StageMembership $stage.expected @(Get-StageInventory $stage.root $true)
  Assert-StageBudget $clock.ElapsedMilliseconds
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
  Assert-StageMembership $stage.expected @(Get-StageInventory $stage.root)
  foreach($member in $stage.manifest){
   $memberPath=Resolve-PreflightLiteral ($stage.root+'\'+$member.relative)
   if((Get-FileHash -LiteralPath $memberPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $member.sha256){throw 'UNAVAILABLE: stage path bytes changed before CDB'}
  }
  if($clock.ElapsedMilliseconds -ge 240000){throw 'UNAVAILABLE: whole deadline before CDB'}
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
    if($loadedPaths.Count -eq 0){Emit-PreflightRecord 'FONT_DEBUGGER_MODULE_PROFILE_UNAVAILABLE' @{reason='empty bounded module snapshot';symbol_availability='PENDING';module_closure='PENDING_UNPROVED';diagnostic_only=$true}}
    foreach($loadedPath in $loadedPaths){
     if($clock.ElapsedMilliseconds -ge $deadline){throw 'UNAVAILABLE: module observation deadline'}
     $path=Resolve-PreflightLiteral $loadedPath
     $relative=if($path.StartsWith($stage.root+'\',[StringComparison]::OrdinalIgnoreCase)){$path.Substring($stage.root.Length+1).ToLowerInvariant()}else{[IO.Path]::GetFileName($path).ToLowerInvariant()}
     $observed=Get-StageDiskRecord $path $relative $false
     Assert-StageModule $path $stage.root $system $stage.manifest $observed
     Emit-PreflightRecord 'FONT_DEBUGGER_LOADED_MODULE_DISK_PROFILE' @{path=$path;identity=$observed;scope='BOUNDED_OBSERVATION_NOT_LOADER_CLOSURE_OR_MEMORY_ATTESTATION'}
     if($clock.ElapsedMilliseconds -ge $deadline){throw 'UNAVAILABLE: module observation deadline'}
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
 try{Stop-ExactOwnedProcess $debugger $processCompletion 'debugger'}finally{
  try{Stop-ExactOwnedProcess $child $processCompletion 'child'}finally{
   $stageCleanupError=$null;$stageHandlesClosed=$false
   $bothCompleted=$processCompletion.child -and $processCompletion.debugger
   try{
    if(!$bothCompleted){throw 'UNAVAILABLE: process kernel completion failed; stage handles/files retained for surviving parent lifetime'}
    $closeError=$null
    foreach($held in $stage.handles){try{$held.stream.Dispose()}catch{$closeError=$_.Exception.Message}}
    $stageHandlesClosed=@($stage.handles|Where-Object{!$_.handle.IsClosed}).Count -eq 0
    if($closeError -or !$stageHandlesClosed){throw 'UNAVAILABLE: retained stage handle closure failed'}
    Remove-ExactOwnedStage $stage $work ($bothCompleted -and $stageHandlesClosed)
   }catch{
    $stageCleanupError=$_.Exception.Message
    Emit-PreflightRecord 'FONT_DEBUGGER_STAGE_CLEANUP_ERROR' @{error=$stageCleanupError;process_completion=$processCompletion;stage_handles_closed=$stageHandlesClosed;files_retained=$true}
   }
   foreach($pipe in @($out,$err,$dout,$derr)){
    if($null -ne $pipe -and $null -ne $pipe.Task){try{$null=$pipe.Task.Wait(100)}catch{}}
   }
   $closed=($null -eq $childHandle -or $childHandle.IsClosed) -and ($null -eq $debuggerHandle -or $debuggerHandle.IsClosed)
   Emit-PreflightRecord 'FONT_DEBUGGER_PREFLIGHT_SUMMARY' @{tool_request_and_detach_completed=($ok -and $closed -and $bothCompleted -and $stageHandlesClosed -and !$stageCleanupError);owned_handles_closed=$closed;owned_kernel_completion=$bothCompleted;stage_handles_closed=$stageHandlesClosed;stage_cleanup_error=$stageCleanupError;layout='STAGED_SIGNED_COPY_FROM_REGISTERED_SDK';loader_closure='UNPROVED_BOUNDED_MODULE_SNAPSHOTS_ONLY';font_owner_qualified=$false;host_WCT_Windows2025_qualified=$false;private_CRT_owner_symbols_qualified=$false;full_qualification=$false;symbol_availability='OBSERVATION_PENDING';elapsed_ms=$clock.ElapsedMilliseconds}
   if($stageCleanupError){throw $stageCleanupError}
   if(!$closed){throw 'UNAVAILABLE: owned process handle closure failed'}
  }
 }
}

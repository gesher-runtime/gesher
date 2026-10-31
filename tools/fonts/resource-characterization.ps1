# CI-only harmless debugger prerequisite. Never an ownership/host/full gate.
# Flags: https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/cdb-command-line-options
# Extension path: https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-extpath--set-extension-path-
param([Parameter(Mandatory)][string]$FixturesDirectory)
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
function New-StageLifetimeRoot([hashtable]$Owner) {
 if($null -eq $Owner -or !$Owner.Contains('handles')){throw 'UNAVAILABLE: missing complete stage owner'}
 # A normal CLR handle is a durable strong root until explicit Free, even if
 # the script/runspace unwinds. A global PowerShell variable is insufficient.
 # https://learn.microsoft.com/en-us/dotnet/api/system.runtime.interopservices.gchandletype
 @{handle=[Runtime.InteropServices.GCHandle]::Alloc($Owner,[Runtime.InteropServices.GCHandleType]::Normal);active=$true}
}
function Release-StageLifetimeRoot([hashtable]$Root,[bool]$BothCompleted,[bool]$StreamsClosed) {
 if($null -eq $Root){return $false}
 if(!$BothCompleted -or !$StreamsClosed){return $false}
 if($Root.active){$Root.handle.Free();$Root.active=$false}
 $true
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

function Get-ResourcePathRoles {
 @{
  FIXTURE_ROOT='FixturesDirectory parameter';FIXTURE_EXE='current verified fixture root + fixed EXE leaf'
  WINDOWS_ROOT='Environment.GetFolderPath Windows';SYSTEM_ROOT='validated Windows root + System32'
  CHECKOUT_ROOT='GITHUB_WORKSPACE path only';TEMP_ROOT='Path.GetTempPath path only'
  WORK_CHILD='new owned work root + child';WORK_IMPORTS='new owned work root + imports'
  WORK_PREWARM='new owned work root + prewarm';WORK_CAPTURE='new owned work root + capture'
  WORK_CACHE='new owned work root + cache';WORK_COMMANDS='new owned work root + commands';WORK_TEMP='new owned work root + temp'
  STAGED_CDB='admitted signed stage root + cdb.exe';STAGED_EXTENSION='admitted signed stage root + winext'
  PE_CDB='admitted signed stage root + cdb.exe';PE_DBGENG='admitted signed stage root + dbgeng.dll';PE_DBGHELP='admitted signed stage root + dbghelp.dll'
  PYTHON_APPLICATION_SOURCE='Get-Command Application -All execution precedence; single first Source only'
  IMPORT_SCRIPT='verified checkout root + fixed import inventory script';CHILD_PROVIDER='owned prepared child ucrtbase/ntdll module filename'
  SYSTEM_NTDLL_SCHEMA='native64 fixed System32/ntdll.dll signed retained schema identity'
  SYSTEM_UCRT_CODE='native64 fixed System32/ucrtbase.dll signed retained provider'
  CODE_ENVELOPE_SCRIPT='current source exact envelope script'
  PREWARM_MODULE='owned prewarm process bounded module filename'
 }
}
function Get-ResourcePathDiagnostic([string]$Role,[object]$Candidate,[bool]$Directory=$false) {
 $roles=Get-ResourcePathRoles
 if(!$roles.ContainsKey($Role)){throw 'Unapproved resource path diagnostic role'}
 $values=@();if($null -ne $Candidate){$values=@($Candidate)}
 $rows=[Collections.Generic.List[object]]::new()
 for($i=0;$i -lt [Math]::Min(8,$values.Count);$i++){
  $value=$values[$i];$type=if($null -eq $value){'NULL'}else{$value.GetType().FullName}
  $row=@{index=$i;type=$type;value_logged=$false}
  if($value -is [string]){
   $prefix=$value.Substring(0,[Math]::Min(512,$value.Length))
   $chars=$value.Substring(0,[Math]::Min(4096,$value.Length)).ToCharArray();$bytes=[byte[]]::new($chars.Length*2)
   if($bytes.Length){[Buffer]::BlockCopy($chars,0,$bytes,0,$bytes.Length)}
   $row.value_logged=$true;$row.length=$value.Length;$row.prefix=$prefix
   $row.prefix_utf16=@($prefix.ToCharArray()|ForEach-Object{([int]$_).ToString('x4')})
   $row.sha256_utf16le=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
   $row.hash_scope=if($value.Length -le 4096){'FULL_UTF16LE'}else{'FIRST_4096_UTF16_UNITS_ONLY'}
   $row.prefix_truncated=$value.Length -gt 512
  }
  # No ToString/property traversal of an unexpected object/credential bag.
  $rows.Add($row)
 }
 @{role=$Role;provenance=$roles[$Role];directory=$Directory;input_type=$(if($null -eq $Candidate){'NULL'}else{$Candidate.GetType().FullName});count=$values.Count;items=$rows.ToArray();items_truncated=($values.Count -gt 8);scope='KNOWN_PATH_INPUT_ONLY_NOT_ENVIRONMENT_OR_COMMANDLINE';selection_changed=$false}
}
function Resolve-ResourceLiteral([string]$Role,[object]$Candidate,[bool]$Directory=$false) {
 $diagnostic=Get-ResourcePathDiagnostic $Role $Candidate $Directory
 Emit-PreflightRecord 'FONT_RESOURCE_PATH_INPUT' $diagnostic
 try{
  # Keep the ORIGINAL typed parameter binder and exact path predicate. Do not
  # select/join/coerce/filter/normalize the candidate in this wrapper.
  Resolve-PreflightLiteral $Candidate $Directory
 }catch{
  Emit-PreflightRecord 'FONT_RESOURCE_PATH_REJECTED' @{input=$diagnostic;reason=$_.Exception.Message;predicate='ORIGINAL_Resolve-PreflightLiteral_AND_Assert-OrdinaryWindowsPath_UNCHANGED';exception_type=$_.Exception.GetType().FullName}
  throw
 }
}

function Select-ResourcePythonSource([object[]]$Records) {
 # Pure metadata projection. Runtime below must validate REAL ApplicationInfo
 # instances before creating these records; synthetic tests prove no OS lookup.
 if($Records.Count -lt 1 -or $Records.Count -gt 9){throw 'Python application candidates missing or exceed bounded ninth sentinel'}
 foreach($record in $Records){
  if($null -eq $record -or $record -isnot [System.Collections.IDictionary] -or $record.Count -ne 2 -or
     !$record.Contains('type') -or !$record.Contains('source') -or $record.type -cne 'System.Management.Automation.ApplicationInfo' -or
     $record.source -isnot [string] -or [string]::IsNullOrEmpty($record.source)){throw 'Invalid typed Python application metadata; no fallback'}
 }
 $sources=@($Records|ForEach-Object{$_.source})
 Emit-PreflightRecord 'FONT_RESOURCE_PYTHON_CANDIDATES' (Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' $sources)
 if($Records.Count -gt 8){throw 'Python application candidates exceed eight; ninth sentinel; no selection or fallback'}
 # Microsoft Get-Command Example12: -All returns execution precedence order.
 # Do not sort, join arrays, hardcode versions/drives or skip a bad first path.
 $selected=$Records[0].source
 Emit-PreflightRecord 'FONT_RESOURCE_PYTHON_SELECTION' @{query='Microsoft.PowerShell.Core\Get-Command -Name python -CommandType Application -All -TotalCount 9';selected_index=0;candidate_count=$Records.Count;selected_source=(Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' $selected);type=$Records[0].type;policy='FIRST_APPLICATION_EXECUTION_PRECEDENCE';selected_path_validation='UNCHANGED_ORIGINAL_RESOLVER_NEXT';fallback=$false}
 $selected
}
function Get-ResourcePythonSource {
 # Only metadata discovery, never invoke Python to choose an application.
 # TotalCount9 is a bounded sentinel: more than8 fails, never truncates to pass.
 $applications=@(Microsoft.PowerShell.Core\Get-Command -Name python -CommandType Application -All -TotalCount 9 -ErrorAction Stop)
 if($applications.Count -lt 1 -or $applications.Count -gt 9){throw 'Python application candidates missing or exceed bounded ninth sentinel'}
 $records=@()
 foreach($application in $applications){
  if($application -isnot [System.Management.Automation.ApplicationInfo]){throw 'Expected actual Python ApplicationInfo; no fallback'}
  $records+=@{type=$application.GetType().FullName;source=$application.Source}
 }
 Select-ResourcePythonSource $records
}

function Read-ResourceReady([string]$Text,[int]$OwnedId,[string]$Phase) {
 if($Phase -cnotin @('PREPARED','HOLD_READY')){throw 'Invalid readiness phase'}
 $pattern='^FONT_RESOURCE_'+$Phase+' pid=([1-9][0-9]{0,9}) writer=([1-9][0-9]{0,9}) handler=([1-9][0-9]{0,9})\r?\n$'
 if($Text.Length -gt 512 -or $Text -cnotmatch $pattern){throw 'Malformed readiness record'}
 $values=@([uint64]$Matches[1],[uint64]$Matches[2],[uint64]$Matches[3])
 if(@($values|Where-Object{$_ -gt [uint32]::MaxValue}).Count -or $values[0] -ne $OwnedId -or $values[1] -eq $values[2]){throw 'Readiness identity mismatch'}
 @{pid=[uint32]$values[0];writer=[uint32]$values[1];handler=[uint32]$values[2]}
}
function New-ResourceCommands([string]$Extension,[uint32]$Writer,[uint32]$Handler) {
 if(!$Writer -or !$Handler -or $Writer -eq $Handler){throw 'Distinct nonzero OS thread IDs required'}
 $base=New-PreflightCommands $Extension
 # Official k and n references: explicit 0x14 means20decimal regardless
 # of default radix. No setting command or raw/argument form is needed.
 $extra=@('.echo FONT_RESOURCE_WRITER',('~~[0x'+$Writer.ToString('x')+'] kL 0x14'),'.echo FONT_RESOURCE_HANDLER',('~~[0x'+$Handler.ToString('x')+'] kL 0x14')) -join "`n"
 $base.Substring(0,$base.Length-2)+$extra+"`nqd"
}
# Pure definitions; metadata controls select their AST without runner execution.
function Get-ResourceCodeHash([byte[]]$Bytes) {
 [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
function Get-ResourceCodeProfile([string]$Hash,[string]$Version) {
 $profiles=@{
  d06d0a421398fb663c6c49586e09774143460fdb1dce489aa9844846b9398555=@{version='10.0.20348.5756';start=0xb84b4;extent=0x110000}
  '9601850e2ede2cd86b441223557cae0fa7f5e33b1dd9d481c58c8cc2d688bdc3'=@{version='10.0.26100.33648';start=0x3c1d8;extent=0x14b000}
 }
 if(!$profiles.ContainsKey($Hash) -or $Version -cne $profiles[$Hash].version){throw 'UNAVAILABLE: exact signed UCRT profile'}
 $profiles[$Hash]
}
function New-ResourceCodeCommands([string]$Base,[int]$Start) {
 if($Start -notin @(0xb84b4,0x3c1d8) -or !$Base.EndsWith("`nqd")){throw 'Fixed code command profile'}
 $extra=@('.echo FONT_RESOURCE_CODE_BEGIN',('u ucrtbase+0x'+$Start.ToString('x')+' ucrtbase+0x'+($Start+95).ToString('x')),'.echo FONT_RESOURCE_CODE_END','.echo FONT_RESOURCE_TYPE_BEGIN','dt -v -t ntdll!_RTL_CRITICAL_SECTION','.echo FONT_RESOURCE_TYPE_END') -join "`n"
 $Base.Substring(0,$Base.Length-2)+$extra+"`nqd"
}
function Read-ResourceCodeBlock([string]$Text,[string]$Kind) {
 if($Kind -cnotin @('CODE','TYPE')){throw 'Fixed block kind'}
 $pattern='(?ms)^FONT_RESOURCE_'+$Kind+'_BEGIN\r?\n(.*?)^FONT_RESOURCE_'+$Kind+'_END\r?$'
 $found=[regex]::Matches($Text,$pattern)
 if($found.Count -ne 1){throw 'UNAVAILABLE: missing/duplicate code/type block'}
 $value=$found[0].Groups[1].Value
 # CDB echoes the next command before the ending echo output. Remove ONLY
 # that fixed echoed marker line, never a code/data command or arbitrary text.
 $value=[regex]::Replace($value,'(?m)^\d+:[0-9a-fA-F]+> \.echo FONT_RESOURCE_'+$Kind+'_END\r?\n?','')
 if([Text.Encoding]::UTF8.GetByteCount($value) -gt 8192){throw 'UNAVAILABLE: code/type raw bound'}
 $value
}
function Read-ResourceLoadedUCRT([string]$Text,[string]$Path,[uint64]$ExpectedBase,[int]$Extent,[string]$Version) {
 $rows=[regex]::Matches($Text,'(?mi)^([0-9a-f`]{8,17})\s+([0-9a-f`]{8,17})\s+ucrtbase\s+[^\r\n]*\r?\n(?<body>.*?)(?=^\d+:[0-9a-f]+>|\z)',[Text.RegularExpressions.RegexOptions]::Singleline)
 if($rows.Count -ne 1){throw 'UNAVAILABLE: unique loaded UCRT block'}
 $row=$rows[0];$base=[Convert]::ToUInt64($row.Groups[1].Value.Replace('`',''),16);$end=[Convert]::ToUInt64($row.Groups[2].Value.Replace('`',''),16)
 $body=$row.Groups['body'].Value
 $paths=[regex]::Matches($body,'(?m)^\s+Image path:\s+([^\r\n]+)\r?$')
 $sizes=[regex]::Matches($body,'(?m)^\s+ImageSize:\s+([0-9a-fA-F]+)\s*\r?$')
 $versions=[regex]::Matches($body,'(?m)^\s+File version:\s+([^\r\n]+)\r?$')
 if($base -ne $ExpectedBase -or $end -le $base -or $end-$base -ne $Extent -or $paths.Count -ne 1 -or $paths[0].Groups[1].Value.Trim() -ine $Path -or $sizes.Count -ne 1 -or [Convert]::ToUInt64($sizes[0].Groups[1].Value,16) -ne $Extent -or $versions.Count -ne 1 -or $versions[0].Groups[1].Value.Trim() -cne $Version){throw 'UNAVAILABLE: same-child loaded UCRT path/base/extent/profile mismatch'}
 @{base=$base;end=$end;path=$Path;extent=$Extent;version=$Version;scope='LMV_AND_RETAINED_DISK_NOT_WHOLE_MEMORY_ATTESTATION'}
}
function Read-ResourceCodeWindow([string]$Text,[uint64]$Base,[int]$Start,[string]$CacheHex) {
 if($Start -notin @(0xb84b4,0x3c1d8) -or $CacheHex -cnotmatch '^[0-9a-f]{222}$' -or $Base -gt [uint64]::MaxValue-($Start+111)){throw 'UNAVAILABLE: cache/window bounds'}
 $raw=Read-ResourceCodeBlock $Text 'CODE'
 $cursor=$Base+[uint64]$Start;$first=$cursor;$end=$first+95;$rows=[Collections.Generic.List[object]]::new()
 foreach($line in ($raw -split "`n")){
  $line=$line.TrimEnd("`r")
  if(!$line.Trim() -or $line -cmatch '^\d+:[0-9a-fA-F]+> u ucrtbase\+0x[0-9a-f]+ ucrtbase\+0x[0-9a-f]+$' -or $line -cmatch '^ucrtbase![^\r\n:]{1,256}:$' -or $line -ceq 'ucrtbase!`common_putenv<char>''::`1''::fin$0:'){continue}
  if($line -cnotmatch '^([0-9a-fA-F`]{8,17})\s+([0-9a-fA-F]{2,30})\s+(.{1,512})$'){throw 'UNAVAILABLE: unknown/truncated code row'}
  $at=[Convert]::ToUInt64($Matches[1].Replace('`',''),16);$hex=$Matches[2].ToLowerInvariant();$decoded=$Matches[3]
  $length=$hex.Length/2
  if($hex.Length%2 -or $length -lt 1 -or $length -gt 15 -or $at -ne $cursor -or $at -gt $end -or $decoded -match '\?\?|\b(?:db|invalid|bad)\b' -or $at-$first+$length -gt 110){throw 'UNAVAILABLE: noncontiguous/overflow/unknown instruction candidate'}
  $offset=[int]($at-$first)*2
  if($hex -cne $CacheHex.Substring($offset,$hex.Length)){throw 'UNAVAILABLE: BYTE_MISMATCH (no relocation normalization)'}
  $rows.Add(@{address=$at;rva=$Start+[int]($at-$first);hex=$hex;decode=$decoded});$cursor+=[uint64]$length
 }
 if(!$rows.Count -or $cursor -le $end){throw 'UNAVAILABLE: incomplete96-byte code coverage'}
 @{state='BYTE_MATCH';rows=@($rows.ToArray());requested_bytes=96;cached_bytes=111;accepted_last_rva=$Start+[int]($cursor-$first)-1;instruction_boundary_proved=$false;internal_debugger_read_set='UNKNOWN';owner_qualified=$false}
}
function Read-ResourceTypeState([string]$Text) {
 $raw=Read-ResourceCodeBlock $Text 'TYPE'
 $raw=[regex]::Replace($raw,'(?m)^\d+:[0-9a-fA-F]+> dt -v -t ntdll!_RTL_CRITICAL_SECTION\r?\n?','')
 $schema=$raw -match '(?m)^\s*\+0x[0-9a-fA-F]+\s+[A-Za-z_][A-Za-z0-9_]*\s+:'
 $named=$raw -cmatch '(?m)^\s*(?:ntdll!|struct |union |class |0x[0-9a-fA-F]+\s+struct )_RTL_CRITICAL_SECTION\b'
 @{state=$(if($schema -and $named){'TYPE_SCHEMA_PARTIAL'}elseif($named){'TYPE_ENUM_ONLY'}else{'TYPE_UNAVAILABLE'});raw=$raw;full_layout_qualified=$false;owner_qualified=$false}
}

function New-ResourceWitnessPrewarmCommands([string]$Extension,[int]$Start) {
 $commands=New-ResourceCodeCommands (New-PreflightCommands $Extension) $Start
 $commands.Substring(0,$commands.Length-2)+'.echo FONT_RESOURCE_PREWARM_INPUT_READY'+"`n"
}
function New-ResourceWitnessCaptureCommands([string]$Extension,[uint32]$Writer,[uint32]$Handler,[int]$Start) {
 $commands=New-ResourceCodeCommands (New-ResourceCommands $Extension $Writer $Handler) $Start
 $commands.Substring(0,$commands.Length-2)+'.echo FONT_RESOURCE_WITNESS_INPUT_READY'+"`n"
}
function New-ResourceOwnerTail([uint64]$Address) {
 if($Address -lt 65536 -or $Address -gt 0x00007fffffffffd7 -or $Address%8){throw 'UNAVAILABLE: canonical aligned one-object address'}
 ".echo FONT_RESOURCE_OWNER_BEGIN`n"+'dt ntdll!_RTL_CRITICAL_SECTION OwningThread 0x'+$Address.ToString('x16')+"`n.echo FONT_RESOURCE_OWNER_END`nqd`n"
}
function Wait-ResourceInputReady($Record,[string]$Kind,[long]$Deadline) {
 if($Kind -cnotin @('PREWARM','WITNESS') -or $Record.role -cne $(if($Kind -ceq 'PREWARM'){'prewarm'}else{'capture'})){throw 'Wrong input-ready role'}
 $marker='(?m)^FONT_RESOURCE_'+$Kind+'_INPUT_READY\r?$'
 $pattern='(?m)^FONT_RESOURCE_'+$Kind+'_INPUT_READY\r?\n\d+:[0-9a-fA-F]+> \z'
 while($true){
  Update-BoundedPipe $Record.out;Update-BoundedPipe $Record.err
  $null=Get-ResourceRemaining $clock.ElapsedMilliseconds $Deadline
  if($Record.process.Id -ne $Record.pid -or $Record.handle.IsClosed -or $Record.process.WaitForExit(0) -or $Record.err.Text.Length){throw 'UNAVAILABLE: input-ready process/pipe'}
  $text=$Record.out.Text.ToString();$matches=[regex]::Matches($text,$pattern)
  if([regex]::Matches($text,$marker).Count -gt 1){throw 'UNAVAILABLE: duplicate input-ready marker'}
  if($matches.Count -eq 1){
   $Record.readySnapshot=$text;$Record.readyHash=Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($text))
   Emit-PreflightRecord 'FONT_RESOURCE_READY_RAW' @{role=$Record.role;pid=$Record.pid;stdout=$text;sha256=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($text)));owner_qualified=$false}
   return $text
  }
  Start-Sleep -Milliseconds 5
 }
}
function Send-ResourceControlOnce($Record,[string]$Text,[long]$Deadline) {
 if($Record.control_reserved -or $Record.state -cne 'RUNNING' -or $Record.role -cnotin @('prewarm','capture')){throw 'One control tail only'}
 $bytes=[Text.Encoding]::ASCII.GetBytes($Text)
 if($bytes.Length -gt 512 -or [Text.Encoding]::ASCII.GetString($bytes) -cne $Text -or !$Text.EndsWith("qd`n")){throw 'Fixed control tail encoding/budget'}
 if($Record.role -ceq 'prewarm' -and $Text -cne "qd`n"){throw 'Prewarm never reads instance'}
 if($Record.role -ceq 'capture' -and $Text -cne "qd`n" -and $Text -cnotmatch '^\.echo FONT_RESOURCE_OWNER_BEGIN\ndt ntdll!_RTL_CRITICAL_SECTION OwningThread 0x[0-9a-f]{16}\n\.echo FONT_RESOURCE_OWNER_END\nqd\n$'){throw 'One fixed field tail grammar'}
 $null=Get-ResourceRemaining $clock.ElapsedMilliseconds $Deadline
 if($Record.process.Id -ne $Record.pid -or $Record.handle.IsClosed -or $Record.process.WaitForExit(0)){throw 'Owned control process changed/exited'}
 # Reserve BEFORE WriteAsync, even a synchronous failure forbids resubmission.
 $Record.control_reserved=$true;$Record.writeBuffer=$bytes
 $Record.controlProcess=$Record.process;$Record.controlHandle=$Record.handle;$Record.controlPid=$Record.pid
 $Record.controlHash=Get-ResourceCodeHash $bytes
 Emit-PreflightRecord 'FONT_RESOURCE_CONTROL_PLAN' @{role=$Record.role;pid=$Record.pid;text=$Text;sha256=(Get-ResourceCodeHash $bytes);one_write=$true;owner_qualified=$false}
 $Record.writeTask=$Record.process.StandardInput.BaseStream.WriteAsync($bytes,0,$bytes.Length)
 while(!$Record.writeTask.IsCompleted){Update-BoundedPipe $Record.out;Update-BoundedPipe $Record.err;$null=Get-ResourceRemaining $clock.ElapsedMilliseconds $Deadline;Start-Sleep -Milliseconds 1}
 $null=$Record.writeTask.GetAwaiter().GetResult();$Record.process.StandardInput.Close()
 $Record.controlCompletedTask=$Record.writeTask
}
function Assert-ResourceWitnessProfiles([string]$Text,$Cache,$Code,$Ntdll) {
 $null=Read-ResourceLoadedUCRT $Text $Code.path $Code.base $Cache.size_of_image $Cache.profile.version
 $blocks=[regex]::Matches($Text,'(?mis)^([0-9a-f`]{8,17})\s+([0-9a-f`]{8,17})\s+ntdll\s+[^\r\n]*\r?\n(.*?)(?=^\d+:[0-9a-f]+>|\z)')
 Write-ResourceNtdllDiagnostic $Ntdll $blocks
 if($blocks.Count -ne 1){throw 'UNAVAILABLE: unique current ntdll block'}
 $block=$blocks[0];$base=[Convert]::ToUInt64($block.Groups[1].Value.Replace('`',''),16);$end=[Convert]::ToUInt64($block.Groups[2].Value.Replace('`',''),16);$body=$block.Groups[3].Value
 $paths=[regex]::Matches($body,'(?m)^\s+Image path:\s+([^\r\n]+)\r?$');$sizes=[regex]::Matches($body,'(?m)^\s+ImageSize:\s+([0-9a-fA-F]+)\s*\r?$');$versions=[regex]::Matches($body,'(?m)^\s+File version:\s+([^\r\n]+)\r?$')
 if($base -ne $Ntdll.base -or $end -le $base -or $end-$base -ne $Ntdll.extent -or $paths.Count -ne 1 -or $paths[0].Groups[1].Value.Trim() -ine $Ntdll.path -or $sizes.Count -ne 1 -or [Convert]::ToUInt64($sizes[0].Groups[1].Value,16) -ne $Ntdll.extent -or $versions.Count -ne 1 -or $versions[0].Groups[1].Value.Trim() -cne $Ntdll.version){throw 'UNAVAILABLE: retained current ntdll profile/module mismatch'}
}
function Write-ResourceNtdllDiagnostic($Expected,$Blocks) {
 # Diagnostics NEVER supply expected values to the unchanged predicate.
 if($Blocks.Count -gt 4){throw 'UNAVAILABLE: ntdll diagnostic selection cap'}
 $rows=@();foreach($block in $Blocks){
  $raw=$block.Value;$bytes=[Text.Encoding]::UTF8.GetBytes($raw)
  if($bytes.Length -gt 65536){throw 'UNAVAILABLE: bounded complete ntdll diagnostic'}
  $body=$block.Groups[3].Value;$fields=@{}
  foreach($key in @('Image path','ImageSize','File version','Product version')){
   $values=@([regex]::Matches($body,'(?m)^\s+'+[regex]::Escape($key)+':\s+([^\r\n]+)\r?$')|ForEach-Object{$_.Groups[1].Value.Trim()})
   if(@($values|Where-Object{$_.Length -gt 4096}).Count){throw 'UNAVAILABLE: ntdll diagnostic field cap'};$fields[$key]=$values
  }
  $b=[Convert]::ToUInt64($block.Groups[1].Value.Replace('`',''),16);$e=[Convert]::ToUInt64($block.Groups[2].Value.Replace('`',''),16)
  $rows+=@{raw=$raw;raw_bytes=$bytes.Length;sha256=(Get-ResourceCodeHash $bytes);base=$b;end=$e;fields=$fields;outcomes=@{base=($b -eq $Expected.base);end=($e -gt $b);extent=($e -gt $b -and $e-$b -eq $Expected.extent);path=($fields['Image path'].Count -eq 1 -and $fields['Image path'][0] -ieq $Expected.path);size=($fields['ImageSize'].Count -eq 1 -and $fields['ImageSize'][0] -match '^[0-9a-fA-F]{1,16}$' -and [Convert]::ToUInt64($fields['ImageSize'][0],16) -eq $Expected.extent);version=($fields['File version'].Count -eq 1 -and $fields['File version'][0] -ceq $Expected.version)}}
 }
 Emit-PreflightRecord 'FONT_RESOURCE_NTDLL_COMPARISON' @{schema='RETAINED_NTDLL_COMPARISON_V1';expected=$Expected;selected_count=$Blocks.Count;received=$rows;qualification=$false}
}
function Read-ResourceWitnessSchema([string]$Text) {
 $state=Read-ResourceTypeState $Text
 if((Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($state.raw))) -cne '6aba31bc35c03db9c20cc841ed6e889616b23cc77e7cdd601855aabb1bd841c9'){throw 'UNAVAILABLE: exact six-root-field schema'}
 @{size=0x28;fields=6;owner_offset=0x10;owner_width=8;schema_sha256='6aba31bc35c03db9c20cc841ed6e889616b23cc77e7cdd601855aabb1bd841c9';private_PDB_attested=$false}
}
function Read-ResourceWitnessFrames([string]$Text,$Identity,$Cache,$Fixture) {
 if(!$Identity.writer -or !$Identity.handler -or $Identity.writer -eq $Identity.handler){throw 'Distinct held native IDs required'}
 $basis=$Cache.basis;$base=[uint64]$Fixture.ucrt_base;$results=@{}
 foreach($state in @($Cache.unwind,$basis)){
  if($state.scope -isnot [string] -or $state.scope -cne 'POST_PROLOGUE_COMMON_BODY' -or $state.partial_prologue_qualified -isnot [bool] -or $state.partial_prologue_qualified -ne $false -or $state.runtime_unwind_integrity -isnot [bool] -or $state.runtime_unwind_integrity -ne $false){throw 'UNAVAILABLE: typed body-only frame scope'}
 }
 if($Cache.unwind.prolog_bytes -ne 10 -or $basis.body_after_rva -ne $Cache.start_rva+10 -or $basis.body_before_rva -ne $Cache.start_rva+58 -or $basis.writer_return_rva -ne $Cache.start_rva+29 -or $basis.handler_return_rva -ne $Cache.start_rva+41){throw 'UNAVAILABLE: exact settled body PCs'}
 foreach($role in @('WRITER','HANDLER')){
  $tid=if($role -ceq 'WRITER'){$Identity.writer}else{$Identity.handler}
  $block=[regex]::Matches($Text,'(?ms)^FONT_RESOURCE_'+$role+'\r?\n(.*?)(?=^\d+:[0-9a-f]+> \.echo FONT_RESOURCE_|\z)')
  if($block.Count -ne 1 -or !$block[0].Groups[1].Value.Contains(('~~[0x'+$tid.ToString('x')+'] kL 0x14'))){throw 'UNAVAILABLE: same native thread block'}
  $rows=[regex]::Matches($block[0].Groups[1].Value,'(?m)^([0-9a-f`]{8,17})\s+([0-9a-f`]{8,17})\s+([^\r\n]+)\r?$')
  if($rows.Count -lt 3 -or $rows.Count -gt 20 -or $block[0].Value -match '(?i)unwind|stack unwind|following frames may'){throw 'UNAVAILABLE: bounded reliable frame transcript'}
  $last=[uint64]0;$pc=[uint64]0;$bound=@{};$matched=0;$callerMatched=0;$previousSite=''
  $firstSite=$rows[0].Groups[3].Value
  if(($role -ceq 'WRITER' -and $firstSite -cnotmatch '^ntdll!NtWaitForAlertByThreadId\+0x[0-9a-f]+$') -or ($role -ceq 'HANDLER' -and $firstSite -cnotmatch '^ntdll!NtWaitForSingleObject\+0x[0-9a-f]+$')){throw 'UNAVAILABLE: actual held native wait frame'}
  foreach($row in $rows){
   $sp=[Convert]::ToUInt64($row.Groups[1].Value.Replace('`',''),16);$ret=[Convert]::ToUInt64($row.Groups[2].Value.Replace('`',''),16);$site=$row.Groups[3].Value
   if($sp -le $last){throw 'UNAVAILABLE: nonmonotonic stack frames'};$last=$sp
   if($role -ceq 'WRITER' -and $site -cmatch '^ntdll!RtlEnterCriticalSection\+0x[0-9a-f]+$'){
    if($ret -ne $base+$basis.writer_return_rva){throw 'UNAVAILABLE: preceding callee return'};$matched++
   }
   if($role -ceq 'HANDLER' -and $site -cmatch '^ucrtbase!common_putenv_nolock<char>\+0x[0-9a-f]+$'){
    if($ret -ne $base+$basis.handler_return_rva -or $pc -lt $base+$Cache.function.nolock_begin -or $pc -ge $base+$Cache.function.nolock_end){throw 'UNAVAILABLE: held nolock return/range'};$matched++
   }
   if($site -cmatch '^ucrtbase!common_putenv<char>\+0x[0-9a-f]+$'){
    $expected=if($role -ceq 'WRITER'){$basis.writer_return_rva}else{$basis.handler_return_rva}
    if($pc -ne $base+$expected -or $pc -le $base+$basis.body_after_rva -or $pc -ge $base+$basis.body_before_rva){throw 'UNAVAILABLE: adjacent caller frame PC in settled body'};$callerMatched++
   }
   foreach($name in @('Invalid','Writer','Handler')){
    $f=$Fixture.frames[$name]
    if($pc -ge $Fixture.base+$f.begin_rva -and $pc -lt $Fixture.base+$f.end_rva -and $site -cmatch '^font_environment_resource_characterization(?:\+0x[0-9a-f]+|!.*)$'){
     if($name -ceq 'Invalid' -and $previousSite -cnotmatch '^KERNELBASE!WaitForSingleObjectEx\+0x[0-9a-f]+$'){throw 'UNAVAILABLE: callback is not in actual native wait'}
     if($bound.ContainsKey($name)){throw 'UNAVAILABLE: duplicate fixture function frame'};$bound[$name]=$pc
    }
   }
   $pc=$ret;$previousSite=$site
  }
  if($matched -ne 1 -or $callerMatched -ne 1 -or ($role -ceq 'WRITER' -and (!$bound.ContainsKey('Writer') -or $bound.ContainsKey('Invalid') -or $bound.ContainsKey('Handler'))) -or ($role -ceq 'HANDLER' -and (!$bound.ContainsKey('Invalid') -or !$bound.ContainsKey('Handler') -or $bound.ContainsKey('Writer')))){throw 'UNAVAILABLE: exact current callback/creator ancestry'}
  $results[$role]=@{tid=$tid;fixture_frames=$bound;callee_return_validated=$true}
 }
 $results
}
function Get-ResourceWitnessAddress($Cache,[uint64]$Base,$CodeResult) {
 foreach($state in @($Cache.unwind,$Cache.basis)){
  if($state.scope -isnot [string] -or $state.scope -cne 'POST_PROLOGUE_COMMON_BODY' -or $state.partial_prologue_qualified -isnot [bool] -or $state.partial_prologue_qualified -ne $false -or $state.runtime_unwind_integrity -isnot [bool] -or $state.runtime_unwind_integrity -ne $false){throw 'UNAVAILABLE: typed body-only address scope'}
 }
 if($Cache.unwind.prolog_bytes -ne 10 -or $Cache.basis.body_after_rva -ne $Cache.start_rva+10 -or $Cache.basis.body_before_rva -ne $Cache.start_rva+58 -or $Cache.basis.writer_return_rva -ne $Cache.start_rva+29 -or $Cache.basis.handler_return_rva -ne $Cache.start_rva+41){throw 'UNAVAILABLE: exact settled body address PCs'}
 if($Cache.function.begin -ne $Cache.start_rva -or $Cache.basis.function_begin -ne $Cache.start_rva -or $Cache.basis.function_end -ne $Cache.function.end -or $Cache.function.end-$Cache.start_rva -ne 69){throw 'UNAVAILABLE: independent whole function anchor'}
 $ends=@($CodeResult.rows|ForEach-Object{[int]($_.rva-$Cache.start_rva+$_.hex.Length/2)}|Where-Object{$_ -le 69})
 if(($ends -join ',') -cne ($Cache.basis.instruction_ends -join ',')){throw 'UNAVAILABLE: full decoded primary instruction boundaries'}
 $rva=[uint64]$Cache.basis.candidate_rva;$region=$Cache.basis.virtual_object
 if($rva%8 -or $rva -lt $region.rva -or $rva+0x28 -gt $region.rva+$region.extent -or !($region.flags -band 0x80000000) -or ($region.flags -band 0x20000000) -or $Base -gt 0x00007fffffffffd7-$rva){throw 'UNAVAILABLE: checked writable nonexec object bound'}
 $address=$Base+$rva;$null=New-ResourceOwnerTail $address;$address
}
function Read-ResourceOwnerField([string]$Text,[uint64]$Address,[uint32]$Handler,[uint32]$Writer,$Owner) {
 if(!$Handler -or !$Writer -or $Handler -eq $Writer){throw 'Same-child distinct DWORD IDs required'}
 $session=$Owner.ownerWitness;$r=$Owner.processes.capture
 if($phase -cne 'HOLD_READY' -or !$root.active -or ![object]::ReferenceEquals($root.handle.Target,$Owner) -or ![object]::ReferenceEquals($session.record,$r) -or ![object]::ReferenceEquals($r.ownerWitness,$session) -or ![object]::ReferenceEquals($session.process,$r.process) -or ![object]::ReferenceEquals($session.handle,$r.handle)){throw 'UNAVAILABLE: rooted original capture session'}
 if($r.role -cne 'capture' -or $r.state -cne 'EXITED' -or $r.pid -ne $session.pid -or $r.process.Id -ne $r.pid -or $r.handle.IsClosed -or ![object]::ReferenceEquals($r.process.SafeHandle,$r.handle) -or !$r.process.WaitForExit(0) -or $r.process.ExitCode -ne 0 -or !$r.out.Closed -or !$r.err.Closed -or $r.err.Text.Length -or $Text -cne $r.out.Text.ToString()){throw 'UNAVAILABLE: completed owned capture IO'}
 if($session.address -ne $Address -or $Owner.code.armedAddress -ne $Address -or $session.handler -ne $Handler -or $session.writer -ne $Writer -or $session.childPid -ne $Owner.processes.child.pid){throw 'UNAVAILABLE: current held address/IDs'}
 $profiles=@($Owner.manifest|Where-Object{$_.relative -ceq 'cdb.exe'})
 if($profiles.Count -ne 1 -or ![object]::ReferenceEquals($profiles[0],$session.profile) -or $session.profile.signature_status -cne 'Valid' -or $session.profile.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $session.profile.sha256 -cne $session.profileHash -or $session.profile.file_version -cne $session.profileVersion -or $r.process.StartInfo.FileName -cne ($Owner.root+'\cdb.exe')){throw 'UNAVAILABLE: admitted session debugger profile'}
 $canonical=[Text.Encoding]::ASCII.GetBytes((New-ResourceOwnerTail $Address))
 if(!$r.control_reserved -or ![object]::ReferenceEquals($r.controlProcess,$r.process) -or ![object]::ReferenceEquals($r.controlHandle,$r.handle) -or $r.controlPid -ne $r.pid -or ![object]::ReferenceEquals($r.controlCompletedTask,$r.writeTask) -or !$r.writeTask.IsCompletedSuccessfully -or (Get-ResourceCodeHash $canonical) -cne $r.controlHash -or (Get-ResourceCodeHash $r.writeBuffer) -cne $r.controlHash -or [Convert]::ToHexString($canonical) -cne [Convert]::ToHexString($r.writeBuffer)){throw 'UNAVAILABLE: original successful canonical ONEwrite'}
 $null=$r.writeTask.GetAwaiter().GetResult()
 $ready=$session.ready
 if($ready -cne $r.readySnapshot -or (Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($ready))) -cne $r.readyHash -or $r.readyHash -cne $session.readyHash -or !$ready.EndsWith("FONT_RESOURCE_WITNESS_INPUT_READY`n0:000> ") -or [regex]::Matches($ready,'(?m)^FONT_RESOURCE_WITNESS_INPUT_READY\r?$').Count -ne 1 -or !$Text.StartsWith($ready,[StringComparison]::Ordinal)){throw 'UNAVAILABLE: immutable validated ready boundary'}
 $raw='0:000> '+$Text.Substring($ready.Length)
 if([Text.Encoding]::UTF8.GetByteCount($raw) -gt 8192){throw 'UNAVAILABLE: bounded owner tail'}
 $expected='\A0:000> FONT_RESOURCE_OWNER_BEGIN\n0:000>    \+0x010 OwningThread : 0x([0-9a-f]{8}`[0-9a-f]{8}) Void\n0:000> FONT_RESOURCE_OWNER_END\n0:000> quit:\n\z'
 if($raw -cnotmatch $expected){throw 'UNAVAILABLE: CDB_REDIRECTED_OWNER_PROMPT_VOID_V1'}
 $owner=[Convert]::ToUInt64($Matches[1].Replace('`',''),16)
 if(!$owner -or $owner -ne [uint64]$Handler -or $owner -eq [uint64]$Writer){throw 'UNAVAILABLE: SAMEADDRESS full native TID differs'}
 @{schema='CDB_REDIRECTED_OWNER_PROMPT_VOID_V1';address=$Address;offset=0x10;width=8;owner=$owner;handler=[uint64]$Handler;writer=[uint64]$Writer;same_address_owner_observed=$true;full_qualification=$false}
}
function Assert-ResourceWitnessRootTasks($Owner) {
 foreach($record in $Owner.processes.Values){
  if($record.control_reserved -and (!$record.writeBuffer -or !$record.writeTask)){throw 'Control buffer/task lost from durable root'}
  if($record.control_reserved -and !$record.writeTask.IsCompleted){throw 'Incomplete rooted one-write IO'}
 }
}
function Get-ResourceRemaining([long]$Now,[long]$Deadline) {
 if($Now -lt 0 -or $Now -ge $Deadline){throw 'Original monotonic phase deadline expired'}
 $Deadline-$Now
}
function Start-ResourceRole($Owner,[string]$Role,$Info) {
 if(!$Owner.processes.Contains($Role) -or $Owner.processes[$Role].state -cne 'NOT_STARTED'){throw 'Process role replacement forbidden'}
 $record=$Owner.processes[$Role]
 Emit-PreflightRecord 'FONT_RESOURCE_REQUEST' @{role=$Role;executable=$Info.FileName;arguments=@($Info.ArgumentList);environment_keys=@($Info.Environment.Keys|Sort-Object);working_directory=$Info.WorkingDirectory;gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE'}
 $record.process=[Diagnostics.Process]::new();$record.settlementProcess=$record.process;$record.process.StartInfo=$Info
 if(!$record.process.Start()){throw 'Owned process start failed'}
 $record.pid=$record.process.Id;$record.handle=$record.process.SafeHandle;$null=$record.handle.DangerousGetHandle()
 $record.settlementHandle=$record.handle;$record.settlementPid=$record.pid
 $record.state='RUNNING';$record.out=New-BoundedPipe $record.process.StandardOutput;$record.err=New-BoundedPipe $record.process.StandardError
 $record
}
function Wait-ResourceRole($Record,[long]$Deadline) {
 $profiled=$false
 while(!$Record.process.WaitForExit(0) -or !$Record.out.Closed -or !$Record.err.Closed){
  Update-BoundedPipe $Record.out;Update-BoundedPipe $Record.err
  $null=Get-ResourceRemaining $clock.ElapsedMilliseconds $Deadline
  if($Record.process.Id -ne $Record.pid -or $Record.handle.IsClosed){throw 'Owned role identity changed'}
  if($Record.role -ceq 'prewarm' -and !$profiled -and !$Record.process.WaitForExit(0)){
   $paths=@();try{$Record.process.Refresh();$paths=@($Record.process.Modules|ForEach-Object{$_.FileName})}catch{if(!$Record.process.WaitForExit(0)){throw}}
   if($paths.Count -gt 128){throw 'Prewarm module snapshot bound'}
   foreach($loaded in $paths){
    $path=Resolve-ResourceLiteral 'PREWARM_MODULE' $loaded
    $relative=if($path.StartsWith($stage.root+'\',[StringComparison]::OrdinalIgnoreCase)){$path.Substring($stage.root.Length+1).ToLowerInvariant()}else{[IO.Path]::GetFileName($path).ToLowerInvariant()}
    $observed=Get-StageDiskRecord $path $relative $false
    Assert-StageModule $path $stage.root $system $stage.manifest $observed
    Emit-PreflightRecord 'FONT_RESOURCE_PREWARM_LOADED_MODULE' @{path=$path;identity=$observed;scope='BOUNDED_NOT_LOADER_CLOSURE'}
    $null=Get-ResourceRemaining $clock.ElapsedMilliseconds $Deadline
   }
   if(!$paths.Count){Emit-PreflightRecord 'FONT_RESOURCE_MODULES_PENDING' @{role='prewarm';reason='empty or exited snapshot';closure=$false}}
   $profiled=$true
  }
  Start-Sleep -Milliseconds 10
 }
 if($Record.role -ceq 'prewarm' -and !$profiled){Emit-PreflightRecord 'FONT_RESOURCE_MODULES_PENDING' @{role='prewarm';reason='exited before snapshot';closure=$false}}
 $stdout=$Record.out.Text.ToString();$stderr=$Record.err.Text.ToString()
 Emit-PreflightRecord 'FONT_RESOURCE_RAW' @{role=$Record.role;pid=$Record.pid;stdout=$stdout;stderr=$stderr;exit=$Record.process.ExitCode;private_owner_qualified=$false}
 if($Record.process.ExitCode -ne 0){throw 'Owned role nonzero exit'}
 $Record.state='EXITED'
 foreach($task in @($Record.writeTask,$Record.out.Task,$Record.err.Task)){if($null -ne $task -and !$task.IsCompleted){throw 'UNAVAILABLE: terminal role has live IO task'}}
 if($Record.role -ceq 'capture' -and $Record.ownerWitness){return} # Retain live identity through parser; finally still owns containment.
 if(!(Close-ResourceRecord $Record)){throw 'UNAVAILABLE: normal close outcome uncertain'}
}
function Send-ResourceBytes($Record,[byte[]]$Bytes,[int]$Count) {
 $Record.writeBuffer=$Bytes
 $Record.writeTask=$Record.process.StandardInput.BaseStream.WriteAsync($Bytes,0,$Count)
 $task=$Record.writeTask
 while(!$task.IsCompleted){Update-BoundedPipe $Record.out;Update-BoundedPipe $Record.err;$null=Get-ResourceRemaining $clock.ElapsedMilliseconds 90000;Start-Sleep -Milliseconds 1}
 $task.GetAwaiter().GetResult();$null=Get-ResourceRemaining $clock.ElapsedMilliseconds 90000
}
function Write-ResourceCommand($Owner,[string]$Path,[string]$Text) {
 $bytes=[Text.Encoding]::ASCII.GetBytes($Text)
 if([Text.Encoding]::ASCII.GetString($bytes) -cne $Text -or $bytes.Length -gt 4096){throw 'Command encoding/bound'}
 $writer=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
 try{$writer.Write($bytes,0,$bytes.Length);$writer.Flush($true)}finally{$writer.Dispose()}
 $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 $Owner.handles.Add(@{stream=$stream;handle=$stream.SafeFileHandle})
 if($Owner.handles.Count -gt 128){throw 'Session held-stream cap'}
 Emit-PreflightRecord 'FONT_RESOURCE_COMMAND_PLAN' @{path=$Path;commands=$Text;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();owner_qualified=$false}
}

function Test-ResourceTerminalTasks($Record) {
 foreach($task in @($Record.writeTask,$Record.out.Task,$Record.err.Task)){
  if($null -ne $task -and !$task.IsCompleted){return $false}
 }
 $true
}
function Close-ResourceRecord($Record) {
 # Shared NORMAL + CAPTURE + FAILURE close ledger; this helper NEVER kills.
 if(!(Test-ResourceTerminalTasks $Record)){throw 'Live IO forbids close'}
 if(![object]::ReferenceEquals($Record.process,$Record.settlementProcess)){throw 'Original close process changed'}
 if($Record.state -ceq 'DISPOSED'){
  foreach($name in @('StandardInput','StandardOutput','StandardError','Process')){if($Record.disposeState[$name] -cne 'DONE'){throw 'Disposed without complete original close ledger'}}
  return $true
 }
 if($Record.pid -ne $Record.settlementPid -or $Record.process.Id -ne $Record.pid -or ![object]::ReferenceEquals($Record.handle,$Record.settlementHandle) -or $Record.handle.IsClosed -or !$Record.process.WaitForExit(0)){throw 'Original terminal kernel required for close'}
 if(!$Record.disposeState){$Record.disposeState=@{};$Record.disposeResources=@{}}
 foreach($name in @('StandardInput','StandardOutput','StandardError','Process')){
  if($Record.disposeState[$name] -ceq 'FAILED'){return $false}
  if($Record.disposeState[$name] -cne 'DONE'){
   $Record.disposeState[$name]='FAILED' # before getter/Dispose; unknown outcome is NEVER retried
   $resource=if($name -ceq 'Process'){$Record.process}else{$Record.process.$name}
   $Record.disposeResources[$name]=$resource
   $resource.Dispose()
   $Record.disposeState[$name]='DONE'
  }
 }
 $Record.state='DISPOSED';$true
}
function Settle-ResourceRecord($Owner,$Record) {
 if(!(Test-ResourceTerminalTasks $Record)){ $tasksDone=$false }else{$tasksDone=$true}
 if($null -eq $Record.process){return ($Record.state -cin @('NOT_STARTED','OMITTED') -and $tasksDone)}
 if(![object]::ReferenceEquals($Record.process,$Record.settlementProcess)){throw 'Retained process changed'}
 if($Record.state -ceq 'DISPOSED'){return (Close-ResourceRecord $Record)}
 # Normal closes may have failed after Process.Dispose closed its handle;
 # ledger uncertainty is authoritative and requires quarantine, not a requery.
 if($Record.disposeState -and @($Record.disposeState.Values|Where-Object{$_ -ceq 'FAILED'}).Count){return $false}
 if($Record.pid -ne $Record.settlementPid -or $Record.process.Id -ne $Record.pid -or ![object]::ReferenceEquals($Record.handle,$Record.settlementHandle) -or $Record.handle.IsClosed){throw 'Retained kernel identity unavailable'}
 $kernel=$Record.process.WaitForExit(0)
 if(!$kernel -and !$Record.killReserved){
  $Record.killReserved=$true # One attempt, even if Kill/Wait throws.
  $Record.process.Kill();$kernel=$Record.process.WaitForExit(2000)
 }
 if(!$kernel -or !$tasksDone){return $false}
 # No stream/process disposal may precede BOTH facts above.
 foreach($task in @($Record.writeTask,$Record.out.Task,$Record.err.Task)){
  if($null -ne $task){try{$null=$task.GetAwaiter().GetResult()}catch{$Owner.failed=$true;$Owner.settlementErrors.Add($_.Exception.Message)}}
 }
 Close-ResourceRecord $Record
}
function Complete-ResourceSettlement($Owner,$Root,[scriptblock]$ReleaseFiles,[scriptblock]$FailureSink) {
 # Passive same-pwsh owner barrier, NOT more observation or a refreshed deadline.
 # External forced process/workflow/host destruction is not proved containable.
 $done=$false;$reported=$false
 while(!$done){
  try{
   $all=$true
   foreach($role in @('capture','prewarm','imports','child','help')){if($Owner.processes.Contains($role)){if(!(Settle-ResourceRecord $Owner $Owner.processes[$role])){$all=$false}}}
   if(!$all){$Owner.failed=$true}
   if(!$reported -and $Owner.failed){$reported=$true;& $FailureSink}
   if($all){
    if(!$Owner.filesAttempted){
     $Owner.filesAttempted=$true;& $ReleaseFiles;$Owner.filesReleased=$true
    }
    if($Owner.filesReleased){$done=if($null -eq $Root){$true}else{Release-StageLifetimeRoot $Root $true $true}}
   }
  }catch{$Owner.failed=$true;try{$Owner.settlementErrors.Add($_.Exception.Message)}catch{}}
  if(!$done){try{Start-Sleep -Milliseconds 50}catch{$Owner.failed=$true;try{$Owner.settlementErrors.Add($_.Exception.Message)}catch{}}}
 }
 return (!$Owner.failed) # Caller MUST preserve non-success after safe settlement.
}

if($env:GITHUB_ACTIONS -cne 'true' -or !$IsWindows){throw 'Windows CI only'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$stage=@{root=$null;handles=[Collections.Generic.List[object]]::new();expected=@();manifest=@();processes=@{};failed=$false;settlementErrors=[Collections.Generic.List[string]]::new()}
foreach($role in @('child','imports','help','prewarm','capture')){$stage.processes[$role]=@{role=$role;state=$(if($role -ceq 'help'){'OMITTED'}else{'NOT_STARTED'});process=$null;handle=$null;pid=0;out=$null;err=$null}}
$stage.code=@{stream=$null;profile=$null;cache=$null;frame=$null;loaded=$null};$root=$null;$work=$null;$ok=$false;$phase='PREPARED';$completion=@{}
try{
 $root=New-StageLifetimeRoot $stage
 $fixtureRoot=Resolve-ResourceLiteral 'FIXTURE_ROOT' $FixturesDirectory $true
 # The preceding immutable workflow step verifies the complete current receipt.
 # Bind its context and EXE bytes again under this session's retained read lock.
 $receipt=Get-Content -LiteralPath ($fixtureRoot+'\font-resource-provenance.json') -Raw -Encoding utf8|ConvertFrom-Json -AsHashtable
 if($receipt.context.source -cne $env:GITHUB_SHA -or $receipt.context.run -cne $env:GITHUB_RUN_ID -or $receipt.context.attempt -cne $env:GITHUB_RUN_ATTEMPT -or $receipt.role -cne 'CURRENT_SOURCE_HOST_FONT_OWNER_RESOURCE_CHARACTERIZATION'){throw 'Resource context mismatch'}
 $probe=Resolve-ResourceLiteral 'FIXTURE_EXE' ($fixtureRoot+'\font_environment_resource_characterization.exe')
 $probeStream=[IO.File]::Open($probe,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read);$stage.handles.Add(@{stream=$probeStream;handle=$probeStream.SafeFileHandle})
 if((Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash.ToLowerInvariant() -cne $receipt.outputs['font_environment_resource_characterization.exe']){throw 'Current EXE changed after receipt'}
 $sourceTool=Get-RegisteredDebugger;$sdk=[IO.Path]::GetDirectoryName($sourceTool)
 $windows=Resolve-ResourceLiteral 'WINDOWS_ROOT' ([Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)) $true
 $system=Resolve-ResourceLiteral 'SYSTEM_ROOT' ($windows+'\System32') $true
 $checkout=Resolve-ResourceLiteral 'CHECKOUT_ROOT' $env:GITHUB_WORKSPACE $true
 $temp=Resolve-ResourceLiteral 'TEMP_ROOT' ([IO.Path]::GetTempPath().TrimEnd('\')) $true
 foreach($path in @($sdk,$temp)){if($path -ieq $checkout -or $path.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'SDK/temp inside checkout'}}
 if($temp -ieq $sdk -or $temp.StartsWith($sdk+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Temp inside source SDK'}
 $work=$temp+'\font-resource-'+[guid]::NewGuid().ToString('N');$null=New-Item -ItemType Directory -Path $work
 foreach($leaf in @('child','imports','prewarm','capture','cache','commands','temp')){$null=New-Item -ItemType Directory -Path ($work+'\'+$leaf);$null=Resolve-ResourceLiteral ('WORK_'+$leaf.ToUpperInvariant()) ($work+'\'+$leaf) $true}
 Initialize-SignedStage $sdk ($work+'\stage') $stage
 $tool=Resolve-ResourceLiteral 'STAGED_CDB' ($stage.root+'\cdb.exe');$extension=Resolve-ResourceLiteral 'STAGED_EXTENSION' ($stage.root+'\winext') $true
 $entries=@();foreach($name in @('cdb.exe','dbgeng.dll','dbghelp.dll')){
  $row=@($stage.manifest|Where-Object{$_.relative -ceq $name});if($row.Count -ne 1){throw 'Missing exact admitted PE member'}
  $path=Resolve-ResourceLiteral (@{'cdb.exe'='PE_CDB';'dbgeng.dll'='PE_DBGENG';'dbghelp.dll'='PE_DBGHELP'}[$name]) ($stage.root+'\'+$name)
  $held=@($stage.handles|Where-Object{$_.stream.Name -ceq $path});if($held.Count -ne 1 -or $held[0].handle.IsClosed -or !$held[0].stream.CanRead -or $held[0].stream.Length -ne $row[0].size){throw 'Missing actual retained manifest PE lock'}
  $entries+=@{name=$name;bytes=$row[0].size;sha256=$row[0].sha256}
 }
 # Separate fixed native System32 provider admission before any child/HOLD.
 if(![Environment]::Is64BitProcess){throw 'Native64 System32 code view required'}
 $codePath=Resolve-ResourceLiteral 'SYSTEM_UCRT_CODE' ($system+'\ucrtbase.dll')
 $codeStream=[IO.File]::Open($codePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 $stage.code.stream=$codeStream;$stage.handles.Add(@{stream=$codeStream;handle=$codeStream.SafeFileHandle})
 if($stage.handles.Count -gt 128){throw 'Session held-stream cap'}
 if($codeStream.Length -lt 64 -or $codeStream.Length -gt 134217728){throw 'Provider full-file bound'}
 $codeIdentity=Get-PreflightFileProfile $codePath
 $codeProfile=Get-ResourceCodeProfile $codeIdentity.sha256 $codeIdentity.product_version
 # Actual saved a8e79 disk profiles include this literal FileVersion suffix;
 # lmv's numeric File version corresponds to the exact ProductVersion field.
 if($codeIdentity.file_version -cne ($codeProfile.version+' (WinBuild.160101.0800)')){throw 'UNAVAILABLE: exact observed disk FileVersion'}
 $stage.code.profile=$codeProfile
 $codeEntry=@{schema=2;role='SIGNED_SYSTEM32_UCRT_CODE';name='ucrtbase.dll';bytes=$codeStream.Length;sha256=$codeIdentity.sha256;version=$codeIdentity.product_version}
 $stage.code.frame=$codeEntry
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_DISK_ADMISSION' @{path=$codePath;identity=$codeIdentity;frame=$codeEntry;profile=$codeProfile;lock='RETAINED_READ_SHARE_READ';memory_attestation=$false;owner_qualified=$false}
 $ntdllPath=Resolve-ResourceLiteral 'SYSTEM_NTDLL_SCHEMA' ($system+'\ntdll.dll')
 $ntdllStream=[IO.File]::Open($ntdllPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 $stage.code.ntdllStream=$ntdllStream;$stage.handles.Add(@{stream=$ntdllStream;handle=$ntdllStream.SafeFileHandle})
 if($ntdllStream.Length -lt 64 -or $ntdllStream.Length -gt 134217728){throw 'Native ntdll identity-file bound'}
 if($stage.handles.Count -gt 128){throw 'Session held-stream cap'}
 $ntdllIdentity=Get-PreflightFileProfile $ntdllPath
 $ntdllVersions=@{'bcfe8a70a371a7d2fbcb429ab0a9e170aeebe4d73ae08d03a2da8fedec2efc78'='10.0.20348.5622';'160ef9b82e2895ef48fa031cb1cf683a8b123febf7cf7d5590cd3104864424b3'='10.0.26100.33648'}
 if(!$ntdllVersions.ContainsKey($ntdllIdentity.sha256) -or $ntdllIdentity.product_version -cne $ntdllVersions[$ntdllIdentity.sha256] -or $ntdllIdentity.file_version -cne ($ntdllIdentity.product_version+' (WinBuild.160101.0800)')){throw 'UNAVAILABLE: pinned native ntdll schema provider'}
 if(($codeProfile.start -eq 0xb84b4) -ne ($ntdllIdentity.product_version -ceq '10.0.20348.5622')){throw 'UNAVAILABLE: exact UCRT/NTDLL profile pair'}
 $stage.code.ntdll=@{path=$ntdllPath;version=$ntdllIdentity.product_version;sha256=$ntdllIdentity.sha256}
 $pythonSource=Get-ResourcePythonSource
 $python=Resolve-ResourceLiteral 'PYTHON_APPLICATION_SOURCE' $pythonSource
 $psi=New-PreflightStartInfo $python ($work+'\imports') $windows ($work+'\temp') @((Resolve-ResourceLiteral 'CODE_ENVELOPE_SCRIPT' ($checkout+'\tools\fonts\resource-code-envelope.py'))) $false
 $imports=Start-ResourceRole $stage 'imports' $psi
 $header=[Text.Encoding]::UTF8.GetBytes((@{entries=$entries}|ConvertTo-Json -Depth 8 -Compress)+"`n")
 $legacyLength=[long]$header.Length;foreach($entry in $entries){$legacyLength+=[long]$entry.bytes+22}
 $outer=[Text.Encoding]::UTF8.GetBytes((@{version=2;legacy_bytes=$legacyLength;context=@{source=$env:GITHUB_SHA;run=$env:GITHUB_RUN_ID;attempt=$env:GITHUB_RUN_ATTEMPT}}|ConvertTo-Json -Depth 8 -Compress)+"`n")
 $stage.code.outer=$outer;Send-ResourceBytes $imports $outer $outer.Length;Send-ResourceBytes $imports $header $header.Length
 foreach($entry in $entries){
  $stream=@($stage.handles|Where-Object{$_.stream.Name -ceq ($stage.root+'\'+$entry.name)})[0].stream
  $stream.Position=0;$buffer=[byte[]]::new(65536);$total=0L
  while(($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0){$total+=$read;if($total -gt $entry.bytes -or $total -gt 134217728){throw 'Bounded admitted stream size changed'};Send-ResourceBytes $imports $buffer $read}
  if($total -ne $entry.bytes -or $stream.ReadByte() -ne -1){throw 'Admitted full stream/EOF mismatch'}
  $end=[Text.Encoding]::ASCII.GetBytes("`nFONT_RESOURCE_PE_END`n");Send-ResourceBytes $imports $end $end.Length
 }
 $providerHeader=[Text.Encoding]::UTF8.GetBytes(($codeEntry|ConvertTo-Json -Compress)+"`n");$stage.code.providerHeader=$providerHeader
 Send-ResourceBytes $imports $providerHeader $providerHeader.Length
 $codeStream.Position=0;$buffer=[byte[]]::new(65536);$total=0L
 while(($read=$codeStream.Read($buffer,0,$buffer.Length)) -gt 0){$total+=$read;if($total -gt $codeEntry.bytes){throw 'Provider stream overflow'};Send-ResourceBytes $imports $buffer $read}
 if($total -ne $codeEntry.bytes -or $codeStream.ReadByte() -ne -1){throw 'Provider full stream/EOF mismatch'}
 $providerEnd=[Text.Encoding]::ASCII.GetBytes("`nFONT_UCRT_CODE_END_V2`n`0`n");Send-ResourceBytes $imports $providerEnd $providerEnd.Length
 $imports.process.StandardInput.Close()
 Wait-ResourceRole $imports 90000
 $cacheRows=@($imports.out.Text.ToString().Split("`n")|Where-Object{$_.Trim()}|ForEach-Object{ConvertFrom-Json -InputObject $_ -AsHashtable})
 $cache=@($cacheRows|Where-Object{$_.record -ceq 'PROVIDER_CACHE'})
 if($cacheRows.Count -ne 4 -or $cache.Count -ne 1){throw 'Exact three-tool + provider output count'}
 $stage.code.cache=$cache[0];$cache=$stage.code.cache
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_CACHE' $cache
 if($cache.role -cne 'SIGNED_SYSTEM32_UCRT_CODE' -or $cache.profile.sha256 -cne $codeEntry.sha256 -or $cache.profile.version -cne $codeEntry.version -or $cache.context.source -cne $env:GITHUB_SHA -or $cache.context.run -cne $env:GITHUB_RUN_ID -or $cache.context.attempt -cne $env:GITHUB_RUN_ATTEMPT -or $cache.start_rva -ne $codeProfile.start -or $cache.size_of_image -ne $codeProfile.extent -or $cache.cache_length -ne 111 -or $cache.cache_hex -cnotmatch '^[0-9a-f]{222}$'){throw 'Provider cache source/profile/window mismatch'}
 if($cache.profile.role -cne $codeEntry.role -or $cache.profile.name -cne $codeEntry.name -or $cache.profile.bytes -ne $codeEntry.bytes -or $cache.input_record_sha256 -cnotmatch '^[0-9a-f]{64}$' -or $cache.end_rva -ne $codeProfile.start+95 -or $cache.comparison_last_rva -ne $codeProfile.start+109){throw 'Provider cache exact role/frame/range mismatch'}
 $cacheBytes=[Convert]::FromHexString($cache.cache_hex);$stage.code.bytes=$cacheBytes
 $cacheKey=@($env:GITHUB_SHA,$env:GITHUB_RUN_ID,$env:GITHUB_RUN_ATTEMPT,$codeEntry.role,$codeEntry.sha256,$codeProfile.start.ToString('x'),'111',$cache.input_record_sha256) -join "`n"
 if((Get-ResourceCodeHash $cacheBytes) -cne $cache.cache_sha256 -or (Get-ResourceCodeHash ([Text.Encoding]::ASCII.GetBytes($cacheKey))) -cne $cache.cache_key_sha256){throw 'Provider cache content/context key hash mismatch'}
 $child=Start-ResourceRole $stage 'child' (New-PreflightStartInfo $probe ($work+'\child') $windows ($work+'\temp') @() $false)
 while($child.out.Text.ToString() -cnotmatch '^FONT_RESOURCE_PREPARED [^\r\n]+\r?\n$'){
  Update-BoundedPipe $child.out;Update-BoundedPipe $child.err;Assert-OwnedAlive $child.process $child.pid $child.handle
  $null=Get-ResourceRemaining $clock.ElapsedMilliseconds 90000
  if($child.out.Text.Length -gt 512 -or $child.err.Text.Length){throw 'PREPARED bound/error'};Start-Sleep -Milliseconds 10
 }
 $preparedText=$child.out.Text.ToString();$identity=Read-ResourceReady $preparedText $child.pid 'PREPARED'
 Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase='PREPARED';identity=$identity;imports='DISPOSED';help='OMITTED';owner_qualified=$false}
 # All cold module/signature work is before START. Same admitted tools are
 # prewarmed against this real prepared child, not an unrelated help process.
 $child.process.Refresh();$modules=@($child.process.Modules);if($modules.Count -gt 128){throw 'Module count bound'}
 $providers=@{};foreach($module in $modules){if($module.ModuleName -in @('ucrtbase.dll','ntdll.dll')){$path=Resolve-ResourceLiteral 'CHILD_PROVIDER' $module.FileName;if([IO.Path]::GetDirectoryName($path) -ine $system -or $providers.ContainsKey($module.ModuleName)){throw 'Provider identity'};$providers[$module.ModuleName]=$path;Emit-PreflightRecord 'FONT_RESOURCE_PROVIDER' (Get-PreflightFileProfile $path)}}
 if($providers.Count -ne 2){throw 'Actual prepared providers missing'}
 $ucrt=@($modules|Where-Object{$_.ModuleName -ieq 'ucrtbase.dll'})
 if($ucrt.Count -ne 1 -or $ucrt[0].FileName -ine $codePath -or $ucrt[0].ModuleMemorySize -ne $cache.size_of_image){throw 'Prepared current provider differs from signed retained cache'}
 $codeBase=[uint64]$ucrt[0].BaseAddress.ToInt64()
 $stage.code.loaded=@{base=$codeBase;extent=$ucrt[0].ModuleMemorySize;path=$codePath;version=$codeEntry.version}
 $ntdllModule=@($modules|Where-Object{$_.ModuleName -ieq 'ntdll.dll'})
 if($ntdllModule.Count -ne 1 -or $ntdllModule[0].FileName -ine $ntdllPath){throw 'UNAVAILABLE: current ntdll module'}
 $stage.code.ntdll.base=[uint64]$ntdllModule[0].BaseAddress.ToInt64();$stage.code.ntdll.extent=$ntdllModule[0].ModuleMemorySize
 $stage.code.ntdll.context=$receipt.context;$stage.code.ntdll.childPid=$child.pid;$stage.code.ntdll.role='prewarm';$stage.code.ntdll.phase='PREPARED';$stage.code.ntdll.provider=$ntdllIdentity
 $exeModule=$child.process.MainModule
 if($exeModule.FileName -ine $probe -or !$receipt.fixture_frame_bindings){throw 'UNAVAILABLE: exact fixture module/frame bindings'}
 $stage.fixture=@{base=[uint64]$exeModule.BaseAddress.ToInt64();frames=$receipt.fixture_frame_bindings;ucrt_base=$codeBase}
 foreach($f in $stage.fixture.frames.Values){if($f.exe_sha256 -cne $receipt.outputs['font_environment_resource_characterization.exe'] -or $f.object_sha256 -cne $receipt.compiled_objects.fixture.object_sha256 -or $f.map_sha256 -cne $receipt.outputs['font_environment_resource_characterization.map']){throw 'UNAVAILABLE: current fixture frame source'} }
 $prewarmFile=$work+'\commands\prewarm.txt';Write-ResourceCommand $stage $prewarmFile (New-ResourceWitnessPrewarmCommands $extension $codeProfile.start)
 $prewarm=Start-ResourceRole $stage 'prewarm' (New-PreflightStartInfo $tool ($work+'\prewarm') $windows ($work+'\temp') (New-PreflightArguments $child.pid $prewarmFile $system ($work+'\cache') $extension) $false)
 $cold=Wait-ResourceInputReady $prewarm 'PREWARM' 90000
 Assert-ResourceWitnessProfiles $cold $cache $stage.code.loaded $stage.code.ntdll
 $null=Read-ResourceWitnessSchema $cold
 $coldCode=Read-ResourceCodeWindow $cold $codeBase $codeProfile.start $cache.cache_hex
 $stage.code.armedAddress=Get-ResourceWitnessAddress $cache $codeBase $coldCode
 Send-ResourceControlOnce $prewarm "qd`n" 90000
 Wait-ResourceRole $prewarm 90000
 Assert-PreflightRawMarkers $prewarm.out.Text.ToString();Assert-OwnedAlive $child.process $child.pid $child.handle
 $loaded=Read-ResourceLoadedUCRT $prewarm.out.Text.ToString() $codePath $codeBase $cache.size_of_image $codeEntry.version
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_PREWARM_BINDING' $loaded
 Assert-StageMembership $stage.expected @(Get-StageInventory $stage.root)
 $phase='PREWARM_COMPLETE';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE';help='OMITTED';owner_qualified=$false}
 $null=Get-ResourceRemaining $clock.ElapsedMilliseconds 90000
 $captureFile=$work+'\commands\capture.txt';Write-ResourceCommand $stage $captureFile (New-ResourceWitnessCaptureCommands $extension $identity.writer $identity.handler $codeProfile.start)
 $holdDeadline=[Math]::Min(300000,$clock.ElapsedMilliseconds+180000)
 $readyDeadline=[Math]::Min($holdDeadline,$clock.ElapsedMilliseconds+10000)
 $child.process.StandardInput.Write("START`n");$child.process.StandardInput.Flush()
 while($child.out.Text.ToString().Length -le $preparedText.Length){Update-BoundedPipe $child.out;Update-BoundedPipe $child.err;Assert-OwnedAlive $child.process $child.pid $child.handle;$null=Get-ResourceRemaining $clock.ElapsedMilliseconds $readyDeadline;Start-Sleep -Milliseconds 10}
 # Wait for complete bounded line; never interpolate raw child text.
 while(!$child.out.Text.ToString().EndsWith("`n")){Update-BoundedPipe $child.out;Update-BoundedPipe $child.err;$null=Get-ResourceRemaining $clock.ElapsedMilliseconds $readyDeadline;if($child.out.Text.Length -gt 1024){throw 'HOLD_READY bound'};Start-Sleep -Milliseconds 10}
 $held=Read-ResourceReady ($child.out.Text.ToString().Substring($preparedText.Length)) $child.pid 'HOLD_READY'
 if($held.writer -ne $identity.writer -or $held.handler -ne $identity.handler){throw 'Thread identity changed'}
 $phase='HOLD_READY';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;identity=$held;capture_preconditions_only=$true;owner_qualified=$false}
 $capture=Start-ResourceRole $stage 'capture' (New-PreflightStartInfo $tool ($work+'\capture') $windows ($work+'\temp') (New-PreflightArguments $child.pid $captureFile $system ($work+'\cache') $extension) $false)
 $captureDeadline=[Math]::Min($holdDeadline-10000,$clock.ElapsedMilliseconds+120000)
 $heldRaw=Wait-ResourceInputReady $capture 'WITNESS' $captureDeadline
 $stage.code.ntdll.role='capture';$stage.code.ntdll.phase='HOLD_READY'
 Assert-OwnedAlive $child.process $child.pid $child.handle
 Assert-ResourceWitnessProfiles $heldRaw $cache $stage.code.loaded $stage.code.ntdll
 $null=Read-ResourceWitnessSchema $heldRaw
 $heldCode=Read-ResourceCodeWindow $heldRaw $codeBase $codeProfile.start $cache.cache_hex
 $heldAddress=Get-ResourceWitnessAddress $cache $codeBase $heldCode
 if($heldAddress -ne $stage.code.armedAddress){throw 'UNAVAILABLE: cold/held address basis changed'}
 $frames=Read-ResourceWitnessFrames $heldRaw $held $cache $stage.fixture
 Emit-PreflightRecord 'FONT_RESOURCE_WITNESS_BASIS' @{address=$heldAddress;basis=$cache.basis;frames=$frames;owner_observed=$false}
 Assert-OwnedAlive $child.process $child.pid $child.handle
 $capture.ownerWitness=@{record=$capture;process=$capture.process;handle=$capture.handle;pid=$capture.pid;ready=$heldRaw;readyHash=$capture.readyHash;address=$heldAddress;handler=$held.handler;writer=$held.writer;childPid=$child.pid;profile=@($stage.manifest|Where-Object{$_.relative -ceq 'cdb.exe'})[0]}
 $stage.ownerWitness=$capture.ownerWitness
 $capture.ownerWitness.profileHash=$capture.ownerWitness.profile.sha256;$capture.ownerWitness.profileVersion=$capture.ownerWitness.profile.file_version
 Send-ResourceControlOnce $capture (New-ResourceOwnerTail $heldAddress) $captureDeadline
 Wait-ResourceRole $capture $captureDeadline
 $owner=Read-ResourceOwnerField $capture.out.Text.ToString() $heldAddress $held.handler $held.writer $stage
 if(!(Test-ResourceTerminalTasks $capture)){throw 'Capture still has live IO'}
 if(!(Close-ResourceRecord $capture)){throw 'UNAVAILABLE: capture close outcome uncertain'}
 Emit-PreflightRecord 'FONT_RESOURCE_ONE_ADDRESS_OWNER' $owner
 Assert-ResourceWitnessRootTasks $stage
 Assert-PreflightRawMarkers $capture.out.Text.ToString();Assert-OwnedAlive $child.process $child.pid $child.handle
 # Raw complete capture was emitted by Wait-ResourceRole BEFORE all gates.
 $loaded=Read-ResourceLoadedUCRT $capture.out.Text.ToString() $codePath $codeBase $cache.size_of_image $codeEntry.version
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_CAPTURE_BINDING' $loaded
 $rawCode=Read-ResourceCodeBlock $capture.out.Text.ToString() 'CODE';$rawType=Read-ResourceCodeBlock $capture.out.Text.ToString() 'TYPE'
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_TYPE_RAW' @{code=$rawCode;type=$rawType;code_sha256=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($rawCode)));type_sha256=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($rawType)));cache_key_sha256=$cache.cache_key_sha256;state='CODE_RAW_CANDIDATE';alignment_qualified=$false;owner_qualified=$false}
 $codeResult=Read-ResourceCodeWindow $capture.out.Text.ToString() $codeBase $codeProfile.start $cache.cache_hex
 $typeResult=Read-ResourceTypeState $capture.out.Text.ToString()
 Emit-PreflightRecord 'FONT_RESOURCE_CODE_BYTE_MATCH' $codeResult
 Emit-PreflightRecord 'FONT_RESOURCE_TYPE_ENUM_OR_SCHEMA' $typeResult
 $null=Get-ResourceRemaining $clock.ElapsedMilliseconds ($holdDeadline-10000)
 $phase='CAPTURE_COMPLETE';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;owner_qualified=$false};$child.process.StandardInput.Write("RELEASE`n");$child.process.StandardInput.Flush();$child.process.StandardInput.Close();$phase='RELEASED';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;owner_qualified=$false}
 Wait-ResourceRole $child ([Math]::Min(330000,$clock.ElapsedMilliseconds+20000))
 if($child.err.Text.Length -or $child.out.Text.ToString() -cne ($preparedText+'FONT_RESOURCE_HOLD_READY pid='+$held.pid+' writer='+$held.writer+' handler='+$held.handler+"`n"+'FONT_RESOURCE_COMPLETE pid='+$held.pid+' writer='+$held.writer+' handler='+$held.handler+"`n")){throw 'Same child exact COMPLETE receipt mismatch'}
 $phase='COMPLETE';$ok=$true
}catch{
 $stage.failed=$true
 foreach($r in @($stage.processes['capture'],$stage.processes['prewarm'])){if($r.out){$partial=$r.out.Text.ToString();Emit-PreflightRecord 'FONT_RESOURCE_WITNESS_FAILURE_RAW' @{role=$r.role;pid=$r.pid;stdout=$partial;stderr=$r.err.Text.ToString();sha256=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($partial)));owner_qualified=$false}}}
 foreach($r in @($stage.processes['capture'],$stage.processes['prewarm'])){if($r.process -and $r.state -ceq 'RUNNING' -and !$r.control_reserved){try{Send-ResourceControlOnce $r "qd`n" ([Math]::Min(330000,$clock.ElapsedMilliseconds+2000))}catch{}}}
 $phase='FAILED';Emit-PreflightRecord 'FONT_RESOURCE_FAILURE' @{reason=$_.Exception.Message;phase=$phase;owner_qualified=$false};throw}finally{
 # Every query/exit/release/error sink is inside the existing-pwsh barrier.
 $settlementSuccess=Complete-ResourceSettlement $stage $root {
  foreach($held in $stage.handles){$held.stream.Dispose()}
  if(@($stage.handles|Where-Object{!$_.handle.IsClosed}).Count){throw 'Owned file lease still open'}
  if($stage.root){Remove-ExactOwnedStage $stage $work $true}
 } {
  foreach($r in @($stage.processes['capture'],$stage.processes['prewarm'])){if($r.out){$partial=$r.out.Text.ToString();Emit-PreflightRecord 'FONT_RESOURCE_WITNESS_CLEANUP_RAW' @{role=$r.role;pid=$r.pid;stdout=$partial;stderr=$r.err.Text.ToString();sha256=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($partial)));owner_qualified=$false}}}
 }
 $all=$true;$streamsClosed=$true;$cleanupError=$null;$released=$true;$ok=$ok -and $settlementSuccess
 $roleStates=@{};foreach($role in $stage.processes.Keys){$completion[$role]=$true;$r=$stage.processes[$role];$roleStates[$role]=@{state=$r.state;pid=$r.pid;kernel_completed=$true}}
 Emit-PreflightRecord 'FONT_RESOURCE_SUMMARY' @{phase=$phase;characterization_completed=($ok -and $all -and $streamsClosed -and $released -and !$cleanupError);process_completion=$completion;role_states=$roleStates;stage_root_released=$released;root_retained=($root -and $root.active);native_failure_lifetime='QUARANTINE_UNTIL_BOTH_NATIVE_KERNELS_OR_EXACT_CHILD_TERMINATION';gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE';help='OMITTED';font_owner_qualified=$false;host_WCT_Windows2025_qualified=$false;private_CRT_owner_symbols_qualified=$false;full_qualification=$false;elapsed_ms=$clock.ElapsedMilliseconds;cleanup_error=$cleanupError}
 if(!$all -or !$streamsClosed -or !$released -or $cleanupError -or !$settlementSuccess){throw 'Exact owned settlement retained a permanent failure'}
}

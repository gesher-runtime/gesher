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
  PYTHON_APPLICATION_SOURCE='unchanged Get-Command python -CommandType Application .Source selection'
  IMPORT_SCRIPT='verified checkout root + fixed import inventory script';CHILD_PROVIDER='owned prepared child ucrtbase/ntdll module filename'
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
function Get-ResourceRemaining([long]$Now,[long]$Deadline) {
 if($Now -lt 0 -or $Now -ge $Deadline){throw 'Original monotonic phase deadline expired'}
 $Deadline-$Now
}
function Start-ResourceRole($Owner,[string]$Role,$Info) {
 if(!$Owner.processes.Contains($Role) -or $Owner.processes[$Role].state -cne 'NOT_STARTED'){throw 'Process role replacement forbidden'}
 $record=$Owner.processes[$Role]
 Emit-PreflightRecord 'FONT_RESOURCE_REQUEST' @{role=$Role;executable=$Info.FileName;arguments=@($Info.ArgumentList);environment_keys=@($Info.Environment.Keys|Sort-Object);working_directory=$Info.WorkingDirectory;gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE'}
 $record.process=[Diagnostics.Process]::new();$record.process.StartInfo=$Info
 if(!$record.process.Start()){throw 'Owned process start failed'}
 $record.pid=$record.process.Id;$record.handle=$record.process.SafeHandle;$null=$record.handle.DangerousGetHandle()
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
 foreach($name in @('StandardInput','StandardOutput','StandardError')){$Record.process.$name.Dispose()}
 $Record.process.Dispose();$Record.state='DISPOSED'
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

if($env:GITHUB_ACTIONS -cne 'true' -or !$IsWindows){throw 'Windows CI only'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$stage=@{root=$null;handles=[Collections.Generic.List[object]]::new();expected=@();manifest=@();processes=@{}}
foreach($role in @('child','imports','help','prewarm','capture')){$stage.processes[$role]=@{role=$role;state=$(if($role -ceq 'help'){'OMITTED'}else{'NOT_STARTED'});process=$null;handle=$null;pid=0;out=$null;err=$null}}
$root=$null;$work=$null;$ok=$false;$phase='PREPARED';$completion=@{}
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
 $pythonSource=(Get-Command python -CommandType Application -ErrorAction Stop).Source
 $python=Resolve-ResourceLiteral 'PYTHON_APPLICATION_SOURCE' $pythonSource
 $psi=New-PreflightStartInfo $python ($work+'\imports') $windows ($work+'\temp') @((Resolve-ResourceLiteral 'IMPORT_SCRIPT' ($checkout+'\tools\fonts\resource-import-inventory.py'))) $false
 $imports=Start-ResourceRole $stage 'imports' $psi
 $header=[Text.Encoding]::UTF8.GetBytes((@{entries=$entries}|ConvertTo-Json -Depth 8 -Compress)+"`n");Send-ResourceBytes $imports $header $header.Length
 foreach($entry in $entries){
  $stream=@($stage.handles|Where-Object{$_.stream.Name -ceq ($stage.root+'\'+$entry.name)})[0].stream
  $stream.Position=0;$buffer=[byte[]]::new(65536);$total=0L
  while(($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0){$total+=$read;if($total -gt $entry.bytes -or $total -gt 134217728){throw 'Bounded admitted stream size changed'};Send-ResourceBytes $imports $buffer $read}
  if($total -ne $entry.bytes -or $stream.ReadByte() -ne -1){throw 'Admitted full stream/EOF mismatch'}
  $end=[Text.Encoding]::ASCII.GetBytes("`nFONT_RESOURCE_PE_END`n");Send-ResourceBytes $imports $end $end.Length
 }
 $imports.process.StandardInput.Close()
 Wait-ResourceRole $imports 90000
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
 $prewarmFile=$work+'\commands\prewarm.txt';Write-ResourceCommand $stage $prewarmFile ((New-PreflightCommands $extension)+"`n")
 $prewarm=Start-ResourceRole $stage 'prewarm' (New-PreflightStartInfo $tool ($work+'\prewarm') $windows ($work+'\temp') (New-PreflightArguments $child.pid $prewarmFile $system ($work+'\cache') $extension) $false)
 $prewarm.process.StandardInput.Close();Wait-ResourceRole $prewarm 90000
 Assert-PreflightRawMarkers $prewarm.out.Text.ToString();Assert-OwnedAlive $child.process $child.pid $child.handle
 Assert-StageMembership $stage.expected @(Get-StageInventory $stage.root)
 $phase='PREWARM_COMPLETE';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE';help='OMITTED';owner_qualified=$false}
 $null=Get-ResourceRemaining $clock.ElapsedMilliseconds 90000
 $captureFile=$work+'\commands\capture.txt';Write-ResourceCommand $stage $captureFile ((New-ResourceCommands $extension $identity.writer $identity.handler)+"`n")
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
 $capture.process.StandardInput.Close();Wait-ResourceRole $capture ([Math]::Min($holdDeadline-10000,$clock.ElapsedMilliseconds+120000))
 Assert-PreflightRawMarkers $capture.out.Text.ToString();Assert-OwnedAlive $child.process $child.pid $child.handle
 $phase='CAPTURE_COMPLETE';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;owner_qualified=$false};$child.process.StandardInput.Write("RELEASE`n");$child.process.StandardInput.Flush();$child.process.StandardInput.Close();$phase='RELEASED';Emit-PreflightRecord 'FONT_RESOURCE_PHASE' @{phase=$phase;owner_qualified=$false}
 Wait-ResourceRole $child ([Math]::Min(330000,$clock.ElapsedMilliseconds+20000))
 if($child.err.Text.Length -or $child.out.Text.ToString() -cne ($preparedText+'FONT_RESOURCE_HOLD_READY pid='+$held.pid+' writer='+$held.writer+' handler='+$held.handler+"`n"+'FONT_RESOURCE_COMPLETE pid='+$held.pid+' writer='+$held.writer+' handler='+$held.handler+"`n")){throw 'Same child exact COMPLETE receipt mismatch'}
 $phase='COMPLETE';$ok=$true
}catch{$phase='FAILED';Emit-PreflightRecord 'FONT_RESOURCE_FAILURE' @{reason=$_.Exception.Message;phase=$phase;owner_qualified=$false};throw}finally{
 # The reserved Normal root owns every process/pipe/handle before Start.
 $all=$true
 foreach($role in @('capture','prewarm','imports','child','help')){$record=$stage.processes[$role];try{if($record.state -ceq 'DISPOSED'){$completion[$role]=$true}else{Stop-ExactOwnedProcess $record.process $completion $role}}catch{$all=$false};if(!$completion[$role]){$all=$false}}
 # Kill/wait and stream disposal must finish every retained asynchronous IO
 # task too. Otherwise keep the whole session root and every file lease alive.
 foreach($record in $stage.processes.Values){
  foreach($task in @($record.writeTask,$record.out.Task,$record.err.Task)){
   if($null -ne $task){try{$null=$task.Wait(100)}catch{};if(!$task.IsCompleted){$all=$false}}
  }
 }
 $streamsClosed=$false;$cleanupError=$null
 if($all){try{foreach($held in $stage.handles){$held.stream.Dispose()};$streamsClosed=@($stage.handles|Where-Object{!$_.handle.IsClosed}).Count -eq 0;if($stage.root){Remove-ExactOwnedStage $stage $work $streamsClosed}}catch{$cleanupError=$_.Exception.Message}}
 $roleStates=@{};foreach($role in $stage.processes.Keys){$r=$stage.processes[$role];$roleStates[$role]=@{state=$r.state;pid=$r.pid;kernel_completed=$completion[$role]}}
 $released=Release-StageLifetimeRoot $root $all $streamsClosed
 Emit-PreflightRecord 'FONT_RESOURCE_SUMMARY' @{phase=$phase;characterization_completed=($ok -and $all -and $streamsClosed -and $released -and !$cleanupError);process_completion=$completion;role_states=$roleStates;stage_root_released=$released;root_retained=($root -and $root.active);native_failure_lifetime='QUARANTINE_UNTIL_BOTH_NATIVE_KERNELS_OR_EXACT_CHILD_TERMINATION';gallery='NOT_DISABLED';loader='NOT_EXHAUSTIVE';help='OMITTED';font_owner_qualified=$false;host_WCT_Windows2025_qualified=$false;private_CRT_owner_symbols_qualified=$false;full_qualification=$false;elapsed_ms=$clock.ElapsedMilliseconds;cleanup_error=$cleanupError}
 if(!$all -or !$streamsClosed -or !$released -or $cleanupError){throw 'Exact owned cleanup failed; any live owners remain rooted'}
}

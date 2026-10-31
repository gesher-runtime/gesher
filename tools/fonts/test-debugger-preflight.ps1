# Pure AST/ProcessStartInfo checks only. Never execute or dot-source runner body.
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'debugger-preflight.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$pure=@('Assert-OrdinaryWindowsPath','Join-WindowsLiteral','Select-UniqueDebugger','New-PreflightCommands','New-PreflightArguments','New-PreflightStartInfo','Assert-PreflightRequest','Assert-PreflightRawMarkers','Get-StageLimits','Assert-StageBudget','Assert-StageRelative','Assert-StageTrustRecord','Test-StageEligible','New-StageAdmission','Assert-StageIdentity','Assert-StageMembership','Assert-OwnedStageCleanupPath','Get-StageManifestText','Assert-StageModule')
foreach($name in $pure){
 $nodes=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
 if($nodes.Count -ne 1){throw 'Missing/duplicate pure function'}
 $body=$nodes[0].Extent.Text
 if($body -match '(?i)::Start\(|\.Start\(|Start-Process|Invoke-Expression|Get-Process|Get-Item|Resolve-Path|Get-ChildItem'){throw 'Pure extraction contains process/filesystem action'}
 Invoke-Expression $body
}
$script:checks=0
function Check([bool]$Condition,[string]$Message){if(!$Condition){throw $Message};$script:checks++}
function Reject([scriptblock]$Action){$rejected=$false;try{& $Action|Out-Null}catch{$rejected=$true};Check $rejected 'Negative control unexpectedly accepted'}
function Exact($Actual,$Expected){Check ((ConvertTo-Json -InputObject @($Actual) -Compress) -ceq (ConvertTo-Json -InputObject @($Expected) -Compress)) 'Exact sequence differs'}
foreach($path in @('C:\Windows\System32','D:\A folder\another folder\probe.exe','C:\אב גד\漢字\probe.exe')){Check ((Assert-OrdinaryWindowsPath $path) -ceq $path) 'Ordinary Windows path lost'}
foreach($bad in @('','c:foo','C:','\Windows\probe.exe','/tmp/probe','relative\probe','\\server\share\probe','\\?\C:\probe','\\.\pipe\x','C:\one\..\two','C:\one\.\two','C:\one\\two','C:\one\two.','C:\one\two ','C:\NUL.txt','C:\COM1\x','C:\one:stream','C:\x;qd','C:\x"','C:\one/other',"C:\one`0other",'C:\')){Reject {Assert-OrdinaryWindowsPath $bad}}
Check ((Join-WindowsLiteral 'D:\fake drive' 'child\probe.exe') -ceq 'D:\fake drive\child\probe.exe') 'Lexical construction used host drive'
Reject {Select-UniqueDebugger @()};Reject {Select-UniqueDebugger @('C:\sdk1\cdb.exe','C:\sdk2\cdb.exe')}
Check ((Select-UniqueDebugger @('C:\sdk\cdb.exe','C:\sdk\cdb.exe')) -ceq 'C:\sdk\cdb.exe') 'Same registered candidate not deduplicated'
$cf='D:\owned work\commands\preflight.txt';$images='C:\Windows\System32';$cache='D:\owned work\cache';$ext='C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\winext'
$args=New-PreflightArguments 1234 $cf $images $cache $ext
$expected=@('-pv','-p','1234','-noshell','-sins','-nosqm','-ses','-sflags','0x00081600','-i',$images,'-y',('srv*'+$cache+'*https://msdl.microsoft.com/download/symbols'),('-a'+$ext+'\ext'),'-cf',$cf)
Exact $args $expected
Check (@($args|Where-Object{$_ -ceq '-cf'}).Count -eq 1) 'Exactly one cf required'
foreach($forbidden in @('-c','-cfr','-pvr','-pn','-z','-g','-G','-hd','-o')){Check ($args -cnotcontains $forbidden) 'Forbidden debugger mode'}
Reject {New-PreflightArguments 0 $cf $images $cache $ext};Reject {New-PreflightArguments -1 $cf $images $cache $ext}
Reject {New-PreflightArguments 1234 'relative.txt' $images $cache $ext}
Reject {New-PreflightArguments 1234 $cf $images 'D:cache' $ext}
$expectedCommands=@(('.extpath '+$ext),'.symopt- 0x00000040','.symopt+ 0x00000400','.symopt','.echo FONT_DEBUGGER_MODULES','lmv m ntdll','lmv m ucrtbase','.reload /f ntdll.dll','.reload /f ucrtbase.dll','.echo FONT_DEBUGGER_PUBLIC_OBSERVATION','x ntdll!RtlEnterCriticalSection','x ucrtbase!*putenv_s*','.echo FONT_DEBUGGER_PRIVATE_OBSERVATION_PENDING','x ucrtbase!__acrt_lock_table','qd') -join "`n"
Check ((New-PreflightCommands $ext) -ceq $expectedCommands) 'Exact command bytes differ'
Check ((New-PreflightCommands $ext).Split("`n")[-1] -ceq 'qd') 'qd must be last'
Reject {New-PreflightCommands 'C:\sdk; .shell whoami'}
foreach($expansion in @('C:\sdk$<commands','C:\sdk%PATH%','C:\sdk`name')){Reject {New-PreflightCommands $expansion}}
$tool='C:\SDK\cdb.exe';$cwd='D:\owned work\debugger'
$request=New-PreflightStartInfo $tool $cwd 'C:\Windows' 'D:\owned work\temp' $args $false
Assert-PreflightRequest $request $tool $cwd 1234 $cf $images $cache $ext ($expectedCommands+"`n")
foreach($index in 0..($args.Count-1)){
 $original=$request.ArgumentList[$index];$request.ArgumentList[$index]='MUTATED'
 Reject {Assert-PreflightRequest $request $tool $cwd 1234 $cf $images $cache $ext ($expectedCommands+"`n")}
 $request.ArgumentList[$index]=$original
}
foreach($extra in @('-c','-cfr','-cf','-p','-pn','-z','-pvr')){
 $request.ArgumentList.Add($extra)
 Reject {Assert-PreflightRequest $request $tool $cwd 1234 $cf $images $cache $ext ($expectedCommands+"`n")}
 $request.ArgumentList.RemoveAt($request.ArgumentList.Count-1)
}
Reject {Assert-PreflightRequest $request $tool $cwd 9999 $cf $images $cache $ext ($expectedCommands+"`n")}
foreach($injection in @('.shell whoami','!peb','r','dd 0','q','qq','qd; q')){
 Reject {Assert-PreflightRequest $request $tool $cwd 1234 $cf $images $cache $ext ($expectedCommands+"`n"+$injection+"`n")}
}
$markers="FONT_DEBUGGER_MODULES`nFONT_DEBUGGER_PUBLIC_OBSERVATION`nFONT_DEBUGGER_PRIVATE_OBSERVATION_PENDING`n"
Assert-PreflightRawMarkers $markers
Reject {Assert-PreflightRawMarkers ''}
Reject {Assert-PreflightRawMarkers ($markers+"FONT_DEBUGGER_MODULES`n")}
Reject {Assert-PreflightRawMarkers ($markers.Replace('FONT_DEBUGGER_PUBLIC_OBSERVATION','0:000> .echo FONT_DEBUGGER_PUBLIC_OBSERVATION'))}
$poisons=@('GH_TOKEN','GITHUB_TOKEN','ACTIONS_RUNTIME_TOKEN','_NT_SYMBOL_PATH','_NT_DEBUGGER_EXTENSION_PATH','UNLISTED_TEST_KEY')
$saved=@{}
try{
 foreach($key in $poisons){$saved[$key]=[Environment]::GetEnvironmentVariable($key);[Environment]::SetEnvironmentVariable($key,'POISON_NEVER_INHERIT')}
 foreach($isProbe in @($true,$false)){
  $psi=New-PreflightStartInfo 'D:\אב גד\probe.exe' 'D:\owned work\child' 'C:\Windows' 'D:\owned work\temp' $(if($isProbe){@()}else{$args}) $isProbe
  Check (!$psi.UseShellExecute -and $psi.CreateNoWindow -and $psi.RedirectStandardInput -and $psi.RedirectStandardError -and $psi.RedirectStandardOutput) 'Unsafe process metadata'
  $keys=@('SystemRoot','WINDIR','TEMP','TMP','PATH','GITHUB_ACTIONS');if($isProbe){$keys+= 'FONT_DEBUGGER_PREFLIGHT_CANARY'}
  Exact @($psi.Environment.Keys|Sort-Object) @($keys|Sort-Object)
  foreach($key in $poisons){Check (!$psi.Environment.ContainsKey($key)) 'Poison environment survived clear'}
  Check ($psi.Environment['PATH'] -ceq 'C:\Windows\System32') 'Unsafe PATH'
  Check ($psi.Environment['GITHUB_ACTIONS'] -ceq 'true') 'Missing CI environment'
  if($isProbe){Check ($psi.Environment['FONT_DEBUGGER_PREFLIGHT_CANARY'] -ceq 'probe-v1') 'Missing canary';Check ($psi.ArgumentList.Count -eq 0) 'Probe extra arguments'}else{Exact @($psi.ArgumentList) $expected}
 }
}finally{foreach($key in $poisons){[Environment]::SetEnvironmentVariable($key,$saved[$key])}}
# Synthetic trust metadata only: no real signatures, copies or binary opens.
function New-SyntheticTrust([string]$Name){
 @{relative=$Name;size=1024L;sha256=('a'*64);file_version='1.2.3.4';product_version='1.2.3.4';signature_status='Valid';signature_type='Authenticode';signature_message='synthetic fixture';signer='CN=Synthetic, O=Microsoft Corporation, C=US';signer_thumbprint=('B'*40);query_error=''}
}
$required=@('cdb.exe','dbgeng.dll','dbghelp.dll','symsrv.dll','winext\ext.dll')
$records=@($required|ForEach-Object{New-SyntheticTrust $_})
$admission=New-StageAdmission $records 1
Check ($admission.admitted.Count -eq 5 -and $admission.excluded.Count -eq 0) 'Eligible required synthetic set rejected'
$unsigned=New-SyntheticTrust 'optional.dll';$unsigned.signature_status='NotSigned';$unsigned.signature_type='None';$unsigned.signer='';$unsigned.signer_thumbprint=''
$admission=New-StageAdmission ($records+@($unsigned)) 1
Check ($admission.admitted.Count -eq 5 -and $admission.excluded.Count -eq 1 -and $admission.admitted.relative -cnotcontains 'optional.dll') 'Unsigned optional admitted'
foreach($field in @('signature_status','signer','file_version','product_version')){
 $bad=$records[0].Clone()
 switch($field){'signature_status'{$bad[$field]='NotTrusted'} 'signer'{$bad[$field]='CN=Synthetic, O=Other Company'} default{$bad[$field]=''}}
 Reject {New-StageAdmission (@($bad)+$records[1..4]) 1}
}
Reject {New-StageAdmission $records[1..4] 1}
Reject {New-StageAdmission ($records+@($records[0].Clone())) 1}
$collision=$records[0].Clone();$collision.relative='CDB.EXE'
Reject {New-StageAdmission ($records+@($collision)) 1}
foreach($bad in @($null,@{},@{relative='optional.dll'})){Reject {Assert-StageTrustRecord $bad}}
foreach($field in @('query_error','signature_status','signature_type','signer_thumbprint')){
 $bad=$records[0].Clone();$bad[$field]='MALFORMED_OR_QUERY_ERROR'
 Reject {Assert-StageTrustRecord $bad}
}
foreach($relative in @('winext\nested\ext.dll','..\cdb.exe','C:\cdb.exe','cdb.exe:ads','winext\NUL.dll','a.txt','winext\\ext.dll')){Reject {Assert-StageRelative $relative}}
foreach($field in @('sha256','size','file_version','product_version','signature_status','signature_type','signer','signer_thumbprint','relative')){
 $changed=$records[0].Clone()
 switch($field){'sha256'{$changed[$field]='b'*64} 'size'{$changed[$field]=2048L} 'signature_status'{$changed[$field]='NotTrusted'} 'signature_type'{$changed[$field]='Catalog'} 'signer_thumbprint'{$changed[$field]='C'*40} 'relative'{$changed[$field]='another.dll'} default{$changed[$field]='changed'}}
 Reject {Assert-StageIdentity $records[0] $changed}
}
Assert-StageMembership $required $required
Reject {Assert-StageMembership $required ($required+@('extra.dll'))}
Reject {Assert-StageMembership $required $required[1..4]}
Assert-StageMembership $required $required[1..4] $true
Reject {Assert-StageMembership $required @('CDB.EXE') $true}
Check ((Get-StageManifestText $records) -ceq (Get-StageManifestText @($records[4..0]))) 'Nondeterministic manifest ordering'
$large=$records[0].Clone();$large.size=134217729L
Reject {New-StageAdmission (@($large)+$records[1..4]) 1}
$aggregate=@($records|ForEach-Object{$r=$_.Clone();$r.size=134217728L;$r})
Reject {New-StageAdmission $aggregate 1}
$many=@($records)+@(0..123|ForEach-Object{New-SyntheticTrust ('optional'+$_+'.dll')})
Reject {New-StageAdmission $many 1}
$inventory=@(0..256|ForEach-Object{New-SyntheticTrust ('optional'+$_+'.dll')})
Reject {New-StageAdmission $inventory 1}
Reject {Assert-StageBudget 90000};Reject {Assert-StageBudget -1};Assert-StageBudget 89999
$manifestLarge=@(0..99|ForEach-Object{$r=New-SyntheticTrust ('member'+$_+'.dll');$r.file_version='v'*4096;$r.product_version='p'*4096;$r})
Reject {Get-StageManifestText $manifestLarge}
Assert-StageModule 'D:\owned\stage\cdb.exe' 'D:\owned\stage' 'C:\Windows\System32' $records $records[0]
Assert-StageModule 'D:\owned\stage\CDB.EXE' 'D:\owned\stage' 'C:\Windows\System32' $records $records[0]
$systemRecord=New-SyntheticTrust 'ntdll.dll'
Assert-StageModule 'C:\Windows\System32\ntdll.dll' 'D:\owned\stage' 'C:\Windows\System32' $records $systemRecord
Reject {Assert-StageModule 'C:\SDK\cdb.exe' 'D:\owned\stage' 'C:\Windows\System32' $records $records[0]}
Reject {Assert-StageModule 'D:\owned\stage\extra.dll' 'D:\owned\stage' 'C:\Windows\System32' $records (New-SyntheticTrust 'extra.dll')}
$wrong=$records[0].Clone();$wrong.sha256='b'*64
Reject {Assert-StageModule 'D:\owned\stage\cdb.exe' 'D:\owned\stage' 'C:\Windows\System32' $records $wrong}
Reject {Assert-StageModule 'C:\Windows\System32\optional.dll' 'D:\owned\stage' 'C:\Windows\System32' $records $unsigned}
# Source guards are not execution tests; verify critical lifetime and CI boundaries.
$text=$ast.Extent.Text
foreach($fn in @('Get-StageDiskRecord','Get-StageInventory','Initialize-SignedStage','Remove-ExactOwnedStage')){
 $node=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $fn},$true)
 Check ($node.Body.Extent.Text -match 'Assert-StageCI') 'Staging IO lacks Windows CI gate'
}
Check ($text -notmatch 'FileShare\]::Inheritable|Remove-Item[^\r\n]*-Recurse') 'Unsafe inherited stage handle/recursive removal'
Check ($text.IndexOf('$writer.Dispose()') -lt $text.IndexOf('$State.handles.Add(')) 'Stage writer remains open before held read'
Check ($text.IndexOf('Stop-ExactOwnedProcess $child') -lt $text.LastIndexOf('Remove-ExactOwnedStage $stage')) 'Stage cleanup precedes process cleanup'
Assert-OwnedStageCleanupPath 'D:\owned temp\font-debugger-abc\stage' 'D:\owned temp\font-debugger-abc'
foreach($badRoot in @('C:\SDK','C:\Windows\System32','D:\owned temp\font-debugger-abcd\stage','D:\owned temp\font-debugger-abc\stage\extra','D:\owned temp\font-debugger-abc\..\stage')){
 Reject {Assert-OwnedStageCleanupPath $badRoot 'D:\owned temp\font-debugger-abc'}
}
Check ($text -match 'if\(!\$bothCompleted\)\{throw[^\r\n]+stage handles/files retained') 'Missing completed-process guard before closing stage handles'
Check ($text -match 'tool_request_and_detach_completed=\(\$ok -and \$closed -and \$bothCompleted -and \$stageHandlesClosed -and !\$stageCleanupError\)') 'Success ignores cleanup/completion failure'
# Exercise the actual disk-record control flow in a finite child scope. Every
# external operation is shadowed before invocation; no runner body is evaluated.
$diskNode=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'Get-StageDiskRecord'},$true))
Check ($diskNode.Count -eq 1) 'Missing/duplicate mocked disk-record function'
foreach($case in @('success','query_throw','null','multiple','missing_property','null_status','bad_enum','bad_signer')){
 & {
  param($Definition,$Case)
  $mock=@{signature_calls=0;item_calls=0;hash_calls=0;events=[Collections.Generic.List[object]]::new()}
  $clock=[pscustomobject]@{ElapsedMilliseconds=1L}
  function Assert-StageCI {}
  function Resolve-PreflightLiteral([string]$Path){$Path}
  function Get-Item([string]$LiteralPath,[switch]$Force,[string]$ErrorAction){
   $mock.item_calls++
   [pscustomobject]@{Length=1024L;VersionInfo=[pscustomobject]@{FileVersion='1.2.3.4';ProductVersion='1.2.3.4'}}
  }
  function Get-FileHash([string]$LiteralPath,[string]$Algorithm){$mock.hash_calls++;[pscustomobject]@{Hash=('A'*64)}}
  function Get-AuthenticodeSignature([string]$LiteralPath,[string]$ErrorAction){
   $mock.signature_calls++
   $fake=[pscustomobject]@{Status='Valid';StatusMessage='synthetic query';SignatureType='Authenticode';SignerCertificate=[pscustomobject]@{Subject='CN=Synthetic, O=Microsoft Corporation, C=US';Thumbprint=('B'*40)}}
   switch($Case){
    'query_throw'{throw 'SYNTHETIC_SIGNATURE_QUERY_EXCEPTION'}
    'null'{return $null}
    'multiple'{return @($fake,$fake)}
    'missing_property'{$fake.PSObject.Properties.Remove('SignatureType')}
    'null_status'{$fake.Status=$null}
    'bad_enum'{$fake.Status='SYNTHETIC_INVALID_ENUM'}
    'bad_signer'{$fake.SignerCertificate=$null}
   }
   $fake
  }
  function Emit-PreflightRecord([string]$Stage,$Value){$mock.events.Add(@{stage=$Stage;value=$Value})}
  Invoke-Expression $Definition
  $threw=$false;$result=$null
  try{$result=Get-StageDiskRecord 'D:\synthetic only\stage\cdb.exe' 'cdb.exe'}catch{$threw=$true}
  Check ($mock.signature_calls -eq 1 -and $mock.item_calls -eq 1) ('Signature mock branch did not execute: '+$Case)
  if($Case -ceq 'success'){
   Check (!$threw -and $mock.hash_calls -eq 1 -and $mock.events.Count -eq 0) 'Mocked valid query unexpectedly failed'
   Check ($result.relative -ceq 'cdb.exe' -and $result.sha256 -ceq ('a'*64) -and (Test-StageEligible $result)) 'Mocked success identity differs'
  }else{
   Check $threw ('Signature failure became an optional exclusion: '+$Case)
   Check ($mock.events.Count -eq 1) ('Missing structured query diagnostic: '+$Case)
   $event=$mock.events[0]
   $expectedStage=if($Case -ceq 'query_throw'){'FONT_DEBUGGER_STAGE_SIGNATURE_QUERY_ERROR'}else{'FONT_DEBUGGER_STAGE_SIGNATURE_MALFORMED'}
   Check ($event.stage -ceq $expectedStage) 'Signature failure misclassified as unsigned exclusion'
   Check ($event.value.path -ceq 'D:\synthetic only\stage\cdb.exe' -and $event.value.relative -ceq 'cdb.exe') 'Diagnostic lost exact synthetic path/relative'
   Check (!$event.value.signature_validated -and !$event.value.Contains('copied')) 'Generic signature diagnostic asserts copy state'
   Check ($event.value.query_succeeded -eq ($Case -cne 'query_throw')) 'Query-success fact differs from mocked branch'
  }
 } $diskNode[0].Extent.Text $case
}
Write-Host ('FONT_DEBUGGER_PURE_TESTS checks='+$script:checks+' process_start_count=0 synthetic_stage_trust_only=true')

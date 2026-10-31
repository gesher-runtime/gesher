# Pure AST/ProcessStartInfo checks only. Never execute or dot-source runner body.
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'debugger-preflight.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$pure=@('Assert-OrdinaryWindowsPath','Join-WindowsLiteral','Select-UniqueDebugger','New-PreflightCommands','New-PreflightArguments','New-PreflightStartInfo','Assert-PreflightRequest','Assert-PreflightRawMarkers')
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
Write-Host ('FONT_DEBUGGER_PURE_TESTS checks='+$script:checks+' process_start_count=0')

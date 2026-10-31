# Pure AST, metadata and real managed root controls ONLY. No runner invocation.
$ErrorActionPreference='Stop'
function Parse([string]$Name){$t=$null;$e=$null;$a=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $Name),[ref]$t,[ref]$e);if($e.Count){throw ($e|Out-String)};$a}
$old=Parse 'debugger-preflight.ps1';$new=Parse 'resource-characterization.ps1'
$oldFunctions=@($old.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true))
$newFunctions=@($new.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true))
$script:checks=0
function Check($Value){if(!$Value){throw ('Resource metadata control failed at '+(Get-PSCallStack|Select-Object -Skip 1 -First 1).ScriptLineNumber)};$script:checks++}
function Reject([scriptblock]$Body){$bad=$false;try{& $Body|Out-Null}catch{$bad=$true};Check $bad}
foreach($node in $oldFunctions){$same=@($newFunctions|Where-Object{$_.Name -ceq $node.Name});Check ($same.Count -eq 1 -and $same[0].Extent.Text -ceq $node.Extent.Text)}
foreach($name in @('Assert-OrdinaryWindowsPath','New-PreflightCommands','New-PreflightArguments','New-PreflightStartInfo','Read-ResourceReady','New-ResourceCommands','Get-ResourceRemaining','New-StageLifetimeRoot','Release-StageLifetimeRoot')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
foreach($phase in @('PREPARED','HOLD_READY')){
 $record=Read-ResourceReady ("FONT_RESOURCE_$phase pid=12 writer=13 handler=14`n") 12 $phase
 Check ($record.writer -eq 13 -and $record.handler -eq 14)
 foreach($bad in @("FONT_RESOURCE_$phase pid=11 writer=13 handler=14`n","FONT_RESOURCE_$phase pid=12 writer=13 handler=13`n","FONT_RESOURCE_$phase pid=12 writer=0 handler=14`n","FONT_RESOURCE_$phase pid=12 writer=4294967296 handler=14`n","FONT_RESOURCE_$phase pid=12 writer=13;qd handler=14`n","FONT_RESOURCE_$phase pid=012 writer=13 handler=14`n")){Reject {Read-ResourceReady $bad 12 $phase}}
}
Reject {Read-ResourceReady '' 12 'COMPLETE'}
$commands=New-ResourceCommands 'C:\stage\winext' 13 14
Check ($commands.EndsWith("~~[0xe] kL 0x14`nqd"))
Check ($commands.Contains(".echo FONT_RESOURCE_WRITER`n~~[0xd] kL 0x14`n"))
Check ($commands -notmatch '(?m)^(uf|r|g|q|\.shell|!|\.dump)\b')
Reject {New-ResourceCommands 'C:\stage\winext' 13 13};Reject {New-ResourceCommands 'C:\stage\winext' 0 14}
Check ((Get-ResourceRemaining 10 20) -eq 10);Reject {Get-ResourceRemaining 20 20};Reject {Get-ResourceRemaining 21 20};Reject {Get-ResourceRemaining -1 20}
$psi=New-PreflightStartInfo 'C:\stage\cdb.exe' 'C:\owned\capture' 'C:\Windows' 'C:\owned\temp' @('only') $false
Check ($psi.Environment.Count -eq 6 -and !$psi.Environment.ContainsKey('GH_TOKEN') -and !$psi.Environment.ContainsKey('GITHUB_TOKEN') -and !$psi.Environment.ContainsKey('_NT_DEBUGGER_EXTENSION_PATH'))
$owner=@{handles=[Collections.Generic.List[object]]::new();processes=@{child=@{state='RUNNING'};imports=@{state='NOT_STARTED'};help=@{state='OMITTED'};prewarm=@{state='NOT_STARTED'};capture=@{state='NOT_STARTED'}}}
$root=New-StageLifetimeRoot $owner
try{Check ([object]::ReferenceEquals($root.handle.Target,$owner));Check (!(Release-StageLifetimeRoot $root $false $true));Check (!(Release-StageLifetimeRoot $root $true $false));Check $root.active;Check (Release-StageLifetimeRoot $root $true $true);Check (!$root.active)}finally{if($root.active){$root.handle.Free();$root.active=$false}}
Write-Host "FONT_RESOURCE_METADATA_PASS checks=$script:checks native_processes_started=0 real_tool_reads=0"

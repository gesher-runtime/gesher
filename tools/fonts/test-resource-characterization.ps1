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
foreach($name in @('Get-ResourcePathRoles','Get-ResourcePathDiagnostic','Resolve-ResourceLiteral')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
$single=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' 'C:\Python\python.exe'
Check ($single.count -eq 1 -and $single.items[0].prefix -ceq 'C:\Python\python.exe' -and $single.input_type -ceq 'System.String')
$multiple=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' @('C:\one\python.exe','C:\two\python.exe')
Check ($multiple.count -eq 2 -and $multiple.items.Count -eq 2 -and $multiple.input_type -ceq 'System.Object[]' -and !$multiple.selection_changed)
Check ((Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' $null).count -eq 0)
Check ((Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' '').items[0].length -eq 0)
Reject {Get-ResourcePathDiagnostic 'GH_TOKEN' 'SENSITIVE_CANARY'}
$secretBag=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' @{GH_TOKEN='SENSITIVE_CANARY';CommandLine='SECRET_LINE'}
$secretJson=ConvertTo-Json $secretBag -Depth 12 -Compress
Check ($secretJson -cnotmatch 'SENSITIVE_CANARY|SECRET_LINE|GH_TOKEN|CommandLine' -and !$secretBag.items[0].value_logged)
$long=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' ('x'*5000)
Check ($long.items[0].prefix.Length -eq 512 -and $long.items[0].prefix_utf16.Count -eq 512 -and $long.items[0].hash_scope -ceq 'FIRST_4096_UTF16_UNITS_ONLY')
$many=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' (@('C:\python.exe')*10)
Check ($many.count -eq 10 -and $many.items.Count -eq 8 -and $many.items_truncated)
$surrogate=Get-ResourcePathDiagnostic 'PYTHON_APPLICATION_SOURCE' ([string][char]0xd800)
Check ($surrogate.items[0].prefix_utf16[0] -ceq 'd800')
# Stub ONLY the original filesystem resolver boundary: exact old string binder
# and lexical predicate, without any CI/filesystem/tool operation. Test the new
# wrapper's metadata and rethrow behavior, never execute the real resolver body.
function Resolve-PreflightLiteral([string]$Path,[bool]$Directory=$false){Assert-OrdinaryWindowsPath $Path}
$script:pathRecords=[Collections.Generic.List[object]]::new()
function Emit-PreflightRecord([string]$Label,$Record){$script:pathRecords.Add(@{label=$Label;record=$Record})}
foreach($candidate in @('C:\Python\python.exe','C:relative','',123,@('C:\one.exe','C:\two.exe'),'C:\folder\..\python.exe')){
 $oldResult=$null;$newResult=$null;$oldError=$null;$newError=$null
 try{$oldResult=Resolve-PreflightLiteral $candidate}catch{$oldError=$_.Exception.Message}
 $script:pathRecords.Clear()
 try{$newResult=Resolve-ResourceLiteral 'PYTHON_APPLICATION_SOURCE' $candidate}catch{$newError=$_.Exception.Message}
 Check ($oldResult -ceq $newResult -and $oldError -ceq $newError)
 Check ($script:pathRecords[0].label -ceq 'FONT_RESOURCE_PATH_INPUT')
 if($oldError){Check ($script:pathRecords.Count -eq 2 -and $script:pathRecords[1].record.reason -ceq $oldError -and $script:pathRecords[1].record.input.role -ceq 'PYTHON_APPLICATION_SOURCE')}
}
# No original32 helper was modified and every direct resolver use introduced
# after the original helper prefix is routed through a fixed-role diagnostic.
$oldNames=@($oldFunctions|ForEach-Object{$_.Name})
foreach($function in $newFunctions|Where-Object{$_.Name -cnotin $oldNames -and $_.Name -cne 'Resolve-ResourceLiteral'}){
 $direct=@($function.FindAll({param($n)$n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -ceq 'Resolve-PreflightLiteral'},$true))
 Check ($direct.Count -eq 0)
}
$topCommands=@($new.EndBlock.Statements|ForEach-Object{$_.FindAll({param($n)$n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -ceq 'Resolve-PreflightLiteral'},$true)}|Where-Object{$_.Extent.StartLineNumber -gt $newFunctions[-1].Extent.EndLineNumber})
Check ($topCommands.Count -eq 0)
Write-Host "FONT_RESOURCE_METADATA_PASS checks=$script:checks native_processes_started=0 real_tool_reads=0"

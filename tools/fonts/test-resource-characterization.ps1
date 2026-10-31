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
foreach($name in @('Select-ResourcePythonSource')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
$kind='System.Management.Automation.ApplicationInfo'
$records=@(@{type=$kind;source='D:\first\python.exe'},@{type=$kind;source='C:\WindowsApps\python.exe'})
$script:pathRecords.Clear()
Check ((Select-ResourcePythonSource $records) -ceq 'D:\first\python.exe')
Check ($script:pathRecords.Count -eq 2 -and $script:pathRecords[1].record.selected_index -eq 0 -and !$script:pathRecords[1].record.fallback)
Check ($script:pathRecords[0].record.items[0].prefix -ceq 'D:\first\python.exe' -and $script:pathRecords[0].record.items[1].prefix -ceq 'C:\WindowsApps\python.exe')
Check ((Select-ResourcePythonSource @($records[1],$records[0])) -ceq 'C:\WindowsApps\python.exe')
Check ((Select-ResourcePythonSource @($records[0])) -ceq 'D:\first\python.exe')
foreach($bad in @(@(),(@($records[0])*9),@(@{type='AliasInfo';source='D:\python.exe'}),@(@{type=$kind;source=''}),@(@{type=$kind;source=@('D:\a.exe','D:\b.exe')}),@(@{type=$kind;source='D:\a.exe';extra='no'}),@('D:\python.exe'),@(@{type=$kind;source=$null}))){Reject {Select-ResourcePythonSource $bad}}
$script:pathRecords.Clear()
$badFirst=@(@{type=$kind;source='C:relative'},@{type=$kind;source='D:\valid\python.exe'})
Check ((Select-ResourcePythonSource $badFirst) -ceq 'C:relative')
Reject {Resolve-ResourceLiteral 'PYTHON_APPLICATION_SOURCE' (Select-ResourcePythonSource $badFirst)}
Check ($script:pathRecords[-1].label -ceq 'FONT_RESOURCE_PATH_REJECTED')
foreach($n in 1..8){Check ((Select-ResourcePythonSource (@($records[0])*$n)) -ceq 'D:\first\python.exe')}
$lookup=@($newFunctions|Where-Object{$_.Name -ceq 'Get-ResourcePythonSource'})[0].Extent.Text
Check ($lookup.Contains('Microsoft.PowerShell.Core\Get-Command -Name python -CommandType Application -All -TotalCount 9 -ErrorAction Stop'))
Check ($lookup.Contains('-isnot [System.Management.Automation.ApplicationInfo]'))
Check ($lookup -notmatch 'Sort-Object|Start-Process|\.Start\(|Invoke-Expression|Where-Object')
# New code/type controls are synthetic strings and AST only. No disassembler,
# provider metadata, signature or actual tool operation occurs here.
foreach($name in @('Get-ResourceCodeHash','Get-ResourceCodeProfile','New-ResourceCodeCommands','Read-ResourceCodeBlock','Read-ResourceLoadedUCRT','Read-ResourceCodeWindow','Read-ResourceTypeState')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
$hash='9601850e2ede2cd86b441223557cae0fa7f5e33b1dd9d481c58c8cc2d688bdc3'
Check ((Get-ResourceCodeProfile $hash '10.0.26100.33648').start -eq 0x3c1d8)
Check ((Get-ResourceCodeProfile 'd06d0a421398fb663c6c49586e09774143460fdb1dce489aa9844846b9398555' '10.0.20348.5756').start -eq 0xb84b4)
Check ((Get-ResourceCodeHash ([Text.Encoding]::ASCII.GetBytes('abc'))) -ceq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
Reject {Get-ResourceCodeProfile $hash 'different'};Reject {Get-ResourceCodeProfile ('0'*64) '10.0.26100.33648'}
$cmd=New-ResourceCodeCommands (New-ResourceCommands 'C:\stage\winext' 13 14) 0x3c1d8
Check ($cmd.EndsWith("`nqd") -and $cmd.Contains('u ucrtbase+0x3c1d8 ucrtbase+0x3c237') -and $cmd.Contains('dt -v -t ntdll!_RTL_CRITICAL_SECTION'))
Check ($cmd -cnotmatch '(?m)^(?:r |uf|ub|uu|\.frame|dt .*0x|!critsec)')
Check ((New-ResourceCodeCommands (New-ResourceCommands 'C:\stage\winext' 13 14) 0xb84b4).Contains('u ucrtbase+0xb84b4 ucrtbase+0xb8513'))
Reject {New-ResourceCodeCommands $cmd 0};Reject {New-ResourceCodeCommands 'qd' 0x3c1d8}
$base=[uint64]0x10000000;$start=0x3c1d8
$rows=@();foreach($i in 0..95){$rows+=('{0:x16} 90 nop' -f ($base+$start+$i))}
$text="FONT_RESOURCE_CODE_BEGIN`nucrtbase!synthetic:`n"+($rows -join "`n")+"`n0:000> .echo FONT_RESOURCE_CODE_END`nFONT_RESOURCE_CODE_END`n"
$cache='90'*111
$code=Read-ResourceCodeWindow $text $base $start $cache
Check ($code.state -ceq 'BYTE_MATCH' -and $code.rows.Count -eq 96 -and !$code.instruction_boundary_proved -and $code.internal_debugger_read_set -ceq 'UNKNOWN')
$straddleRows=$rows[0..94]+(('{0:x16} {1} synthetic-final' -f ($base+$start+95),('90'*15)))
$straddle="FONT_RESOURCE_CODE_BEGIN`n"+($straddleRows -join "`n")+"`nFONT_RESOURCE_CODE_END`n"
Check ((Read-ResourceCodeWindow $straddle $base $start $cache).accepted_last_rva -eq $start+109)
foreach($bad in @($text.Replace($rows[0],''),$text.Replace($rows[2],$rows[1]),$text.Replace('90 nop','?? unknown'),$text.Replace($rows[0],$rows[0].Replace('90','91')),$text.Replace($rows[-1],''),$text.Replace($rows[-1],$rows[-1]+"`n"+('{0:x16} 90 nop' -f ($base+$start+96))),$text+$text,$text.Replace('ucrtbase!synthetic:',('x'*8193)), $text.Replace($rows[0],$rows[0].Replace('90','909')))){
 Reject {Read-ResourceCodeWindow $bad $base $start $cache}
}
Reject {Read-ResourceCodeWindow $text ([uint64]::MaxValue) $start $cache}
Reject {Read-ResourceCodeWindow $text $base $start ('90'*110)}
$type="FONT_RESOURCE_TYPE_BEGIN`n0:000> dt -v -t ntdll!_RTL_CRITICAL_SECTION`nSymbol not found`nFONT_RESOURCE_TYPE_END`n"
Check ((Read-ResourceTypeState $type).state -ceq 'TYPE_UNAVAILABLE')
Check ((Read-ResourceTypeState ($type.Replace('Symbol not found','ntdll!_RTL_CRITICAL_SECTION'))).state -ceq 'TYPE_ENUM_ONLY')
Check ((Read-ResourceTypeState ($type.Replace('Symbol not found',"struct _RTL_CRITICAL_SECTION`n +0x010 OwningThread : Ptr64 Void"))).state -ceq 'TYPE_SCHEMA_PARTIAL')
$lmv="00000000``10000000 00000000``1014b000 ucrtbase (pdb symbols)`n    Image path: C:\Windows\System32\ucrtbase.dll`n    ImageSize: 0014b000`n    File version: 10.0.26100.33648`n0:000> qd`n"
Check ((Read-ResourceLoadedUCRT $lmv 'C:\Windows\System32\ucrtbase.dll' $base 0x14b000 '10.0.26100.33648').base -eq $base)
foreach($bad in @($lmv+$lmv,$lmv.Replace('10000000','20000000'),$lmv.Replace('1014b000','1014c000'),$lmv.Replace('System32','SysWOW64'),$lmv.Replace('0014b000','0014c000'),$lmv.Replace('33648','99999'))){Reject {Read-ResourceLoadedUCRT $bad 'C:\Windows\System32\ucrtbase.dll' $base 0x14b000 '10.0.26100.33648'}}
Check ($new.Extent.Text.Contains("$"+'stage.code.cache=$cache[0]'))
Check ($new.Extent.Text.IndexOf("Get-PreflightFileProfile $"+'codePath') -lt $new.Extent.Text.IndexOf('$child=Start-ResourceRole'))
Check ($new.Extent.Text.Contains("'CODE_ENVELOPE_SCRIPT' ($"+'checkout'))
Write-Host "FONT_RESOURCE_METADATA_PASS checks=$script:checks native_processes_started=0 real_tool_reads=0 actual_application_lookup=NOT_EXECUTED code_rows=SYNTHETIC"

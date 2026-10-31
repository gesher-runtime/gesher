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
# The entire parser body differs from immutable9df by exactly ONE literal test.
$priorRunner=(& git show '9df03ff1accac2c41a8831c490d2c6d1a0dcea53:tools/fonts/resource-characterization.ps1') -join "`n"
$priorTokens=$null;$priorErrors=$null
$priorAst=[Management.Automation.Language.Parser]::ParseInput($priorRunner,[ref]$priorTokens,[ref]$priorErrors)
Check ($priorErrors.Count -eq 0)
$priorWindow=@($priorAst.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'Read-ResourceCodeWindow'},$true))[0].Extent.Text
$currentWindow=@($newFunctions|Where-Object{$_.Name -ceq 'Read-ResourceCodeWindow'})[0].Extent.Text
$literalAddition=" -or "+'$line'+" -ceq 'ucrtbase!``common_putenv<char>''::``1''::fin"+'$0'+":'"
Check ($currentWindow.Replace($literalAddition,'') -ceq $priorWindow)
# Saved CI TEXT golden fixtures ONLY. No real PE/disassembler operation.
# Frozen run41187455486 source9df; both full blocks include original CDB echoes.
$golden2022=@'
0:000> u ucrtbase+0xb84b4 ucrtbase+0xb8513
ucrtbase!common_putenv<char>:
00007ffd`c14a84b4 48895c2408      mov     qword ptr [rsp+8],rbx
00007ffd`c14a84b9 57              push    rdi
00007ffd`c14a84ba 4883ec20        sub     rsp,20h
00007ffd`c14a84be 488bda          mov     rbx,rdx
00007ffd`c14a84c1 488bf9          mov     rdi,rcx
00007ffd`c14a84c4 488d0d9d7f0400  lea     rcx,[ucrtbase!_environ_table+0x1c8 (00007ffd`c14f0468)]
00007ffd`c14a84cb ff158ff50000    call    qword ptr [ucrtbase!__imp_EnterCriticalSection (00007ffd`c14b7a60)]
00007ffd`c14a84d1 90              nop
00007ffd`c14a84d2 488bd3          mov     rdx,rbx
00007ffd`c14a84d5 488bcf          mov     rcx,rdi
00007ffd`c14a84d8 e8a7000000      call    ucrtbase!common_putenv_nolock<char> (00007ffd`c14a8584)
00007ffd`c14a84dd 8bd8            mov     ebx,eax
00007ffd`c14a84df 488d0d827f0400  lea     rcx,[ucrtbase!_environ_table+0x1c8 (00007ffd`c14f0468)]
00007ffd`c14a84e6 ff156cf50000    call    qword ptr [ucrtbase!__imp_LeaveCriticalSection (00007ffd`c14b7a58)]
00007ffd`c14a84ec 8bc3            mov     eax,ebx
00007ffd`c14a84ee 488b5c2430      mov     rbx,qword ptr [rsp+30h]
00007ffd`c14a84f3 4883c420        add     rsp,20h
00007ffd`c14a84f7 5f              pop     rdi
00007ffd`c14a84f8 c3              ret
ucrtbase!`common_putenv<char>'::`1'::fin$0:
00007ffd`c14a84f9 4055            push    rbp
00007ffd`c14a84fb 4883ec20        sub     rsp,20h
00007ffd`c14a84ff 488bea          mov     rbp,rdx
00007ffd`c14a8502 488d0d5f7f0400  lea     rcx,[ucrtbase!_environ_table+0x1c8 (00007ffd`c14f0468)]
00007ffd`c14a8509 4883c420        add     rsp,20h
00007ffd`c14a850d 5d              pop     rbp
00007ffd`c14a850e 48ff2543f50000  jmp     qword ptr [ucrtbase!__imp_LeaveCriticalSection (00007ffd`c14b7a58)]
'@
$golden2022+=[char]10
Check ((Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($golden2022))) -ceq 'd8123c68ca2f6bb0feda5be0c67930d4eb88fb0d91d5701327087546dbc72d7f')
$full2022="FONT_RESOURCE_CODE_BEGIN`n"+$golden2022+"FONT_RESOURCE_CODE_END`n"
$result2022=Read-ResourceCodeWindow $full2022 ([uint64]140727845584896) 754868 '48895c2408574883ec20488bda488bf9488d0d9d7f0400ff158ff5000090488bd3488bcfe8a70000008bd8488d0d827f0400ff156cf500008bc3488b5c24304883c4205fc340554883ec20488bea488d0d5f7f04004883c4205d48ff2543f50000cccccccccccccc48895c24085748'
Check ($result2022.rows.Count -eq 26 -and (($result2022.rows|ForEach-Object{$_.hex.Length/2}|Measure-Object -Sum).Sum) -eq 97 -and !$result2022.instruction_boundary_proved)
$golden2025=@'
0:000> u ucrtbase+0x3c1d8 ucrtbase+0x3c237
ucrtbase!common_putenv<char>:
00007ffa`e326c1d8 48895c2408      mov     qword ptr [rsp+8],rbx
00007ffa`e326c1dd 57              push    rdi
00007ffa`e326c1de 4883ec20        sub     rsp,20h
00007ffa`e326c1e2 488bda          mov     rbx,rdx
00007ffa`e326c1e5 488bf9          mov     rdi,rcx
00007ffa`e326c1e8 488d0d89c80f00  lea     rcx,[ucrtbase!?m_CHPENameOffset@UnDecorator@@0KA+0x1e8 (00007ffa`e3368a78)]
00007ffa`e326c1ef ff15e3080c00    call    qword ptr [ucrtbase!__imp_EnterCriticalSection (00007ffa`e332cad8)]
00007ffa`e326c1f5 90              nop
00007ffa`e326c1f6 488bd3          mov     rdx,rbx
00007ffa`e326c1f9 488bcf          mov     rcx,rdi
00007ffa`e326c1fc e823000000      call    ucrtbase!common_putenv_nolock<char> (00007ffa`e326c224)
00007ffa`e326c201 8bd8            mov     ebx,eax
00007ffa`e326c203 488d0d6ec80f00  lea     rcx,[ucrtbase!?m_CHPENameOffset@UnDecorator@@0KA+0x1e8 (00007ffa`e3368a78)]
00007ffa`e326c20a ff15e0080c00    call    qword ptr [ucrtbase!__imp_LeaveCriticalSection (00007ffa`e332caf0)]
00007ffa`e326c210 8bc3            mov     eax,ebx
00007ffa`e326c212 488b5c2430      mov     rbx,qword ptr [rsp+30h]
00007ffa`e326c217 4883c420        add     rsp,20h
00007ffa`e326c21b 5f              pop     rdi
00007ffa`e326c21c c3              ret
00007ffa`e326c21d cc              int     3
00007ffa`e326c21e cc              int     3
00007ffa`e326c21f cc              int     3
00007ffa`e326c220 cc              int     3
00007ffa`e326c221 cc              int     3
00007ffa`e326c222 cc              int     3
00007ffa`e326c223 cc              int     3
ucrtbase!common_putenv_nolock<char>:
00007ffa`e326c224 48895c2408      mov     qword ptr [rsp+8],rbx
00007ffa`e326c229 48896c2410      mov     qword ptr [rsp+10h],rbp
00007ffa`e326c22e 4889742418      mov     qword ptr [rsp+18h],rsi
00007ffa`e326c233 57              push    rdi
00007ffa`e326c234 4883ec20        sub     rsp,20h
'@
$golden2025+=[char]10
Check ((Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($golden2025))) -ceq 'd5ebcf231bc9d7f86443cb6f20a4a7ee80a99736252e95a225a5305409054619')
$full2025="FONT_RESOURCE_CODE_BEGIN`n"+$golden2025+"FONT_RESOURCE_CODE_END`n"
$result2025=Read-ResourceCodeWindow $full2025 ([uint64]140715529273344) 246232 '48895c2408574883ec20488bda488bf9488d0d89c80f00ff15e3080c0090488bd3488bcfe8230000008bd8488d0d6ec80f00ff15e0080c008bc3488b5c24304883c4205fc3cccccccccccccc48895c240848896c24104889742418574883ec20488bf2488bf9e81981fdff488d2db6'
Check ($result2025.rows.Count -eq 31 -and (($result2025.rows|ForEach-Object{$_.hex.Length/2}|Measure-Object -Sum).Sum) -eq 96 -and !$result2025.instruction_boundary_proved)
$exactScoped='ucrtbase!`common_putenv<char>''::`1''::fin$0:'
foreach($badHeader in @($exactScoped.Replace('ucrtbase!','other!'),$exactScoped.Replace('::',':'),$exactScoped.Replace('::',':::'),($exactScoped+':'),($exactScoped+'junk'),(' '+$exactScoped),($exactScoped+' '), $exactScoped.Replace('fin$0','fin$1'),$exactScoped.Replace('fin$0','qd;fin$0'),$exactScoped.Replace('char',''),$exactScoped.Replace('fin$0',('x'*257)),$exactScoped.Replace('fin$0',('x'+[char]0+'y')),$exactScoped.Replace('fin$0',('x'+[char]0x5d0+'y')),$exactScoped.Replace('fin$0',"x`ny"))){
 Reject {Read-ResourceCodeWindow ($full2022.Replace($exactScoped,$badHeader)) ([uint64]0x7ffdc13f0000) 0xb84b4 '48895c2408574883ec20488bda488bf9488d0d9d7f0400ff158ff5000090488bd3488bcfe8a70000008bd8488d0d827f0400ff156cf500008bc3488b5c24304883c4205fc340554883ec20488bea488d0d5f7f04004883c4205d48ff2543f50000cccccccccccccc48895c24085748'}
}
# The exact extra header consumes no bytes and cannot hide invalid machine rows.
$machineRows=$result2022.rows
$firstText=@($golden2022 -split "`n"|Where-Object{$_ -match '^00007ffd`c14a84b4 '})[0]
foreach($changed in @($full2022.Replace($firstText,''),$full2022.Replace($firstText,$firstText+"`n"+$firstText),$full2022.Replace('c14a84b9','c14a84ba'),$full2022.Replace('c14a84b9','c14a84b8'),$full2022.Replace('48895c2408','48895c240'),$full2022.Replace('48895c2408','48'*16),$full2022.Replace('48895c2408','??'),$full2022.Replace('48895c2408','48895c2409'),$full2022.Replace('mov     qword ptr [rsp+8],rbx','???'))){
 Reject {Read-ResourceCodeWindow $changed ([uint64]0x7ffdc13f0000) 0xb84b4 '48895c2408574883ec20488bda488bf9488d0d9d7f0400ff158ff5000090488bd3488bcfe8a70000008bd8488d0d827f0400ff156cf500008bc3488b5c24304883c4205fc340554883ec20488bea488d0d5f7f04004883c4205d48ff2543f50000cccccccccccccc48895c24085748'}
}
# Extra address even after valid scoped label must fail start<=E.
Reject {Read-ResourceCodeWindow ($full2022.Replace("FONT_RESOURCE_CODE_END`n","00007ffd``c14a8515 90 nop`nFONT_RESOURCE_CODE_END`n")) ([uint64]0x7ffdc13f0000) 0xb84b4 '48895c2408574883ec20488bda488bf9488d0d9d7f0400ff158ff5000090488bd3488bcfe8a70000008bd8488d0d827f0400ff156cf500008bc3488b5c24304883c4205fc340554883ec20488bea488d0d5f7f04004883c4205d48ff2543f50000cccccccccccccc48895c24085748'}
# Witness tests use synthetic strings and managed-memory task doubles only.
foreach($name in @('New-ResourceWitnessPrewarmCommands','New-ResourceWitnessCaptureCommands','New-ResourceOwnerTail','Wait-ResourceInputReady','Send-ResourceControlOnce','Assert-ResourceWitnessProfiles','Write-ResourceNtdllDiagnostic','Read-ResourceWitnessSchema','Read-ResourceWitnessFrames','Get-ResourceWitnessAddress','Read-ResourceOwnerField','Assert-ResourceWitnessRootTasks')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
$coldPlan=New-ResourceWitnessPrewarmCommands 'C:\stage\winext' 0x3c1d8
$heldPlan=New-ResourceWitnessCaptureCommands 'C:\stage\winext' 13 14 0x3c1d8
Check ($coldPlan.EndsWith(".echo FONT_RESOURCE_PREWARM_INPUT_READY`n") -and !$coldPlan.EndsWith("qd`n"))
Check ($heldPlan.EndsWith(".echo FONT_RESOURCE_WITNESS_INPUT_READY`n") -and $heldPlan.Contains('~~[0xd] kL 0x14') -and !$heldPlan.Contains('OwningThread 0x'))
$tail=New-ResourceOwnerTail ([uint64]0x10003200)
Check ($tail -ceq ".echo FONT_RESOURCE_OWNER_BEGIN`ndt ntdll!_RTL_CRITICAL_SECTION OwningThread 0x0000000010003200`n.echo FONT_RESOURCE_OWNER_END`nqd`n")
foreach($bad in @(0,1,0x10003201,[uint64]::MaxValue)){Reject {New-ResourceOwnerTail $bad}}
$schema="struct _RTL_CRITICAL_SECTION, 6 elements, 0x28 bytes`n   +0x000 DebugInfo        : Ptr64 to struct _RTL_CRITICAL_SECTION_DEBUG, 9 elements, 0x30 bytes`n   +0x008 LockCount        : Int4B`n   +0x00c RecursionCount   : Int4B`n   +0x010 OwningThread     : Ptr64 to Void`n   +0x018 LockSemaphore    : Ptr64 to Void`n   +0x020 SpinCount        : Uint8B`n"
$schemaText="FONT_RESOURCE_TYPE_BEGIN`n0:000> dt -v -t ntdll!_RTL_CRITICAL_SECTION`n"+$schema+"FONT_RESOURCE_TYPE_END`n"
Check ((Read-ResourceWitnessSchema $schemaText).fields -eq 6)
foreach($bad in @($schemaText.Replace('0x010','0x014'),$schemaText.Replace('Ptr64 to Void','Ptr32 to Void'),$schemaText.Replace('6 elements','7 elements'),$schemaText+$schemaText)){Reject {Read-ResourceWitnessSchema $bad}}
# Saved 88a/run41201795245/a1/job124539168894 rawSHA c042b6f544172a89138c031f301875c06ff207b00797ea6073fff196be277bb2.
# PlanSHA49217fc784065339d10ee0eab402dfb84f815453378a50bb7544707474400f63.
function Test-OwnerSession([scriptblock]$Mutate) {
 $address=[uint64]0x7fff0e608a78;$handler=[uint32]6060;$writer=[uint32]9820
 $ready="FONT_RESOURCE_WITNESS_INPUT_READY`n0:000> "
 $tail="FONT_RESOURCE_OWNER_BEGIN`n0:000>    +0x010 OwningThread : 0x00000000``000017ac Void`n0:000> FONT_RESOURCE_OWNER_END`n0:000> quit:`n"
 $h=[pscustomobject]@{IsClosed=$false};$proc=[pscustomobject]@{Id=8044;SafeHandle=$h;ExitCode=0;StartInfo=@{FileName='C:\stage\cdb.exe'}}
 $proc|Add-Member ScriptMethod WaitForExit {param($ms) $true}
 $task=[Threading.Tasks.Task]::CompletedTask;$buffer=[Text.Encoding]::ASCII.GetBytes((New-ResourceOwnerTail $address))
 $r=@{role='capture';state='EXITED';pid=8044;handle=$h;process=$proc;out=@{Closed=$true;Text=[Text.StringBuilder]::new($ready+$tail)};err=@{Closed=$true;Text=[Text.StringBuilder]::new()};control_reserved=$true;controlProcess=$proc;controlHandle=$h;controlPid=8044;writeBuffer=$buffer;controlHash=(Get-ResourceCodeHash $buffer);writeTask=$task;controlCompletedTask=$task;readySnapshot=$ready;readyHash=(Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($ready)))}
 $profile=@{relative='cdb.exe';signature_status='Valid';sha256=('a'*64);file_version='saved-profile'}
 $session=@{record=$r;process=$proc;handle=$h;pid=8044;ready=$ready;readyHash=$r.readyHash;address=$address;handler=$handler;writer=$writer;childPid=5916;profile=$profile}
 $r.ownerWitness=$session;$o=@{root='C:\stage';handles=@();processes=@{capture=$r;child=@{pid=5916}};ownerWitness=$session;code=@{armedAddress=$address};manifest=@($profile)}
 $session.profileHash=$profile.sha256;$session.profileVersion=$profile.file_version
 $script:root=New-StageLifetimeRoot $o;$script:phase='HOLD_READY'
 try{& $Mutate $o $r $session;Read-ResourceOwnerField $r.out.Text.ToString() $address $handler $writer $o}finally{if($root.active){$root.handle.Free();$root.active=$false}}
}
Check ((Test-OwnerSession {}).owner -eq 6060)
$ownerNegatives=@(
 {param($o,$r,$s)$script:phase='PREPARED'},
 {param($o,$r,$s)$s.pid++},
 {param($o,$r,$s)$s.address+=8},
 {param($o,$r,$s)$s.handler++},
 {param($o,$r,$s)$s.writer++},
 {param($o,$r,$s)$s.childPid++},
 {param($o,$r,$s)$s.profile=@{}},
 {param($o,$r,$s)$s.profile.signature_status='NotSigned'},
 {param($o,$r,$s)$s.profile.sha256='bad'},
 {param($o,$r,$s)$r.control_reserved=$false},
 {param($o,$r,$s)$r.controlPid++},
 {param($o,$r,$s)$r.controlHash='0'*64},
 {param($o,$r,$s)$r.writeBuffer=[Text.Encoding]::ASCII.GetBytes("qd`n")},
 {param($o,$r,$s)$r.controlCompletedTask=[Threading.Tasks.Task]::Delay(100)},
 {param($o,$r,$s)$r.writeTask=[Threading.Tasks.Task]::FromException([Exception]::new('failed'))},
 {param($o,$r,$s)$r.err.Closed=$false},
 {param($o,$r,$s)$r.out.Closed=$false},
 {param($o,$r,$s)$r.err.Text.Append('error')|Out-Null},
 {param($o,$r,$s)$r.process.Id++},
 {param($o,$r,$s)$r.handle.IsClosed=$true},
 {param($o,$r,$s)$r.process.ExitCode=1},
 {param($o,$r,$s)$r.state='RUNNING'},
 {param($o,$r,$s)$r.readyHash='0'*64},
 {param($o,$r,$s)$s.ready+='changed'},
 {param($o,$r,$s)$r.out.Text.Replace('0:000>','1:000>')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('+0x010','+0x018')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace(' Void',' Other')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('00000000`000017ac','00000001`000017ac')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('000017ac','0000265c')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('000017ac','00000000')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('0x00000000`000017ac','6060')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Append('extra')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Append('x'*8193)|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('FONT_RESOURCE_OWNER_END','FONT_RESOURCE_OWNER_BEGIN')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('quit:','dt other')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('OwningThread :','OwningThread : ??')|Out-Null},
 {param($o,$r,$s)$r.out.Text.Replace('FONT_RESOURCE_OWNER_BEGIN','FONT_RESOURCE_OWNER_BEGIN`nFONT_RESOURCE_OWNER_BEGIN')|Out-Null}
)
Check ($ownerNegatives.Count -eq 37)
foreach($mutate in $ownerNegatives){Reject {Test-OwnerSession $mutate}}
$packet=@{start_rva=0x3c1d8;function=@{begin=0x3c1d8;end=0x3c21d;nolock_begin=0x3c224;nolock_end=0x3c300};basis=@{function_begin=0x3c1d8;function_end=0x3c21d;candidate_rva=0x138a78;instruction_ends=@(5,6,10,13,16,23,29,30,33,36,41,43,50,56,58,63,67,68,69);writer_return_rva=0x3c1f5;handler_return_rva=0x3c201;virtual_object=@{rva=0x130000;extent=0x20000;flags=[uint32]3221225536}}}
$packet.unwind=@{scope='POST_PROLOGUE_COMMON_BODY';partial_prologue_qualified=$false;runtime_unwind_integrity=$false;prolog_bytes=10}
$packet.basis.scope='POST_PROLOGUE_COMMON_BODY';$packet.basis.partial_prologue_qualified=$false;$packet.basis.runtime_unwind_integrity=$false
$packet.basis.body_after_rva=$packet.start_rva+10;$packet.basis.body_before_rva=$packet.start_rva+58
Check ((Get-ResourceWitnessAddress $packet ([uint64]0x10000000) $result2025) -eq [uint64]0x10138a78)
Reject {Get-ResourceWitnessAddress $packet ([uint64]::MaxValue) $result2025}
$fixture=@{base=[uint64]0x20000000;ucrt_base=[uint64]0x10000000;frames=@{Invalid=@{begin_rva=0x400;end_rva=0x440};Writer=@{begin_rva=0x200;end_rva=0x240};Handler=@{begin_rva=0x300;end_rva=0x340}}}
$frames=@'
FONT_RESOURCE_WRITER
0:000> ~~[0xd] kL 0x14
00000000`30001000 00000000`20000220 ntdll!NtWaitForAlertByThreadId+0x14
00000000`30001020 00000000`1003c1f5 ntdll!RtlEnterCriticalSection+0x42
00000000`30001040 00000000`20000220 ucrtbase!common_putenv<char>+0x1d
00000000`30001060 00000000`00000000 font_environment_resource_characterization+0x220
0:000> .echo FONT_RESOURCE_HANDLER
FONT_RESOURCE_HANDLER
0:000> ~~[0xe] kL 0x14
00000000`30002000 00000000`40000100 ntdll!NtWaitForSingleObject+0x14
00000000`30002020 00000000`20000420 KERNELBASE!WaitForSingleObjectEx+0x8e
00000000`30002040 00000000`1003c250 font_environment_resource_characterization+0x420
00000000`30002060 00000000`1003c201 ucrtbase!common_putenv_nolock<char>+0x68
00000000`30002080 00000000`20000320 ucrtbase!common_putenv<char>+0x29
00000000`300020a0 00000000`00000000 font_environment_resource_characterization+0x320
0:000> .echo FONT_RESOURCE_CODE_BEGIN
'@
Check ((Read-ResourceWitnessFrames $frames @{writer=[uint32]13;handler=[uint32]14} $packet $fixture).HANDLER.fixture_frames.Count -eq 2)
foreach($bad in @($frames.Replace('1003c1f5','1003c1f6'),$frames.Replace('1003c201','1003c202'),$frames.Replace('~~[0xe]','~~[0xd]'),$frames.Replace('20000420','20000520'),$frames.Replace('KERNELBASE!WaitForSingleObjectEx','KERNELBASE!Other'),$frames+$frames,$frames.Replace('30002060','30002020'))){Reject {Read-ResourceWitnessFrames $bad @{writer=[uint32]13;handler=[uint32]14} $packet $fixture}}
foreach($part in @('unwind','basis')){
 foreach($key in @('scope','partial_prologue_qualified','runtime_unwind_integrity')){
  foreach($value in @($null,0,'false',$true)){
   $bad=ConvertFrom-Json -AsHashtable ($packet|ConvertTo-Json -Depth 20);$bad[$part][$key]=$value
   Reject {Get-ResourceWitnessAddress $bad ([uint64]0x10000000) $result2025}
   Reject {Read-ResourceWitnessFrames $frames @{writer=[uint32]13;handler=[uint32]14} $bad $fixture}
  }
  $bad=ConvertFrom-Json -AsHashtable ($packet|ConvertTo-Json -Depth 20);$bad[$part].Remove($key)
  Reject {Get-ResourceWitnessAddress $bad ([uint64]0x10000000) $result2025}
  Reject {Read-ResourceWitnessFrames $frames @{writer=[uint32]13;handler=[uint32]14} $bad $fixture}
 }
}
foreach($offset in @(0,5,6,10,11,28,30,40,42,58,63,67,68,69)){
 $bad=ConvertFrom-Json -AsHashtable ($packet|ConvertTo-Json -Depth 20);$bad.basis.writer_return_rva=$packet.start_rva+$offset
 Reject {Get-ResourceWitnessAddress $bad ([uint64]0x10000000) $result2025}
 Reject {Read-ResourceWitnessFrames $frames @{writer=[uint32]13;handler=[uint32]14} $bad $fixture}
 $badPC=([uint64]0x10000000+$packet.start_rva+$offset).ToString('x8')
 Reject {Read-ResourceWitnessFrames ($frames.Replace('1003c1f5',$badPC)) @{writer=[uint32]13;handler=[uint32]14} $packet $fixture}
}
# No actual OS Process. ScriptMethods and MemoryStream model completed IO.
$clock=[Diagnostics.Stopwatch]::StartNew();$memory=[IO.MemoryStream]::new()
$inputDouble=[pscustomobject]@{BaseStream=$memory}
$inputDouble|Add-Member ScriptMethod Close { $this.BaseStream.Flush() }
$processDouble=[pscustomobject]@{Id=22;StandardInput=$inputDouble}
$processDouble|Add-Member ScriptMethod WaitForExit { param($milliseconds) $false }
$mock=@{role='capture';state='RUNNING';pid=22;handle=@{IsClosed=$false};process=$processDouble}
Send-ResourceControlOnce $mock $tail 90000
Check ($mock.control_reserved -and $mock.writeTask.IsCompleted -and [Text.Encoding]::ASCII.GetString($memory.ToArray()) -ceq $tail)
Reject {Send-ResourceControlOnce $mock $tail 90000}
Assert-ResourceWitnessRootTasks @{processes=@{capture=$mock}}
$mock.writeBuffer=$null;Reject {Assert-ResourceWitnessRootTasks @{processes=@{capture=$mock}}}
$memory.Dispose()
# Ready/prompt logic is exercised with a no-IO Update-BoundedPipe test double.
# Original helper bytes were compared above; the actual reader is never invoked.
function Update-BoundedPipe($State) {}
$clock=[Diagnostics.Stopwatch]::StartNew()
$readyDouble=@{role='prewarm';pid=22;handle=@{IsClosed=$false};process=$processDouble;out=@{Text=[Text.StringBuilder]::new("FONT_RESOURCE_PREWARM_INPUT_READY`n0:000> ")};err=@{Text=[Text.StringBuilder]::new()}}
Check ((Wait-ResourceInputReady $readyDouble 'PREWARM' 90000).EndsWith('0:000> '))
$readyDouble.pid=23;Reject {Wait-ResourceInputReady $readyDouble 'PREWARM' 90000};$readyDouble.pid=22
$readyDouble.out.Text.Append("`nFONT_RESOURCE_PREWARM_INPUT_READY`n0:000> ")|Out-Null
Reject {Wait-ResourceInputReady $readyDouble 'PREWARM' 90000}
$readyDouble.err.Text.Append('diagnostic error')|Out-Null
Reject {Wait-ResourceInputReady $readyDouble 'PREWARM' 90000}
Reject {Wait-ResourceInputReady $readyDouble 'CAPTURE' 90000}
$readyDouble.err.Text.Clear()|Out-Null;$readyDouble.out.Text.Clear()|Out-Null
$readyDouble.out.Text.Append("FONT_RESOURCE_PREWARM_INPUT_READY`n0:000> `nUNEXPECTED_TRAILING_DATA")|Out-Null
Reject {Wait-ResourceInputReady $readyDouble 'PREWARM' ($clock.ElapsedMilliseconds+20)}
$ntdllText="00000000``20000000 00000000``20001000 ntdll (pdb symbols)`n    Image path: C:\Windows\System32\ntdll.dll`n    ImageSize: 00001000`n    File version: 10.0.1.2`n0:000> qd`n"
$cacheDouble=@{size_of_image=0x14b000;profile=@{version='10.0.26100.33648'}}
$codeDouble=@{path='C:\Windows\System32\ucrtbase.dll';base=[uint64]0x10000000}
$ntdllDouble=@{path='C:\Windows\System32\ntdll.dll';base=[uint64]0x20000000;extent=0x1000;version='10.0.1.2'}
Assert-ResourceWitnessProfiles ($lmv+$ntdllText) $cacheDouble $codeDouble $ntdllDouble
Check $true
foreach($bad in @(($lmv+$ntdllText).Replace('20001000','20002000'),($lmv+$ntdllText).Replace('10.0.1.2','10.0.1.3'),($lmv+$ntdllText).Replace('System32\ntdll','SysWOW64\ntdll'))){Reject {Assert-ResourceWitnessProfiles $bad $cacheDouble $codeDouble $ntdllDouble}}
# Exact saved 88a/run41201795245/a1/job124539168941 NTDLL block (raw SHA cb30893c2764bd8fb18003771e97fea31a532870e603123f5e8452b14e212d56).
$savedNtdll=@'
00007ffd`f5bc0000 00007ffd`f5dc4000   ntdll      (pdb symbols)          c:\users\runneradmin\appdata\local\temp\font-resource-c1faf5bbb63545b9974cfcbcf991a2ea\cache\ntdll.pdb\58CA28F4C1F00E50F15DE78A44B054C21\ntdll.pdb
    Loaded symbol image file: C:\Windows\SYSTEM32\ntdll.dll
    Image path: C:\Windows\SYSTEM32\ntdll.dll
    Image name: ntdll.dll
    Image was built with /Brepro flag.
    Timestamp:        D0106634 (This is a reproducible build file hash, not a timestamp)
    CheckSum:         00213EC2
    ImageSize:        00204000
    File version:     10.0.20348.5756
    Product version:  10.0.20348.5756
    File flags:       0 (Mask 3F)
    File OS:          40004 NT Win32
    File type:        2.0 Dll
    File date:        00000000.00000000
    Translations:     0409.04b0
    Information from resource tables:
        CompanyName:      Microsoft Corporation
        ProductName:      Microsoft� Windows� Operating System
        InternalName:     ntdll.dll
        OriginalFilename: ntdll.dll
        ProductVersion:   10.0.20348.5756
        FileVersion:      10.0.20348.5756 (WinBuild.160101.0800)
        FileDescription:  NT Layer DLL
        LegalCopyright:   � Microsoft Corporation. All rights reserved.
'@
Check ((Get-ResourceCodeHash ([Text.Encoding]::UTF8.GetBytes($savedNtdll+"`n"))) -ceq '079483c383ad0bfffab61e34ca26a841b0499f66b48aa8879b920cc8f886b1f1')
$retained=@{path='C:\Windows\SYSTEM32\ntdll.dll';base=[uint64]0x7ffdf5bc0000;extent=0x204000;version='10.0.20348.5622';sha256='bcfe8a70a371a7d2fbcb429ab0a9e170aeebe4d73ae08d03a2da8fedec2efc78';context=@{source='88a02325e724ff1ee6393aa68c9cbea6a6258aa1';run='41201795245';attempt='1'}}
# Expected base/extent HERE are SYNTHETIC controls, not recovered historical expectations.
$before=$script:pathRecords.Count
Reject {Assert-ResourceWitnessProfiles ($lmv+$savedNtdll+"`n0:000> qd`n") $cacheDouble $codeDouble $retained}
$diag=$script:pathRecords[$script:pathRecords.Count-1].record
Check ($diag.selected_count -eq 1 -and !$diag.received[0].outcomes.version -and $diag.expected.version -ceq '10.0.20348.5622')
Check ($diag.received[0].fields['Product version'][0] -ceq '10.0.20348.5756')
$positive=$retained.Clone();$positive.version='10.0.20348.5756'
Assert-ResourceWitnessProfiles ($lmv+$savedNtdll+"`n0:000> qd`n") $cacheDouble $codeDouble $positive
Check $true # Explicit synthetic equal-profile sensitivity, not a new admission.
$ntNegatives=@(
 ($savedNtdll.Replace('f5bc0000','f5bb0000')),
 ($savedNtdll.Replace('f5dc4000','f5dc5000')),
 ($savedNtdll.Replace('ImageSize:        00204000','ImageSize:        00205000')),
 ($savedNtdll.Replace('Image path: C:\Windows\SYSTEM32','Image path: C:\Windows\SysWOW64')),
 ($savedNtdll.Replace('File version:     10.0.20348.5756','File version:     10.0.20348.5622')),
 ($savedNtdll.Replace('    File version:     10.0.20348.5756','')),
 ($savedNtdll.Replace('    ImageSize:        00204000','    ImageSize:        malformed')),
 ($savedNtdll+"`n"+$savedNtdll)
)
Check ($ntNegatives.Count -eq 8)
foreach($bad in $ntNegatives){Reject {Assert-ResourceWitnessProfiles ($lmv+$bad+"`n0:000> qd`n") $cacheDouble $codeDouble $positive}}
# Actual resource-only cleanup/barrier functions, managed fakes ONLY.
foreach($name in @('Test-ResourceTerminalTasks','Close-ResourceRecord','Settle-ResourceRecord','Complete-ResourceSettlement','Wait-ResourceRole')){
 $node=@($newFunctions|Where-Object{$_.Name -ceq $name});Check ($node.Count -eq 1);Invoke-Expression $node[0].Extent.Text
}
function New-SettlementFake {
 $m=@{kernel=$false;killThrows=$false;killCompletes=$false;waitThrows=$false;disposeThrows=$false;kills=0;streamDisposes=0;processDisposes=0}
 $h=[pscustomobject]@{IsClosed=$false}
 $p=[pscustomobject]@{Id=42;SafeHandle=$h;m=$m}
 $p|Add-Member ScriptMethod WaitForExit {param($ms) if($this.m.waitThrows){$this.m.waitThrows=$false;throw 'query interruption'};return $this.m.kernel}
 $p|Add-Member ScriptMethod Kill {$this.m.kills++;if($this.m.killThrows){throw 'kill failed'};if($this.m.killCompletes){$this.m.kernel=$true}}
 $p|Add-Member ScriptMethod Dispose {$this.m.processDisposes++}
 foreach($name in @('StandardInput','StandardOutput','StandardError')){
  $stream=[pscustomobject]@{m=$m};$stream|Add-Member ScriptMethod Dispose {$this.m.streamDisposes++;if($this.m.disposeThrows){throw 'dispose failed'}}
  $p|Add-Member NoteProperty $name $stream
 }
 $r=@{process=$p;settlementProcess=$p;pid=42;settlementPid=42;handle=$h;settlementHandle=$h;state='RUNNING';out=@{Task=$null};err=@{Task=$null};writeTask=$null}
 $o=@{processes=@{capture=$r};failed=$false;settlementErrors=[Collections.Generic.List[string]]::new();handles=@()}
 @{record=$r;owner=$o;metrics=$m}
}
$f=New-SettlementFake;$f.metrics.killThrows=$true
Reject {Settle-ResourceRecord $f.owner $f.record}
Check ($f.metrics.kills -eq 1 -and $f.metrics.streamDisposes -eq 0 -and $f.metrics.processDisposes -eq 0)
Check (!(Settle-ResourceRecord $f.owner $f.record));Check ($f.metrics.kills -eq 1)
$f.metrics.kernel=$true;Check (Settle-ResourceRecord $f.owner $f.record)
Check (Settle-ResourceRecord $f.owner $f.record);Check ($f.metrics.streamDisposes -eq 3 -and $f.metrics.processDisposes -eq 1)
$f=New-SettlementFake
Check (!(Settle-ResourceRecord $f.owner $f.record));Check ($f.metrics.kills -eq 1 -and $f.metrics.streamDisposes -eq 0)
foreach($where in @('read','write')){
 $f=New-SettlementFake;$f.metrics.kernel=$true;$tcs=[Threading.Tasks.TaskCompletionSource[int]]::new()
 if($where -ceq 'read'){$f.record.out.Task=$tcs.Task}else{$f.record.writeTask=$tcs.Task}
 Check (!(Settle-ResourceRecord $f.owner $f.record));Check ($f.metrics.streamDisposes -eq 0)
 $tcs.SetResult(0);Check (Settle-ResourceRecord $f.owner $f.record)
}
$f=New-SettlementFake;$f.metrics.kernel=$true;$f.record.out.Task=[Threading.Tasks.Task]::FromException([Exception]::new('reader error'))
Check (Settle-ResourceRecord $f.owner $f.record);Check $f.owner.failed
$f=New-SettlementFake;$f.record.state='EXITED';$f.metrics.kernel=$true
Check (Settle-ResourceRecord $f.owner $f.record);Check ($f.metrics.kills -eq 0)
$f=New-SettlementFake;$f.metrics.kernel=$true;$f.metrics.disposeThrows=$true
Reject {Settle-ResourceRecord $f.owner $f.record};Check (!(Settle-ResourceRecord $f.owner $f.record))
Check ($f.metrics.streamDisposes -eq 1 -and $f.metrics.processDisposes -eq 0)
# Effective SAME-pwsh barrier paths: initial query error, sink error, repeated
# passive interruption and current task completion; root/files cannot exit early.
function Start-Sleep {param($Milliseconds) & $script:passiveHook}
foreach($mode in @('kill','wait','sink','task','interrupt')){
 $f=New-SettlementFake;$rootDouble=New-StageLifetimeRoot $f.owner;$script:barrierSteps=0;$script:files=0;$script:sinks=0
 if($mode -ceq 'kill'){$f.metrics.killThrows=$true}
 if($mode -ceq 'wait'){$f.metrics.waitThrows=$true}
 $tcs=[Threading.Tasks.TaskCompletionSource[int]]::new();$f.record.writeTask=$tcs.Task
 $script:passiveHook={
  Check $rootDouble.active;Check ($script:files -eq 0 -and $f.metrics.streamDisposes -eq 0)
  $script:barrierSteps++
  if($mode -ceq 'interrupt' -and $script:barrierSteps -eq 1){throw 'passive interruption'}
  $f.metrics.kernel=$true;if(!$tcs.Task.IsCompleted){$tcs.SetResult(0)}
 }
 try{
  $result=Complete-ResourceSettlement $f.owner $rootDouble {Check $rootDouble.active;Check ($f.metrics.processDisposes -eq 1);$script:files++} {$script:sinks++;Check $rootDouble.active;if($mode -ceq 'sink'){throw 'failure sink interrupted'}}
  Check (!$result -and $f.owner.failed -and !$rootDouble.active -and $script:files -eq 1)
  Check ($f.metrics.kills -le 1 -and $f.metrics.streamDisposes -eq 3)
 }finally{if($rootDouble.active){$rootDouble.handle.Free();$rootDouble.active=$false}}
}
Check ($new.Extent.Text -notmatch 'Stop-ExactOwnedProcess \$record.process')
Check ($new.Extent.Text.Contains('-or !$settlementSuccess'))
# Effective normal WaitRole and EXACT capture-success close call, then actual
# barrier. A test-only outer labeled break bounds the indefinitely retained fake;
# it is NOT a product exit/forced-termination containment claim.
$captureCall='if(!(Close-ResourceRecord $capture)){throw ''UNAVAILABLE: capture close outcome uncertain''}'
Check ($new.Extent.Text.Contains($captureCall))
foreach($route in @('normal','capture')){
 foreach($failure in @('StandardInput','StandardOutput','Process','NONE')){
  $f=New-SettlementFake;$m=$f.metrics;$m.kernel=$true;$m.closeCounts=@{StandardInput=0;StandardOutput=0;StandardError=0;Process=0};$m.failName=$failure;$m.failedOnce=$false
  $f.record.role=if($route -ceq 'normal'){'imports'}else{'capture'};$f.record.state='EXITED'
  $f.record.out.Closed=$true;$f.record.err.Closed=$true;$f.record.out.Text=[Text.StringBuilder]::new();$f.record.err.Text=[Text.StringBuilder]::new()
  $f.record.process|Add-Member NoteProperty ExitCode 0
  foreach($name in @('StandardInput','StandardOutput','StandardError')){
   $stream=$f.record.process.$name;$stream|Add-Member NoteProperty name $name
   $stream|Add-Member ScriptMethod Dispose { $this.m.closeCounts[$this.name]++;if($this.m.failName -ceq $this.name -and !$this.m.failedOnce){$this.m.failedOnce=$true;throw 'normal stream close uncertain'} } -Force
  }
  $f.record.process|Add-Member ScriptMethod Dispose {$this.m.closeCounts.Process++;if($this.m.failName -ceq 'Process' -and !$this.m.failedOnce){$this.m.failedOnce=$true;throw 'normal Process close uncertain'}} -Force
  $caught=$false
  try{if($route -ceq 'normal'){Wait-ResourceRole $f.record 90000}else{$capture=$f.record;& ([scriptblock]::Create($captureCall))}}catch{$caught=$true;$f.owner.failed=$true}
  Check ($caught -eq ($failure -cne 'NONE'))
  $rootDouble=New-StageLifetimeRoot $f.owner;$script:files=0;$script:passes=0
  $script:passiveHook={
   $script:passes++;Check $rootDouble.active;Check ($script:files -eq 0)
   Check ($f.record.disposeState[$failure] -ceq 'FAILED' -and $m.closeCounts[$failure] -eq 1)
   if($script:passes -eq 3){break settlementWitness}
  }
  try{
   :settlementWitness while($true){
    $answer=Complete-ResourceSettlement $f.owner $rootDouble {$script:files++} {}
    Check ($failure -ceq 'NONE' -and $answer -and !$rootDouble.active -and $script:files -eq 1)
    break
   }
   if($failure -cne 'NONE'){
    Check ($script:passes -eq 3 -and $rootDouble.active -and $script:files -eq 0 -and $f.owner.failed)
    Check ($m.closeCounts[$failure] -eq 1)
   }
   foreach($name in @('StandardInput','StandardOutput','StandardError','Process')){Check ($m.closeCounts[$name] -le 1)}
   if($failure -ceq 'NONE'){Check (Settle-ResourceRecord $f.owner $f.record);Check ($m.closeCounts.Process -eq 1)}
  }finally{if($rootDouble.active){$rootDouble.handle.Free();$rootDouble.active=$false}}
 }
}
Write-Host "FONT_RESOURCE_METADATA_PASS checks=$script:checks native_processes_started=0 real_tool_reads=0 actual_application_lookup=NOT_EXECUTED code_rows=SYNTHETIC_AND_SAVED_CI_TEXT"

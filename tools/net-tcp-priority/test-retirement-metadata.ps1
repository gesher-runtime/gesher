# Synthetic controller metadata only; no process, native fixture or C# compilation.
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'run-component.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Consumer parse failed'}
$fn=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'RequireDiagnosticRetirement'},$true))
if($fn.Count -ne 1){throw 'Missing exact consumer retirement predicate'}
. ([scriptblock]::Create($fn[0].Extent.Text))
$positive=[pscustomobject]@{Success=$true;Reason='exit';ExitCode=0;RootSignaled=$true;JobEmpty=$true;OutputRead=$true;Errors=@();OwnedHandles=4;ClosedHandles=4;ActiveProcessLimit=1;TotalProcesses=1;PreResumeActive=1;Processes=@([pscustomobject]@{Role='root'});WritersRetired=$true;ExclusiveSnapshot=$true;RootHandleClosed=$true;ThreadHandleClosed=$true}
function Clone($Value){$Value|ConvertTo-Json -Depth 10|ConvertFrom-Json}
function Reject($Value,[int]$Exit,[string]$Name){$rejected=$false;try{RequireDiagnosticRetirement $Value $Exit}catch{$rejected=$true};if(-not $rejected){throw "Synthetic negative accepted: $Name"}}
RequireDiagnosticRetirement $positive
$negative=Clone $positive;$negative.Success=$false;$negative.ExitCode=1
RequireDiagnosticRetirement $negative 1
Reject $negative 0 'negative-exit-not-normal-success'
Reject $positive 1 'normal-exit-not-negative-success'
Reject $negative 2 'unsupported-expected-exit'
foreach($field in @('RootSignaled','JobEmpty','OutputRead','WritersRetired','ExclusiveSnapshot','RootHandleClosed','ThreadHandleClosed')){
 $bad=Clone $negative;$bad.$field=$false;Reject $bad 1 $field
}
foreach($pair in @(@{N='Reason';V='timeout'},@{N='ExitCode';V=2},@{N='OwnedHandles';V=5},@{N='ActiveProcessLimit';V=2},@{N='TotalProcesses';V=2},@{N='PreResumeActive';V=2},@{N='Success';V=$true},@{N='Errors';V=@('error')},@{N='Processes';V=@()})){
 $bad=Clone $negative;$bad.($pair.N)=$pair.V;Reject $bad 1 $pair.N
}
$bad=Clone $negative;$bad.Processes[0].Role='descendant';Reject $bad 1 'root-identity-role'
Write-Host 'TCP_RETIREMENT_METADATA_PASS synthetic=1 normal_exit0=UNCHANGED expected_negative_exit1=STRICT mutations=20 native_processes_started=0'

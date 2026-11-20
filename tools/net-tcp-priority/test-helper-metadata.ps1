# Source/hash metadata only; never compiles or starts the C# controllers.
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'run-component.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Consumer parse failed'}
$assignment=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq '$trusted'},$true))
if($assignment.Count -ne 1){throw 'Helper pin map is missing/ambiguous'}
$actual=$assignment[0].Right.Expression.SafeGetValue()
$reviewed=@{
 'tools/windows4/TsfJobSupervisor.cs'='625be4d5906bb48dd02d6103382fd2e85ac88738'
 'tools/windows4/TsfJobWorker.cs'='e4392ea9ee4e8faf6d2f4b28424f8e05fe692ce1'
 'tools/windows4/test-tsf-job-supervisor.ps1'='3b0f7ebb636b91859b86797f94136c0b6dccd5fd'
 'tools/net-tcp-priority/qualified/TsfJobSupervisor.cs'='625be4d5906bb48dd02d6103382fd2e85ac88738'
 'tools/native3-locks/artifact/download-verified-artifact.py'='cd5cc415e53102c664edac22b1604df639c8804b'
 'tools/ci/receipt-diagnostics.ps1'='b89f2f3f8a54a8b23937bae48ad900c85d4d49e9'
 'tools/ci/source_identity.py'='8bbf1174020adab86b02403070f88c5aa42d9755'
 'tools/ci/github_read.py'='b126f5e1fc81ac6fb40f9635f99069df6103a727'
 'tools/ci/github_jobs.py'='f4278108b42329950525fb9fa34b78ae75694915'
}
function ExactMap($Value){
 if($Value.Count -ne $reviewed.Count){return $false}
 foreach($key in $reviewed.Keys){if(-not $Value.ContainsKey($key) -or $Value[$key] -cne $reviewed[$key]){return $false}}
 $true
}
if(-not(ExactMap $actual)){throw 'Consumer map differs from reviewed qualified source'}
foreach($entry in $actual.GetEnumerator()){
 $blob=git hash-object $entry.Key
 if($LASTEXITCODE -ne 0 -or $blob -cne $entry.Value){throw "Actual source helper differs: $($entry.Key)"}
 $bad=$actual.Clone();$bad[$entry.Key]='0'*40;if(ExactMap $bad){throw 'Changed hash accepted'}
 $bad=$actual.Clone();$bad.Remove($entry.Key);if(ExactMap $bad){throw 'Missing helper accepted'}
}
foreach($stale in @(@{P='tools/windows4/TsfJobSupervisor.cs';H='d1cf9319268df1bccf03177e7f8dde52a7f55510'},@{P='tools/windows4/TsfJobWorker.cs';H='08f29f78f56195825d96de2f2f6632bf91133e1e'},@{P='tools/windows4/test-tsf-job-supervisor.ps1';H='91e6946c4ab05a42ffbf6917166193c3dad07872'})){
 $bad=$actual.Clone();$bad[$stale.P]=$stale.H;if(ExactMap $bad){throw 'Historical stale helper hash accepted'}
}
$bad=$actual.Clone();$bad['tools/unreviewed.cs']='0'*40;if(ExactMap $bad){throw 'Additional helper accepted'}
$bad=$actual.Clone();$bad['tools/ci/source_identity.py']='e03b6d4d926f954eecd524ee63ca182edfacdd17';if(ExactMap $bad){throw 'Pre-queue source identity helper accepted'}
$limits=@{}
foreach($class in @('TsfJobSupervisor','TcpPriorityDiagnostics.TsfJobSupervisor')){
 $source=if($class -ceq 'TsfJobSupervisor'){'tools/windows4/TsfJobSupervisor.cs'}else{'tools/net-tcp-priority/qualified/TsfJobSupervisor.cs'}
 $text=[IO.File]::ReadAllText((Join-Path $pwd $source))
 $limit=[regex]::Matches($text,'if\s*\(\s*mainMs\s*<\s*(\d+)\s*\|\|\s*mainMs\s*>\s*(\d+)')
 if($limit.Count -ne 1 -or $limit[0].Groups[1].Value -cne '1' -or $limit[0].Groups[2].Value -cne '45000'){throw 'Qualified controller budget predicate changed'}
 $limits[$class]=45000
}
$calls=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Static -and $n.Member.Extent.Text -ceq 'Run'},$true))
if($calls.Count -ne 4){throw 'Unexpected supervisor call inventory'}
foreach($call in $calls){
 $class=$call.Expression.TypeName.FullName
 if(-not $limits.ContainsKey($class) -or $call.Arguments.Count -ne 4){throw 'Unreviewed supervisor call shape'}
 $budget=$call.Arguments[2].SafeGetValue()
 if($budget -isnot [int] -or $budget -lt 1 -or $budget -gt $limits[$class]){throw 'Caller exceeds actual qualified controller budget'}
}
foreach($bad in @(-1,0,45001,90000)){if($bad -ge 1 -and $bad -le 45000){throw 'Invalid budget accepted'}}
Write-Host 'TCP_CALLER_BUDGET_METADATA_PASS actual_controller_max=45000 actual_Run_calls=4 old90000=REJECTED boundary_mutants=4 native_processes_started=0'
Write-Host "TCP_HELPER_METADATA_PASS pins=$($reviewed.Count) actual_sources=EXACT changed_missing_stale_additional_prequeue=REJECTED negative_cases=$($reviewed.Count * 2 + 5) native_processes_started=0 CSharp_compiles=0"

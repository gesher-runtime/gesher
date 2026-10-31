# Pure source/JSON/PowerShell metadata. No network, downloads or native execution.
param([string] $JobsSnapshot='')
$ErrorActionPreference='Stop'
$root=Resolve-Path (Join-Path $PSScriptRoot '../..')
$source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'run-component.ps1'))
$baseline=git -C $root show f6b15746e1dfac3711c2687f728eafbd48094eba:tools/net-tcp-priority/run-component.ps1
if($LASTEXITCODE -ne 0){throw 'Cannot read immutable original predicates'}
function Parse($Text){
 $tokens=$null;$errors=$null
 $value=[Management.Automation.Language.Parser]::ParseInput(($Text -join "`n"),[ref]$tokens,[ref]$errors)
 if($errors.Count){throw 'Source parse failed'}
 $value
}
$ast=Parse $source
$old=Parse $baseline
function Condition($Tree,$Message){
 $matches=@($Tree.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.Contains("throw '$Message'")},$true))
 if($matches.Count -ne 1){throw "Missing/ambiguous original predicate $Message"}
 $matches[0].Clauses[0].Item1.Extent.Text
}
$producer=Condition $ast 'Producer mismatch'
foreach($message in @('Producer mismatch','Artifact not unique','Artifact source/run/attempt provenance mismatch')){
 if((Condition $ast $message) -cne (Condition $old $message)){throw "Acceptance predicate changed: $message"}
}
. (Join-Path $root 'tools/ci/receipt-diagnostics.ps1')
$env:GITHUB_REPOSITORY='gesher-runtime/gesher';$env:GITHUB_REPOSITORY_ID='1'
$env:GITHUB_RUN_ID='41101529359';$env:GITHUB_RUN_ATTEMPT='1'
$env:GITHUB_SHA='f6b15746e1dfac3711c2687f728eafbd48094eba'
$env:GITHUB_WORKFLOW='net-tcp-coherent-component'
$valid=[pscustomobject]@{id=124219437252;name='compile-canonical';workflow_name=$env:GITHUB_WORKFLOW;run_id=41101529359;run_attempt=1;head_sha=$env:GITHUB_SHA;status='completed';conclusion='success';started_at='2026-10-30T17:47:28Z';completed_at='2026-10-30T17:54:42Z'}
$expectedProducer='compile-canonical'
function Rejects($Value){$jobs=@($Value);[bool](& ([scriptblock]::Create($producer)))}
if(Rejects @($valid)){throw 'Valid synthetic identity rejected'}
$negative=0
foreach($field in @('run_id','run_attempt','head_sha','conclusion')){
 foreach($missing in @($false,$true)){
  $mutant=$valid | ConvertTo-Json | ConvertFrom-Json
  if($missing){$mutant.PSObject.Properties.Remove($field)}else{$mutant.$field='WRONG'}
  if(-not(Rejects @($mutant))){throw "Wrong/missing identity accepted: $field"};$negative++
 }
}
foreach($rows in @(@(),@($valid,$valid))){if(-not(Rejects $rows)){throw 'Wrong producer count accepted'};$negative++}
foreach($count in 0,1,2){
 $jobs=@(if($count -eq 1){$valid}elseif($count -eq 2){$valid;$valid})
 $observations=@(Write-CiProducerIdentity -Producer $expectedProducer -Rows $jobs 6>&1)
 if($observations.Count -ne 1){throw 'Producer observations altered output shape'}
 $record=($observations[0].ToString() -replace '^CI_RECEIPT_IDENTITY ','') | ConvertFrom-Json
 if($record.stage -cne 'producer' -or @($record.received).Count -ne $count -or $record.expected.producer -cne $expectedProducer -or "$($record.expected.run_id)" -cne $env:GITHUB_RUN_ID){throw 'Producer expected/received diagnostics incomplete'}
 $artifact=[pscustomobject]@{id=1;name='synthetic-only';size_in_bytes=1;digest=('sha256:'+('1'*64));expired=$false;created_at=$valid.started_at;workflow_run=[pscustomobject]@{id=$valid.run_id;head_sha=$valid.head_sha;repository_id=1;head_repository_id=1}}
 $selected=@(if($count -eq 1){$artifact}elseif($count -eq 2){$artifact;$artifact})
 $inventory=[pscustomobject]@{total_count=$count;artifacts=$selected}
 $observations=@(Write-CiArtifactInventory -Name 'synthetic-only' -Inventory $inventory -Rows $selected -Producer $valid 6>&1)
 if($observations.Count -ne 1+$count){throw 'Artifact inventory/candidate diagnostics incomplete'}
 $record=($observations[0].ToString() -replace '^CI_RECEIPT_IDENTITY ','') | ConvertFrom-Json
 if($record.stage -cne 'artifact_inventory' -or $record.received.selected_count -ne $count){throw 'Artifact count observation missing'}
}
if($JobsSnapshot){
 $saved=Get-Content -LiteralPath $JobsSnapshot -Raw | ConvertFrom-Json
 foreach($role in @('canonical','retirement')){
  $jobs=@($saved.jobs | Where-Object name -CEQ "compile-$role")
  if(Rejects $jobs){throw 'Actual later snapshot no longer matches original predicate'}
  Write-CiProducerIdentity -Producer "compile-$role" -Rows $jobs
 }
 Write-Host 'TCP_RECEIPT_SNAPSHOT actual_later_metadata=PASS historical_live_failure_cause=UNKNOWN replay_is_not_original_live_observation=1'
}
Write-Host "TCP_RECEIPT_METADATA_PASS original3predicates=EXACT synthetic_wrong_missing_count_negatives=$negative producer_artifact_counts=0,1,2 retry_logic=NONE native_processes_started=0"

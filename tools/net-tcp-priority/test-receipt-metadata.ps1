# Pure source/JSON/PowerShell metadata. No network, downloads or native execution.
param([string] $JobsSnapshot='')
$ErrorActionPreference='Stop'
$root=Resolve-Path (Join-Path $PSScriptRoot '../..')
$source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'run-component.ps1'))
$baseline=git -C $root show c0d497aac1c7cbb80686397dd5b5336b418087cf:tools/net-tcp-priority/run-component.ps1
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
 # A containing workflow branch also includes every nested throw in its extent.
 # Only the clause that directly owns the sole throw supplies its predicate.
 $conditions=@(foreach($statement in $Tree.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
  foreach($clause in $statement.Clauses){
   $body=@($clause.Item2.Statements)
   if($body.Count -eq 1 -and $body[0] -is [Management.Automation.Language.ThrowStatementAst] -and
      $body[0].Extent.Text -ceq "throw '$Message'"){$clause.Item1}
  }
 })
 if($conditions.Count -ne 1){throw "Missing/ambiguous original predicate $Message"}
 $conditions[0].Extent.Text
}
function AssertCondition($Tree,$Expected,$Message){
 if((Condition $Tree $Message) -cne $Expected){throw "Acceptance predicate changed: $Message"}
}
# Exercise the real selector/comparison before extracting any consumer gates.
# These script fragments contain metadata only and are never executed.
$selectorMessage='Predicate selector control'
foreach($sample in @(
 @{Source='if ($direct) { throw ''Predicate selector control'' }';Expected='$direct'},
 @{Source='if ($outer) { if ($inner) { throw ''Predicate selector control'' } }';Expected='$inner'},
 @{Source='if ($first) { throw ''other'' } elseif ($second) { throw ''Predicate selector control'' }';Expected='$second'}
)){
 AssertCondition (Parse $sample.Source) $sample.Expected $selectorMessage
}
$selectorNegatives=0
foreach($sample in @(
 'if ($missing) { throw ''other'' }',
 'if ($changed) { throw ''Changed predicate selector control'' }',
 'if ($text) { "throw ''Predicate selector control''" }',
 'if ($delegated) { & { throw ''Predicate selector control'' } }',
 'if ($first) { ''other'' } else { throw ''Predicate selector control'' }',
 'if ($extra) { ''additional statement''; throw ''Predicate selector control'' }',
 'if ($one) { throw ''Predicate selector control'' }; if ($two) { throw ''Predicate selector control'' }'
)){
 $rejected=$false
 try{$null=Condition (Parse $sample) $selectorMessage}catch{
  if($_.Exception.Message -cne "Missing/ambiguous original predicate $selectorMessage"){throw}
  $rejected=$true
 }
 if(-not $rejected){throw 'Missing/ambiguous/nonthrow selector owner accepted'}
 $selectorNegatives++
}
$rejected=$false
try{AssertCondition (Parse 'if ($outer) { if ($mutated) { throw ''Predicate selector control'' } }') '$inner' $selectorMessage}catch{
 if($_.Exception.Message -cne "Acceptance predicate changed: $selectorMessage"){throw}
 $rejected=$true
}
if(-not $rejected){throw 'Mutated predicate accepted by immutable comparison'}
Write-Host "TCP_RECEIPT_SELECTOR_METADATA_PASS direct_nested_elseif=PASS ownership_negatives=$selectorNegatives changed_predicate=rejected native_processes_started=0"
$producer=Condition $ast 'Producer mismatch'
foreach($message in @('Producer mismatch','Artifact not unique','Artifact source/run/attempt provenance mismatch')){
 # API head/source repository differ from the target's PR merge checkout.
 # Every other identity/conclusion/digest/count/time constraint is unchanged.
 $expected=(Condition $old $message).Replace('$env:GITHUB_SHA','$ciSource.api_head_sha')
 $expected=$expected.Replace('"$($a.workflow_run.head_repository_id)" -cne $env:GITHUB_REPOSITORY_ID',
                             '"$($a.workflow_run.head_repository_id)" -cne $ciSource.head_repository_id')
 AssertCondition $ast $expected $message
}
. (Join-Path $root 'tools/ci/receipt-diagnostics.ps1')
$env:GITHUB_REPOSITORY='gesher-runtime/gesher';$env:GITHUB_REPOSITORY_ID='1'
$env:GITHUB_RUN_ID='41101529359';$env:GITHUB_RUN_ATTEMPT='1'
$env:GITHUB_SHA='f6b15746e1dfac3711c2687f728eafbd48094eba'
$ciSource=[pscustomobject]@{api_head_sha=$env:GITHUB_SHA;checkout_sha=('a'*40);head_repository_id='2'}
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
 $observations=@(Write-CiProducerIdentity -Producer $expectedProducer -Rows $jobs -SourceIdentity $ciSource 6>&1)
 if($observations.Count -ne 1){throw 'Producer observations altered output shape'}
 $record=($observations[0].ToString() -replace '^CI_RECEIPT_IDENTITY ','') | ConvertFrom-Json
 if($record.stage -cne 'producer' -or @($record.received).Count -ne $count -or $record.expected.producer -cne $expectedProducer -or "$($record.expected.run_id)" -cne $env:GITHUB_RUN_ID -or $record.expected.head_repository_id -cne '2' -or $record.expected.checkout -cne $ciSource.checkout_sha){throw 'Producer expected/received diagnostics incomplete'}
 $artifact=[pscustomobject]@{id=1;name='synthetic-only';size_in_bytes=1;digest=('sha256:'+('1'*64));expired=$false;created_at=$valid.started_at;workflow_run=[pscustomobject]@{id=$valid.run_id;head_sha=$valid.head_sha;repository_id=1;head_repository_id=2}}
 $selected=@(if($count -eq 1){$artifact}elseif($count -eq 2){$artifact;$artifact})
 $inventory=[pscustomobject]@{total_count=$count;artifacts=$selected}
 $observations=@(Write-CiArtifactInventory -Name 'synthetic-only' -Inventory $inventory -Rows $selected -Producer $valid -SourceIdentity $ciSource 6>&1)
 if($observations.Count -ne 1+$count){throw 'Artifact inventory/candidate diagnostics incomplete'}
 $record=($observations[0].ToString() -replace '^CI_RECEIPT_IDENTITY ','') | ConvertFrom-Json
 if($record.stage -cne 'artifact_inventory' -or $record.received.selected_count -ne $count -or $record.expected.head_repository_id -cne '2' -or $record.expected.source -cne $ciSource.api_head_sha){throw 'Artifact count/source observation missing'}
}
# Execute the actual acceptance expressions from all three current-PR consumers.
# The fork's API head and repository must match; the merge checkout remains the
# source of the built artifact and must never substitute for its API metadata.
$predicates=@(Condition $ast 'Artifact source/run/attempt provenance mismatch')
$tsf=[IO.File]::ReadAllText((Join-Path $root 'tools/windows4/run-tsf-union.ps1'))
$predicates+=Condition (Parse $tsf) 'Artifact provenance mismatch'
$native=[IO.File]::ReadAllText((Join-Path $root '.github/workflows/native3-locks-blocking.yml'))
$nativeIf=[regex]::Match($native,"(?s)if \(\`$artifact\.id.*?\{ throw 'Artifact source/repository mismatch' \}")
if(-not $nativeIf.Success){throw 'Missing native artifact predicate'}
$predicates+=Condition (Parse $nativeIf.Value) 'Artifact source/repository mismatch'
$jobs=@($valid)
$forkNegatives=0
foreach($predicate in $predicates){
 foreach($headRepo in '1','2'){
  $ciSource.head_repository_id=$headRepo
  $good=[pscustomobject]@{id=1;size_in_bytes=1;digest=('sha256:'+('1'*64));expired=$false;created_at=$valid.started_at;workflow_run=[pscustomobject]@{id=$valid.run_id;head_sha=$valid.head_sha;repository_id=1;head_repository_id=$headRepo}}
  $a=$good;$artifact=$good
  if(& ([scriptblock]::Create($predicate))){throw 'Valid fork/same-repository artifact rejected'}
  foreach($field in 'id','head_sha','repository_id','head_repository_id'){
   foreach($missing in @($false,$true)){
    $bad=$good | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    if($missing){$bad.workflow_run.PSObject.Properties.Remove($field)}
    else{$bad.workflow_run.$field=if($field -eq 'head_sha'){$ciSource.checkout_sha}else{'999'}}
    $a=$bad;$artifact=$bad
    if(-not(& ([scriptblock]::Create($predicate)))){throw "Wrong/missing artifact identity accepted: $field"}
    $forkNegatives++
   }
  }
  foreach($field in 'id','size_in_bytes','digest','expired'){
   $bad=$good | ConvertTo-Json -Depth 8 | ConvertFrom-Json
   $bad.$field=switch($field){'id'{0};'size_in_bytes'{0};'digest'{'WRONG'};'expired'{$true}}
   $a=$bad;$artifact=$bad
   if(-not(& ([scriptblock]::Create($predicate)))){throw "Invalid artifact accepted: $field"}
   $forkNegatives++
  }
 }
}
$ciSource.head_repository_id='2'
Write-Host "TCP_FORK_ARTIFACT_METADATA_PASS consumers=3 same_repository_and_fork=PASS identity_digest_size_expiry_negatives=$forkNegatives native_processes_started=0"
if($JobsSnapshot){
 $saved=Get-Content -LiteralPath $JobsSnapshot -Raw | ConvertFrom-Json
 foreach($role in @('canonical','retirement')){
  $jobs=@($saved.jobs | Where-Object name -CEQ "compile-$role")
  if(Rejects $jobs){throw 'Actual later snapshot no longer matches original predicate'}
  Write-CiProducerIdentity -Producer "compile-$role" -Rows $jobs
 }
 Write-Host 'TCP_RECEIPT_SNAPSHOT actual_later_metadata=PASS historical_live_failure_cause=UNKNOWN replay_is_not_original_live_observation=1'
}
Write-Host "TCP_RECEIPT_METADATA_PASS original3predicates=EXACT_except_API_head_and_source_repository synthetic_wrong_missing_count_negatives=$negative producer_artifact_counts=0,1,2 retry_logic=NONE native_processes_started=0"

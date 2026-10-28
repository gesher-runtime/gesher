# CI-only current-attempt supplier identity gate for boot and runtime consumers.
param([ValidateSet('Boot','Runtime')][string]$Stage = 'Runtime')
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
function Exact-Run($r) {
    return $r.name -ceq 'binder2-stable-readonly-component' -and "$($r.id)" -ceq $env:GITHUB_RUN_ID -and $r.head_sha -ceq $env:GITHUB_SHA -and
        "$($r.run_attempt)" -ceq $env:GITHUB_RUN_ATTEMPT -and $r.repository.full_name -ceq $env:GITHUB_REPOSITORY -and
        "$($r.repository.id)" -ceq $env:GITHUB_REPOSITORY_ID -and "$($r.head_repository.id)" -ceq $env:GITHUB_REPOSITORY_ID
}
$run = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not (Exact-Run $run)) { throw 'Current run/source/attempt/repository mismatch' }
foreach ($field in 'head_sha','id','run_attempt','repository') {
    $negative = $run | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    switch ($field) {
        head_sha { $negative.head_sha = '0' * 40 }
        id { $negative.id = 0 }
        run_attempt { $negative.run_attempt = 0 }
        repository { $negative.repository.id = 0 }
    }
    if (Exact-Run $negative) { throw "Identity negative accepted $field" }
    Write-Host "ACTUAL_BINDER_SUPPLIER_NEGATIVE field=$field rejected=True"
}
$names = @('prerequisite','host','framework / host-tools','framework / framework')
$packages = @{'windows-x64'='host';framework='framework / framework'}
if ($Stage -eq 'Runtime') {
    $names += @('translator / fpsr-reference / reference','translator / build','fixtures / host-compile','fixtures / modes-build','boot-image / compile')
    $packages['arm64']='translator / build'
    $packages['boot-image']='boot-image / compile'
    $packages['binder2-sg-modes']='fixtures / modes-build'
}
$jobs = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $jobs.total_count -gt 100) { throw 'Cannot inspect all current job attempts' }
$selected=@{}
foreach ($name in $names) {
    $rows=@($jobs.jobs | Where-Object name -CEQ $name)
    if ($rows.Count -ne 1 -or $rows[0].conclusion -cne 'success' -or $rows[0].head_sha -cne $run.head_sha -or
        "$($rows[0].run_id)" -cne $env:GITHUB_RUN_ID -or "$($rows[0].run_attempt)" -cne $env:GITHUB_RUN_ATTEMPT) { throw "Unproved current producer $name" }
    $selected[$name]=$rows[0]
    Write-Host "ACTUAL_SG_SUPPLIER job=$name id=$($rows[0].id) source=$($rows[0].head_sha) run=$($rows[0].run_id) attempt=$($rows[0].run_attempt) conclusion=$($rows[0].conclusion)"
}
$inventory=gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $inventory.total_count -gt 100) { throw 'Cannot inspect complete current artifact inventory' }
foreach($name in $packages.Keys) {
    $rows=@($inventory.artifacts | Where-Object name -CEQ $name)
    if ($rows.Count -ne 1) { throw "Artifact ambiguity $name" }
    $a=$rows[0]; $job=$selected[$packages[$name]]
    if ($a.expired -or $a.id -le 0 -or $a.size_in_bytes -le 0 -or $a.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or
        "$($a.workflow_run.id)" -cne $env:GITHUB_RUN_ID -or $a.workflow_run.head_sha -cne $env:GITHUB_SHA -or
        "$($a.workflow_run.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or "$($a.workflow_run.head_repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or
        [datetime]$a.created_at -lt [datetime]$job.started_at -or [datetime]$a.created_at -gt [datetime]$job.completed_at) { throw "Wrong current producer artifact $name" }
    $key=$name.Replace('-','_')
    "$key=$($a.id)" >> $env:GITHUB_OUTPUT
    "${key}_digest=$($a.digest)" >> $env:GITHUB_OUTPUT
    Write-Host "ACTUAL_SG_ARTIFACT name=$name id=$($a.id) digest=$($a.digest) source=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT repository=$env:GITHUB_REPOSITORY_ID"
}

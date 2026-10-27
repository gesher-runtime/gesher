# Same-run canonical full inputs; only CI downloads artifacts, never the local agent.
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Full supplier proof executes only in CI' }
$expectedHead = $env:GITHUB_SHA
$expectedHeadRepository = $env:GITHUB_REPOSITORY_ID
if ($env:GITHUB_EVENT_NAME -ceq 'pull_request') {
  $event = Get-Content -LiteralPath $env:GITHUB_EVENT_PATH -Raw | ConvertFrom-Json
  $expectedHead = $event.pull_request.head.sha
  $expectedHeadRepository = [string]$event.pull_request.head.repo.id
}
$actual = git rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -cne $env:GITHUB_SHA) { throw 'Full checkout SHA mismatch' }
$run = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $run.id -ne $env:GITHUB_RUN_ID -or $run.run_attempt -ne $env:GITHUB_RUN_ATTEMPT -or
    $run.name -cne 'ci' -or $run.head_sha -cne $expectedHead -or $run.repository.id -ne $env:GITHUB_REPOSITORY_ID -or
    $run.head_repository.id -ne $expectedHeadRepository -or $run.repository.full_name -cne $env:GITHUB_REPOSITORY) {
  throw 'Canonical full event/head/checkout/run/attempt/repository mismatch'
}
$jobs = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $jobs.total_count -gt 100) { throw 'Cannot inspect exact full producer jobs' }
foreach ($name in 'build','framework / host-tools','framework / framework','native-libs / native-libs',
                   'arm64 / build','graphics / build','angle / build','boot-image / compile',
                   'webview / build','exec / exec','media / media') {
  $rows = @($jobs.jobs | Where-Object name -CEQ $name)
  if ($rows.Count -ne 1 -or $rows[0].conclusion -cne 'success' -or $rows[0].head_sha -cne $expectedHead -or
      $rows[0].run_id -ne $run.id -or $rows[0].run_attempt -ne $run.run_attempt) {
    throw "Full supplier job identity/success mismatch: $name"
  }
}
$inventory = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $inventory.total_count -gt 100) { throw 'Cannot inspect full artifact packages' }
foreach ($name in 'windows-x64','framework','native-libs','arm64','graphics','angle','boot-image',
                   'webview','exec','media','web3-query-hook') {
  $rows = @($inventory.artifacts | Where-Object name -CEQ $name)
  if ($rows.Count -ne 1 -or $rows[0].id -le 0 -or $rows[0].expired -or $rows[0].size_in_bytes -le 0 -or
      $rows[0].digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or $rows[0].workflow_run.id -ne $run.id -or
      $rows[0].workflow_run.head_sha -cne $expectedHead -or
      $rows[0].workflow_run.repository_id -ne $env:GITHUB_REPOSITORY_ID -or
      $rows[0].workflow_run.head_repository_id -ne $expectedHeadRepository) {
    throw "Full artifact source/repository/run/digest mismatch: $name"
  }
  $key = $name.Replace('-','_')
  "$key=$($rows[0].id)" >> $env:GITHUB_OUTPUT
  Write-Host "VERIFIED_WEB3_FULL_SUPPLIER name=$name artifact=$($rows[0].id) digest=$($rows[0].digest) event-head=$expectedHead checkout=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT"
}

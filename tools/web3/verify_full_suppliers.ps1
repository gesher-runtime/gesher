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
$runJson = python tools/ci/github_read.py "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect exact full workflow run' }
$run = $runJson | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $run.id -ne $env:GITHUB_RUN_ID -or $run.run_attempt -ne $env:GITHUB_RUN_ATTEMPT -or
    $run.name -cne 'ci' -or $run.head_sha -cne $expectedHead -or $run.repository.id -ne $env:GITHUB_REPOSITORY_ID -or
    $run.head_repository.id -ne $expectedHeadRepository -or $run.repository.full_name -cne $env:GITHUB_REPOSITORY) {
  throw 'Canonical full event/head/checkout/run/attempt/repository mismatch'
}
$packages = [ordered]@{
  'windows-x64' = 'build'; 'framework' = 'framework / framework'
  'native-libs' = 'native-libs / native-libs'; 'arm64' = 'arm64 / build'
  'graphics' = 'graphics / build'; 'angle' = 'angle / probe-build'
  'boot-image' = 'boot-image / compile'; 'webview' = 'webview / build'
  'exec' = 'exec / exec'; 'media' = 'media / media'; 'web3-query-hook' = 'build'
}
$request = @{
  head=$expectedHead; head_repository_id=[string]$expectedHeadRepository
  roles=$packages; additional_jobs=@('framework / host-tools','angle / build')
}
$artifactsJson = $request | ConvertTo-Json -Depth 12 -Compress | python tools/ci/artifact_candidates.py
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify exact-attempt full artifact packages' }
$artifacts = $artifactsJson | ConvertFrom-Json
foreach ($name in $packages.Keys) {
  $artifact = $artifacts.PSObject.Properties[$name].Value
  if ($null -eq $artifact) { throw "Full artifact missing from exact selection: $name" }
  $key = $name.Replace('-','_')
  "$key=$($artifact.id)" >> $env:GITHUB_OUTPUT
  Write-Host "VERIFIED_WEB3_FULL_SUPPLIER name=$name artifact=$($artifact.id) digest=$($artifact.digest) event-head=$expectedHead checkout=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT"
}

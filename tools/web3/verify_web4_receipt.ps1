# Supplemental receipt; verify_full_suppliers.ps1 already checked full event/head and producers.
$ErrorActionPreference = 'Stop'
$run = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $run.head_sha -cne $env:GITHUB_SHA -or $run.run_attempt -ne $env:GITHUB_RUN_ATTEMPT -or $run.repository.id -ne $env:GITHUB_REPOSITORY_ID -or $run.head_repository.id -ne $env:GITHUB_REPOSITORY_ID) { throw 'Web4 full run identity mismatch' }
$jobs = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $jobs.total_count -gt 100) { throw 'Web4 job inventory unavailable' }
$producer = @($jobs.jobs | Where-Object name -CEQ 'framework / framework')
if ($producer.Count -ne 1 -or $producer[0].conclusion -cne 'success' -or $producer[0].head_sha -cne $env:GITHUB_SHA -or $producer[0].run_attempt -ne $env:GITHUB_RUN_ATTEMPT) { throw 'Web4 producer mismatch' }
$inventory = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $inventory.total_count -gt 100) { throw 'Web4 artifact inventory unavailable' }
$rows = @($inventory.artifacts | Where-Object name -CEQ 'web4-insecure-characterization')
if ($rows.Count -ne 1 -or $rows[0].expired -or $rows[0].size_in_bytes -le 0 -or $rows[0].digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or $rows[0].workflow_run.id -ne $run.id -or $rows[0].workflow_run.head_sha -cne $env:GITHUB_SHA -or $rows[0].workflow_run.repository_id -ne $env:GITHUB_REPOSITORY_ID) { throw 'Web4 artifact receipt mismatch' }
"artifact=$($rows[0].id)" >> $env:GITHUB_OUTPUT
"digest=$($rows[0].digest.Substring(7))" >> $env:GITHUB_OUTPUT
Write-Host "VERIFIED_WEB4_INSECURE_RECEIPT artifact=$($rows[0].id) digest=$($rows[0].digest) producer=$($producer[0].id) source=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT"

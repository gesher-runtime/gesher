$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../ci/receipt-diagnostics.ps1')
$jobsRaw = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect producer' }
$jobs = @(($jobsRaw | ConvertFrom-Json).jobs | Where-Object name -eq 'union-tsf / compile')
Write-CiProducerIdentity -Producer 'union-tsf / compile' -Rows $jobs
if ($jobs.Count -ne 1 -or $jobs[0].conclusion -cne 'success' -or "$($jobs[0].run_id)" -cne $env:GITHUB_RUN_ID -or "$($jobs[0].run_attempt)" -cne $env:GITHUB_RUN_ATTEMPT -or $jobs[0].head_sha -cne $env:GITHUB_SHA) { throw 'Producer identity mismatch' }
$raw = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect artifacts' }
$expected = "tsf-format-$env:GITHUB_SHA-a$env:GITHUB_RUN_ATTEMPT"
$rows = @(($raw | ConvertFrom-Json).artifacts | Where-Object name -ceq $expected)
Write-CiArtifactInventory -Name $expected -Inventory ($raw | ConvertFrom-Json) -Rows $rows -Producer $jobs[0]
if ($rows.Count -ne 1) { throw 'Artifact is not unique' }
$a = $rows[0]
if ($a.id -le 0 -or $a.size_in_bytes -le 0 -or $a.expired -or $a.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or "$($a.workflow_run.id)" -cne $env:GITHUB_RUN_ID -or "$($a.workflow_run.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or "$($a.workflow_run.head_repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or $a.workflow_run.head_sha -cne $env:GITHUB_SHA -or [datetime]$a.created_at -lt [datetime]$jobs[0].started_at -or [datetime]$a.created_at -gt [datetime]$jobs[0].completed_at) { throw 'Artifact provenance mismatch' }
python tools/native3-locks/artifact/download-verified-artifact.py --id "$($a.id)" --digest $a.digest --destination tsf-fixture
if ($LASTEXITCODE -ne 0) { throw 'Raw archive verification failed' }
$m = Get-Content -LiteralPath tsf-fixture/provenance.json -Raw | ConvertFrom-Json
if ($m.repository -cne $env:GITHUB_REPOSITORY -or "$($m.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or "$($m.run)" -cne $env:GITHUB_RUN_ID -or "$($m.attempt)" -cne $env:GITHUB_RUN_ATTEMPT -or $m.source -cne $env:GITHUB_SHA) { throw 'Manifest provenance mismatch' }
$names = @('windows4_text_store_probe.exe','windows4_tsf_service.dll','windows4_text_store_probe.exe.imports.txt','windows4_tsf_service.dll.imports.txt')
if (@(Compare-Object $names @($m.outputs.PSObject.Properties.Name) -CaseSensitive).Count -ne 0 -or @(Compare-Object @($names + 'provenance.json') @(Get-ChildItem tsf-fixture -Force | ForEach-Object { if ($_.PSIsContainer) { throw 'Unexpected directory' }; $_.Name }) -CaseSensitive).Count -ne 0) { throw 'Unexpected artifact members' }
if (@($m.sources.PSObject.Properties).Count -eq 0 -or @($m.compiler.commands.PSObject.Properties).Count -ne 4 -or $m.compiler.sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $m.compiler.version) { throw 'Missing source or compiler proof' }
foreach ($f in $m.sources.PSObject.Properties) { if ((Get-FileHash -LiteralPath $f.Name -Algorithm SHA256).Hash.ToLowerInvariant() -cne $f.Value) { throw "Source mismatch $($f.Name)" } }
foreach ($f in $m.outputs.PSObject.Properties) { if ((Get-FileHash -LiteralPath (Join-Path tsf-fixture $f.Name) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $f.Value) { throw "Output mismatch $($f.Name)" } }
Write-Host "TSF_PRODUCER source=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT job=$($jobs[0].id) artifact=$($a.id) digest=$($a.digest)"
Write-Host ($m.compiler | ConvertTo-Json -Depth 8)
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'TSF execution is CI only' }
Add-Type -Path tools/windows4/TsfJobSupervisor.cs
$result = [TsfJobSupervisor]::Run((Resolve-Path tsf-fixture/windows4_text_store_probe.exe).Path, '', 45000, '')
Write-Host ('TSF_JOB_KERNEL_EVIDENCE ' + ($result | ConvertTo-Json -Depth 6 -Compress))
if (-not $result.Success) { throw "TSF fixture lifetime failed: $($result.Reason), exit $($result.ExitCode), errors $($result.Errors -join '; ')" }
$text = $result.Output + $result.Error
Write-Host $text
if ($result.ExitCode -ne 0 -or $text -match '(?m)^FAIL' -or $text -notmatch '(?m)^0 windows4 TSF store check\(s\) failed\s*$') { throw "TSF fixture failed: exit $($result.ExitCode)" }
foreach ($label in @('embedded NUL range rejected','range excluding NUL accepted','end one selects exactly one unit','empty range accepted','invalid range rejected')) {
  if (-not $text.Contains("PASS  windows4 TSF store: $label")) { throw "Missing assertion: $label" }
}

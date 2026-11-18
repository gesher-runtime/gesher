# The actual evidence writer, isolated from the Windows UI and runtime. Fixture
# bytes are not APKs and never enter the application's execution path.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'evidence.ps1')
$directory = Join-Path ([IO.Path]::GetTempPath()) ('gesher-evidence-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $directory | Out-Null
$failed = 0

function EvidenceCheck([string] $Name, [bool] $Good) {
  if ($Good) { Write-Host "PASS  e2e evidence: $Name" }
  else { Write-Host "FAIL  e2e evidence: $Name"; $script:failed++ }
}

function ReadEvidence {
  return Get-Content -LiteralPath (Join-Path $directory 'observations.json') -Raw | ConvertFrom-Json
}

try {
  $manifest = [pscustomobject]@{ apps = @(
    [pscustomobject]@{ package = 'fixture.a'; versionCode = 1; surfaceTrace = $true },
    [pscustomobject]@{ package = 'fixture.b'; versionCode = 2 }
  ) }
  $manifestPath = Join-Path $directory 'apps.json'
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
  New-E2eEvidence $manifest $manifestPath @('fixture.a') $directory
  $read = ReadEvidence
  EvidenceCheck 'initial checkpoint contains every planned scenario and explicit exclusion' (
    $read.checks.Count -eq 5 -and
    @($read.checks | Where-Object { $_.status -eq 'not_run' }).Count -eq 4 -and
    @($read.checks | Where-Object { $_.package -eq 'fixture.b' -and $_.status -eq 'skipped' -and $_.reason -eq 'excluded_by_only' }).Count -eq 1 -and
    -not $read.completed)
  EvidenceCheck 'manifest hash identifies actual input bytes' (
    $read.manifest_sha256 -eq (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant())

  Set-E2eCheck 'fixture.a' 'launch' 'running'
  $read = ReadEvidence
  $launch = $read.checks | Where-Object { $_.scenario -eq 'launch' -and $_.package -eq 'fixture.a' }
  EvidenceCheck 'interruption preserves the running check and untouched denominator' (
    $launch.status -eq 'running' -and $launch.started_at -and -not $launch.finished_at -and $read.checks.Count -eq 5)
  Set-E2eCheck 'fixture.a' 'launch' 'failed' @('fixture failure')
  Set-E2eCheck 'fixture.a' 'surface-trace' 'running'
  Set-E2eCheck 'fixture.a' 'surface-trace' 'passed'
  $read = ReadEvidence
  EvidenceCheck 'a traced pass preserves the default failure and problem' (
    @($read.checks | Where-Object { $_.scenario -eq 'launch' -and $_.status -eq 'failed' -and $_.problems[0] -eq 'fixture failure' }).Count -eq 1 -and
    @($read.checks | Where-Object { $_.scenario -eq 'surface-trace' -and $_.status -eq 'passed' }).Count -eq 1)
  $rejected = $false
  try { Set-E2eCheck 'fixture.a' 'launch' 'passed' } catch { $rejected = $_ -match 'terminal' }
  EvidenceCheck 'terminal result cannot be overwritten by a retry' $rejected

  $rejected = $false
  try { Set-E2eCheck 'fixture.b' 'launch' 'running' } catch { $rejected = $_ -match 'terminal' }
  EvidenceCheck 'excluded check cannot be reported as running' $rejected

  Set-Content -LiteralPath (Join-Path $directory 'dalvikvm.exe') 'isolated runtime fixture'
  Set-Content -LiteralPath (Join-Path $directory 'build.prop') "ro.build.version.sdk=36`nro.build.version.sdk_full=36.0`nro.product.cpu.abilist=x86_64,arm64-v8a"
  Set-E2eRuntime $directory
  $read = ReadEvidence
  EvidenceCheck 'installed bytes and declared properties are recorded without invented source identity' (
    $read.runtime.installed_files.'dalvikvm.exe' -eq (Get-FileHash -LiteralPath (Join-Path $directory 'dalvikvm.exe')).Hash.ToLowerInvariant() -and
    $read.runtime.declared_properties.'ro.build.version.sdk_full' -eq '36.0' -and
    $null -eq $read.runtime.source_revision -and $null -eq $read.runtime.android_reference)
  Add-E2eApk 'fixture.a' ('a' * 64)
  $read = ReadEvidence
  EvidenceCheck 'APK record preserves unmeasured signer and selected ABI as null' (
    $read.apks.Count -eq 1 -and $read.apks[0].sha256 -eq ('a' * 64) -and
    $null -eq $read.apks[0].signer_sha256 -and $null -eq $read.apks[0].selected_abi)

  $rejected = $false
  try { New-E2eEvidence $manifest $manifestPath @('fixture.unknown') $directory } catch { $rejected = $_ -match 'Unknown -Only' }
  EvidenceCheck 'unknown selection is rejected instead of creating an empty success' $rejected

  $tokens = $null; $parseErrors = $null
  [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'run-e2e.ps1'), [ref]$tokens, [ref]$parseErrors)
  EvidenceCheck 'instrumented Windows runner parses' ($parseErrors.Count -eq 0)
  foreach ($error in $parseErrors) { Write-Host $error }
  Close-E2eEvidenceRun
  $rejected = $false
  try { Write-E2eEvidence } catch { $rejected = $_ -match 'ownership' }
  EvidenceCheck 'a released writer cannot overwrite a later invocation' $rejected
} finally {
  Close-E2eEvidenceRun
  Remove-Item -LiteralPath $directory -Recurse -Force
}
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }

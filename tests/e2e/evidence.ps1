# Structured observations from the actual e2e runner. PASS means only that this
# runner's visual/lifecycle smoke checks passed; it is never full APK compatibility.
# Keep an initial plan and write after every transition so interrupted runs retain
# their denominator. coverage.py validates and summarizes this evidence in CI.

function Open-E2eEvidenceRun([string] $Directory, [string] $InvocationId = ([guid]::NewGuid().ToString('N'))) {
  if ($null -ne $script:E2eRunLock) { throw 'An e2e evidence run is already open in this process' }
  if ($InvocationId -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid e2e invocation ID' }
  $directoryPath = [IO.Path]::GetFullPath($Directory)
  try {
    $handle = [IO.File]::Open((Join-Path $directoryPath '.e2e-run.lock'),
      [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
  } catch {
    throw "Cannot exclusively own e2e output directory '$directoryPath': $_"
  }
  $script:E2eRunLock = $handle
  $script:E2eRunDirectory = $directoryPath
  $script:E2eInvocationId = $InvocationId
  $script:E2eEvidence = $null
  try {
    # Only the owner may remove earlier reports. The lock file is persistent:
    # unlinking it on release could let two processes lock different inodes.
    foreach ($name in @('observations.json', 'coverage.json', 'coverage.md')) {
      $path = Join-Path $directoryPath $name
      if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -ErrorAction Stop }
    }
  } catch {
    Close-E2eEvidenceRun
    throw
  }
}

function Close-E2eEvidenceRun {
  if ($null -ne $script:E2eRunLock) {
    $script:E2eRunLock.Dispose()
    $script:E2eRunLock = $null
  }
}

function Write-E2eEvidence {
  if ($null -eq $script:E2eRunLock -or -not $script:E2eRunLock.CanWrite -or
      $script:E2eEvidence.invocation_id -ne $script:E2eInvocationId) {
    throw 'Writing e2e observations requires ownership of the active invocation'
  }
  $script:E2eEvidence.updated_at = [DateTime]::UtcNow.ToString('o')
  $temporary = "$script:E2eEvidencePath.$script:E2eInvocationId.tmp"
  $script:E2eEvidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
  Move-Item -LiteralPath $temporary -Destination $script:E2eEvidencePath -Force
}

function New-E2eEvidence($ManifestData, [string] $ManifestPath, [string[]] $Only, [string] $Directory) {
  $packages = @($ManifestData.apps | ForEach-Object { $_.package })
  if ($packages.Count -eq 0 -or @($packages | Sort-Object -Unique).Count -ne $packages.Count) {
    throw 'e2e manifest must contain distinct packages'
  }
  foreach ($package in $Only) {
    if ($packages -notcontains $package) { throw "Unknown -Only package: $package" }
  }
  if ($null -eq $script:E2eRunLock) { Open-E2eEvidenceRun $Directory }
  if (-not [string]::Equals([IO.Path]::GetFullPath($Directory), $script:E2eRunDirectory,
      [StringComparison]::Ordinal)) { throw 'The output directory is not owned by this invocation' }
  if ($null -ne $script:E2eEvidence) { throw 'The active e2e invocation is already initialized' }
  $checks = @()
  foreach ($entry in $ManifestData.apps) {
    $selected = $Only.Count -eq 0 -or $Only -contains $entry.package
    $scenarios = @('launch')
    if ($entry.surfaceTrace) { $scenarios += 'surface-trace' }
    foreach ($scenario in $scenarios) {
      $checks += [ordered]@{
        package = $entry.package; scenario = $scenario
        status = $(if ($selected) { 'not_run' } else { 'skipped' })
        reason = $(if ($selected) { $null } else { 'excluded_by_only' })
        started_at = $null; finished_at = $null; problems = @()
      }
    }
  }
  foreach ($scenario in @('association', 'shortcut')) {
    $checks += [ordered]@{
      package = $null; scenario = $scenario; status = 'not_run'
      reason = $null; started_at = $null; finished_at = $null; problems = @()
    }
  }
  $script:E2eEvidencePath = Join-Path $Directory 'observations.json'
  $script:E2eEvidence = [ordered]@{
    schema_version = 1
    invocation_id = $script:E2eInvocationId
    manifest_sha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    started_at = [DateTime]::UtcNow.ToString('o'); updated_at = $null
    completed = $false
    runner = [ordered]@{
      os = [Runtime.InteropServices.RuntimeInformation]::OSDescription
      architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
      test_checkout_sha = $env:GITHUB_SHA
      github_run_id = $env:GITHUB_RUN_ID
      github_run_attempt = $env:GITHUB_RUN_ATTEMPT
      only = @($Only)
    }
    runtime = [ordered]@{
      installer_sha256 = $null; installation_mode = $null
      installed_files = [ordered]@{}; declared_properties = [ordered]@{}
      source_revision = $null; aosp_tag = $null; modules = $null
      providers = $null; hardware_profile = $null; android_reference = $null
    }
    apks = @(); checks = $checks
  }
  Write-E2eEvidence
}

function Set-E2eCheck([string] $Package, [string] $Scenario, [string] $Status,
                     [string[]] $Problems = @(), [string] $Reason = '') {
  if ($Status -notin @('running', 'passed', 'failed', 'blocked')) { throw "Invalid e2e status: $Status" }
  $matches = @($script:E2eEvidence.checks | Where-Object {
    $_.scenario -eq $Scenario -and
      ($Scenario -in @('association', 'shortcut') -or $_.package -eq $Package)
  })
  if ($matches.Count -ne 1) { throw "No distinct planned check: $Package/$Scenario" }
  $check = $matches[0]
  if ($check.status -in @('skipped', 'passed', 'failed', 'blocked')) {
    throw "Cannot replace terminal e2e result: $Package/$Scenario"
  }
  $check.package = $Package
  $check.status = $Status
  $check.reason = $Reason
  $check.problems = @($Problems)
  if ($Status -eq 'running') { $check.started_at = [DateTime]::UtcNow.ToString('o') }
  else { $check.finished_at = [DateTime]::UtcNow.ToString('o') }
  Write-E2eEvidence
}

function Set-E2eRuntime([string] $InstallationDirectory) {
  # These are actual installed bytes, including for -SkipInstall. They identify
  # components, not a complete SBOM or an inferred runtime source commit.
  foreach ($name in @('dalvikvm.exe', 'gesher-app.jar', 'framework.jar', 'build.prop')) {
    $file = Join-Path $InstallationDirectory $name
    if (Test-Path -LiteralPath $file -PathType Leaf) {
      $script:E2eEvidence.runtime.installed_files[$name] =
        (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  $properties = Join-Path $InstallationDirectory 'build.prop'
  if (Test-Path -LiteralPath $properties -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $properties) {
      if ($line -match '^(ro\.build\.version\.(sdk|sdk_full|release)|ro\.product\.cpu\.abilist)=(.*)$') {
        $script:E2eEvidence.runtime.declared_properties[$Matches[1]] = $Matches[3]
      }
    }
  }
  Write-E2eEvidence
}

function Add-E2eApk([string] $Package, [string] $Sha256) {
  $script:E2eEvidence.apks += [ordered]@{
    package = $Package; sha256 = $Sha256; target_sdk = $null
    selected_abi = $null; signer_sha256 = $null
  }
  Write-E2eEvidence
}

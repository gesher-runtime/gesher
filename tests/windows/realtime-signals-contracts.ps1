# SPDX-License-Identifier: Apache-2.0
# Bounded executions of the actual native POSIX binary and NDK ELF programs.
# Every contract assertion is required. Missing artifacts and timeouts fail.
param(
  [Parameter(Mandatory=$true)][string] $Dir,
  [switch] $HostOnly,
  [switch] $GuestOnly,
  [string] $EvidenceDirectory = 'realtime-signals-evidence'
)
$ErrorActionPreference = 'Stop'
if ($HostOnly -and $GuestOnly) { throw 'Choose one role or both roles' }
$Dir = [IO.Path]::GetFullPath($Dir)
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force $EvidenceDirectory | Out-Null
$results = [Collections.Generic.List[object]]::new()
$fixture = Join-Path $PSScriptRoot '../exec/realtime_signals_contract.c'
$fixtureHash = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
$revision = (& git -C (Join-Path $PSScriptRoot '../..') rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') { throw 'Cannot bind tested checkout identity' }

function Invoke-RealtimeCase([string] $Role, [string] $Case, [string] $Executable, [string[]] $Arguments) {
  $record = [ordered]@{ role=$Role; case=$Case; revision=$revision; fixture_sha256=$fixtureHash;
    executable_sha256=$null; exit=$null; timeout=$false; stdout=''; stderr=''; pass=$false; elapsed_ms=0 }
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $process = $null
  try {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Missing required executable $Executable" }
    $record.executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $Dir
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(20000)) {
      $record.timeout = $true
      # Only the process tree created by this exact fixture invocation.
      $process.Kill($true)
      if (-not $process.WaitForExit(5000)) { throw 'Owned fixture did not terminate after bounded cleanup' }
    }
    if (-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) { throw 'Owned fixture output readers did not complete' }
    $record.stdout = $stdout.Result
    $record.stderr = $stderr.Result
    $record.exit = $process.ExitCode
    $sentinel = if ($Case -eq 'legacy') { 'realtime_signals: 62 assertions, 0 failures' }
                else { 'realtime contract: 0 failures' }
    $record.pass = -not $record.timeout -and $record.exit -eq 0 -and
        $record.stdout.Contains($sentinel) -and $record.stdout -notmatch '(?m)^FAIL'
  } catch {
    $record.stderr += "`n$($_.Exception.Message)"
    if ($null -ne $process) {
      try { if (-not $process.HasExited) { $process.Kill($true); $null = $process.WaitForExit(5000) } }
      catch { $record.stderr += "`nOwned process cleanup failed: $_" }
    }
  } finally {
    if ($null -ne $process) { $process.Dispose() }
    $record.elapsed_ms = $clock.ElapsedMilliseconds
  }
  $results.Add([pscustomobject]$record)
  Write-Host "$(if ($record.pass) { 'PASS' } else { 'FAIL' }) realtime signals $Role $Case"
  Write-Host $record.stdout
  if ($record.stderr) { Write-Host $record.stderr }
  $record | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $EvidenceDirectory "$Role-$Case.json")
}

$within = @('fifo', 'ignore', 'quota', 'priority', 'timeout', 'thread', 'handler')
if (-not $GuestOnly) {
  Invoke-RealtimeCase 'windows-posix' 'legacy' (Join-Path $Dir 'realtime_signals_test.exe') @()
  foreach ($case in $within) {
    Invoke-RealtimeCase 'windows-posix' $case (Join-Path $Dir 'realtime_signal_contract_test.exe') @($case)
  }
}
if (-not $HostOnly) {
  $manifestPath = Join-Path $Dir 'realtime-signals-build.json'
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  if ($manifest.revision -ne $revision -or $manifest.fixture_sha256 -ne $fixtureHash -or
      $manifest.ndk -ne '27.2.12479018') { throw 'NDK signal fixture provenance does not match tested checkout' }
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    $programName = "realtime-signals-$abi"
    $program = Join-Path $Dir $programName
    $hash = (Get-FileHash -LiteralPath $program -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.artifacts.$programName -ne $hash) { throw "NDK signal fixture digest mismatch: $abi" }
    $guestPath = ($program -replace '\\', '/') -replace '^[A-Za-z]:', ''
    foreach ($case in ($within + @('cross', 'stop', 'status'))) {
      Invoke-RealtimeCase "ndk-$abi" $case (Join-Path $Dir 'gesher-exec.exe') @(
        '--elf', $guestPath, 'realtime-signals', $case, $guestPath)
    }
  }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $EvidenceDirectory 'results.json')
$failed = @($results | Where-Object { -not $_.pass }).Count
Write-Host "realtime signals gates: $($results.Count) actual processes, $failed failing contracts"
if ($failed -ne 0) { exit 1 }
exit 0

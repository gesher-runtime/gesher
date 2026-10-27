# Core libraries (docs/core-libs.md): AOSP's own unit tests of libutils,
# libcutils and libandroidfw, built by .github/workflows/libs.yml into
# $Dir/libs-tests. Every test runs except those listed below, each excluded
# for the reason given here and in docs/core-libs.md.
#
# Dot-sourced by run-tests.ps1 (and by libs.yml on its own), so it adds to
# $failed in the caller's scope.

$libsTests = Join-Path $Dir 'libs-tests'

# Runs a googletest executable with the exclusions given; true when every
# test that ran passed.
. "$PSScriptRoot/../helpers/GtestProcess.ps1"

function Invoke-CoreLibsTest([string] $Name, [string[]] $Exclude = @(), [int] $TimeoutSec = 900) {
  $exe = Join-Path $libsTests "$Name.exe"
  if (-not (Test-Path $exe)) {
    Write-Host "FAIL  $Name (missing: $exe)"
    return $false
  }
  $arguments = @('--gtest_color=no')
  if ($Exclude) { $arguments += "--gtest_filter=-$($Exclude -join ':')" }
  $execution = Invoke-GtestProcess -Executable $exe -Arguments $arguments -WorkingDirectory $libsTests -TimeoutSec $TimeoutSec
  if ($env:GESHER_CI_PROCESS_TRACE -eq '1') {
    Write-Host "INFO  gtest process: complete=$($execution.ProcessComplete) readers=$($execution.ReadersComplete) disposed=$($execution.Disposed) exit=$($execution.Exit)"
  }
  $timedOut = $execution.TimedOut
  $stdout = $execution.Stdout
  $stderr = $execution.Stderr
  $text = $stdout + $stderr
  $passed = if ($text -match '\[  PASSED  \] (\d+) test') { [int] $Matches[1] } else { 0 }
  $failedTests = @($text -split "`r?`n" | Where-Object { $_ -match '^\[  FAILED  \] [^ ]+\.[^ ]+' } |
    ForEach-Object { ($_ -replace '^\[  FAILED  \] ', '') -replace ',.*$', '' } | Sort-Object -Unique)
  $excluded = if ($Exclude) { " ($($Exclude.Count) excluded, docs/core-libs.md)" } else { '' }
  if (-not $timedOut -and $execution.ProcessComplete -and $execution.ReadersComplete -and
      $execution.Disposed -and -not $execution.Problem -and $execution.Exit -eq 0 -and $passed -gt 0 -and -not $failedTests) {
    Write-Host "PASS  $Name`: $passed tests passed$excluded"
    return $true
  }
  $why = if ($timedOut) { "timed out after $TimeoutSec s" } elseif ($null -ne $execution.Exit) { 'exit 0x{0:X8}' -f $execution.Exit } else { 'incomplete process' }
  if ($execution.Problem) { $why += '; ' + $execution.Problem }
  Write-Host "FAIL  $Name`: $why, $passed passed, $($failedTests.Count) failed$excluded"
  foreach ($t in $failedTests) { Write-Host "        $t" }
  # The failures' messages, and the end of the output (a crash, a hang).
  $lines = $text -split "`r?`n"
  $lines | Select-String -Pattern 'Failure|error:|Expected|Which is|Value of|Actual' -Context 2, 4 |
    Select-Object -First 80 | Out-Host
  Write-Host '        ...'
  $lines | Select-Object -Last 40 | Out-Host
  return $false
}

# libutils_binder_test and its custom-allocator variant (libutils/binder).
foreach ($t in 'libutils_binder_test', 'libutils_binder_test_custom_allocator') {
  if (-not (Invoke-CoreLibsTest $t)) { $failed++ }
}

# libutils_test (libutils).
if (-not (Invoke-CoreLibsTest 'libutils_test')) { $failed++ }

# libcutils_test (libcutils). android_get_control_socket binds under
# /dev/socket, init's socket directory, which gesher-base's paths do not have.
if (-not (Invoke-CoreLibsTest 'libcutils_test' -Exclude @('SocketsTest.android_get_control_socket'))) {
  $failed++
}

# libandroidfw_tests (frameworks/base/libs/androidfw), on its tests/data.
# ObbFile_test writes into $EXTERNAL_STORAGE, which Android sets for apps.
$externalStorage = Join-Path ([IO.Path]::GetTempPath()) "gesher-libs-sdcard-$PID"
New-Item -ItemType Directory -Force $externalStorage | Out-Null
$env:EXTERNAL_STORAGE = $externalStorage.Replace([char]92, [char]47)
try {
  if (-not (Invoke-CoreLibsTest 'libandroidfw_tests')) { $failed++ }
} finally {
  Remove-Item Env:EXTERNAL_STORAGE
  Remove-Item -Recurse -Force $externalStorage -ErrorAction SilentlyContinue
}

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
function Invoke-CoreLibsTest([string] $Name, [string[]] $Exclude = @(), [int] $TimeoutSec = 900) {
  $exe = Join-Path $libsTests "$Name.exe"
  if (-not (Test-Path $exe)) {
    Write-Host "FAIL  $Name (missing: $exe)"
    return $false
  }
  $arguments = @('--gtest_color=no')
  if ($Exclude) { $arguments += "--gtest_filter=-$($Exclude -join ':')" }
  $out = New-TemporaryFile
  $err = New-TemporaryFile
  $p = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $libsTests `
    -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
  $null = $p.Handle  # keeps ExitCode readable after the process ends
  $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
  if ($timedOut) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
  $p.WaitForExit()
  $text = (Get-Content -Raw $out) + (Get-Content -Raw $err)
  Remove-Item $out, $err -ErrorAction SilentlyContinue
  $passed = if ($text -match '\[  PASSED  \] (\d+) test') { [int] $Matches[1] } else { 0 }
  $failedTests = @($text -split "`r?`n" | Where-Object { $_ -match '^\[  FAILED  \] [^ ]+\.[^ ]+' } |
    ForEach-Object { ($_ -replace '^\[  FAILED  \] ', '') -replace ',.*$', '' } | Sort-Object -Unique)
  $excluded = if ($Exclude) { " ($($Exclude.Count) excluded, docs/core-libs.md)" } else { '' }
  if (-not $timedOut -and $p.ExitCode -eq 0 -and $passed -gt 0 -and -not $failedTests) {
    Write-Host "PASS  $Name`: $passed tests passed$excluded"
    return $true
  }
  $why = if ($timedOut) { "timed out after $TimeoutSec s" } else { 'exit 0x{0:X8}' -f $p.ExitCode }
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

# libcutils_test (libcutils).
if (-not (Invoke-CoreLibsTest 'libcutils_test')) { $failed++ }

# libandroidfw_tests (frameworks/base/libs/androidfw), on its tests/data.
if (-not (Invoke-CoreLibsTest 'libandroidfw_tests')) { $failed++ }

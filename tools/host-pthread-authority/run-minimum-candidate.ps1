$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI-only native controls' }

function Invoke-OwnedControl([string]$Tag, [string]$Mode, [string[]]$Arguments) {
  $folder=Join-Path $pwd 'minimum-controls'
  $stdout=Join-Path $env:RUNNER_TEMP "minimum-$Tag-$Mode.out"
  $stderr=Join-Path $env:RUNNER_TEMP "minimum-$Tag-$Mode.err"
  $process=Start-Process -FilePath (Join-Path $folder "$Tag.exe") -ArgumentList $Arguments -WorkingDirectory $folder -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  try {
    if (-not $process.WaitForExit(60000)) { throw "Minimum deadline tag=$Tag mode=$Mode" }
    $process.Refresh()
    Get-Content -LiteralPath $stdout
    Get-Content -LiteralPath $stderr
    if ($process.ExitCode -ne 0) { throw "Minimum actual exit=$($process.ExitCode) tag=$Tag mode=$Mode" }
    python tools/host-pthread-authority/minimum_bundle.py trace $Tag $Mode $stdout
    if ($LASTEXITCODE -ne 0) { throw "Minimum actual trace failed tag=$Tag mode=$Mode" }
  } finally {
    if (-not $process.HasExited) { $process.Kill($true); if (-not $process.WaitForExit(5000)) { throw 'Minimum owned process cleanup failed' } }
    $process.Dispose()
    if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout }
    if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr }
  }
}

Invoke-OwnedControl 'reference' 'attachment' @('attachment','0')
foreach ($tag in @('static','dynamic')) {
  Invoke-OwnedControl $tag 'attachment' @('attachment','2')
  Invoke-OwnedControl $tag 'contracts' @('contracts')
}

# Same original four-mode C++ assertions; this diagnostic role cannot waive census.
$role='minimum-candidate'
$stdout=Join-Path $env:RUNNER_TEMP 'late-minimum-candidate.out'
$stderr=Join-Path $env:RUNNER_TEMP 'late-minimum-candidate.err'
$process=Start-Process -FilePath (Join-Path $pwd "late-cpp-$role/late-cpp.exe") -WorkingDirectory (Join-Path $pwd "late-cpp-$role") -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
  if (-not $process.WaitForExit(30000)) { throw 'Minimum genuine C++ fixture deadline' }
  $process.Refresh()
  $actual=Get-Content -Raw -LiteralPath $stdout
  Write-Host $actual
  python tools/host-pthread-authority/late_cpp_bundle.py observations $role $stdout
  if ($LASTEXITCODE -ne 0) { throw 'Missing actual minimum phase characterization' }
  if ($process.ExitCode -ne 0) { throw "Minimum genuine C++ actual exit=$($process.ExitCode)" }
  if (-not $actual.Contains('CPP_TLS_RESULT cases=4 failures=0 conformant=1')) { throw 'Incomplete minimum genuine C++ evidence' }
  python tools/host-pthread-authority/late_cpp_bundle.py trace $role $stdout
  if ($LASTEXITCODE -ne 0) { throw 'Minimum genuine C++ strict trace failed' }
  foreach ($mode in @('pthread_return','pthread_exit','std_thread_return','native_ExitThread')) {
    if (-not $actual.Contains("CPP_TLS_CASE mode=$mode constructed=2 a=1 b=1 mismatch=0 a_done=1 b_done=1 conformant=1")) { throw "Missing actual C++ case $mode" }
    if ($actual -notmatch "(?m)^CPP_TLS_KERNEL mode=$mode signaled=1 exit=[0-9]+ owned_handle_closed=1\r?$") { throw "Missing actual C++ kernel completion $mode" }
  }
  foreach ($mode in @('pthread_return','pthread_exit','std_thread_return')) {
    if (-not $actual.Contains("CPP_TLS_JOIN mode=$mode phase=join-return a=1 b=1 mismatch=0 sequence=2 a_done=1 b_done=1 conformant=1")) { throw "Incomplete callbacks at join return $mode" }
    if ($actual -notmatch "(?m)^CPP_TLS_JOIN_KERNEL mode=$mode phase=join-return wait=(0|258) observation_only=1\r?$") { throw "Missing join-return kernel observation $mode" }
  }
} finally {
  if (-not $process.HasExited) { $process.Kill($true); if (-not $process.WaitForExit(5000)) { throw 'Minimum C++ owned cleanup failed' } }
  $process.Dispose()
  if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout }
  if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr }
}

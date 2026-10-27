# CI-only diagnostic runner. The exact RunTimed body is copied from
# Root source ec0f003cc149a0f985b5f280924477dc20414872, renamed only.
# It preserves native status/-1 and bounds process, termination and pipe waits.
function Web3DiagnosticRun([string] $Exe, [string[]] $Arguments = @(), [int] $Seconds = 120) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  try {
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    $incomplete = -not $p.WaitForExit($Seconds * 1000)
    $out = ''
    if ($incomplete) {
      $out += "`n(timed out after $Seconds s)"
      try { $p.Kill($true) } catch { $out += "`n(process kill failed: $_)" }
      if (-not $p.WaitForExit(5000)) { $out += "`n(process still alive five seconds after kill)" }
    }
    # An exited parent can leave descendants holding redirected output open.
    foreach ($pipe in @(@{ Task = $stdout; Name = 'stdout' }, @{ Task = $stderr; Name = 'stderr' })) {
      try {
        if ($pipe.Task.Wait(5000)) { $out += $pipe.Task.Result }
        else { $incomplete = $true; $out += "`n($($pipe.Name) did not complete within five seconds)" }
      } catch { $incomplete = $true; $out += "`n($($pipe.Name) read failed: $_)" }
    }
    $global:LASTEXITCODE = if ($incomplete) { -1 } else { $p.ExitCode }
    return $out
  } finally { $p.Dispose() }
}

function Web3NativeProcessState([int] $TargetId) {
  # This is called only after the original Web3Run teardown deadline failed.
  # WCT's synchronous API is not cancellable, so it runs in a bounded child.
  $out = Web3DiagnosticRun "$script:web3Observer" @("$TargetId") 25
  $code = $LASTEXITCODE
  Write-Host "INFO  web3 native diagnostic: pid $TargetId, observer exit $code (never a test success)"
  Write-Host $out
}

function Web3ObserverControl {
  $holdLog = Join-Path $env:RUNNER_TEMP "web3-observer-hold-$env:GITHUB_RUN_ID.log"
  $hold = Start-Process "$script:web3Observer" -ArgumentList '--hold' -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $holdLog -RedirectStandardError "$holdLog.err"
  try {
    $deadline = (Get-Date).AddSeconds(5)
    do {
      $text = Get-Content -LiteralPath $holdLog -Raw -ErrorAction SilentlyContinue
      if ($text -match 'WEB3_OBSERVER_HOLD pid=(\d+) tid=(\d+)') { break }
      Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline -and -not $hold.HasExited)
    $identity = [regex]::Match("$text", 'WEB3_OBSERVER_HOLD pid=(\d+) tid=(\d+)')
    if (-not $identity.Success -or [int]$identity.Groups[1].Value -ne $hold.Id) {
      throw 'The actual observer control did not publish its owned PID/TID'
    }
    $tid = [int]$identity.Groups[2].Value
    $out = Web3DiagnosticRun "$script:web3Observer" @("$($hold.Id)") 25
    $code = $LASTEXITCODE
    Write-Host $out
    if ($code -ne 0 -or -not $out.Contains("WEB3_OBSERVER_BEGIN pid=$($hold.Id)") -or
        -not $out.Contains("waits (thread ${tid}):") -or
        $out -notmatch '#00 (?:KERNELBASE|ntdll|kernel32)!' -or
        -not $out.Contains("WEB3_OBSERVER_END pid=$($hold.Id)")) {
      throw 'The real held-thread control produced no complete native stack/wait chain'
    }
    Write-Host 'PASS  web3 diagnostic observer: independent held process has an actual native stack and wait chain'
  } finally {
    try {
      if (-not $hold.HasExited) { $hold.Kill($true) }
      if (-not $hold.WaitForExit(5000)) { throw 'The owned observer control survived termination' }
    } finally { $hold.Dispose() }
  }
}

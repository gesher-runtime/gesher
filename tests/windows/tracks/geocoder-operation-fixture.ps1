param([switch] $NoncooperativeChild)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'geocoder-operation.ps1')
Add-Type -Path (Join-Path $PSScriptRoot 'geocoder-controlled-worker.cs')

function Assert-True([bool] $condition, [string] $message) {
  if (-not $condition) { throw "fixture assertion: $message" }
}
function Invoke-ControlledCase([string] $case) {
  $worker = [ControlledGeocoderWorker]::new(($case -eq 'error'), $false)
  $worker.AwaitStarted()
  $state = @{ calls = 0 }
  # The first wait is the controlled deadline decision. The controller releases
  # completion on either side of that decision; no scheduler sleep is involved.
  $adapter = @{
    Wait = {
      param($ms)
      $state.calls++
      if ($state.calls -eq 1) {
        if ($case -in @('success', 'error')) {
          $worker.Release()
          if (-not $worker.Wait(10000)) { throw 'worker completion barrier timed out' }
          return $true
        }
        return $false
      }
      if ($case -eq 'unobserved') { return $false }
      return $worker.Wait($ms)
    }
    Cancel = {
      if ($case -eq 'after') {
        $worker.Release()
        if (-not $worker.Wait(10000)) { throw 'late completion barrier timed out' }
      }
      $worker.Cancel()
    }
    Status = { $worker.Status }
    Close = { $worker.Close() }
    Result = { $worker.Result() }
  }
  $failed = $false
  try { $answer = Invoke-GeocoderOperation $adapter 30000 5000 }
  catch { $failed = $true; $failure = $_.Exception.Message }
  if ($case -eq 'unobserved') {
    Assert-True ($worker.CloseCalls -eq 0) 'unobserved task must not be closed'
    Assert-True ($worker.Wait(10000)) 'unobserved worker eventually exits'
    $worker.Close()
  }
  Assert-True $worker.WorkerExited "${case}: worker exited"
  Assert-True $worker.ResourcesDisposed "${case}: resources disposed"
  Assert-True ($worker.Status -in @('Completed', 'Canceled', 'Error')) "${case}: terminal status"
  if ($case -eq 'success') {
    Assert-True (-not $failed -and $answer -eq 'controlled result') 'success returns result'
    Assert-True ($worker.ResultReads -eq 1 -and $worker.CancelCalls -eq 0) 'success result and cancel counts'
  } else {
    Assert-True $failed "${case}: must fail"
    Assert-True ($worker.ResultReads -eq 0) "${case}: no result read"
    if ($case -eq 'error') {
      Assert-True ($worker.Status -eq 'Error' -and $worker.CancelCalls -eq 0) 'error remains error'
    } else {
      Assert-True ($failure -match 'did not complete within 30 seconds') "${case}: timeout remains failure"
      Assert-True ($worker.CancelCalls -eq 1) "${case}: same worker received cancel"
    }
  }
  if ($case -in @('timeout', 'unobserved')) {
    Assert-True ($worker.CancelObserved -eq 1 -and $worker.Status -eq 'Canceled') 'worker observed cancellation'
    Assert-True ($worker.SuccessCallbacks -eq 0) 'no success callback after cancellation and worker exit'
  }
  $worker.Close()
  Assert-True ($worker.CloseCalls -eq 2 -and $worker.ResourcesDisposed) "${case}: repeated cleanup"
}

if ($NoncooperativeChild) {
  $worker = [ControlledGeocoderWorker]::new($false, $true)
  $worker.AwaitStarted()
  $adapter = @{
    Wait = { param($ms) return $false }
    Cancel = { $worker.Cancel() }
    Status = { $worker.Status }
    Close = { $worker.Close() }
    Result = { $worker.Result() }
  }
  [Console]::Out.WriteLine('worker-started'); [Console]::Out.Flush()
  Invoke-GeocoderOperation $adapter 30000 5000
  throw 'noncooperative cancel unexpectedly returned'
}

foreach ($case in @('success', 'error', 'timeout', 'after', 'unobserved')) { Invoke-ControlledCase $case }
# This proves containment of an owned child, not shutdown of a Maps provider.
$psi = [Diagnostics.ProcessStartInfo]::new('powershell.exe')
foreach ($arg in @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-NoncooperativeChild')) { $psi.ArgumentList.Add($arg) }
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [Diagnostics.Process]::Start($psi)
try {
  foreach ($expected in @('worker-started', 'cancel-entered')) {
    $line = $p.StandardOutput.ReadLineAsync()
    Assert-True ($line.Wait(30000) -and $line.Result -eq $expected) "child barrier $expected"
  }
  Assert-True (-not $p.WaitForExit(1000)) 'noncooperative child remains blocked'
  $p.Kill($true)
  Assert-True ($p.WaitForExit(10000)) 'noncooperative child reaped'
  Assert-True ($p.ExitCode -ne 0) 'noncooperative child fails'
} finally {
  if (-not $p.HasExited) { $p.Kill($true); $p.WaitForExit() }
  $p.Dispose()
}
Write-Host 'PASS controlled geocoder worker and child containment'

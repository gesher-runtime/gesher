param([switch] $NoncooperativeChild)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'geocoder-operation.ps1')
Add-Type -Path (Join-Path $PSScriptRoot 'geocoder-controlled-worker.cs')

function Assert-True([bool] $condition, [string] $message) {
  if (-not $condition) { throw "fixture assertion: $message" }
}
function Invoke-ControlledCase([string] $case) {
  $elapsed = [Diagnostics.Stopwatch]::StartNew()
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
  $elapsed.Stop()
  Write-Host "case=$case elapsed_ms=$($elapsed.ElapsedMilliseconds) status=$($worker.Status) cancel_calls=$($worker.CancelCalls) cancel_observed=$($worker.CancelObserved) result_reads=$($worker.ResultReads) success_callbacks=$($worker.SuccessCallbacks) worker_exited=$($worker.WorkerExited) resources_disposed=$($worker.ResourcesDisposed) close_calls=$($worker.CloseCalls)"
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
# The supervisor owns the actual PowerShell child through timeout and retirement.
# Output is inspected only after the root is signaled and the job is empty.
Add-Type -Path (Join-Path $PSScriptRoot '../../../tools/windows4/TsfJobSupervisor.cs')
$exe = [Environment]::ProcessPath
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -NoncooperativeChild'
$r = [TsfJobSupervisor]::Run($exe, $arguments, 45000, '')
Write-Host ('SERVICES_JOB_KERNEL_EVIDENCE ' + ($r | ConvertTo-Json -Depth 6 -Compress))
Assert-True ($r.Reason -ceq 'timeout' -and $r.ExitCode -eq 124 -and $r.Errors.Count -eq 0) 'intentional blocked Cancel timeout'
Assert-True ($r.Assigned -and $r.MemberBeforeResume -and $r.SuspendedSingleThread -and $r.PreResumeActive -eq 1 -and $r.SuspendedThreadCount -eq 1) 'suspended job assignment'
Assert-True ($r.Pid -ne 0 -and $r.Birth -gt 0 -and $r.RootHandleValue -ne 0 -and $r.JobHandleValue -ne 0 -and [string]::Equals($r.Image, $exe, [StringComparison]::OrdinalIgnoreCase)) 'child identity'
Assert-True ($r.TerminateRequested -and $r.TerminateSucceeded -and $r.RootSignaled -and $r.RootWaitResult -eq 0 -and $r.RootHandleClosed -and $r.ThreadHandleClosed -and $r.JobEmpty -and $r.LastJobActive -eq 0 -and $r.OwnedHandles -eq $r.ClosedHandles -and $r.CleanupMilliseconds -lt 5000) 'job terminated, reaped and handles closed'
Assert-True ($r.WritersRetired -and $r.OutputRead -and $r.ExclusiveSnapshot -and $r.StdoutBytes -le 1048576 -and $r.StderrBytes -le 1048576) 'bounded exclusive output after retirement'
Assert-True ($r.TotalProcesses -eq $r.Processes.Count -and @($r.Processes | Where-Object { -not $_.Member -or -not $_.Signaled -or -not $_.Closed -or $_.WaitResult -ne 0 -or $_.Birth -le 0 -or $_.HandleValue -eq 0 }).Count -eq 0) 'all job identities reaped'
Assert-True ($r.Output -match '(?m)^worker-started\r?$' -and $r.Output -match '(?m)^cancel-entered\r?$') 'actual child reached both barriers'
Write-Host 'PASS controlled geocoder worker and child containment'

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Qualification is CI only' }
if ((git rev-parse HEAD) -cne $env:GITHUB_SHA -or $LASTEXITCODE -ne 0) { throw 'Controller checkout identity mismatch' }
foreach ($source in @('tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/windows4/run-tsf-component.ps1','.github/workflows/tsf-format-component.yml')) {
  $blob = git hash-object $source
  if ($LASTEXITCODE -ne 0) { throw "Cannot hash $source" }
  Write-Host "TSF_CONTROLLER_SOURCE source=$env:GITHUB_SHA path=$source blob=$blob sha256=$((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant())"
}
Add-Type -Path tools/windows4/TsfJobSupervisor.cs
$directory = Join-Path $env:RUNNER_TEMP ('tsf-worker-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)
$worker = Join-Path $directory 'TsfJobWorker.exe'
try {
  # Framework csc emits a real console executable; no fixture or compilation occurs locally.
  $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
  $workerSource = Join-Path $PWD 'tools\windows4\TsfJobWorker.cs'
  & $compiler /nologo /target:exe "/out:$worker" $workerSource
  if ($LASTEXITCODE -ne 0) { throw 'Controlled worker compilation failed' }
  Write-Host "TSF_CONTROL_COMPILER path=$compiler sha256=$((Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()) worker_sha256=$((Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash.ToLowerInvariant())"
  foreach ($case in @('normal','exit23','hang','descendant','output','terminate-denied','query-denied')) {
    $mode = if ($case -in @('terminate-denied','query-denied')) { 'hang' } else { $case }
    $fault = if ($case -in @('terminate-denied','query-denied')) { $case } else { '' }
    $r = [TsfJobSupervisor]::Run($worker, $mode, 1500, $fault)
    Write-Host ('TSF_JOB_KERNEL_EVIDENCE ' + ($r | ConvertTo-Json -Depth 6 -Compress))
    if ($r.RootHandleValue -eq 0 -or $r.JobHandleValue -eq 0 -or $r.RootWaitResult -ne 0 -or $r.SuspendedThreadCount -ne 1 -or $r.PreResumeActive -ne 1) { throw "Raw handle/wait/census evidence failed: $case" }
    if (-not $r.Assigned -or -not $r.MemberBeforeResume -or -not $r.SuspendedSingleThread -or $r.Pid -eq 0 -or $r.Birth -le 0 -or -not [string]::Equals($r.Image, $worker, [StringComparison]::OrdinalIgnoreCase) -or -not $r.RootSignaled -or -not $r.RootHandleClosed -or -not $r.ThreadHandleClosed -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.OwnedHandles -lt 7 -or $r.CleanupMilliseconds -ge 5000 -or $r.MainMilliseconds -gt 5000) { throw "Kernel ownership/lifetime evidence failed: $case" }
    if ($case -eq 'query-denied') {
      if ($r.Success -or $r.JobEmpty -or $r.OutputRead -or -not $r.ControlStarted -or $r.Reason -cne 'timeout' -or $r.ExitCode -ne 124 -or -not $r.TerminateRequested -or -not $r.TerminateSucceeded -or $r.Errors.Count -ne 2 -or $r.Errors[0] -cne 'QueryInformationJobObject Win32=5' -or $r.Errors[1] -cne 'Cleanup unproved within shared 5000ms budget') { throw 'Unproved cleanup accepted' }
      continue
    }
    if (-not $r.JobEmpty -or $r.LastJobActive -ne 0 -or -not $r.WritersRetired -or -not $r.ControlStarted -or $r.TotalProcesses -ne $r.Processes.Count -or @($r.Processes | Where-Object { -not $_.Member -or -not $_.Signaled -or -not $_.Closed -or $_.WaitResult -ne 0 -or $_.Birth -le 0 -or $_.HandleValue -eq 0 -or -not $_.Role }).Count -ne 0 -or @($r.Processes.Pid | Sort-Object -Unique).Count -ne $r.Processes.Count) { throw "Job identities not reaped: $case" }
    if ($case -eq 'normal' -and (-not $r.Success -or $r.ExitCode -ne 0 -or $r.Output -notmatch 'CONTROL normal' -or $r.Error -notmatch 'CONTROL stderr')) { throw 'Normal control failed' }
    if ($case -eq 'exit23' -and ($r.Success -or $r.ExitCode -ne 23 -or $r.Reason -cne 'exit' -or $r.Errors.Count -ne 0)) { throw 'Nonzero exit accepted or hidden' }
    if ($case -eq 'descendant' -and (-not $r.Success -or -not $r.DescendantSuspended -or $r.DescendantThreadCount -ne 1 -or -not $r.RetainedWriterReady -or @($r.Processes | Where-Object Role -eq 'descendant').Count -ne 1 -or -not $r.TerminateRequested -or -not $r.TerminateSucceeded -or $r.Output -notmatch "DESCENDANT_PID=$($r.DescendantPid)\r?\n" -or $r.Output -notmatch 'RETAINED_WRITER_READY' -or -not $r.ExclusiveSnapshot)) { throw 'Retained descendant not proved reaped' }
    if ($case -eq 'hang' -and ($r.Success -or $r.Reason -cne 'timeout' -or $r.ExitCode -ne 124 -or $r.Errors.Count -ne 0 -or -not $r.TerminateSucceeded -or $r.Output -notmatch 'CONTROL hang')) { throw 'Timeout control failed' }
    if ($case -eq 'output' -and ($r.Success -or $r.OutputRead -or $r.ExclusiveSnapshot -or $r.CapturedBytes -ne 0 -or $r.StdoutBytes -le 1048576 -or $r.Reason -cne 'output-limit' -or $r.ExitCode -ne 124 -or $r.Errors.Count -ne 0 -or -not $r.TerminateSucceeded)) { throw 'Output cap not proved by byte count and exact reason' }
    if ($case -eq 'terminate-denied' -and ($r.Success -or $r.Reason -cne 'timeout' -or -not $r.TerminateRequested -or -not $r.TerminateSucceeded -or $r.Errors.Count -ne 1 -or $r.Errors[0] -cne 'Terminate denied Win32=5' -or $r.ExitCode -ne 124)) { throw 'Actual denied termination was hidden' }
  }
} finally {
  # This is a local executable/file cleanup, never a process wait/disposal path.
  if (Test-Path -LiteralPath $worker) { Remove-Item -LiteralPath $worker }
  Remove-Item -LiteralPath $directory
}

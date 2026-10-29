$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'Qualification is CI only' }
if ((git rev-parse HEAD) -cne $env:GITHUB_SHA -or $LASTEXITCODE -ne 0) { throw 'Controller checkout identity mismatch' }
foreach ($source in @('tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/windows4/run-tsf-component.ps1','.github/workflows/tsf-format-component.yml')) {
  $blob = git hash-object $source
  if ($LASTEXITCODE -ne 0) { throw "Cannot hash $source" }
  Write-Host "TSF_CONTROLLER_SOURCE source=$env:GITHUB_SHA path=$source blob=$blob sha256=$((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant())"
}
Add-Type -Path tools/windows4/TsfJobSupervisor.cs
# The PID is absent from a subsequent process census, but the retained
# handle still refers to the exact exited child and must expose its parent.
$start = [Diagnostics.ProcessStartInfo]::new($env:ComSpec, '/d /c pause >nul')
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$raceChild = [Diagnostics.Process]::Start($start)
try {
  $raceHandle = $raceChild.Handle
  $racePid = [uint32]$raceChild.Id
  if ($raceChild.HasExited) { throw 'Parent-query control exited before live handle capture' }
  $raceChild.StandardInput.WriteLine()
  $raceChild.StandardInput.Close()
  if (-not $raceChild.WaitForExit(5000) -or $raceChild.ExitCode -ne 0) { throw 'Parent-query control child did not exit normally' }
  if ([TsfJobSupervisor]::TestToolhelpContains($racePid)) { throw 'Exited retained child still present in Toolhelp snapshot' }
  if ([TsfJobSupervisor]::TestRetainedParent($raceHandle, $racePid) -ne [uint32]$PID) { throw 'Retained exited child parent mismatch' }
  if (-not [TsfJobSupervisor]::TestDeniedParent($raceHandle, $racePid)) { throw 'Parent query accepted a denied handle' }
  $rejected = $false
  try { [void][TsfJobSupervisor]::TestRetainedParent($raceHandle, ($racePid + 1)) } catch { $rejected = $true }
  if (-not $rejected) { throw 'Wrong retained PID accepted' }
  Write-Host "TSF_RETAINED_PARENT exited=$racePid toolhelp=absent expected_parent=$PID denied=actual wrong_pid=rejected"
} finally {
  if (-not $raceChild.HasExited) { $raceChild.Kill(); if (-not $raceChild.WaitForExit(5000)) { throw 'Control child cleanup unproved' } }
  $raceChild.Dispose()
}
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
  # The identical executable and arguments can actually run outside the limited
  # job; a missing executable or broken command must not masquerade as denial.
  $positiveDir = Join-Path $directory 'limit-positive'
  [void][IO.Directory]::CreateDirectory($positiveDir)
  try {
    [TsfJobSupervisor]::TestLimitPositive($worker, $positiveDir)
    Write-Host 'TSF_LIMIT_POSITIVE child=executed marker=present'
  } finally {
    Remove-Item -LiteralPath (Join-Path $positiveDir 'limit-child-ran') -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $positiveDir
  }
  foreach ($fault in @('console-invalid','console-missing','console-changed')) {
    $r = [TsfJobSupervisor]::Run($worker, 'limit', 1500, $fault)
    $expected = switch ($fault) { 'console-invalid' { 'Console process list unavailable' }; 'console-missing' { 'Console membership missing' }; default { 'Console membership changed before resume' } }
    if ($r.Success -or $r.Reason -cne 'controller-error' -or $r.Errors.Count -ne 1 -or $r.Errors[0] -cne $expected -or $r.ControlStarted -or $r.OutputRead -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.CleanupMilliseconds -ge 5000) { throw "Console negative control failed: $fault" }
    if ($fault -eq 'console-changed' -and ($r.ConsoleChangedPid -eq 0 -or -not $r.Assigned -or -not $r.RootSignaled -or -not $r.JobEmpty -or $r.TotalProcesses -ne 1)) { throw 'Real console change or suspended root retirement unproved' }
    Write-Host "TSF_CONSOLE_NEGATIVE case=$fault rejected=true handles_closed=true"
  }
  foreach ($case in @('normal','exit23','hang','descendant','race','unknown','orphan','limit','output','terminate-denied','query-denied')) {
    $mode = if ($case -in @('terminate-denied','query-denied')) { 'hang' } else { $case }
    $fault = if ($case -in @('terminate-denied','query-denied')) { $case } else { '' }
    $r = [TsfJobSupervisor]::Run($worker, $mode, 1500, $fault)
    Write-Host ('TSF_JOB_KERNEL_EVIDENCE ' + ($r | ConvertTo-Json -Depth 6 -Compress))
    if ($case -in @('unknown','orphan')) {
      if ($r.Success -or $r.Reason -cne 'controller-error' -or -not $r.TerminateSucceeded -or -not $r.RootSignaled -or -not $r.JobEmpty -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.Errors.Count -lt 1 -or $r.Errors[0] -notmatch '^Unexpected job identity:') { throw 'Unknown actual child was not rejected and cleaned up' }
      Write-Host "TSF_UNEXPECTED_CHILD case=$case actual=rejected cleanup=proved"
      continue
    }
    if ($r.RootHandleValue -eq 0 -or $r.JobHandleValue -eq 0 -or $r.RootWaitResult -ne 0 -or $r.SuspendedThreadCount -ne 1 -or $r.PreResumeActive -ne 1) { throw "Raw handle/wait/census evidence failed: $case" }
    if (-not $r.Assigned -or -not $r.MemberBeforeResume -or -not $r.SuspendedSingleThread -or $r.Pid -eq 0 -or $r.Birth -le 0 -or -not [string]::Equals($r.Image, $worker, [StringComparison]::OrdinalIgnoreCase) -or -not $r.RootSignaled -or -not $r.RootHandleClosed -or -not $r.ThreadHandleClosed -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.OwnedHandles -lt 7 -or $r.CleanupMilliseconds -ge 5000 -or $r.MainMilliseconds -gt 5000) { throw "Kernel ownership/lifetime evidence failed: $case" }
    if ($case -eq 'query-denied') {
      if ($r.Success -or $r.JobEmpty -or $r.OutputRead -or -not $r.ControlStarted -or $r.Reason -cne 'timeout' -or $r.ExitCode -ne 124 -or -not $r.TerminateRequested -or -not $r.TerminateSucceeded -or $r.Errors.Count -ne 2 -or $r.Errors[0] -cne 'QueryInformationJobObject Win32=5' -or $r.Errors[1] -cne 'Cleanup unproved within shared 5000ms budget') { throw 'Unproved cleanup accepted' }
      continue
    }
    if (-not $r.JobEmpty -or $r.LastJobActive -ne 0 -or -not $r.WritersRetired -or -not $r.ControlStarted -or $r.TotalProcesses -ne $r.Processes.Count -or @($r.Processes | Where-Object { -not $_.Member -or -not $_.Signaled -or -not $_.Closed -or $_.WaitResult -ne 0 -or $_.Birth -le 0 -or $_.HandleValue -eq 0 -or -not $_.Role }).Count -ne 0 -or @($r.Processes.Pid | Sort-Object -Unique).Count -ne $r.Processes.Count) { throw "Job identities not reaped: $case" }
    if ($case -eq 'normal' -and (-not $r.Success -or $r.ExitCode -ne 0 -or $r.Output -notmatch 'CONTROL normal' -or $r.Error -notmatch 'CONTROL stderr')) { throw 'Normal control failed' }
    if ($case -eq 'race' -and (-not $r.Success -or @($r.Processes | Where-Object Role -eq 'descendant').Count -ne 1)) { throw 'Exited retained child census failed' }
    if ($case -eq 'limit' -and (-not $r.Success -or $r.LimitChildRan -or $r.ActiveProcessLimit -ne 1 -or $r.TotalProcesses -ne 1 -or $r.Output -notmatch '(?m)^LIMIT_CHILD_DENIED win32=1816\r?$')) { throw 'Production process limit failed to deny child with ERROR_NOT_ENOUGH_QUOTA' }
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

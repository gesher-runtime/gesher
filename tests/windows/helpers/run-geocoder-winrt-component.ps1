$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'WinRT qualification is CI only' }
Add-Type -Path (Join-Path $PSScriptRoot '../../../tools/windows4/TsfJobSupervisor.cs')
$exe = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
foreach ($case in @('fixture', 'provider')) {
  $file = if ($case -eq 'fixture') { 'geocoder-winrt-fixture.ps1' } else { 'geocoder-provider-probe.ps1' }
  $path = Join-Path $PSScriptRoot $file
  $r = [TsfJobSupervisor]::Run($exe, ('-NoProfile -ExecutionPolicy Bypass -File "' + $path + '"'), 45000, '')
  Write-Host ('WINRT_JOB_KERNEL_EVIDENCE ' + ($r | ConvertTo-Json -Depth 6 -Compress))
  if (-not $r.Success -or $r.Reason -cne 'exit' -or $r.ExitCode -ne 0 -or $r.Errors.Count -ne 0) { throw "WinRT $case did not succeed" }
  if (-not $r.Assigned -or -not $r.MemberBeforeResume -or -not $r.SuspendedSingleThread -or $r.PreResumeActive -ne 1 -or $r.SuspendedThreadCount -ne 1) { throw "WinRT $case assignment missing" }
  if ($r.Pid -eq 0 -or $r.Birth -le 0 -or $r.RootHandleValue -eq 0 -or $r.JobHandleValue -eq 0 -or -not [string]::Equals($r.Image, $exe, [StringComparison]::OrdinalIgnoreCase)) { throw "WinRT $case identity missing" }
  if (-not $r.RootSignaled -or $r.RootWaitResult -ne 0 -or -not $r.RootHandleClosed -or -not $r.ThreadHandleClosed -or -not $r.JobEmpty -or $r.LastJobActive -ne 0 -or $r.OwnedHandles -ne $r.ClosedHandles -or $r.CleanupMilliseconds -ge 5000) { throw "WinRT $case not reaped" }
  if (-not $r.WritersRetired -or -not $r.OutputRead -or -not $r.ExclusiveSnapshot -or $r.StdoutBytes -gt 1048576 -or $r.StderrBytes -gt 1048576) { throw "WinRT $case output retirement missing" }
  if ($r.TotalProcesses -ne $r.Processes.Count -or @($r.Processes | Where-Object { -not $_.Member -or -not $_.Signaled -or -not $_.Closed -or $_.WaitResult -ne 0 -or $_.Birth -le 0 -or $_.HandleValue -eq 0 }).Count -ne 0) { throw "WinRT $case child census missing" }
  Write-Host $r.Output
  if ($case -eq 'fixture') {
    foreach ($name in @('success','error','timeout','late')) {
      if ($r.Output -notmatch "WINRT_ADAPTER case=$name .*disposed=True supplier_disposed=True") { throw "WinRT $name trace missing" }
    }
    if ($r.Output -notmatch 'WINRT_ORDER case=late bridge_completed_before_cancel=True') { throw 'WinRT completion ordering missing' }
  } else {
    if ($r.Output -notmatch '(?m)^status: [A-Za-z]+\r?$' -or $r.Output -notmatch 'WINRT_PROVIDER task_status=RanToCompletion result_type=Windows.Services.Maps.MapLocationFinderResult result_reads=1 resources_disposed=True') { throw 'Actual Maps provider result missing' }
  }
  Write-Host "PASS WinRT $case with actual bridge and owned child retirement"
}

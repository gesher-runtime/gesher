$ErrorActionPreference = 'Stop'
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $pwd 'routing-probe-deferred/deferred-state.exe'
$start.WorkingDirectory = Join-Path $pwd 'routing-probe-deferred'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
$lines = [System.Collections.Generic.List[string]]::new()
$witnessed = $false
$started = $false
try {
  if (-not $process.Start()) { throw 'Deferred fixture did not start' }
  $started = $true
  $errors = $process.StandardError.ReadToEndAsync()
  $next = $process.StandardOutput.ReadLineAsync()
  $deadline = [datetime]::UtcNow.AddSeconds(90)
  while ([datetime]::UtcNow -lt $deadline) {
    if ($next.IsCompleted) {
      $line = $next.GetAwaiter().GetResult()
      if ($null -eq $line) { break }
      Write-Host $line
      $lines.Add($line)
      if ($line -cmatch '^JOIN_WAIT_WITNESS tid=([0-9]+)$') {
        if ($witnessed) { throw 'Repeated join wait marker' }
        $joinerId = [int]$Matches[1]
        $waitDeadline = [datetime]::UtcNow.AddSeconds(15)
        $observations = [System.Collections.Generic.HashSet[string]]::new()
        while ([datetime]::UtcNow -lt $waitDeadline -and -not $process.HasExited) {
          $process.Refresh()
          $threads = @($process.Threads | Where-Object Id -eq $joinerId)
          if ($threads.Count -ne 1) { throw 'Specific live joiner thread missing or ambiguous' }
          $thread = $threads[0]
          $reason = if ($thread.ThreadState -eq [System.Diagnostics.ThreadState]::Wait) { "$($thread.WaitReason)" } else { 'not-waiting' }
          $sample = "state=$($thread.ThreadState) reason=$reason"
          if ($observations.Add($sample)) { Write-Host "DEFERRED_OS_WAIT_SAMPLE pid=$($process.Id) tid=$joinerId $sample" }
          # The hook signals without waiting immediately before the sole join
          # WaitForMultipleObjects; the target and cancellation stay withheld.
          if ($thread.ThreadState -eq [System.Diagnostics.ThreadState]::Wait -and
              $thread.WaitReason -eq [System.Diagnostics.ThreadWaitReason]::UserRequest) {
            Write-Host "DEFERRED_OS_WAIT pid=$($process.Id) tid=$joinerId state=$($thread.ThreadState) reason=$($thread.WaitReason) phase=after-nonblocking-join-hook target=held cancel=withheld"
            $process.StandardInput.WriteLine('observed')
            $process.StandardInput.Flush()
            $witnessed = $true
            break
          }
          Start-Sleep -Milliseconds 20
        }
        if (-not $witnessed) { throw 'Actual specific joiner UserRequest wait was not observed' }
      }
      $next = $process.StandardOutput.ReadLineAsync()
    } else { Start-Sleep -Milliseconds 20 }
  }
  if (-not $process.WaitForExit(5000)) { throw 'Deferred fixture deadline' }
  $process.Refresh()
  $errorText = $errors.GetAwaiter().GetResult()
  if ($errorText) { Write-Host $errorText }
  if ($process.ExitCode -ne 0) { throw "Actual deferred fixture exit=$($process.ExitCode)" }
  if (-not $witnessed) { throw 'Missing actual OS wait witness' }
  $text = $lines -join "`n"
  foreach ($proof in @(
    'PASS deferred finite state/type outputs and self cancellation cleanup=1 destructor=1',
    'PASS deferred public cancel capture crosses native ExitThread61 and returns ESRCH destructor=1',
    'PASS deferred disabled native ExitThread pending counter=0/1/0 destructor_delta=1',
    'PASS deferred disabled normal return pending counter=0/1/0 destructor=1',
    'PASS deferred genuine join wait cancellation unwinds claim/leases cleanup=1 destructor=1 target remains joinable',
    'PASS deferred-only public APIs and actual DLL unload handles='
  )) { if (-not $text.Contains($proof)) { throw "Missing deferred API witness: $proof" } }
  if ($text.Contains('FAIL ')) { throw 'Deferred fixture reported failure' }
} finally {
  if ($started -and -not $process.HasExited) {
    $process.Kill($true)
    if (-not $process.WaitForExit(5000)) { throw 'Owned deferred fixture cleanup failed' }
  }
  $process.Dispose()
}

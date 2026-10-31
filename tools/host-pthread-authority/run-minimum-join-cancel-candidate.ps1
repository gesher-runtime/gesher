# Source-only scaffold: never acknowledges a generic wait as the target join.
# Exact independently verified target/stack evidence and authority are not yet implemented.
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$ErrorActionPreference = 'Stop'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $pwd 'minimum-join-cancel/candidate.exe'
$start.WorkingDirectory = Join-Path $pwd 'minimum-join-cancel'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$started = $false
try {
  if (-not $process.Start()) { throw 'Failed to start owned fixture' }
  $started = $true
  $stderr = $process.StandardError.ReadToEndAsync()
  $next = $process.StandardOutput.ReadLineAsync()
  $deadline = [datetime]::UtcNow.AddSeconds(20)
  while ([datetime]::UtcNow -lt $deadline) {
    if ($next.IsCompleted) {
      $line = $next.GetAwaiter().GetResult()
      if ($null -eq $line) { throw 'Fixture exited before wait evidence' }
      Write-Host $line
      if ($line -cmatch '^JOIN_CANCEL_WAIT_WITNESS tid=([1-9][0-9]*)$') {
        $id = [int]$Matches[1]
        $sampleDeadline = [datetime]::UtcNow.AddSeconds(5)
        $foundWait = $false
        while ([datetime]::UtcNow -lt $sampleDeadline -and -not $process.HasExited) {
          $process.Refresh()
          $found = @($process.Threads | Where-Object Id -eq $id)
          if ($found.Count -ne 1) { throw 'Joiner thread missing or ambiguous' }
          $thread = $found[0]
          if ($thread.ThreadState -eq [Diagnostics.ThreadState]::Wait -and
              $thread.WaitReason -eq [Diagnostics.ThreadWaitReason]::UserRequest) {
            $foundWait = $true
            Write-Host "JOIN_CANCEL_OS_SAMPLE pid=$($process.Id) tid=$id state=Wait reason=UserRequest target_binding=UNPROVED"
            break
          }
          Start-Sleep -Milliseconds 20
        }
        if (-not $foundWait) { throw 'UNKNOWN: no live specific joiner UserRequest sample' }
        # This requests evidence only; it is not a successful witness acknowledgment.
        $process.StandardInput.WriteLine('sample')
        $process.StandardInput.Flush()
        $captureDeadline = [datetime]::UtcNow.AddSeconds(5)
        $frameCount = 0
        $captureComplete = $false
        $captureLine = $process.StandardOutput.ReadLineAsync()
        while ([datetime]::UtcNow -lt $captureDeadline) {
          if ($captureLine.IsCompleted) {
            $line = $captureLine.GetAwaiter().GetResult()
            if ($null -eq $line) { throw 'Fixture exited during stack capture' }
            Write-Host $line
            if ($line -cmatch '^JOIN_CANCEL_FRAME index=([0-9]+) pc=([0-9]+) image=([0-9]+) prefix=([0-9a-f]{32})$') { $frameCount++ }
            if ($line -ceq 'JOIN_CANCEL_STACK_COMPLETE') { $captureComplete = $true; break }
            $captureLine = $process.StandardOutput.ReadLineAsync()
          } else { Start-Sleep -Milliseconds 20 }
        }
        if (-not $captureComplete -or $frameCount -lt 1) { throw 'UNKNOWN: incomplete current stack capture' }
        throw 'UNKNOWN: authenticated callsite and handle dataflow not yet verified'
      }
      $next = $process.StandardOutput.ReadLineAsync()
    } else { Start-Sleep -Milliseconds 20 }
  }
  throw 'Fixture witness deadline'
} finally {
  if ($started -and -not $process.HasExited) {
    $process.Kill($true)
    if (-not $process.WaitForExit(5000)) { throw 'Owned fixture process did not terminate' }
  }
  if ($started -and $null -ne $stderr -and $stderr.IsCompleted) { Write-Host $stderr.GetAwaiter().GetResult() }
  $process.Dispose()
}

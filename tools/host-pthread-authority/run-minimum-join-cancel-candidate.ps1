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
        $process.Refresh()
        $found = @($process.Threads | Where-Object Id -eq $id)
        if ($found.Count -ne 1) { throw 'Joiner thread missing or ambiguous' }
        $thread = $found[0]
        $reason = if ($thread.ThreadState -eq [Diagnostics.ThreadState]::Wait) { "$($thread.WaitReason)" } else { 'not-waiting' }
        Write-Host "JOIN_CANCEL_OS_SAMPLE pid=$($process.Id) tid=$id state=$($thread.ThreadState) reason=$reason target_binding=UNPROVED"
        throw 'UNKNOWN: actual target-bound join wait witness is not implemented'
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

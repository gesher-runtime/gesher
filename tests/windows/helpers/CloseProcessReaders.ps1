. "$PSScriptRoot/ProcessReaderHandles.ps1"

# Cleanup after the caller's ORIGINAL per-reader deadlines. This cannot make
# an expired EOF deadline successful. Pipes are captured before read startup,
# so all acquired resources are attempted even if another startup fails.
function Close-ProcessReaders($Cancellation, [object[]] $Pipes) {
  $problems = @()
  $late = ''
  try { $Cancellation.Cancel() } catch { $problems += "reader cancellation failed: $_" }
  $tasks = [Threading.Tasks.Task[]]@($Pipes | Where-Object { $null -ne $_.Task } | ForEach-Object { $_.Task })
  $complete = $false
  if ($tasks.Count) {
    try { [void][Threading.Tasks.Task]::WaitAll($tasks, 5000) }
    catch {
      foreach ($task in $tasks) { if ($task.IsFaulted) { $problems += "reader cleanup fault: $($task.Exception)" } }
    }
    $complete = @($tasks | Where-Object { -not $_.IsCompleted }).Count -eq 0
    if (-not $complete) { $problems += 'readers did not stop within five seconds after cancellation' }
  }
  $closed = 0
  foreach ($pipe in $Pipes) {
    # Preserve a stream that finished normally in cleanup, never read a
    # canceled/unfinished Task.Result or claim its accumulated prefix exists.
    if ($null -ne $pipe.Task -and -not $pipe.Copied -and
        $pipe.Task.Status -eq [Threading.Tasks.TaskStatus]::RanToCompletion) { $late += $pipe.Task.Result }
    if ($null -ne $pipe.Reader) {
      try {
        if ($null -ne $pipe.Handle) {
          if ([GtestProcessNativeHandles]::CloseOwnedReader($pipe.Reader, $pipe.Handle)) { $closed++ }
          else { $problems += "$($pipe.Name) owned pipe did not prove immediate native invalidity" }
        } else {
          $pipe.Reader.Dispose()
          $problems += "$($pipe.Name) reader started without captured pipe ownership"
        }
      } catch { $problems += "$($pipe.Name) reader disposal failed: $_" }
    }
  }
  try { $Cancellation.Dispose() } catch { $problems += "reader cancellation disposal failed: $_" }
  return [pscustomobject]@{ TasksComplete = $complete; PipesClosed = $closed -eq 2;
    LateText = $late; Problem = $problems -join '; ' }
}

# Finite process/output completion for the two real gtest callers. Test parsing
# and every original timeout/exclusion remain in their respective tracks.
function Invoke-GtestProcess([string] $Executable, [string[]] $Arguments,
                             [string] $WorkingDirectory, [int] $TimeoutSec) {
  $process = [Diagnostics.Process]::new()
  $start = [Diagnostics.ProcessStartInfo]::new($Executable)
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.WorkingDirectory = $WorkingDirectory
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
  $process.StartInfo = $start
  $stdout = $null
  $stderr = $null
  $started = $false
  $handle = $null
  $problems = @()
  $result = [pscustomobject]@{ Stdout = ''; Stderr = ''; Exit = $null;
    TimedOut = $false; ProcessComplete = $false; ReadersComplete = $false;
    Disposed = $false; Problem = '' }
  try {
    if (-not $process.Start()) { throw 'gtest process did not start' }
    $started = $true
    $null = $process.Handle
    $handle = $process.SafeHandle
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $result.TimedOut = -not $process.WaitForExit($TimeoutSec * 1000)
    if ($result.TimedOut) {
      try { $process.Kill($true) } catch { $problems += "kill: $_" }
      $result.ProcessComplete = $process.WaitForExit(5000)
    } else { $result.ProcessComplete = $true }
    if ($result.ProcessComplete) {
      $process.Refresh()
      $result.Exit = $process.ExitCode
    } else { $problems += 'gtest process did not terminate within five seconds after kill' }
    try {
      $result.ReadersComplete = [Threading.Tasks.Task]::WaitAll(
        [Threading.Tasks.Task[]]@($stdout, $stderr), 10000)
      if (-not $result.ReadersComplete) { $problems += 'gtest output did not complete within ten seconds' }
    } catch { $problems += "gtest output reader: $_" }
  } catch { $problems += "gtest process: $_" }
  finally {
    if ($started) {
      try {
        if (-not $process.HasExited) {
          $process.Kill($true)
          if (-not $process.WaitForExit(5000)) { $problems += 'gtest final owned cleanup did not terminate within five seconds' }
        }
      } catch { $problems += "gtest final owned cleanup: $_" }
    }
    # Never read Result on an unfinished/faulted task, including a dead parent
    # whose real child retains the pipe. Preserve either completed stream.
    if ($null -ne $stdout -and $stdout.Status -eq [Threading.Tasks.TaskStatus]::RanToCompletion) { $result.Stdout = $stdout.Result }
    if ($null -ne $stderr -and $stderr.Status -eq [Threading.Tasks.TaskStatus]::RanToCompletion) { $result.Stderr = $stderr.Result }
    try { $process.Dispose(); $result.Disposed = $null -eq $handle -or $handle.IsClosed }
    catch { $problems += "gtest dispose: $_" }
  }
  $result.Problem = $problems -join '; '
  return $result
}

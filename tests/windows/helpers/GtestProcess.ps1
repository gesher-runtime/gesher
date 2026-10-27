# Finite process/output completion for the two real gtest callers. Test parsing
# and every original timeout/exclusion remain in their respective tracks.
if (-not ('GtestProcessNativeHandles' -as [type])) {
  # This test infrastructure is compiled only when the CI scripts execute.
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class GtestProcessNativeHandles {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetHandleInformation(IntPtr handle, out uint flags);
  static GtestProcessNativeHandles() {
    // Resolve/JIT the P/Invoke before the first close/probe interval.
    uint flags;
    GetHandleInformation(IntPtr.Zero, out flags);
  }
  public static bool CloseOwnedReader(StreamReader reader, SafeFileHandle owned) {
    var file = reader.BaseStream as FileStream;
    bool same = file != null && Object.ReferenceEquals(file.SafeFileHandle, owned);
    IntPtr value = owned.DangerousGetHandle();
    reader.Dispose();
    uint flags;
    // Pair the exact owned SafeFileHandle with its stream BEFORE disposal.
    // No allocation/open occurs between that disposal and the native probe.
    // A concurrently reused value that is valid is conservatively a failure.
    return same && owned.IsClosed && !GetHandleInformation(value, out flags) && Marshal.GetLastWin32Error() == 6;
  }
}
'@
}
function Invoke-GtestProcess([string] $Executable, [string[]] $Arguments,
                             [string] $WorkingDirectory, [int] $TimeoutSec) {
  $Executable = (Resolve-Path -LiteralPath $Executable -ErrorAction Stop).Path
  $WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory -ErrorAction Stop).Path
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
  $outReader = $null
  $errReader = $null
  $outHandle = $null
  $errHandle = $null
  $cancellation = [Threading.CancellationTokenSource]::new()
  $started = $false
  $handle = $null
  $problems = @()
  $result = [pscustomobject]@{ Stdout = ''; Stderr = ''; Exit = $null;
    TimedOut = $false; ProcessComplete = $false; ReadersComplete = $false;
    Disposed = $false; ReaderTasksComplete = $false; PipesClosed = $false;
    StdoutHandle = [IntPtr]::Zero; StderrHandle = [IntPtr]::Zero; Problem = '' }
  try {
    if (-not $process.Start()) { throw 'gtest process did not start' }
    $started = $true
    $null = $process.Handle
    $handle = $process.SafeHandle
    $outReader = $process.StandardOutput
    $errReader = $process.StandardError
    $outHandle = $outReader.BaseStream.SafeFileHandle
    $errHandle = $errReader.BaseStream.SafeFileHandle
    $result.StdoutHandle = $outHandle.DangerousGetHandle()
    $result.StderrHandle = $errHandle.DangerousGetHandle()
    $stdout = $outReader.ReadToEndAsync($cancellation.Token)
    $stderr = $errReader.ReadToEndAsync($cancellation.Token)
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
    # Process.Dispose deliberately does not close user-accessed readers. Stop
    # their pending I/O first, prove both tasks finished within a cleanup bound,
    # then explicitly dispose both readers. Cancellation never makes an expired
    # original EOF/exit deadline successful.
    try { $cancellation.Cancel() } catch { $problems += "gtest reader cancellation: $_" }
    $tasks = [Threading.Tasks.Task[]]@(@($stdout, $stderr) | Where-Object { $null -ne $_ })
    if ($tasks.Count) {
      try { [void][Threading.Tasks.Task]::WaitAll($tasks, 5000) }
      catch { # Canceled/faulted tasks are inspected below; no unbounded wait.
        foreach ($task in $tasks) {
          if ($task.IsFaulted) { $problems += "gtest reader cleanup fault: $($task.Exception)" }
        }
      }
      $result.ReaderTasksComplete = @($tasks | Where-Object { -not $_.IsCompleted }).Count -eq 0
      if (-not $result.ReaderTasksComplete) { $problems += 'gtest readers did not stop within five seconds after cancellation' }
    }
    $closedPipes = 0
    foreach ($pipe in @(@{ Reader = $outReader; Handle = $outHandle }, @{ Reader = $errReader; Handle = $errHandle })) {
      if ($null -ne $pipe.Reader) {
        try {
          if ([GtestProcessNativeHandles]::CloseOwnedReader($pipe.Reader, $pipe.Handle)) { $closedPipes++ }
          else { $problems += 'gtest owned pipe did not prove native invalidity immediately after disposal' }
        } catch { $problems += "gtest reader dispose: $_" }
      }
    }
    $result.PipesClosed = $closedPipes -eq 2
    try { $cancellation.Dispose() } catch { $problems += "gtest cancellation dispose: $_" }
    try { $process.Dispose(); $result.Disposed = $null -eq $handle -or $handle.IsClosed }
    catch { $problems += "gtest dispose: $_" }
  }
  $result.Problem = $problems -join '; '
  return $result
}

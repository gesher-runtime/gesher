# The child is a precompiled, single-process native window fixture. Its setup
# never invokes a shell or a compiler inside the 15-second readiness budget.
# This module is used by run-tests.ps1 (PowerShell 7) and the short Windows gate.
function Start-GesherForegroundHelper([string] $Executable, [string] $Receipt,
                                       [string] $Stop, [string] $Diagnostics) {
  if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Native foreground helper was not built: $Executable" }
  if ((Test-Path -LiteralPath $Receipt) -or (Test-Path -LiteralPath $Stop)) {
    throw 'Foreground helper paths must belong to a fresh invocation'
  }
  [void][IO.Directory]::CreateDirectory($Diagnostics)
  $self = [Diagnostics.Process]::GetCurrentProcess()
  try { $parentCreated = $self.StartTime.ToUniversalTime().ToFileTimeUtc() }
  finally { $self.Dispose() }
  $info = [Diagnostics.ProcessStartInfo]::new()
  $info.FileName = (Resolve-Path -LiteralPath $Executable).Path
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($argument in @('--receipt', [IO.Path]::GetFullPath($Receipt), '--stop',
      [IO.Path]::GetFullPath($Stop), '--parent', "$PID", '--parent-created', "$parentCreated")) {
    $info.ArgumentList.Add($argument)
  }
  $process = [Diagnostics.Process]::Start($info)
  [pscustomobject]@{
    Process = $process
    Stdout = $process.StandardOutput.ReadToEndAsync()
    Stderr = $process.StandardError.ReadToEndAsync()
    Stop = [IO.Path]::GetFullPath($Stop)
    Diagnostics = [IO.Path]::GetFullPath($Diagnostics)
  }
}

function Stop-GesherForegroundHelper($Helper) {
  if ($null -eq $Helper) { return }
  $problems = @()
  $process = $Helper.Process
  try {
    try { [IO.File]::WriteAllText($Helper.Stop, 'stop') }
    catch { $problems += "owned foreground stop request failed: $_" }
    if (-not $process.WaitForExit(10000)) {
      $problems += 'owned foreground helper did not retire'
      # The retained Process handle identifies this invocation, not a reused PID.
      # The native helper creates no descendants, but retire any owned tree too.
      $process.Kill($true)
      if (-not $process.WaitForExit(5000)) { $problems += 'owned foreground helper survived termination' }
    } elseif ($process.ExitCode -ne 0) {
      $problems += "owned foreground helper exited $($process.ExitCode)"
    }
    foreach ($stream in @(@('stdout', $Helper.Stdout), @('stderr', $Helper.Stderr))) {
      if (-not $stream[1].Wait(5000)) {
        $problems += "owned foreground $($stream[0]) did not close"
        continue
      }
      $content = $stream[1].Result
      [IO.File]::WriteAllText((Join-Path $Helper.Diagnostics ($stream[0] + '.log')), $content)
      if ($content) { Write-Host $content.TrimEnd() }
    }
  } catch { $problems += "owned foreground cleanup failed: $_" }
  finally { $process.Dispose() }
  $problems
}

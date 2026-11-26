# Run the installed launcher's real command line with redirected handles while
# allowing callers to observe its window before waiting for normal completion.
function Invoke-PackageLauncher(
    [Diagnostics.ProcessStartInfo] $StartInfo, [string] $Name,
    [int] $TimeoutSeconds = 120, [scriptblock] $Observe) {
  $StartInfo.UseShellExecute = $false
  $StartInfo.RedirectStandardOutput = $true
  $StartInfo.RedirectStandardError = $true
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $StartInfo
  $cancellation = [Threading.CancellationTokenSource]::new()
  $started = $false
  $stdoutReader = $null
  $stderrReader = $null
  try {
    $started = $process.Start()
    if (-not $started) { throw "Could not start $Name" }
    $stdoutReader = $process.StandardOutput
    $stderrReader = $process.StandardError
    $stdout = $stdoutReader.ReadToEndAsync($cancellation.Token)
    $stderr = $stderrReader.ReadToEndAsync($cancellation.Token)
    $observation = @()
    if ($Observe) { $observation = @(& $Observe $process) }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { throw "$Name did not exit within ${TimeoutSeconds}s" }
    # A child can retain the output handles after its launcher has exited.
    if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), 5000)) {
      throw "$Name output pipes did not close within 5s of launcher exit"
    }
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result
      Observation = $observation }
  } finally {
    try {
      if ($started -and -not $process.HasExited) {
        try { $process.Kill($true) }
        catch { if (-not $process.HasExited) { throw } }
        if (-not $process.WaitForExit(5000)) { throw "$Name did not exit within 5s of owned tree cleanup" }
      }
    } finally {
      try {
        $cancellation.Cancel()
        if ($stdoutReader) { $stdoutReader.Dispose() }
        if ($stderrReader) { $stderrReader.Dispose() }
      } finally {
        $process.Dispose()
        $cancellation.Dispose()
      }
    }
  }
}

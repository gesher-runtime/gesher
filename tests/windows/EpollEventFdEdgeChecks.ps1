# Focused real-production Windows gate, using an exact-source epoll_probe build.
param([Parameter(Mandatory = $true)][string] $RuntimeDir)
$ErrorActionPreference = 'Stop'
$executable = Join-Path (Resolve-Path $RuntimeDir) 'epoll_probe.exe'
if (-not (Test-Path $executable -PathType Leaf)) { throw "Missing production probe: $executable" }
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $executable
$start.Arguments = '--eventfd-edge'
$start.WorkingDirectory = (Resolve-Path $RuntimeDir).Path
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
try {
  if (-not $process.Start()) { throw 'Could not start production epoll probe' }
  $output = $process.StandardOutput.ReadToEndAsync()
  $errorOutput = $process.StandardError.ReadToEndAsync()
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    $process.WaitForExit()
    throw 'Production eventfd edge gate exceeded 30 seconds'
  }
  $text = $output.GetAwaiter().GetResult() + $errorOutput.GetAwaiter().GetResult()
  Write-Host $text
  if ($process.ExitCode -ne 0 -or $text -match '(?m)^FAIL' -or
      $text -notmatch 'eventfd edge contracts: complete' -or
      $text -notmatch 'eventfd edge wait contracts: complete' -or
      $text -notmatch 'eventfd edge Windows: all passed') {
    throw "Production eventfd edge gate failed (exit $($process.ExitCode))"
  }
  Write-Host 'PASS eventfd edge production Windows gate'
} finally {
  $process.Dispose()
}

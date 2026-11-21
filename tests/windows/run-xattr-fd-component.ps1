param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path (Join-Path $Directory 'xattr_fd_host_x86_64.exe')).Path
$start = [Diagnostics.ProcessStartInfo]::new($exe)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$started = $false
try {
  $started = $process.Start()
  if (-not $started) { throw 'Native xattr fixture did not start' }
  $output = $process.StandardOutput.ReadToEndAsync()
  $errors = $process.StandardError.ReadToEndAsync()
  if (-not $process.WaitForExit(120000)) {
    $process.Kill($true)
    $process.WaitForExit()
    throw 'Native xattr fixture exceeded 120 seconds'
  }
  $text = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
  $text | Tee-Object -FilePath xattr-windows-results.txt
  if ($process.ExitCode -ne 0 -or $text -match '(?m)^FAIL ' -or
      $text -notmatch 'xattr fd contracts: \d+ checks, 0 failures' -or
      $text -notmatch 'xattr Windows contracts: \d+ checks, 0 failures' -or
      $text -notmatch 'xattr authorization measurement: 1000 gets,') {
    throw "Native xattr contracts failed with exit $($process.ExitCode)"
  }
} finally {
  if ($started -and -not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
  $process.Dispose()
}

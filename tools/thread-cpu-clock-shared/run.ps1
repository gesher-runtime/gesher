param([Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference = 'Stop'
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
python tools/thread-cpu-clock-shared/component.py verify $Fixture
if ($LASTEXITCODE -ne 0) { throw 'Fixture source/binary verification failed' }
$stdout = Join-Path $env:RUNNER_TEMP 'thread-cpu-clock-shared.stdout.log'
$stderr = Join-Path $env:RUNNER_TEMP 'thread-cpu-clock-shared.stderr.log'
$process = Start-Process -FilePath (Join-Path $Fixture 'clock-shared.exe') -WorkingDirectory $Fixture `
  -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
try {
  if (-not $process.WaitForExit(180000)) {
    $process.Kill($true)
    $process.WaitForExit()
    throw 'Actual Windows component timed out after 180 seconds'
  }
  $process.Refresh()
  Get-Content -LiteralPath $stdout
  Get-Content -LiteralPath $stderr
  if ($process.ExitCode -ne 0) { throw "Actual Windows component failed with exit $($process.ExitCode)" }
  $text = Get-Content -LiteralPath $stdout -Raw
  if ($text -notmatch 'PASS N28 shared supplier: component completed;') { throw 'Component completion record missing' }
} finally {
  if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
  $process.Dispose()
}

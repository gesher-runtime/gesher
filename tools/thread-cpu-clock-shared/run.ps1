param([Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference = 'Stop'
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
python tools/thread-cpu-clock-shared/component.py verify $Fixture
if ($LASTEXITCODE -ne 0) { throw 'Fixture source/binary verification failed' }
function Invoke-ClockFixture([string]$Name, [string]$Completion) {
  $stdout = Join-Path $env:RUNNER_TEMP "thread-cpu-clock-shared.$Name.stdout.log"
  $stderr = Join-Path $env:RUNNER_TEMP "thread-cpu-clock-shared.$Name.stderr.log"
  $process = Start-Process -FilePath (Join-Path $Fixture "$Name.exe") -WorkingDirectory $Fixture `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
  try {
    if (-not $process.WaitForExit(180000)) { throw "$Name timed out after 180 seconds" }
    $process.Refresh()
    Get-Content -LiteralPath $stdout
    Get-Content -LiteralPath $stderr
    if ($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode)" }
    $text = Get-Content -LiteralPath $stdout -Raw
    if (-not $text -or -not $text.Contains($Completion)) { throw "$Name completion record missing" }
  } finally {
    if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
    $process.Dispose()
  }
}
Invoke-ClockFixture 'clock-shared' 'PASS N28 shared supplier: component completed;'
Invoke-ClockFixture 'clock-unload' 'PASS N28 shared supplier: unload released all retained clock handles'

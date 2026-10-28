# Canonical full: exact qualified156 native controller/watchdog body.
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$ErrorActionPreference = 'Stop'
$stdout = Join-Path $env:RUNNER_TEMP commands.stdout.txt
$stderr = Join-Path $env:RUNNER_TEMP commands.stderr.txt
$pythonCommand = Get-Command python -CommandType Application | Select-Object -First 1
$python = $pythonCommand.Source
if ($python -isnot [string] -or -not (Test-Path -LiteralPath $python -PathType Leaf) -or
    [IO.Path]::GetExtension($python) -ine '.exe') { throw 'Missing single actual Python executable' }
Write-Host "ACTUAL_COMMAND_PYTHON path=$python"
& $python --version
if ($LASTEXITCODE -ne 0) { throw 'Selected Python executable failed version check' }
foreach ($name in 'ucrtbase.dll','kernelbase.dll','ntdll.dll') {
  $modulePath = Join-Path $env:SystemRoot "System32/$name"
  $moduleInfo = Get-Item -LiteralPath $modulePath
  Write-Host "ACTUAL_SYSTEM_MODULE name=$name path=$modulePath version=$($moduleInfo.VersionInfo.FileVersion) sha256=$((Get-FileHash -LiteralPath $modulePath -Algorithm SHA256).Hash)"
}
$process = $null
try {
  $process = Start-Process -FilePath $python -ArgumentList @('tests/native3-locks/command_oracle.py', '--worker', 'command-fixture/native3_lock_command_worker.exe', '--reference', 'command-fixture/linux-reference.json', '--output', 'command-fixture/windows-actual.json', '--duplicate-worker', 'command-fixture/native3_lock_duplicate_test.exe', '--broker', 'command-fixture/binderd.exe') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  if (-not $process.WaitForExit(180000)) { throw 'Actual command and duplicate suites exceeded external180s bound' }
  $process.Refresh()
  & $python tests/native3-locks/validate_stdio_evidence.py --stderr $stderr --output command-fixture/stdio-evidence.json
  if ($LASTEXITCODE -ne 0) { throw 'Missing or inconsistent four-cycle actual stdio conjunctive evidence/route' }
  if ($process.ExitCode -ne 0) { throw "Actual command suite exit=$($process.ExitCode)" }
  if (-not (Select-String -LiteralPath $stdout -Pattern '^ACTUAL_COMMAND_ORACLE rows=[1-9][0-9]* mismatches=0$' -Quiet)) { throw 'Missing actual command comparator completion' }
  if (-not (Select-String -LiteralPath $stdout -Pattern '^ACTUAL_DUPLICATE_ORACLE modes=13 failures=0 semantics=bounded-resource-controls$' -Quiet)) { throw 'Missing actual duplicate controller completion' }
} finally {
  $cleanupProblems = @()
  if ($null -ne $process) {
    try {
      if (-not $process.HasExited) {
        $process.Kill($true)
        if (-not $process.WaitForExit(5000)) { $cleanupProblems += 'Owned command process tree did not terminate' }
      }
    } catch { $cleanupProblems += "Owned command cleanup: $_" }
    try { $process.Dispose() } catch { $cleanupProblems += "Owned command disposal: $_" }
  }
  try { if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout } }
  catch { $cleanupProblems += "Actual stdout replay: $_" }
  try { if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr } }
  catch { $cleanupProblems += "Actual stderr replay: $_" }
  if ($cleanupProblems.Count) { throw ($cleanupProblems -join '; ') }
}
